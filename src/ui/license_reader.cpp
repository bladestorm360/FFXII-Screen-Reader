#include "ui/license_reader.h"
#include "ui/menu_state.h"
#include "ui/text_capture.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "speech/speech.h"
#include "speech/phrasebook.h"
#include "core/logger.h"
#include "input/input_tracker.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

using MemRead::Obj0;
using MemRead::PtrAt;
using MemRead::SafeReadPtr;
using MemRead::SafeReadU8;
using MemRead::SafeReadU16;
using MemRead::SafeReadU32;
using MemRead::SafeReadU64;
using MemRead::SafeReadS16;

// ---- controller procs (dedicated window-proc hooks) --------------------------------------------
constexpr uint32_t RVA_CHARSEL_WND = 0x440910;  // FUN_00560910 — license character-select
constexpr uint32_t RVA_BOARD_WND   = 0x43CD40;  // FUN_0055cd40 — license board node grid

// ---- pure getters (game calls; game thread only) ------------------------------------------------
constexpr uint32_t RVA_RESOLVE_DEF = 0x23D330;  // FUN_0035d330(cat,id) -> record (name codec @ +0x18)
constexpr uint32_t RVA_RESOLVE_MSG = 0x1D9860;  // FUN_002f9860(id) -> codec ptr (menu message books)
constexpr uint32_t RVA_NODE_STATUS = 0x203600;  // FUN_00323600(charId, panel, flags) -> status byte

// ---- globals ------------------------------------------------------------------------------------
constexpr uint32_t RVA_PAUSE_CTX  = 0x1F7AC30;  // DAT_0209ac30 (ptr) — pause-menu context
constexpr uint32_t RVA_SAVE_BLOCK = 0x2D9F190;  // DAT_02ebf190 (ptr) — party save block

// ---- pause-context offsets ----------------------------------------------------------------------
constexpr uint32_t OFF_CTX_LIC_CTRL   = 0x158;  // ctx+0x158 = char-select controller
constexpr uint32_t OFF_CTX_BOARD      = 0x320;  // ctx+0x320 = active board sub-window (ring OR grid)
constexpr uint32_t OFF_CTX_BLOCKS     = 0xAC8;  // ctx+0xac8 + memberIdx*8 = per-char HUD block (== FUN_00282df0)
constexpr uint32_t OFF_CTX_ACTINGCHAR = 0xDE0;  // ctx+0xde0 = highlighted/acting member (i16) — set by
                                                // FUN_00285f20 first, then copied to ctrl+0xd0 by FUN_00560ee0

// ---- HUD block ----------------------------------------------------------------------------------
constexpr uint32_t OFF_BLK_CHARID = 0x60;       // block+0x60 = char id (i16; < 0 = empty slot)

// ---- save record (base +8, stride 0x1c8, indexed by charId) -------------------------------------
constexpr uint32_t SAVE_BASE_OFF = 8;
constexpr uint32_t SAVE_STRIDE   = 0x1C8;
constexpr uint32_t OFF_SAVE_LP   = 0x190;       // current License Points (u32)
constexpr uint32_t OFF_SAVE_JOB1 = 0x1C3;       // primary job (u8; 0xFF = none)
constexpr uint32_t OFF_SAVE_JOB2 = 0x1C4;       // secondary job (u8; 0xFF = none)

// ---- board grid ---------------------------------------------------------------------------------
constexpr uint32_t OFF_BOARD_MEMBER = 0x160;    // board+0x160 = active member index
constexpr uint32_t OFF_BOARD_JOB    = 0x558;    // board+0x558 = viewed job/board id (0..0xB)

// ---- board cell (stride 0x38, array at board+0x120) ---------------------------------------------
constexpr uint32_t OFF_CELL_NAME     = 0x00;    // name codec ptr (already variant-selected by the builder)
constexpr uint32_t OFF_CELL_ID       = 0x08;    // node/panel id (u16; 0xFFFF = locked/not-yet-reachable)
constexpr uint32_t OFF_CELL_COST     = 0x0C;    // LP cost (u16)
constexpr uint32_t OFF_CELL_CATEGORY = 0x10;    // CATEGORY word codec ("Weapon"/"Magick"), NOT a
                                                // description — it is the ball icon's text equivalent

// cell+0x18 — THE WORD THE GAME'S OWN CONFIRM BRANCH TESTS. `FUN_0055cd40` case 0xc / sub-message
// 0x8001 is the Confirm handler, and it decides accept-vs-buzz from this one word and nothing else:
//
//     uVar9 = *(uint *)(cell + 0x18);
//     if (((uVar9 & 0x2000) == 0) && ((uVar9 >> 0xc & 1) != 0)) {   // not learned AND reachable
//         if ((uVar9 & 0x6000) != 0) { ...purchase confirm...; FUN_00249c60(0x25); return; }
//         FUN_002ce2f0(board, 10);                                  // the not-enough-LP popup
//     }
//     FUN_00249c60(5);                                              // the invalid-action sound
//
// Bit 0x1000 is set by `FUN_0055e090`, which walks every LEARNED cell and promotes its four
// orthogonal neighbours — that is the board's adjacency rule, and it is the ONLY place reachability
// exists. `FUN_00323600` (below) has no adjacency test of any kind, which is why a node three tiles
// past the frontier was announced as available and then buzzed when confirmed.
//
// Reading the same word the game branches on is what makes the spoken status agree with the sound by
// construction, rather than by a second model that can drift out of step with it.
constexpr uint32_t OFF_CELL_FLAGS   = 0x18;
constexpr uint32_t CELL_REACHABLE   = 0x1000;   // prerequisites met (touches a learned node)
constexpr uint32_t CELL_LEARNED     = 0x2000;
// LP >= cost. NEVER SPOKEN -- see StatusWordFromFlags. Named here because this is the bit table for
// the word, and because the whole word goes to the log (`flags=0x%04X`), where this bit is readable.
constexpr uint32_t CELL_AFFORDABLE  = 0x4000;

// ---- FUN_0035d330 resolved record (shared scratch at DAT_022ca520) ------------------------------
constexpr uint32_t OFF_REC_DESC = 0x08;         // secondary codec — the entry's description, when it has one
constexpr uint32_t OFF_REC_NAME = 0x18;         // name codec
constexpr uint32_t OFF_REC_KIND = 0x23;         // granted-entry resolver kind (0/1/2/3; 0xFF = no entry list)
constexpr uint32_t OFF_REC_IDS  = 0x26;         // 8 x u16 granted action ids
constexpr int      GRANT_MAX    = 8;

// The panel draws an entry's DESCRIPTION only for kind-1 entries whose id lands in the technick
// block — FUN_00559e30's `(ushort)(id - 0x9e) < 0x18` test. Magicks share kind 1 but sit outside
// that range, and kind-0 gear / kind-2/3 entries never render one. Speaking a description outside
// this test would give the player text the game does not draw.
constexpr uint16_t TECHNICK_ID_LO   = 0x9E;
constexpr uint16_t TECHNICK_ID_SPAN = 0x18;

// ---- job-select ring ----------------------------------------------------------------------------
constexpr uint32_t OFF_RING_JOB = 0x358;        // ring+0x358 = highlighted job id (u8; 0..0xB)

// The `F` ability-summary pages reached from the board are a SEPARATE subsystem (a shared
// party-member detail overlay, not part of the license module) — see ui/ability_summary_reader.h.

// ---- window message packet (proc hooks) ---------------------------------------------------------
constexpr uint32_t PKT_CAT_OFF = 0x00;          // *(int*)packet        = category
constexpr uint32_t PKT_MSG_OFF = 0x08;          // *(int64*)(packet+8)  = subcode (for cat 0xc)
constexpr uint32_t CAT_NOTIFY  = 0x0C;
constexpr uint32_t CAT_SHOW    = 0x13;          // menu-visible frame (entry)
constexpr uint32_t CAT_CLOSE   = 0x12;          // teardown
constexpr uint64_t SUB_FOCUS   = 0x8000;        // cursor move

// ---- message-id formulas / resolver categories --------------------------------------------------
constexpr int JOB_NAME_BASE = 0x3ED;            // FUN_002f9860(job + 0x3ED) -> job name  (0x3ED..0x3F8)
constexpr int JOB_DESC_BASE = 0x838;            // FUN_002f9860(job + 0x838) -> job description (0x838..0x843)
constexpr uint32_t CAT_CHARNAME = 2;            // FUN_0035d330 category for character names
constexpr uint32_t CAT_LICENSE  = 0x19;         // license node record
constexpr uint32_t CAT_GEAR     = 0x01;         // granted equipment  (id passed as id<<16)
constexpr uint32_t CAT_MAGICK   = 0x14;         // granted magick
constexpr uint32_t CAT_TECHNICK = 0x1D;         // granted technick / augment
constexpr int JOB_COUNT = 12;

typedef uint64_t (*Pfn_Wnd)(void*, void*);                     // FUN_00560910 / FUN_0055cd40 window procs
typedef const uint8_t* (*Pfn_ResolveDef)(uint32_t, uint32_t);  // FUN_0035d330(cat, id)
typedef const uint8_t* (*Pfn_ResolveMsg)(int);                 // FUN_002f9860(id)
typedef uint8_t (*Pfn_Status)(int, int, int);                  // FUN_00323600(charId, panel, flags)

Pfn_Wnd s_origCharSelWnd = nullptr;
Pfn_Wnd s_origBoardWnd   = nullptr;

// One-shot guards for the possibly-repeating SHOW (cat 0x13) of each controller. NOT a speech
// dedup: they guard the repeating 0x13 message (named here per the CLAUDE.md rule) so entry is
// announced once per controller instance and re-announced on re-open (a fresh instance) / after
// close (cat 0x12). Cursor MOVES are never guarded. Touched only on the game thread.
void* g_charSelShown = nullptr;
void* g_boardShown   = nullptr;

// ---- basic reads --------------------------------------------------------------------------------

void* PauseCtx() {
    void* ctx = nullptr;
    return SafeReadPtr(Hooks::ResolveRva(RVA_PAUSE_CTX), &ctx) ? ctx : nullptr;
}

// HUD block for a member index (== FUN_00282df0): *(ctx+0xac8 + idx*8).
void* MemberBlock(void* ctx, int memberIdx) {
    if (!ctx || memberIdx < 0 || memberIdx > 0x27) return nullptr;
    return PtrAt(ctx, OFF_CTX_BLOCKS + static_cast<uint32_t>(memberIdx) * 8);
}

// Char id from a HUD block (block+0x60, i16). Returns -1 on fault / empty slot.
int CharIdOf(void* block) {
    int16_t id;
    return (block && SafeReadS16(block, OFF_BLK_CHARID, &id) && id >= 0) ? id : -1;
}

// Save record for a char id: *(DAT_02ebf190) + 8 + charId*0x1c8. Null on fault / out of range.
void* SaveRecord(int charId) {
    if (charId < 0 || charId > 0x27) return nullptr;
    void* base = nullptr;
    if (!SafeReadPtr(Hooks::ResolveRva(RVA_SAVE_BLOCK), &base) || !base) return nullptr;
    return reinterpret_cast<char*>(base) + SAVE_BASE_OFF + static_cast<size_t>(charId) * SAVE_STRIDE;
}

// Current LP for a char id (save+0x190). -1 on fault.
long CurrentLP(int charId) {
    void* rec = SaveRecord(charId);
    uint32_t lp;
    return (rec && SafeReadU32(rec, OFF_SAVE_LP, &lp)) ? static_cast<long>(lp) : -1;
}

// ---- text (game getters + codec decode) ---------------------------------------------------------

// Decode a codec string (GameText guards internally). `skip` strips the shared-pool 00 00 prefix.
std::wstring DecodeCodec(const uint8_t* codec, bool skip) {
    if (!codec) return std::wstring();
    const uint8_t* p = skip ? GameText::SkipVariantPrefix(codec) : codec;
    std::wstring s = GameText::Decode(p, 320);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// FUN_0035d330(cat,id) -> record; name codec at record+0x18. Game call on our (game) thread.
const uint8_t* ResolveDefCodec(uint32_t cat, uint32_t id) {
    auto fn = reinterpret_cast<Pfn_ResolveDef>(Hooks::ResolveRva(RVA_RESOLVE_DEF));
    if (!fn) return nullptr;
    __try {
        const uint8_t* rec = fn(cat, id);
        if (!rec) return nullptr;
        return *reinterpret_cast<const uint8_t* const*>(rec + 0x18);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// FUN_0035d330(cat,id) -> the shared scratch record ITSELF, for callers needing more than the
// name. Every call reuses the one record (DAT_022ca520), so copy out what you need before
// resolving anything else. Game call on our (game) thread.
const uint8_t* ResolveDefRecord(uint32_t cat, uint32_t id) {
    auto fn = reinterpret_cast<Pfn_ResolveDef>(Hooks::ResolveRva(RVA_RESOLVE_DEF));
    if (!fn) return nullptr;
    __try { return fn(cat, id); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Codec pointer stored at record+off. The pointer targets the message archive (stable), so the
// decode may happen after a later resolve overwrites the record.
const uint8_t* RecCodec(const uint8_t* rec, uint32_t off) {
    return reinterpret_cast<const uint8_t*>(PtrAt(const_cast<uint8_t*>(rec), off));
}

// FUN_002f9860(id) -> codec ptr (menu message books). Game call.
const uint8_t* ResolveMsgCodec(int id) {
    auto fn = reinterpret_cast<Pfn_ResolveMsg>(Hooks::ResolveRva(RVA_RESOLVE_MSG));
    if (!fn) return nullptr;
    __try { return fn(id); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// FUN_00323600(charId, panel, flags) -> status byte. Game call. -1 on fault.
int NodeStatus(int charId, int panel) {
    auto fn = reinterpret_cast<Pfn_Status>(Hooks::ResolveRva(RVA_NODE_STATUS));
    if (!fn) return -1;
    __try { return static_cast<int>(fn(charId, panel, 0)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

std::wstring CharName(int charId) {
    return DecodeCodec(ResolveDefCodec(CAT_CHARNAME, static_cast<uint32_t>(charId)), /*skip=*/true);
}
std::wstring JobName(int job) {
    if (job < 0 || job >= JOB_COUNT) return std::wstring();
    return DecodeCodec(ResolveMsgCodec(JOB_NAME_BASE + job), /*skip=*/true);
}
std::wstring JobDesc(int job) {
    if (job < 0 || job >= JOB_COUNT) return std::wstring();
    return DecodeCodec(ResolveMsgCodec(JOB_DESC_BASE + job), /*skip=*/true);
}

// The spoken status. It answers exactly ONE question -- "if I press Confirm here, does anything
// happen?" -- because that is the only part of the node's state the player cannot already work out.
//
//   learned              -> "learned"
//   reachable            -> "available"   (Confirm responds: the purchase prompt, or the game's own
//                                          not-enough-LP message)
//   prerequisites unmet  -> NOTHING       (Confirm is a no-op with a buzzer)
//
// "available" rather than "can learn", by user decision after play-confirming the fix: the word
// states the node's standing without promising an outcome the LP might not support, and the game's
// own "insufficient license points" message already speaks (play-confirmed) when it does not.
//
// AFFORDABILITY IS DELIBERATELY NOT SPOKEN, and CELL_AFFORDABLE is read only into the log. The line
// already carries the node's LP cost, `U` reads the character's total, and the game itself puts up a
// message when you confirm a node you cannot pay for -- so the mod announcing it would be telling the
// player something they have two other ways to know and pre-empting a decision that is theirs. It is
// also not the no-op case: a reachable node responds either way.
//
// nullptr = SAY NOTHING, and that is the answer when the prerequisites are not met. The game has no
// wording for that state -- the board draws it as a dim icon, and a cell carries no codec but its
// category word -- so inventing one would be a fabricated label. Silence also makes "available" a
// claim the mod only ever makes when FUN_0055cd40 would really respond to Confirm.
//
// A faulted read leaves `flags` at 0, which lands on the reachable test and appends nothing: a bad
// read degrades to silence rather than to a guess.
//
// STRIKES the old FUN_00323600-return switch (1=learned, 0/9=can learn, 2=not enough LP,
// 3/4/5/8=locked). Its 3/4/5/8 arm was already dead — FUN_0055bff0 zeroes those cells to id 0xFFFF
// before the reader ever sees one — and its 0/9 arm was the bug: that function never asks whether
// the node can be reached.
const wchar_t* StatusWordFromFlags(uint32_t flags) {
    if (flags & CELL_LEARNED)      return Phrase::Get(Phrase::Id::Learned);
    if (!(flags & CELL_REACHABLE)) return nullptr;                            // Confirm buzzes
    return Phrase::Get(Phrase::Id::Available);                                 // Confirm responds
}

// ---- announcements ------------------------------------------------------------------------------

// Character-select: name + job(s) + LP. Jobless -> job clause omitted (no game text for "no job";
// never fabricate a label). Silent on empty slot / unresolved name.
void AnnounceCharSel(void* ctrl) {
    void* ctx = PauseCtx();
    if (!ctx || !ctrl) return;
    // Highlighted member from the global set FIRST (FUN_00285f20), so entry (SHOW) is correct even
    // before FUN_00560ee0 copies it to ctrl+0xd0. This is char-select's member while its proc runs.
    int16_t member;
    if (!SafeReadS16(ctx, OFF_CTX_ACTINGCHAR, &member) || member < 0) return;
    int charId = CharIdOf(MemberBlock(ctx, member));
    if (charId < 0) return;
    std::wstring name = CharName(charId);
    if (name.empty()) return;

    std::wstring line = name;
    void* rec = SaveRecord(charId);
    uint8_t job1 = 0xFF, job2 = 0xFF;
    if (rec) { SafeReadU8(rec, OFF_SAVE_JOB1, &job1); SafeReadU8(rec, OFF_SAVE_JOB2, &job2); }
    std::wstring j1 = (job1 != 0xFF) ? JobName(job1) : std::wstring();
    std::wstring j2 = (job2 != 0xFF) ? JobName(job2) : std::wstring();
    if (!j1.empty() && !j2.empty()) line += L", " + j1 + L" / " + j2;
    else if (!j1.empty())           line += L", " + j1;
    // jobless: omit — the job-select screen (which vocalizes) makes it obvious.

    long lp = CurrentLP(charId);
    if (lp >= 0) line += L", " + std::to_wstring(lp) + Phrase::Get(Phrase::Id::LpSuffix);

    Log::WriteW("LICENSE", "charsel:", ctrl, line);
    Speech::Output(line, /*interrupt=*/true);
}

// Board entry (SHOW): "<job> license board, <LP> LP".
void AnnounceBoardEntry(void* board) {
    void* ctx = PauseCtx();
    if (!ctx || !board) return;
    uint8_t job = 0xFF;
    SafeReadU8(board, OFF_BOARD_JOB, &job);
    std::wstring jobName = JobName(job);

    long lp = -1;
    uint32_t member;
    if (SafeReadU32(board, OFF_BOARD_MEMBER, &member)) {
        int charId = CharIdOf(MemberBlock(ctx, static_cast<int>(member)));
        if (charId >= 0) lp = CurrentLP(charId);
    }

    std::wstring line = jobName.empty() ? std::wstring(Phrase::Get(Phrase::Id::LicenseBoard))
                                        : (jobName + Phrase::Get(Phrase::Id::LicenseBoardSuffix));
    if (lp >= 0) line += L", " + std::to_wstring(lp) + Phrase::Get(Phrase::Id::LpSuffix);
    Log::WriteW("LICENSE", "board:", board, line);
    Speech::Output(line, /*interrupt=*/true);
}

// The `o` detail for a node, mirroring what the info panel actually draws: the CATEGORY word (the
// ball icon's text equivalent) followed by every granted entry the panel lists, each with its
// description when it has one. Built exactly like the game's own list builder FUN_00559e30 -- the
// entry list exists only for kind 0..3, and each id resolves through a kind-specific category. A
// node with no entry list yields just the category, matching a panel that lists nothing.
std::wstring BuildNodeDetail(void* cell, uint16_t nodeId) {
    std::wstring detail = DecodeCodec(
        reinterpret_cast<const uint8_t*>(PtrAt(cell, OFF_CELL_CATEGORY)), /*skip=*/true);

    const uint8_t* rec = ResolveDefRecord(CAT_LICENSE, nodeId);
    if (!rec) return detail;
    // Copy BEFORE resolving anything else -- every FUN_0035d330 reuses the one shared record.
    void*    r = const_cast<uint8_t*>(rec);
    uint8_t  kind = 0xFF;
    uint16_t ids[GRANT_MAX] = {};
    SafeReadU8(r, OFF_REC_KIND, &kind);
    for (int i = 0; i < GRANT_MAX; ++i) SafeReadU16(r, OFF_REC_IDS + i * 2, &ids[i]);
    if (kind > 3) return detail;                    // 0xFF -> the game draws no entry list

    const uint32_t cat = (kind == 0) ? CAT_GEAR : (kind == 1) ? CAT_MAGICK : CAT_TECHNICK;
    for (int i = 0; i < GRANT_MAX; ++i) {
        if (ids[i] == 0xFFFF) continue;
        const uint32_t rid = (kind == 0) ? (static_cast<uint32_t>(ids[i]) << 16) : ids[i];
        const uint8_t* er = ResolveDefRecord(cat, rid);
        if (!er) continue;
        // Decode now -- the next resolve overwrites the record (codec targets stay valid).
        std::wstring ename = DecodeCodec(RecCodec(er, OFF_REC_NAME), /*skip=*/true);
        if (!ename.empty()) { if (!detail.empty()) detail += L", "; detail += ename; }
        // Description ONLY where the game draws one (see TECHNICK_ID_* above).
        if (kind == 1 && static_cast<uint16_t>(ids[i] - TECHNICK_ID_LO) < TECHNICK_ID_SPAN) {
            std::wstring edesc = DecodeCodec(RecCodec(er, OFF_REC_DESC), /*skip=*/true);
            if (!edesc.empty()) { if (!detail.empty()) detail += L", "; detail += edesc; }
        }
    }
    return detail;
}

// Board node move. A revealed node speaks "<name>, <status>, <cost> LP"; a blank tile speaks
// "Locked". The status comes from the cell's own flag word (see OFF_CELL_FLAGS) and is OMITTED
// entirely for a node whose prerequisites are not met, so the line is just "<name>, <cost> LP" —
// the game has no wording for that state and the mod does not invent one. The `o` detail comes
// from BuildNodeDetail.
void OnBoardNode(void* board, void* cell) {
    if (!cell) return;
    uint16_t id;
    if (!SafeReadU16(cell, OFF_CELL_ID, &id)) return;

    if (id == 0xFFFF) {
        // A license the player cannot reach yet: the builder zeroed this cell, and the game draws
        // neither icon nor info panel for it -- so say a license is there without leaking what it
        // is. Spoken rather than silent so scanning the board has no dead air, and NOT deduped:
        // each 0x8000 is one real cursor move. Mod-emitted word, requested by the user.
        Log::WriteW("LICENSE", "node:", board, std::wstring(Phrase::Get(Phrase::Id::LockedUpper)));
        Speech::Output(Phrase::Get(Phrase::Id::LockedUpper), /*interrupt=*/true);
        return;
    }

    std::wstring name = DecodeCodec(
        reinterpret_cast<const uint8_t*>(PtrAt(cell, OFF_CELL_NAME)), /*skip=*/false);  // pre-variant-selected
    if (name.empty()) return;

    uint16_t cost = 0;
    SafeReadU16(cell, OFF_CELL_COST, &cost);

    uint32_t flags = 0;
    SafeReadU32(cell, OFF_CELL_FLAGS, &flags);

    // THE INSTRUMENT, not the source of truth. FUN_00323600 is still called so the log carries both
    // answers side by side: the flags word (which is what the game's Confirm branch reads) and the
    // status byte the reader used to speak. One board sweep then shows, per node, where the two
    // disagree — and a node whose spoken word did not match the sound becomes a lookup rather than a
    // re-derivation. Log-only: nothing downstream reads `status`.
    int status = -1;
    void* ctx = PauseCtx();
    if (ctx) {
        uint32_t member;
        if (SafeReadU32(board, OFF_BOARD_MEMBER, &member)) {
            int charId = CharIdOf(MemberBlock(ctx, static_cast<int>(member)));
            if (charId >= 0) status = NodeStatus(charId, id);
        }
    }

    std::wstring line = name;
    if (const wchar_t* sw = StatusWordFromFlags(flags)) { line += L", "; line += sw; }
    line += L", " + std::to_wstring(cost) + Phrase::Get(Phrase::Id::LpSuffix);

    // The board CLEARS the game's description bar (FUN_00291d80(0,0) in FUN_00561390), so the
    // capture hooks give `o` nothing here -- build the panel's own content and register it.
    std::wstring detail = BuildNodeDetail(cell, id);
    if (!detail.empty()) TextCapture::ProvideHelpText(detail);

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "node[flags=0x%04X status=%d]: ", flags, status);
    Log::WriteW("LICENSE", hdr, board, line);
    if (!detail.empty()) Log::WriteW("LICENSE", "  detail: ", detail);
    Speech::Output(line, /*interrupt=*/true);
}

// Job-select ring move: speak the job name; hand the job description to `o`.
void OnJobRing(void* ring) {
    uint8_t job = 0xFF;
    if (!SafeReadU8(ring, OFF_RING_JOB, &job)) return;
    if (job >= JOB_COUNT) return;
    std::wstring name = JobName(job);
    if (name.empty()) return;
    std::wstring desc = JobDesc(job);
    if (!desc.empty()) TextCapture::ProvideHelpText(desc);
    Log::WriteW("LICENSE", "job:", ring, name);
    Speech::Output(name, /*interrupt=*/true);
}

// ---- hooks --------------------------------------------------------------------------------------

// FUN_00560910(ctrl, packet): the license character-select proc. OBSERVE ONLY — runs the original
// first (so FUN_00560ee0 has written ctrl+0xd0), then announces on SHOW (entry) and cursor move.
// This surface does not reliably route through FUN_00247510, so it is captured at its own proc.
uint64_t HookedCharSelWnd(void* ctrl, void* packet) {
    const uint64_t ret = s_origCharSelWnd ? s_origCharSelWnd(ctrl, packet) : 0;
    uint32_t cat = 0;
    if (!SafeReadU32(packet, PKT_CAT_OFF, &cat)) return ret;

    if (cat == CAT_CLOSE) { g_charSelShown = nullptr; return ret; }   // allow the next SHOW to re-announce
    if (cat == CAT_SHOW) {
        if (g_charSelShown == ctrl) return ret;                       // one-shot the repeating SHOW
        g_charSelShown = ctrl;
        TextCapture::NotifyFocusChanged();   // char-select has no `o` description — invalidate stale help
        AnnounceCharSel(ctrl);
        return ret;
    }
    if (cat == CAT_NOTIFY) {
        uint64_t sub = 0;
        if (SafeReadU64(packet, PKT_MSG_OFF, &sub) && sub == SUB_FOCUS) {
            TextCapture::NotifyFocusChanged();
            AnnounceCharSel(ctrl);
        }
    }
    return ret;
}

// FUN_0055cd40(board, packet): the license board grid proc. Used ONLY for the entry (SHOW) LP
// announce and teardown reset — node cursor moves arrive via the shared FUN_00247510 hook
// (MenuReader::HookedDispatch -> OnDispatchFocus), so cat 0xc/0x8000 is deliberately ignored here.
uint64_t HookedBoardWnd(void* board, void* packet) {
    const uint64_t ret = s_origBoardWnd ? s_origBoardWnd(board, packet) : 0;
    uint32_t cat = 0;
    if (!SafeReadU32(packet, PKT_CAT_OFF, &cat)) return ret;
    if (cat == CAT_CLOSE) { g_boardShown = nullptr; return ret; }
    if (cat == CAT_SHOW) {
        if (g_boardShown == board) return ret;                        // one-shot the repeating SHOW
        g_boardShown = board;
        AnnounceBoardEntry(board);   // no NotifyFocusChanged: let the initial node move own `o` help
    }
    return ret;
}

// `U` key (input thread): speak current LP while the license board is open. Memory-only reads
// (no game call) so it is safe off the game thread; silent unless the board is the active surface.
void OnLicensePointsKey() {
    void* ctx = PauseCtx();
    if (!ctx) return;
    void* board = PtrAt(ctx, OFF_CTX_BOARD);
    if (!MenuState::IsLicenseBoard(board)) return;   // ring/other surface or off-board -> silent
    uint32_t member;
    if (!SafeReadU32(board, OFF_BOARD_MEMBER, &member)) return;
    int charId = CharIdOf(MemberBlock(ctx, static_cast<int>(member)));
    if (charId < 0) return;
    long lp = CurrentLP(charId);
    if (lp < 0) return;
    std::wstring line = std::to_wstring(lp) + Phrase::Get(Phrase::Id::LicensePointsSuffix);
    Log::WriteW("LICENSE", "lp:", board, line);
    Speech::Output(line, /*interrupt=*/true);
}

} // namespace

namespace LicenseReader {

bool Init() {
    InputTracker::SetLicensePointsCallback(&OnLicensePointsKey);
    bool ok = Hooks::InstallTyped(RVA_CHARSEL_WND, &HookedCharSelWnd, &s_origCharSelWnd);
    ok     &= Hooks::InstallTyped(RVA_BOARD_WND,   &HookedBoardWnd,   &s_origBoardWnd);
    Log::Write("LICENSE", ok
        ? "LicenseReader initialized (char-select entry+moves; job ring; board nodes; U -> LP)"
        : "LicenseReader: a hook FAILED to install — see Hooks log.");
    return ok;
}

void Shutdown() {
    InputTracker::SetLicensePointsCallback(nullptr);
    Hooks::Uninstall(RVA_BOARD_WND);
    Hooks::Uninstall(RVA_CHARSEL_WND);
    g_charSelShown = nullptr;
    g_boardShown   = nullptr;
}

bool OnDispatchFocus(void* owner, uintptr_t val) {
    if (MenuState::IsLicenseBoard(owner)) {
        OnBoardNode(owner, reinterpret_cast<void*>(val));   // val = pointer to the focused cell
        return true;
    }
    if (MenuState::IsJobSelectRing(owner)) {
        OnJobRing(owner);
        return true;
    }
    return false;
}

} // namespace LicenseReader

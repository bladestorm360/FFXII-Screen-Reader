#include "ui/char_select_reader.h"
#include "battle/battle_state.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/stall_probe.h"
#include "speech/speech.h"
#include "speech/phrasebook.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

// ---- the shared chooser -----------------------------------------------------------------------
// Read design mirrors the game's own portrait draw FUN_00283e40: ctx = *DAT_0209ac30;
// controller = *(ctx+0xf8); portrait = *(controller+0xc0 + slot*8);
// block = *(ctx+0xac8 + *(int*)(portrait+0xc0)*8); fields are plain loads off `block`.
constexpr uint32_t RVA_STATUS_CURSOR   = 0x165A10; // FUN_00285a10(slot) — chooser cursor-set
constexpr uint32_t RVA_PARTY_TOGGLE    = 0x164C90; // FUN_00284c90(ctrl, row) — the Party membership flip
constexpr uint32_t RVA_PAUSE_CTX       = 0x1F7AC30; // DAT_0209ac30 (ptr) — pause-menu context
constexpr uint32_t OFF_CTX_CTRL        = 0xF8;     // ctx+0xf8 = active chooser controller
constexpr uint32_t OFF_CTX_BLOCKS      = 0xAC8;    // ctx+0xac8 + blockIdx*8 = per-character HUD block ptr
constexpr uint32_t OFF_CTX_PANE        = 0xD8;     // ctx+0xd8 -> the field pane, for the active command id
constexpr uint32_t OFF_PANE_CMD        = 0x268;    // **(int**)(*(ctx+0xd8) + 0x268) = active command id
constexpr uint32_t OFF_CTX_INPARTY     = 0xB10;    // ctx+0xb10 + charId = in-party byte (FUN_00284c90 writes it)
constexpr uint32_t OFF_CTRL_PORTRAITS  = 0xC0;     // controller+0xc0 + slot*8 = portrait child ptr
constexpr uint32_t OFF_PORTRAIT_BLKIDX = 0xC0;     // portrait+0xc0 = index into ctx+0xac8 (int)
constexpr uint32_t OFF_ROW_FLAGS       = 0xFC;     // row+0xfc: bit 3 = in party, bit 4 = leader
constexpr uint32_t ROW_FLAG_INPARTY    = 0x08;
constexpr uint32_t OFF_BLK_CHARID      = 0x60;     // block+0x60 = char id (i16; < 0 = empty slot)
constexpr uint32_t OFF_BLK_FLAGS       = 0x00;     // block+0x00 bit 1 = GUEST (cannot be toggled)
constexpr uint32_t BLK_FLAG_GUEST      = 0x02;
constexpr uint32_t OFF_BLK_CURHP       = 0x20;     // block+0x20 = current HP (i32)
constexpr uint32_t OFF_BLK_MAXHP       = 0x24;     // block+0x24 = max HP (i32)
constexpr uint32_t OFF_BLK_CURMP       = 0x2C;     // block+0x2c = current MP (i32)
constexpr uint32_t OFF_BLK_MAXMP       = 0x30;     // block+0x30 = max MP (i32)
constexpr uint32_t OFF_BLK_LEVEL       = 0xBA;     // block+0xba = level (u8)
constexpr uint32_t CAT_CHARNAME        = 2;        // FUN_0035d330 category for character names

// The field pane's active command. `0x4b3` is Party -- the membership screen.
constexpr int CMD_PARTY = 0x4b3;

typedef void (*Pfn_StatusCursor)(int);
typedef void (*Pfn_PartyToggle)(void*, void*);
Pfn_StatusCursor s_origStatusCursor = nullptr;
Pfn_PartyToggle  s_origPartyToggle  = nullptr;

struct SlotInfo {
    int  charId = -1;
    int  curHP = 0, maxHP = 0, curMP = 0, maxMP = 0, level = 0;
    bool inParty = false;
    bool guest   = false;
};

// Which field-menu command is active. Empty-handed answer is -1, which makes every caller fall through
// to the stat readout -- the pre-existing behaviour, so an unreadable chain degrades to what shipped
// rather than to silence.
int ActiveFieldCmd() {
    __try {
        void* ctx = *reinterpret_cast<void* const*>(Hooks::ResolveRva(RVA_PAUSE_CTX));
        if (!ctx) return -1;
        void* pane = *reinterpret_cast<void* const*>(reinterpret_cast<char*>(ctx) + OFF_CTX_PANE);
        if (!pane) return -1;
        int* p = *reinterpret_cast<int**>(reinterpret_cast<char*>(pane) + OFF_PANE_CMD);
        if (!p) return -1;
        return *p;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// POD-only under __try (decoding a name builds a std::wstring, which cannot unwind through SEH).
bool ReadSlot(int slot, SlotInfo* out) {
    if (slot < 0) return false;
    __try {
        void* ctx = *reinterpret_cast<void* const*>(Hooks::ResolveRva(RVA_PAUSE_CTX));
        if (!ctx) return false;
        char* c = reinterpret_cast<char*>(ctx);
        void* ctrl = *reinterpret_cast<void* const*>(c + OFF_CTX_CTRL);
        if (!ctrl) return false;
        void* portrait = *reinterpret_cast<void* const*>(
            reinterpret_cast<char*>(ctrl) + OFF_CTRL_PORTRAITS + static_cast<size_t>(slot) * 8);
        if (!portrait) return false;
        const int blkIdx = *reinterpret_cast<int*>(reinterpret_cast<char*>(portrait) + OFF_PORTRAIT_BLKIDX);
        if (blkIdx < 0) return false;
        void* block = *reinterpret_cast<void* const*>(
            c + OFF_CTX_BLOCKS + static_cast<size_t>(blkIdx) * 8);
        if (!block) return false;
        char* b = reinterpret_cast<char*>(block);
        const int charId = *reinterpret_cast<int16_t*>(b + OFF_BLK_CHARID);
        if (charId < 0) return false;                              // empty portrait slot
        out->charId  = charId;
        out->curHP   = *reinterpret_cast<int32_t*>(b + OFF_BLK_CURHP);
        out->maxHP   = *reinterpret_cast<int32_t*>(b + OFF_BLK_MAXHP);
        out->curMP   = *reinterpret_cast<int32_t*>(b + OFF_BLK_CURMP);
        out->maxMP   = *reinterpret_cast<int32_t*>(b + OFF_BLK_MAXMP);
        out->level   = *reinterpret_cast<uint8_t*>(b + OFF_BLK_LEVEL);
        out->guest   = (*reinterpret_cast<uint32_t*>(b + OFF_BLK_FLAGS) & BLK_FLAG_GUEST) != 0;
        // MEMBERSHIP FROM THE BYTE THE GAME'S OWN TOGGLE WRITES. FUN_00284c90 flips bit 3 of row+0xfc
        // and then mirrors it to `menuCtx + 0xb10 + charId`, so reading that byte is reading the game's
        // answer rather than a second opinion. debug.md's S86 entry says membership is presence in
        // roster list 3, and that remains true of the COMMITTED state -- but while this screen is open
        // the edit is staged here, so this is the byte that matches what the player just did.
        out->inParty = *reinterpret_cast<uint8_t*>(c + OFF_CTX_INPARTY + static_cast<size_t>(charId)) != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::wstring MembershipWord(bool inParty) {
    return Phrase::Get(inParty ? Phrase::Id::InParty : Phrase::Id::NotInParty);
}

// ONE emit function for this surface, per the one-choke-point rule. Both detectors (highlight and
// toggle) funnel here so they cannot race with two different wordings or two interrupt policies.
void Speak(const std::wstring& line, void* owner, const char* tag) {
    if (line.empty()) return;
    Log::WriteW("INGAME", tag, owner, line);
    Speech::Output(line, /*interrupt=*/true);
}

// FUN_00285a10(slot): the chooser's cursor-set. Fires on each highlight.
//
// NO dedup. All call sites in the decompile are open / cursor-set / close handlers -- none is per-frame
// -- so every fire is a real highlight change, and re-opening the chooser on the same slot re-announces,
// which is the point. If one highlight ever produces TWO lines, two handlers are firing for one input:
// narrow the hook to the one that owns the event, do NOT add a filter.
//
// CORRECTION (Session 93): the block comment this replaced claimed FUN_00285a10 "does NOT fire for the
// highlight on menu ENTRY", derived from a trace of FUN_00285290 case 1 in Session 31. The live log
// refutes it -- the chooser announced a character one second after the row "Party" was spoken, with no
// d-pad movement in between. The Session-31 observation was made on the prologue tutorial party, which
// is ONE character, so there was nothing to move to and nothing to fire; that was a property of the
// sample, not of the handler.
void HookedStatusCursor(int slot) {
    if (s_origStatusCursor) s_origStatusCursor(slot);
    STALL_SCOPE("CharSelect::HookedStatusCursor");           // let the game set +0x114/+0x117 first
    if (slot < 0) return;

    SlotInfo v;
    if (!ReadSlot(slot, &v)) return;

    const std::wstring name = BattleState::DefName(CAT_CHARNAME, static_cast<uint32_t>(v.charId));
    if (name.empty()) return;                                 // no game-supplied name -> stay silent

    const int cmd = ActiveFieldCmd();
    std::wstring line = name;
    if (cmd == CMD_PARTY) {
        // MEMBERSHIP, NOT STATISTICS. On this screen the only thing the highlight means is whether this
        // character is in the active party -- the stats belong to Status (0x4b4), which is a different
        // command on the same controller.
        line += L": ";
        line += MembershipWord(v.inParty);
    } else {
        line += std::wstring(L", ") + Phrase::Get(Phrase::Id::LevelPrefix) + std::to_wstring(v.level);
        line += std::wstring(L", ") + Phrase::Get(Phrase::Id::HPPrefix)
              + std::to_wstring(v.curHP) + L"/" + std::to_wstring(v.maxHP);
        line += std::wstring(L", ") + Phrase::Get(Phrase::Id::MPPrefix)
              + std::to_wstring(v.curMP) + L"/" + std::to_wstring(v.maxMP);
    }
    char tag[48];
    snprintf(tag, sizeof(tag), "chooser cmd=0x%X:", cmd);
    Speak(line, nullptr, tag);
}

// FUN_00284c90(ctrl, row): the Party membership toggle. THE WRITER OF THE STATE IS THE EVENT.
//
// Hooked because Confirm/Left/Right flip membership WITHOUT MOVING THE CURSOR, so the cursor-set hook
// above never fires for it and the player would press a button and hear nothing.
//
// Read the flag AFTER the original runs: the function is the thing that changes it, and reading before
// would announce the state the player just left. Its own guard clauses (invalid charId, a GUEST, the
// party-size rules) can REFUSE a press and return without flipping anything -- in which case the byte is
// unchanged and this correctly re-speaks the current state rather than claiming a change that did not
// happen. The game plays its own reject SE on that path, so the player gets both signals.
//
// NO dedup here either: a refused press re-speaking the unchanged state IS the feedback, and suppressing
// it would make a rejected toggle silent.
// POD-only halves, because __try cannot live in a function that needs object unwinding and the emit
// path builds a std::wstring.
uint32_t ReadRowFlags(void* row, bool* ok) {
    __try {
        const uint32_t v = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(row) + OFF_ROW_FLAGS);
        *ok = true;
        return v;
    } __except (EXCEPTION_EXECUTE_HANDLER) { *ok = false; return 0; }
}

struct ToggleState { int charId; bool rowSaysIn; bool ctxSaysIn; uint32_t flags; };

bool ReadToggleState(void* row, ToggleState* out) {
    __try {
        out->flags     = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(row) + OFF_ROW_FLAGS);
        out->rowSaysIn = (out->flags & ROW_FLAG_INPARTY) != 0;
        void* ctx = *reinterpret_cast<void* const*>(Hooks::ResolveRva(RVA_PAUSE_CTX));
        if (!ctx) return false;
        char* c = reinterpret_cast<char*>(ctx);
        const int blkIdx = *reinterpret_cast<int*>(reinterpret_cast<char*>(row) + OFF_PORTRAIT_BLKIDX);
        if (blkIdx < 0) return false;
        void* block = *reinterpret_cast<void* const*>(
            c + OFF_CTX_BLOCKS + static_cast<size_t>(blkIdx) * 8);
        if (!block) return false;
        out->charId = *reinterpret_cast<int16_t*>(reinterpret_cast<char*>(block) + OFF_BLK_CHARID);
        if (out->charId < 0) return false;
        out->ctxSaysIn = *reinterpret_cast<uint8_t*>(
            c + OFF_CTX_INPARTY + static_cast<size_t>(out->charId)) != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void HookedPartyToggle(void* ctrl, void* row) {
    bool haveBefore = false;
    const uint32_t before = ReadRowFlags(row, &haveBefore);

    if (s_origPartyToggle) s_origPartyToggle(ctrl, row);
    STALL_SCOPE("CharSelect::HookedPartyToggle");

    ToggleState st{ -1, false, false, 0 };
    const bool ok = ReadToggleState(row, &st);

    // BOTH WITNESSES ON ONE LINE. `row+0xfc` bit 3 and `menuCtx+0xb10+charId` are two recordings of one
    // fact, and FUN_00284c90 writes the second FROM the first -- so they must always agree. Printing them
    // together means a divergence reads itself out of the log instead of becoming a session of guesswork
    // about which one the reader should have used. The flags before/after pair also makes a held-Confirm
    // REPEAT visible as a burst of identical lines, which is the one thing about this hook that could not
    // be settled offline.
    char m[208];
    snprintf(m, sizeof(m),
             "party toggle: charId=%d flags 0x%08X -> 0x%08X row=%d ctx=%d%s%s",
             st.charId, haveBefore ? before : 0u, st.flags,
             st.rowSaysIn ? 1 : 0, st.ctxSaysIn ? 1 : 0,
             (haveBefore && before == st.flags) ? "  (REFUSED -- flags unchanged)" : "",
             (ok && st.rowSaysIn != st.ctxSaysIn) ? "  <== WITNESSES DISAGREE" : "");
    Log::Write("INGAME", m);

    if (!ok) return;
    // The toggle speaks the STATE ALONE (the tester's wording this session): the cursor has not moved, so
    // the player already knows who they are on, and repeating the name would delay the one bit they
    // pressed the button for.
    Speak(MembershipWord(st.ctxSaysIn), nullptr, "party toggle:");
}

} // namespace

namespace CharSelectReader {

bool Init() {
    bool ok = Hooks::InstallTyped(RVA_STATUS_CURSOR, &HookedStatusCursor, &s_origStatusCursor);
    ok    &= Hooks::InstallTyped(RVA_PARTY_TOGGLE,  &HookedPartyToggle,  &s_origPartyToggle);
    Log::Write("INGAME", ok
        ? "CharSelectReader: chooser cursor + Party membership toggle hooks installed"
        : "CharSelectReader: a chooser hook FAILED to install");
    return ok;
}

void Shutdown() {}

} // namespace CharSelectReader

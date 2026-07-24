#include "ui/status_reader.h"
#include "ui/ability_entry.h"
#include "ui/virtual_buffer.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "speech/speech.h"
#include "core/logger.h"
#include "input/input_tracker.h"

#include <Windows.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace {

using MemRead::PtrAt;
using MemRead::SafeReadPtr;
using MemRead::SafeReadS16;
using MemRead::SafeReadU8;
using MemRead::SafeReadU32;
using MemRead::SafeReadInt;

// ---- hooks (abs = RVA + 0x120000) ---------------------------------------------------------------
constexpr uint32_t RVA_STATUS_CTRL = 0x1A2320;   // FUN_002c2320(ctrl, packet) — Status/Equipment
// FUN_002c1a80(block, edge, mask, held, rpt) — the overlay PAGE STATE MACHINE, and the only writer
// of menuCtx+0xDE7 anywhere: :24 sets 1 (Magicks opened), :42 sets 2 (Technicks page), :87 sets 0
// (closed BACK to the Attributes page). It returns 1 when it opened/switched/consumed, 2 when it
// closed back to page 1, and 0/-1 when nothing happened — so "the player is back on the Attributes
// page" is exactly `ret == 2`, and the idle per-frame path costs one integer compare. Shared with
// the license board's `F` overlay, which is why the handler is gated on g_active.
constexpr uint32_t RVA_OVERLAY_INPUT = 0x1A1A80;
constexpr uint32_t RVA_RESOLVE_MSG   = 0x1D9860; // FUN_002f9860(id)     -> codec ptr
constexpr uint32_t RVA_RESOLVE_DEF   = 0x23D330; // FUN_0035d330(cat,id) -> record, codec @ +0x18

// ---- globals + message categories ---------------------------------------------------------------
constexpr uint32_t RVA_PAUSE_CTX = 0x1F7AC30;    // DAT_0209ac30 (ptr) -> pause/menu context
constexpr uint32_t PKT_CAT_OFF = 0x00;
constexpr uint32_t CAT_CREATE  = 0x01;           // the ONE one-shot at which the panels are live
constexpr uint32_t CAT_DESTROY = 0x12;           // teardown — confirmed live Session 71

// ---- Status/Equipment container -----------------------------------------------------------------
constexpr uint32_t K_CMDID    = 0x160;           // pause command id that opened this screen
constexpr int      CMD_STATUS = 0x4b4;           // 0x4b6 = Equipment, which this reader ignores

// ---- menuCtx offsets ----------------------------------------------------------------------------
constexpr uint32_t C_MEMBER   = 0xDE0;           // i16 selected member
constexpr uint32_t C_BLOCKS   = 0xAC8;           // + member*8 -> member block
constexpr uint32_t C_BLOCKS_0 = 0x408;           // inline block array, stride 0xC0 (type validation)
constexpr uint32_t C_AILMENT  = 0x110;           // -> status-ailment grid (FUN_002c59d0)
constexpr uint32_t C_ATTRS    = 0x138;           // -> 9-attribute panel (FUN_003fe5d0)
constexpr uint32_t C_STATUS   = 0x140;           // -> the Status/Equipment container (liveness)
constexpr uint32_t C_OVERLAY  = 0xDE7;           // page: 0 Attributes, 1|3 Magicks, 2 Technicks etc.

// ---- member block (0xC0 bytes) — two witnesses: FUN_00283e40 draw, FUN_00329220 fill ------------
constexpr uint32_t B_CURHP = 0x20, B_MAXHP = 0x24, B_CURMP = 0x2C, B_MAXMP = 0x30;
constexpr uint32_t B_EXP   = 0x90, B_NEXT  = 0x94, B_LP    = 0xB0, B_LEVEL = 0xBA;
constexpr uint32_t B_CHARID = 0x60;              // i16 char id (< 0 = empty slot)
constexpr uint32_t DEF_CAT_CHARNAME = 2;         // FUN_0035d330 category for character names

// ---- attribute panel: value at panel+0xC8 + row*4, label = FUN_002f9860(0x4A90 + row) -----------
constexpr uint32_t A_VALUE = 0xC8;
constexpr int      A_ROWS  = 9;
constexpr int      A_LABEL_BASE = 0x4A90;

// ---- ailment grid: id = *(i8*)(grid+0xC8+i*4); name = FUN_0035d330(0x1A, id) --------------------
constexpr uint32_t AIL_ID = 0xC8;
constexpr int      AIL_SLOTS = 32;
constexpr uint32_t DEF_CAT_STATUS = 0x1A;

typedef uint64_t (*Pfn_Wnd)(void*, void*);
typedef int (*Pfn_OverlayInput)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
typedef const uint8_t* (*Pfn_ResolveMsg)(int);
typedef const uint8_t* (*Pfn_ResolveDef)(uint32_t, uint32_t);
Pfn_Wnd          s_origCtrl    = nullptr;
Pfn_OverlayInput s_origOverlay = nullptr;

// ---- state (game thread snapshots; input thread navigates) --------------------------------------
std::mutex        g_mutex;
bool              g_active = false;
void*             g_ctrl   = nullptr;         // the container we activated on (liveness check)
VB::VirtualBuffer g_buffer;                   // the Attributes page
bool              g_haveBuffer = false;
std::wstring      g_attrLabel[A_ROWS];        // cached: the labels never change within a session
bool              g_labelsCached = false;

// ---- game calls: isolated so no C++ object is live inside a __try scope (SEH rule) --------------
const uint8_t* ResolveMsgCodec(int id) {
    auto fn = reinterpret_cast<Pfn_ResolveMsg>(Hooks::ResolveRva(RVA_RESOLVE_MSG));
    if (!fn) return nullptr;
    __try { return fn(id); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

const uint8_t* ResolveDefCodec(uint32_t cat, uint32_t id) {
    auto fn = reinterpret_cast<Pfn_ResolveDef>(Hooks::ResolveRva(RVA_RESOLVE_DEF));
    if (!fn) return nullptr;
    __try {
        const uint8_t* rec = fn(cat, id);
        return rec ? *reinterpret_cast<const uint8_t* const*>(rec + 0x18) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ---- context reads -------------------------------------------------------------------------------
void* PauseCtx() {
    void* ctx = nullptr;
    return SafeReadPtr(Hooks::ResolveRva(RVA_PAUSE_CTX), &ctx) ? ctx : nullptr;
}

// 0 = the Attributes page (ours). Anything else means a summary page is up, and that page has its own
// in-game cursor which ability_summary_reader speaks — so the buffer must not touch the arrow keys.
int CurrentMode(void* ctx) {
    uint8_t m = 0;
    return (ctx && SafeReadU8(ctx, C_OVERLAY, &m)) ? static_cast<int>(m) : 0;
}

// The selected member's block. Type-validated the way FUN_00282ea0 does it — the pointer must be one
// of the nine inline blocks at menuCtx+0x408 + n*0xC0, not merely non-null.
void* MemberBlock(void* ctx) {
    int16_t member = -1;
    if (!ctx || !SafeReadS16(ctx, C_MEMBER, &member) || member < 0 || member > 0x27) return nullptr;
    void* blk = PtrAt(ctx, C_BLOCKS + static_cast<uint32_t>(member) * 8);
    if (!blk) return nullptr;
    for (int n = 0; n < 9; ++n) {
        if (blk == reinterpret_cast<char*>(ctx) + C_BLOCKS_0 + static_cast<size_t>(n) * 0xC0) return blk;
    }
    return nullptr;
}

// ---- buffer assembly (GAME thread) ---------------------------------------------------------------
// Entries are pre-rendered snapshots captured by value. virtual_buffer.h advertises a live-read mode
// (std::function evaluated at navigation time) — that is FORBIDDEN here: navigation runs on the input
// thread, which must never dereference game memory. Same split the combat log uses.

void CacheLabels() {
    if (g_labelsCached) return;
    g_labelsCached = true;
    for (int i = 0; i < A_ROWS; ++i)
        g_attrLabel[i] = AbilityEntry::DecodeCodec(ResolveMsgCodec(A_LABEL_BASE + i), /*skip=*/true);
}

// Three groups: Character / Attributes / Status effects.
void BuildBuffer(void* ctx) {
    std::vector<VB::VirtualBuffer::Entry> entries;
    std::vector<int> groupStarts;
    auto add = [&](const std::wstring& line) {
        if (line.empty()) return;
        std::wstring s = line;
        entries.push_back([s] { return s; });
    };

    void* blk   = MemberBlock(ctx);
    void* panel = ctx ? PtrAt(ctx, C_ATTRS) : nullptr;
    if (!blk) { g_haveBuffer = false; return; }
    CacheLabels();

    // ---- Character. LEVEL / HP / MP / LP / EXP / NEXT are drawn as ART on this screen, so there is
    // no message id to read them from; they are the gauge-label carve-out in CLAUDE.md. Every VALUE
    // is live game data.
    groupStarts.push_back(static_cast<int>(entries.size()));
    int16_t charId = -1;
    if (SafeReadS16(blk, B_CHARID, &charId) && charId >= 0) {
        // A wrong id cannot produce a wrong NAME here: DecodeCodec's printable gate turns garbage
        // into an empty string, and add() drops it — the row goes silent rather than misnaming.
        add(AbilityEntry::DecodeCodec(ResolveDefCodec(DEF_CAT_CHARNAME,
                                                     static_cast<uint32_t>(charId)), /*skip=*/true));
    }
    uint8_t level = 0;
    if (SafeReadU8(blk, B_LEVEL, &level)) add(L"Level " + std::to_wstring(level));
    int curHP = 0, maxHP = 0, curMP = 0, maxMP = 0;
    if (SafeReadInt(reinterpret_cast<char*>(blk) + B_CURHP, &curHP) &&
        SafeReadInt(reinterpret_cast<char*>(blk) + B_MAXHP, &maxHP))
        add(L"HP " + std::to_wstring(curHP) + L" of " + std::to_wstring(maxHP));
    if (SafeReadInt(reinterpret_cast<char*>(blk) + B_CURMP, &curMP) &&
        SafeReadInt(reinterpret_cast<char*>(blk) + B_MAXMP, &maxMP))
        add(L"MP " + std::to_wstring(curMP) + L" of " + std::to_wstring(maxMP));
    uint32_t v = 0;
    if (SafeReadU32(blk, B_LP,   &v)) add(L"LP " + std::to_wstring(v));
    if (SafeReadU32(blk, B_EXP,  &v)) add(L"EXP " + std::to_wstring(v));
    if (SafeReadU32(blk, B_NEXT, &v)) add(L"Next " + std::to_wstring(v));

    // ---- Attributes: nine rows, every label the game's own.
    if (panel) {
        groupStarts.push_back(static_cast<int>(entries.size()));
        for (int r = 0; r < A_ROWS; ++r) {
            int val = 0;
            if (!SafeReadInt(reinterpret_cast<char*>(panel) + A_VALUE + r * 4, &val)) continue;
            if (g_attrLabel[r].empty()) continue;      // no label -> silence, never a fabricated one
            add(g_attrLabel[r] + L" " + std::to_wstring(val));
        }
    }

    // ---- Status effects. Only real ailments; when there are none the game draws "(No status
    // effects.)" as LAYOUT ART (FUN_002c5900 merely hides the rows), so there is no string to read
    // back and the group is simply absent — silence rather than a fabricated line.
    void* grid = ctx ? PtrAt(ctx, C_AILMENT) : nullptr;
    if (grid) {
        const int before = static_cast<int>(entries.size());
        for (int i = 0; i < AIL_SLOTS; ++i) {
            uint8_t raw = 0;
            if (!SafeReadU8(grid, AIL_ID + static_cast<uint32_t>(i) * 4, &raw)) continue;
            const int id = static_cast<int8_t>(raw);
            if (id < 0) continue;
            std::wstring nm = AbilityEntry::DecodeCodec(
                ResolveDefCodec(DEF_CAT_STATUS, static_cast<uint32_t>(id)), /*skip=*/true);
            if (!nm.empty()) {
                if (static_cast<int>(entries.size()) == before)
                    groupStarts.push_back(before);     // open the group only once we have a real one
                add(nm);
            }
        }
    }

    g_buffer = VB::VirtualBuffer(std::move(entries), std::move(groupStarts));
    g_haveBuffer = !g_buffer.IsEmpty();
}

// Rebuild and announce the top of the Attributes page. Used both on entry and on the return from a
// summary page — rebuilding rather than restoring, so a character switch is picked up.
void RefreshAndAnnounce(void* ctrl) {
    void* ctx = PauseCtx();
    std::wstring first;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        BuildBuffer(ctx);
        g_active = true;
        if (ctrl) g_ctrl = ctrl;
        if (g_haveBuffer) first = g_buffer.JumpTop();
    }
    if (!first.empty()) {
        Log::WriteW("STATUS", "page:", g_ctrl, first);
        Speech::Output(first, /*interrupt=*/true);
    }
}

void Deactivate() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_active = false;
    g_ctrl = nullptr;
    g_haveBuffer = false;
    g_buffer = VB::VirtualBuffer();
}

// ---- hooks ---------------------------------------------------------------------------------------
uint64_t HookedCtrl(void* ctrl, void* packet) {
    const uint64_t ret = s_origCtrl ? s_origCtrl(ctrl, packet) : 0;
    STALL_SCOPE("StatusReader::HookedCtrl");
    uint32_t cat = 0;
    int cmd = 0;
    if (!MemRead::SafeReadU32(packet, PKT_CAT_OFF, &cat)) return ret;
    if (cat != CAT_CREATE && cat != CAT_DESTROY) return ret;      // everything else is per-frame
    if (!SafeReadInt(reinterpret_cast<char*>(ctrl) + K_CMDID, &cmd) || cmd != CMD_STATUS) return ret;
    if (cat == CAT_CREATE) RefreshAndAnnounce(ctrl);
    else                   Deactivate();
    return ret;
}

// ret 2 = closed back to the Attributes page; 1 = opened/switched (a summary page now owns the
// cursor and ability_summary_reader speaks it); 0 or -1 = idle.
int HookedOverlayInput(void* blk, uint32_t edge, uint32_t mask, uint32_t held, uint32_t rpt) {
    const int ret = s_origOverlay ? s_origOverlay(blk, edge, mask, held, rpt) : 0;
    if (ret != 2) return ret;
    STALL_SCOPE("StatusReader::OverlayInput");
    bool active;
    { std::lock_guard<std::mutex> lk(g_mutex); active = g_active; }
    if (active) RefreshAndAnnounce(nullptr);   // gated: this function also serves the license board
    return ret;
}

} // namespace

namespace StatusReader {

bool Init() {
    InputTracker::SetMenuNavCallback(&OnMenuNavKey);
    bool ok = Hooks::InstallTyped(RVA_STATUS_CTRL,   &HookedCtrl,         &s_origCtrl);
    ok     &= Hooks::InstallTyped(RVA_OVERLAY_INPUT, &HookedOverlayInput, &s_origOverlay);
    Log::Write("STATUS", ok
        ? "StatusReader initialized (Attributes page virtual buffer; Up-Down = entry, "
          "Left-Right = group, Home-End = ends; returns to it via FUN_002c1a80)"
        : "StatusReader: a hook FAILED to install — see Hooks log.");
    return ok;
}

void Shutdown() {
    InputTracker::SetMenuNavCallback(nullptr);
    Hooks::Uninstall(RVA_OVERLAY_INPUT);
    Hooks::Uninstall(RVA_STATUS_CTRL);
    Deactivate();
}

// INPUT thread. No game calls and no allocation beyond the returned string — two guarded memory reads
// (the container liveness check and the page mode byte), then a walk of strings that were rendered on
// the game thread at snapshot time.
bool OnMenuNavKey(int vk) {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_active) return false;
    }
    void* ctx = PauseCtx();
    // A summary page is up: it has its own in-game cursor and the game owns the arrow keys there, so
    // decline — ability_summary_reader announces those rows, exactly as on the license board.
    if (CurrentMode(ctx) != 0) return false;
    // Liveness. CAT_DESTROY is confirmed, but if it were ever missed the buffer would keep owning
    // Home/End for the rest of the session — so re-check that our container is still the one parked
    // at menuCtx+0x140 before consuming anything.
    void* live = ctx ? PtrAt(ctx, C_STATUS) : nullptr;

    std::wstring line;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_active || !live || live != g_ctrl || !g_haveBuffer) return false;
        switch (vk) {
            case VK_UP:    line = g_buffer.Previous();      break;
            case VK_DOWN:  line = g_buffer.Next();          break;
            case VK_LEFT:  line = g_buffer.PreviousGroup(); break;
            case VK_RIGHT: line = g_buffer.NextGroup();     break;
            case VK_HOME:  line = g_buffer.JumpTop();       break;
            case VK_END:   line = g_buffer.JumpBottom();    break;
            default: return false;
        }
    }
    if (!line.empty()) {
        Log::WriteW("STATUS", "nav:", nullptr, line);
        Speech::Output(line, /*interrupt=*/true);
    }
    return true;
}

} // namespace StatusReader

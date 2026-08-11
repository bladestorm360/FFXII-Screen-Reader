#include "ui/equip_compare.h"
#include "ui/equip_target_reader.h"
#include "ui/mod_menu.h"                // AutoDetailOn -- the per-highlight preview is VOLUNTEERED
#include "ui/shop_reader.h"
#include "ui/text_capture.h"

#include "battle/battle_state.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "speech/phrasebook.h"
#include "speech/speech.h"

#include <Windows.h>
#include <cstdio>
#include <mutex>

namespace {

using MemRead::Obj0;
using MemRead::PtrAt;
using MemRead::SafeReadU8;
using MemRead::SafeReadU16;
using MemRead::SafeReadU32;

// ---- the panel, all probe-confirmed 2026-08-03 (probe_equip_compare_output.log) ----------------
constexpr uint32_t RVA_REFRESH   = 0x1AC4F0;  // FUN_002cc4f0(itemId) -- brackets one refresh pass
constexpr uint32_t RVA_DELTA     = 0x1AC780;  // FUN_002cc780(pair, delta, target, style, outBuf, outSize)
                                              // SIX arguments -- 5 and 6 travel on the STACK. See the
                                              // typedef below; getting this count wrong CORRUPTS MEMORY.
constexpr uint32_t RVA_MENUCTX   = 0x1F7AC30; // DAT_0209ac30 (ptr) -- pause/menu context
constexpr uint32_t RVA_PANELCLS  = 0x1ABF80;  // FUN_002cbf80 -- the panel's own class
constexpr uint32_t RVA_SHOPLIST  = 0x2B89798; // DAT_02ca9798 -- the live shop list container

// ---- the FIELD Equipment screen's own preview, a SECOND and different mechanism ----------------
// The pause menu's Equipment screen shows one character's nine attributes as `current > preview`
// ("Attack Power 108 > 14"), not a per-character signed delta. Different panel, different shape:
//     panel = menuCtx+0x138 (class FUN_003fe5d0), current at panel+0xC8+row*4,
//     preview at panel+0xEC+row*4, label FUN_002f9860(0x4A90+row), rows 0..8
// FUN_003fe490:17-51 draws the preview only when the two differ -- which is exactly the test for
// "is a preview showing", so no extra state is needed. On the Status screen they are always equal,
// which is why that screen shows no arrows and this reports nothing there.
constexpr uint32_t CTX_ATTRPANEL = 0x138;
constexpr uint32_t RVA_ATTRCLS   = 0x2DE5D0;  // FUN_003fe5d0 -- the attribute panel's class
constexpr uint32_t AP_CURRENT    = 0xC8;
constexpr uint32_t AP_PREVIEW    = 0xEC;
constexpr int      AP_ROWS       = 9;
constexpr int      TXT_ATTR_BASE = 0x4A90;    // + row -> Attack Power .. Speed (confirmed S71)
constexpr uint32_t CTX_SELCHAR   = 0xDE0;     // menuCtx+0xDE0 -- the character being equipped
constexpr uint32_t CTX_BLOCKS2   = 0xAC8;

constexpr uint32_t CTX_PANEL     = 0x2E0;
constexpr uint32_t CTX_BLOCKS    = 0xAC8;     // menuCtx + 0xAC8 + memberIdx*8 -> member block
constexpr uint32_t P_COLA        = 0xD0;      // panel + 0xD0  + i*8
constexpr uint32_t P_COLB        = 0x118;     // panel + 0x118 + i*8
constexpr uint32_t P_HEADER      = 0xC8;
constexpr uint32_t COL_MEMBER    = 0xD4;      // colA + 0xD4 = member index (int)
constexpr uint32_t COL_STAT0     = 0xD8;      // FUN_002cc780 target, stat slot 0
constexpr uint32_t COL_STAT1     = 0xE8;      //                     stat slot 1
constexpr uint32_t CB_EQUIPPED   = 0xE0;      // colB + 0xE0 = already wearing this exact item
constexpr uint32_t CB_CANEQUIP   = 0xE4;      // colB + 0xE4 = 0 when the character CANNOT equip
constexpr uint32_t BLK_CHARID    = 0x60;      // block + 0x60 = char id (i16; < 0 = absent)
constexpr uint32_t W_TEXT        = 0x18;      // label widget + 0x18 = codec ptr
constexpr uint32_t W_FLAGS       = 0x08;      // label widget + 0x08, bit 0 = visible

constexpr int kCols  = 9;                     // FUN_002cc4f0 always loops 9 (probe: colCalls=9)
constexpr int kStats = 2;

// FUN_003fe720(cmdId, arg) -- the field Equipment screen's PER-HIGHLIGHT preview fill.
//
// Not per-frame, and that is established rather than assumed: its call site is inside
// FUN_002c2320 **case 0xC**, the child-list event (S71's category map: 0xC is the list event, with
// 0x8000 = cursor move at packet+8), immediately after FUN_00291d80 sets the description bar for
// the newly-highlighted row. So one call per highlight, on the game's own event.
constexpr uint32_t RVA_ATTRFILL = 0x2DE720;

typedef uint64_t (*Pfn_Refresh)(uint32_t);
// ALL SIX ARGUMENTS, and the count is load-bearing -- see HookedDelta.
typedef uint64_t (*Pfn_Delta)(void*, int, void*, int, void*, uint32_t);
typedef void     (*Pfn_AttrFill)(int, int);
Pfn_Refresh  s_origRefresh  = nullptr;
Pfn_Delta    s_origDelta    = nullptr;
Pfn_AttrFill s_origAttrFill = nullptr;

struct Column {
    int          memberIdx = -1;
    int          charId    = -1;
    bool         canEquip  = false;
    bool         equipped  = false;
    bool         haveStat[kStats] = { false, false };
    int          improve[kStats]  = { 0, 0 };   // POSITIVE = the new item is BETTER (see below)
};

struct Snapshot {
    bool         valid = false;
    std::wstring label[kStats];
    bool         labelOn[kStats] = { false, false };
    Column       col[kCols];
    int          resolved = 0;                 // columns with a real character, in panel order
    int          order[kCols] = { 0 };         // resolved index -> column index
};

std::mutex g_mutex;
Snapshot   g_snap;                             // published; read from the input thread
Snapshot   g_building;                         // game thread only, between refresh enter and leave

void* MenuCtx() { return PtrAt(Hooks::ResolveRva(RVA_MENUCTX), 0); }

void* LivePanel() {
    void* ctx = MenuCtx();
    if (!ctx) return nullptr;
    void* panel = PtrAt(ctx, CTX_PANEL);
    if (!panel) return nullptr;
    // Type validation: the class pointer must be the panel's own, not merely non-null.
    return (Obj0(panel) == Hooks::ResolveRva(RVA_PANELCLS)) ? panel : nullptr;
}

// The compare panel is PARKED in menuCtx and survives leaving the shop, so "the object exists" is
// NOT "the comparison is on screen". Without a surface test the last shop item kept answering the
// keys while the player browsed the pause menu's own inventory -- a stale comparison against
// something they were no longer looking at, which is worse than silence.
//
// So require a surface that actually drives this panel: the shop's live list container, or the
// post-purchase equip-target screen. Both are class-validated pointers, not flags, so there is no
// state to get stuck.
bool ShopSurfaceLive() {
    if (EquipTargetReader::IsActive()) return true;
    void* list = PtrAt(Hooks::ResolveRva(RVA_SHOPLIST), 0);
    return list && ShopReader::OwnsSurface(list);
}

// ---- the field Equipment screen's attribute panel ----------------------------------------------
void* LiveAttrPanel() {
    void* ctx = MenuCtx();
    if (!ctx) return nullptr;
    void* panel = PtrAt(ctx, CTX_ATTRPANEL);
    if (!panel) return nullptr;
    return (Obj0(panel) == Hooks::ResolveRva(RVA_ATTRCLS)) ? panel : nullptr;
}

// "Is a preview showing?" is the same test the game's own row renderer uses: current != preview.
bool AttrPreviewActive(void* panel) {
    for (int row = 0; row < AP_ROWS; ++row) {
        uint32_t cur = 0, prev = 0;
        if (!SafeReadU32(panel, AP_CURRENT + static_cast<uint32_t>(row) * 4, &cur)) continue;
        if (!SafeReadU32(panel, AP_PREVIEW + static_cast<uint32_t>(row) * 4, &prev)) continue;
        if (cur != prev) return true;
    }
    return false;
}

std::wstring ComposeAttrPreview() {
    void* panel = LiveAttrPanel();
    if (!panel || !AttrPreviewActive(panel)) return std::wstring();

    void* ctx = MenuCtx();
    std::wstring name;
    if (ctx) {
        uint16_t rawMem = 0;
        if (SafeReadU16(ctx, CTX_SELCHAR, &rawMem)) {
            const int mem = static_cast<int16_t>(rawMem);
            if (mem >= 0 && mem <= 8) {
                void* blk = PtrAt(ctx, CTX_BLOCKS2 + static_cast<uint32_t>(mem) * 8);
                uint16_t rawChar = 0;
                if (blk && SafeReadU16(blk, BLK_CHARID, &rawChar)) {
                    const int charId = static_cast<int16_t>(rawChar);
                    if (charId >= 0)
                        name = BattleState::CharacterName(static_cast<uint8_t>(charId));
                }
            }
        }
    }

    std::wstring stats;
    for (int row = 0; row < AP_ROWS; ++row) {
        uint32_t cur = 0, prev = 0;
        if (!SafeReadU32(panel, AP_CURRENT + static_cast<uint32_t>(row) * 4, &cur)) continue;
        if (!SafeReadU32(panel, AP_PREVIEW + static_cast<uint32_t>(row) * 4, &prev)) continue;
        if (cur == prev) continue;                    // unchanged rows add nothing

        // Game-supplied label. These ids sit outside TextCapture's passive cache band, so the
        // resolving variant is required -- StringById alone reads empty here forever.
        const std::wstring label = TextCapture::ResolveStringById(TXT_ATTR_BASE + row);
        if (label.empty()) continue;
        if (!stats.empty()) stats += L", ";
        // Both halves: the NEW value (what the screen shows) and the direction+size of the change,
        // in the same vocabulary the shop comparison uses.
        const int delta = static_cast<int>(prev) - static_cast<int>(cur);
        stats += label;
        stats += L' ';
        stats += std::to_wstring(prev);
        stats += L", ";
        stats += Phrase::Get(delta > 0 ? Phrase::Id::StatUp : Phrase::Id::StatDown);
        stats += L' ';
        stats += std::to_wstring(delta < 0 ? -delta : delta);
    }
    if (stats.empty()) return std::wstring();
    return name.empty() ? stats : (name + L": " + stats);
}

std::wstring DecodeWidgetText(void* widget) {
    if (!widget) return std::wstring();
    const uint8_t* codec = static_cast<const uint8_t*>(PtrAt(widget, W_TEXT));
    if (!codec) return std::wstring();
    std::wstring s = GameText::Decode(codec, 128);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// Harvest at refresh EXIT, entirely on the game thread, so the published snapshot owns copies and
// the input thread never dereferences a live game object.
void Harvest(Snapshot* out) {
    void* panel = LivePanel();
    if (!panel) return;

    // LABELS ARE GAME TEXT. FUN_002cb1f0 already resolved them onto its own widgets, so this is a
    // pure read -- the mod needs no label ids and makes no game call.
    void* header = PtrAt(panel, P_HEADER);
    void* arr    = header ? PtrAt(header, 0x60) : nullptr;
    if (arr) {
        for (int k = 0; k < kStats; ++k) {
            void* w = PtrAt(arr, static_cast<uint32_t>(k * 8));
            if (!w) continue;
            uint32_t flags = 0;
            out->labelOn[k] = SafeReadU32(w, W_FLAGS, &flags) && (flags & 1) != 0;
            out->label[k]   = DecodeWidgetText(w);
        }
    }

    void* ctx = MenuCtx();
    for (int i = 0; i < kCols; ++i) {
        void* colA = PtrAt(panel, P_COLA + static_cast<uint32_t>(i) * 8);
        void* colB = PtrAt(panel, P_COLB + static_cast<uint32_t>(i) * 8);
        if (!colA) continue;
        Column& c = out->col[i];

        uint32_t mem = 0;
        if (!SafeReadU32(colA, COL_MEMBER, &mem)) continue;
        // The game indexes menuCtx+0xAC8 with this and does NOT bound-check it. A stale panel would
        // otherwise send us reading an arbitrary pointer, so the clamp is ours to do.
        if (mem >= static_cast<uint32_t>(kCols)) continue;
        c.memberIdx = static_cast<int>(mem);

        void* block = ctx ? PtrAt(ctx, CTX_BLOCKS + mem * 8) : nullptr;
        if (!block) continue;
        uint16_t raw = 0;
        if (!MemRead::SafeReadU16(block, BLK_CHARID, &raw)) continue;
        const int charId = static_cast<int16_t>(raw);
        if (charId < 0) continue;                     // empty roster slot: the panel hides it
        c.charId = charId;

        uint32_t v = 0;
        c.canEquip = colB && SafeReadU32(colB, CB_CANEQUIP, &v) && v != 0;
        c.equipped = colB && SafeReadU32(colB, CB_EQUIPPED, &v) && v != 0;

        out->order[out->resolved++] = i;
    }
    out->valid = out->resolved > 0;
}

// Recover (column, stat slot) from FUN_002cc780's target. Probe-confirmed: the target is always
// exactly colA+0xD8 or colA+0xE8 -- zero strays across every observed pass.
bool LocateTarget(void* target, int* colOut, int* slotOut) {
    void* panel = LivePanel();
    if (!panel || !target) return false;
    for (int i = 0; i < kCols; ++i) {
        void* colA = PtrAt(panel, P_COLA + static_cast<uint32_t>(i) * 8);
        if (!colA) continue;
        char* base = static_cast<char*>(colA);
        if (target == base + COL_STAT0) { *colOut = i; *slotOut = 0; return true; }
        if (target == base + COL_STAT1) { *colOut = i; *slotOut = 1; return true; }
    }
    return false;
}

// FUN_002cc780(pair, delta, target, style, outBuf, outSize) -- draws one stat's arrow + number.
//
// ALL SIX ARGUMENTS MUST BE DECLARED AND FORWARDED. This shipped declaring only the first four and
// it CRASHED THE GAME on entering a shop (dump 2026-08-03 17:48, access violation WRITE).
//
// Arguments 5 and 6 are not in registers -- on x64 only the first four are, and the rest travel in
// the CALLER's outgoing stack-argument area at [rsp+0x20] and [rsp+0x28]. The game's caller fills
// them (FUN_002ca7c0, disassembled at its call site):
//     mov  dword [rsp+0x28], 8        ; arg6 = size of the output buffer
//     mov  [rsp+0x20], rcx            ; arg5 = the output buffer itself
//     call FUN_002cc780
// and the callee reads arg5 back as `[rsp+0x70]` after its prologue, hands it to a small writer that
// does `if (size >= 8) { *(uint32_t*)buf = ...; buf[4] = '+'|'-'; }`.
//
// A four-argument detour reserves only the 32-byte shadow space when it calls the trampoline, so
// those two slots are never written and the original reads whatever the previous call left on the
// stack. That is a WILD POINTER and a WILD SIZE: the size check passes on garbage (any large value
// is >= 8) and the write lands wherever the pointer happened to point. Unmapped -> instant crash;
// mapped -> eight bytes of somebody else's memory silently destroyed, which is the worse outcome and
// is why this must never be "fixed" by guarding the crash instead of passing the arguments.
//
// The same reasoning applies to EVERY hook: the detour's arity must equal the game function's, and
// Hooks::InstallTyped cannot check it for us -- it only forces the detour and the trampoline pointer
// to agree with EACH OTHER. Both were wrong here together, which is exactly why it compiled.
//
// We only observe `delta`; the two new arguments are forwarded untouched and never read.
//
// POLARITY, the one fact that must not be wrong. FUN_002ca7c0:115-121 computes
//     FUN_0030a4e0(member, 0,                curStats);
//     FUN_0030a4e0(member, slotsWithNewItem, newStats);
//     delta = curStats[+0x0A] - newStats[+0x0A];
// and Ghidra's own declarations pin which buffer is which: `undefined1 local_b8[10]` is followed by
// `byte local_ae` (= local_b8 + 0x0A) and `undefined1 local_8c[10]` by `byte local_82`. So
// delta = cur - new, and a NEGATIVE delta means the NEW item is BETTER. Confidence 1.00.
//
// We store `improve = -delta` and never expose the raw value, so nothing downstream can re-invert
// it by accident. An inverted announce would be worse than silence: the player buys the wrong sword.
uint64_t HookedDelta(void* pair, int delta, void* target, int style, void* outBuf, uint32_t outSize) {
    const uint64_t ret = s_origDelta ? s_origDelta(pair, delta, target, style, outBuf, outSize) : 0;
    STALL_SCOPE("EquipCompare::HookedDelta");
    int col = 0, slot = 0;
    if (LocateTarget(target, &col, &slot)) {
        g_building.col[col].haveStat[slot] = true;
        g_building.col[col].improve[slot]  = -delta;
    }
    return ret;
}

// FUN_002cc4f0(itemId) -- one whole refresh pass. Event-driven: it runs on a highlight change, a
// confirm, or an equip, never per frame (probe criterion 7: zero passes over an idle highlight).
uint64_t HookedRefresh(uint32_t itemId) {
    g_building = Snapshot();
    const uint64_t ret = s_origRefresh ? s_origRefresh(itemId) : 0;
    STALL_SCOPE("EquipCompare::HookedRefresh");
    Harvest(&g_building);
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_snap = g_building;
    }
    return ret;
}

// One stat clause: "<label> <up|down> <n>", or "<label> <n>" when it does not move.
void AppendStat(std::wstring& line, const std::wstring& label, int improve) {
    if (label.empty()) return;                        // no game text for it -> say nothing
    if (!line.empty()) line += L", ";
    line += label;
    line += L' ';
    if (improve != 0) {
        line += Phrase::Get(improve > 0 ? Phrase::Id::StatUp : Phrase::Id::StatDown);
        line += L' ';
    }
    line += std::to_wstring(improve < 0 ? -improve : improve);
}

std::wstring Compose(const Snapshot& s, int colIdx) {
    if (colIdx < 0 || colIdx >= kCols) return std::wstring();
    const Column& c = s.col[colIdx];
    if (c.charId < 0) return std::wstring();

    const std::wstring name = BattleState::CharacterName(static_cast<uint8_t>(c.charId));
    if (name.empty()) return std::wstring();          // no game-supplied name -> stay silent

    std::wstring line = name + L": ";
    if (!c.canEquip) { line += Phrase::Get(Phrase::Id::CannotEquip); return line; }
    if (c.equipped)  { line += Phrase::Get(Phrase::Id::AlreadyEquipped); return line; }

    std::wstring stats;
    for (int k = 0; k < kStats; ++k) {
        if (!s.labelOn[k] || !c.haveStat[k]) continue;
        AppendStat(stats, s.label[k], c.improve[k]);
    }
    if (stats.empty()) return std::wstring();         // nothing measurable -> silence, not filler
    return line + stats;
}

// The field Equipment screen's ONE emit point. Everything else in this file only produces text; this
// surface has no other reader to own it, so the single-choke-point rule is satisfied here.
//
// QUEUED, never interrupting. The row's own name is announced first, off the child list's 0x8000,
// and this fill runs immediately after inside the same case -- interrupting would cut the item name
// off mid-word and leave the player with a stat line for something they never heard named.
//
// No dedup and no latch: the fill is per-highlight, and on the Status screen current == preview so
// ComposeAttrPreview returns empty and this is silent on its own. Re-entering a row re-announces,
// which is the point.
void HookedAttrFill(int cmdId, int arg) {
    if (s_origAttrFill) s_origAttrFill(cmdId, arg);
    STALL_SCOPE("EquipCompare::HookedAttrFill");
    const std::wstring line = ComposeAttrPreview();
    if (line.empty()) return;
    // LOG UNCONDITIONALLY, SPEAK ONLY WHEN VOLUNTEERING IS ON. The line is still composed and
    // recorded either way, so a log keeps answering "what would it have said" (diagnostics go to the
    // log, never to speech).
    Log::WriteW("EQUIP", "preview:", line);
    // THE AUTO-DETAIL GATE, missing since this hook was written (S125) and reported in play
    // 2026-08-11: *"the delta comparison is vocalizing automatically in the unequip menu with
    // autodetail off, and it should not be."*
    //
    // This is the definition of what that setting controls -- `mod_menu.h:68`, S147: "volunteer the
    // detail on highlight instead of on a key. Default Off". A per-highlight stat line IS volunteered
    // detail, and this was the one such path that never asked. `shop_reader.cpp:145` already gates
    // the very same EquipCompare output on it, so the two surfaces disagreed about one setting.
    //
    // IT DOES NOT GATE THE KEY, and must not: `mod_menu.h:102` is explicit that AutoDetail "never
    // gates a key", and `EquipCompare::LineFor(1)` returns this exact line to the `4` handler in
    // nav_commands.cpp. So with the setting off the preview is still one keypress away -- nothing
    // becomes unreachable, it just stops speaking on its own.
    if (!ModMenu::AutoDetailOn()) return;
    Speech::Output(line, /*interrupt=*/false);
}

} // namespace

namespace EquipCompare {

bool Init() {
    bool ok = Hooks::InstallTyped(RVA_REFRESH,  &HookedRefresh,  &s_origRefresh);
    ok     &= Hooks::InstallTyped(RVA_DELTA,    &HookedDelta,    &s_origDelta);
    ok     &= Hooks::InstallTyped(RVA_ATTRFILL, &HookedAttrFill, &s_origAttrFill);
    Log::Write("EQUIP", ok ? "compare hooks installed (FUN_002cc4f0 + FUN_002cc780 + FUN_003fe720)"
                           : "a compare hook FAILED -- equipment deltas unavailable");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_ATTRFILL);
    Hooks::Uninstall(RVA_DELTA);
    Hooks::Uninstall(RVA_REFRESH);
    s_origRefresh  = nullptr;
    s_origDelta    = nullptr;
    s_origAttrFill = nullptr;
}

// Two INDEPENDENT sources, and only one can be showing at a time: the shop's multi-character
// compare panel, or the field Equipment screen's single-character preview. The shop one is checked
// first because its surface test is the stricter of the two.
bool IsLive() {
    if (LivePanel() && ShopSurfaceLive()) return true;
    void* attr = LiveAttrPanel();
    return attr && AttrPreviewActive(attr);
}

int ColumnCount() {
    if (LivePanel() && ShopSurfaceLive()) {
        std::lock_guard<std::mutex> lk(g_mutex);
        return g_snap.valid ? g_snap.resolved : 0;
    }
    // The field Equipment screen equips ONE character, so it is a single column.
    void* attr = LiveAttrPanel();
    return (attr && AttrPreviewActive(attr)) ? 1 : 0;
}

std::wstring LineFor(int n) {
    if (LivePanel() && ShopSurfaceLive()) {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_snap.valid || n < 1 || n > g_snap.resolved) return std::wstring();
        return Compose(g_snap, g_snap.order[n - 1]);
    }
    return (n == 1) ? ComposeAttrPreview() : std::wstring();
}

std::wstring LineForMember(int memberIdx) {
    if (!ShopSurfaceLive()) return std::wstring();      // never answer from a stale shop snapshot
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_snap.valid) return std::wstring();
    for (int i = 0; i < g_snap.resolved; ++i) {
        const int c = g_snap.order[i];
        if (g_snap.col[c].memberIdx == memberIdx) return Compose(g_snap, c);
    }
    return std::wstring();
}

} // namespace EquipCompare

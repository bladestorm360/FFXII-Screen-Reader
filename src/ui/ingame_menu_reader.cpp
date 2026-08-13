#include "ui/ingame_menu_reader.h"
#include "ui/menu_reader.h"
#include "ui/text_capture.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/item_names.h"
#include "core/mem_read.h"
// The battle command menu's character: parent+0x2FE0 is a scene handle, and battle_state already
// resolves handle -> actor -> name with pure memory reads (no game call).
#include "battle/battle_state.h"
#include "speech/speech.h"
#include "speech/phrasebook.h"
#include "core/logger.h"
#include "core/stall_probe.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <string>

namespace {

using MemRead::Obj0;

// ---- Row-chain family (pause menu command column + submenu tab bars) ------------------------
// Owner class (obj[0] RVA) -> ROW_OFF (row-array pointer offset in the window). The row-record
// layout (name +0x10) is the same across all because the shared builder FUN_002cd3c0 fills them.
struct RowChainClass { uint32_t rva; uint32_t rowOff; };
constexpr RowChainClass ROW_CHAIN[] = {
    { 0x160DE0, 0xD8 },   // FUN_00280de0 — field pause command column (+ submenus)
    { 0x1A2320, 0xC8 },   // FUN_002c2320 — inventory category tab bar (pause menu)
    { 0x445E00, 0xC8 },   // FUN_00565e00 — pause command 0x4B8, NOT the gambit screen. The comment
                          // used to read "gambits" and it is a MISLABEL (corrected S94): the gambit
                          // setup screen is FUN_005691e0 (RVA 0x4491E0, cmd 0x4B9) and has its own
                          // reader, ui/gambit_reader.cpp, because its focus `val` is a display-record
                          // index rather than an offset into a row array. The entry itself is
                          // structurally valid and stays — only the name was wrong.
    { 0x44F810, 0xE0 },   // FUN_0056f810 — sub-panel list
    { 0x45B890, 0xD0 },   // FUN_0057b890 — equip-type screen list
};
constexpr uint32_t ROW_STRIDE   = 0x20;   // row record size
constexpr uint32_t OFF_ROW_NAME = 0x10;   // NAME codec* (built by FUN_002cd3c0)

// ---- The field pause menu's OWN window handler (drives the entry announce) -------------------
// FUN_00280de0 == ROW_CHAIN[0]. The PARTY MENU announces its first row the SAME WAY the battle
// command menu does: the entry focus is STASHED (see MenuReader::HookedFocusSet -> ArmPaneEntry) and
// released by the menu's own "show" event, so speech lands WITH the menu instead of during its
// construction. The battle menu releases on its row DRAW (FUN_00276be0); this window has no per-row
// draw we hook, but it has the direct equivalent -- case 0x13 is the SHOW path: it creates the info
// window, plays the open SE FUN_00249c60(4) ONCE, and clears the "hidden" bit (0x80) on the menu's UI
// resources (battle_4_p / s_font_c / targetline_p / shape / mini_face_c). That is the frame the menu
// becomes visible; we trigger on the MESSAGE, the SE call is only corroboration.
//   msg map (from the decompile): 1 init, 2 close, 0xa teardown, 0xc notify (0x8000 row focus /
//   0x8001 confirm / 0x8002 cancel), 0x13 SHOW, 0x10 destroy. (0x11f ACTIVATE is NEVER sent -- a
//   full session showed 0 occurrences; do not wait on it.)
// Four earlier "menu is ready" signals were each refuted by measurement -- first-string-drawn (next
// frame), the row's own text (31ms), 0x11f ACTIVATE (never), and a timeout fallback (spoke at the
// wrong time). 0x13 is the game's own visible-open event, which is why we use it.
constexpr uint32_t RVA_FIELD_PANE_WND = 0x160DE0;   // FUN_00280de0 (== ROW_CHAIN[0].rva)
constexpr uint32_t PKT_CAT_OFF        = 0x00;       // *(int*)packet       = category
constexpr uint32_t PKT_MSG_OFF        = 0x08;       // *(int64*)(packet+8) = message
constexpr uint32_t WND_CAT_SHOW       = 0x13;       // the menu-visible frame (see above)
constexpr uint32_t WND_CAT_CLOSE      = 0x12;       // pause-menu root teardown (FUN_00280de0 case 0x12,
                                                    // clears DAT_0209ac30+0xdf8) -- the frame the whole
                                                    // pause menu goes away and the field resumes

typedef uint64_t (*Pfn_FieldPaneWnd)(void*, void*);
Pfn_FieldPaneWnd s_origFieldPaneWnd = nullptr;

// ---- Battle command menu (CONFIRMED 2026-07-10 via probe) ------------------------------------
// The in-battle command list (Attack / Magicks & Technicks / Items / ...) routes cursor moves
// through the SAME FUN_00247510 msg-0x8000 dispatch the party menu uses. Its `owner` is the command
// PANEL, window class FUN_0027ad70 (RVA 0x15AD70); the focus `val` is the highlighted row index.
// The highlighted command id is a u16 at panel+0x510 + index*8 (count = int at panel+0x500). The
// NAME is exactly what the game's own per-row draw FUN_00276be0 (RVA 0x156BE0) resolves into
// panel+0x1578 (FUN_0035d330(0x15,id) -> FUN_002b58b0), so we cache the DECODED name per cmdId from
// that draw (memory-only, no game call) and look it up on the 0x8000 focus.
constexpr uint32_t RVA_BCMD_PANEL = 0x15AD70;  // FUN_0027ad70 — battle command panel (owner obj[0])
constexpr uint32_t RVA_BCMD_DRAW  = 0x156BE0;  // FUN_00276be0(panel, geom, row) — command row draw
constexpr uint32_t OFF_BCMD_ARR   = 0x510;     // panel+0x510 = command entries (stride 8; id u16 @ +0)
constexpr uint32_t OFF_BCMD_CNT   = 0x500;     // panel+0x500 = command count (int)
constexpr uint32_t OFF_BCMD_NAME  = 0x1578;    // panel+0x1578 = name codec the draw just resolved
constexpr uint32_t BCMD_STRIDE    = 8;

typedef void (*Pfn_BcmdDraw)(void*, void*, int);
Pfn_BcmdDraw s_origBcmdDraw = nullptr;

// ---- Battle sub-lists (Magicks & Technicks / Items) ------------------------------------------
// Same panel (FUN_0027ad70) + same FUN_00247510 0x8000 focus, but a DIFFERENT per-row draw
// callback resolves each name. Some draws OVERWRITE panel+0x1578 with the MP-cost/quantity after
// the name, so we can't read +0x1578 back — instead we RESOLVE on focus, picking the resolver by
// the panel's draw callback (`*( *(panel+0x1510) + 0x120 )`). Each branch is guarded by an exact
// callback-pointer match, so a wrong guess stays SILENT (never wrong speech).
//   FUN_00276be0 (top-level, cat 0x15)  — memory-only cache below (no overwrite). CONFIRMED.
//   FUN_0027e530 (items)                — FUN_00272cb0(id) returns the name codec. CONFIRMED.
// "Magicks & Technicks" (top cmdId 0x12) is TWO-LEVEL (traced: FUN_0027c3d0 0x12->kind 2->FUN_0027e050
// case 2->type 8; category select->kind 0xa-0xf->type 0xb):
//   FUN_0027d240 (category chooser)     — FUN_0035d330(cat,id), cat = (panel+0x513+row*8 & 4)?0x18:0x15
//                                         (0x18 Technicks / 0x15 Magick schools). +0x1578 holds NAME.
//   FUN_0027ce70 (spell/technick list)  — FUN_0035d330(0x14,id). +0x1578 overwritten by MP -> re-resolve.
constexpr uint32_t OFF_LISTWIDGET  = 0x1510;   // panel+0x1510 = the list widget
constexpr uint32_t OFF_DRAW_CB     = 0x120;    // listWidget+0x120 = per-row draw callback
constexpr uint32_t OFF_BCMD_FLAG   = 0x513;    // panel+0x513 + row*8 = per-row flag byte (bit2 in chooser)

// ---- The SECOND COLUMN the sub-lists draw beside the name (Session 146) -----------------------
// Every battle sub-list that shows a number to the right of the row draws it from ONE place: the
// u16 at panel+0x512 + row*8 (FUN_0027ce70:119-125, via FUN_0029cb80(0x4694, word)). What the word
// MEANS is said by panel+0x50C, which FUN_0031eb20's case 0xB sets in the SAME `if` that writes it:
//
//   rec+0xC & 0x20000000 -- an MP-costing action. Word = FUN_002f95d0(actor, id), the cost AS THIS
//                           CHARACTER PAYS IT, not the master-data figure. Gate = 1, 2 or 4.
//   rec+0xC bit 31       -- an ITEM-consuming action. Word = FUN_00309ec0(FUN_003093b0(rec), 0):
//                           rec+0x22 is the item the action spends, and that call is its OWNED
//                           COUNT. Gate = 3.
//
// TWO WRONG DISCRIMINATORS WERE SHIPPED AND MEASURED AWAY BEFORE THIS ONE, both in one session:
//   * the per-row DRAW CALLBACK -- the battle Items list shares FUN_0027ce70 with the magick list,
//     so every item read "Potion, MP 31";
//   * the LIST KIND at panel+0x4C0 -- the Items list is case 0xB TOO. Measured: `listKind=0xB
//     costGate=3` for items against `listKind=0xB costGate=1` for White Magicks.
// The lesson both times: identify a shared surface by the branch that WROTE the field, not by the
// code that renders it or the container it lives in. Only the gate word is that branch's own output.
//
// THE GATE IS ALSO THE SILENCE TEST, and must not be replaced by "is the number non-zero" -- the
// game's own display test is `& ~2` (FUN_0027ce70:61):
//   0 -- no cost at all (Technicks). Case 0xB never clears +0x512 per row, so the stale word from a
//        previous list is still sitting there. This is exactly the case the tester asked to be SILENT.
//   2 -- the word is a MIST CHARGE count that the draw spends on icons (FUN_0027ce70:91-100), never
//        on a figure. Speaking it would be a wrong number, not a missing one.
constexpr uint32_t OFF_BCMD_COST     = 0x512;  // panel+0x512 + row*8 = u16 second column
constexpr uint32_t OFF_BCMD_COSTKIND = 0x50C;  // panel+0x50C = what that word means, per builder
constexpr uint32_t OFF_BCMD_LISTKIND = 0x4C0;  // panel+0x4C0 = FUN_0031eb20's switch (LOG ONLY --
                                               // 0xB for BOTH lists; that is why it cannot discriminate)
constexpr uint32_t COSTKIND_MIST     = 2;      // gate values that print nothing: 0 and this
constexpr uint32_t COSTKIND_ITEMS    = 3;      // ...and the one that means "owned count", not MP
constexpr int      MAX_SECOND_COLUMN = 999;    // out of range -> say nothing, and log why

// ---- GAMBITS: the one battle command that is a TOGGLE, not a submenu --------------------------
// cmdId 0x0D. Two independent sites single it out: the row draw FUN_00276be0 special-cases exactly
// this id to draw a SECOND, two-state graphic beside the name (FUN_00242600 with a frame index),
// and the confirm handler FUN_0027c3d0 `case 0xd` is the only branch that flips a flag in place and
// returns to the same menu instead of opening a list or entering targeting.
//
// The row's list struct lives at panel+0x4C0; the confirm handler receives exactly that pointer, so
// its field offsets are the panel offsets minus 0x4C0 (count 0x500->0x40, ids 0x510->0x50,
// flags 0x513->0x53). panel+0x4C8 is the acting character's scene handle -- read it from the SAME
// place the game's own toggle reads it rather than going through the controller, so the two cannot
// disagree.
//
// FORWARD RISK (tester raised it): if a later game state turns Gambits into a submenu rather than a
// toggle, the row stops taking the `case 0xd` path. The gate below -- top-level draw callback AND
// cmdId 0x0D AND the row not disabled -- is exactly the condition the game itself uses to draw the
// two-state icon, so the state suffix disappears on its own rather than reporting a stale toggle.
constexpr int      BCMD_ID_GAMBIT      = 0x0D;
constexpr uint8_t  BCMD_FLAG_DISABLED  = 0x02;  // row greyed out: game draws alpha 0x40, confirm rejects
constexpr uint32_t OFF_BCMD_CHAR       = 0x4C8; // panel+0x4C8 = acting character's scene handle
constexpr uint32_t RVA_BCMD_CONFIRM    = 0x15C3D0;  // FUN_0027c3d0(outSel, list, row) confirm handler
constexpr uint32_t OFF_LIST_KIND       = 0x00;  // list+0x00 = list type; 6 and 9 are the top-level list
constexpr uint32_t OFF_LIST_CHAR       = 0x08;  // list+0x08 = scene handle (== panel+0x4C8)
constexpr uint32_t OFF_LIST_IDS        = 0x50;  // list+0x50 + row*8 = u16 command id
constexpr uint32_t OFF_LIST_CNT        = 0x40;  // list+0x40 = command count (int)

typedef uint32_t (*Pfn_BcmdConfirm)(int16_t*, void*, int);
Pfn_BcmdConfirm s_origBcmdConfirm = nullptr;
constexpr uint32_t RVA_DRAW_TOPCMD = 0x156BE0; // FUN_00276be0 -- SAME function as RVA_BCMD_DRAW
                                              // above; two names on purpose, one is the hook target,
                                              // the other the draw-callback identity we compare against.
constexpr uint32_t RVA_DRAW_CHOOSER= 0x15D240; // FUN_0027d240 (Magicks/Technicks category chooser)
constexpr uint32_t RVA_DRAW_MAGICK = 0x15CE70; // FUN_0027ce70 (spell/technick list, cat 0x14)
constexpr uint32_t RVA_DRAW_ITEM   = 0x15E530; // FUN_0027e530 (items) — CONFIRMED working
                                              // (FUN_0035d330's record walk moved to
                                              // battle/battle_state.cpp as DefName -- shared with
                                              // the combat log; FUN_00272cb0, the item name codec,
                                              // moved to core/item_names.cpp -- shared with the
                                              // loot scanner)
constexpr uint32_t CAT_MAGICK      = 0x14;     // FUN_0035d330 category for the spell/technick list
// THE TWO LABELS BELOW WERE THE WRONG WAY ROUND (measured S158, live, on the gambit picker's
// diagnostic walk): **`0x18` is the MAGICK-SCHOOL table** (0-3 resolve to "White Magicks", "Black
// Magicks", "Time Magicks", "Green Magicks") and **`0x15` is the BATTLE-COMMAND table** ("Attack",
// "Magicks", "Technicks", "Items"). The VALUES and the flag test are unchanged and still correct --
// each is passed with an id from the chooser's own rows, so the branch always worked; only the names
// in this comment lied about which table is which. Left as two named constants precisely so the next
// reader gets the corrected names rather than re-deriving them.
constexpr uint32_t CAT_CHOOSER_TECH= 0x18;     // flag bit2 set  -> the magick-SCHOOL name table
constexpr uint32_t CAT_CHOOSER_MAG = 0x15;     // otherwise      -> the battle-COMMAND name table


// Battle target-selection readout lives in battle_target_reader.cpp now (hooks the vitals builder
// FUN_00329220 + the current-target index ctx+0xde0). The old reticle hook (FUN_005528c0) was
// removed: probing proved it never fires for normal Foes/Party/Allies selection (it is the
// free-aim/area mode only), which is why targeting was silent.


std::mutex   g_mutex;
std::wstring g_bcmdName[256];              // top-level cmdId -> decoded name (cached from FUN_00276be0)
// Initial-focus replay for the battle command menu. When the menu OPENS, its 0x8000 fires before
// FUN_00276be0 has drawn (and therefore cached) any command name, so the highlighted command
// resolves to nothing and the entry is silent -- the field pause menu gets its entry announce from
// the FUN_00244830 pane replay, but the battle menu is a separate system with no such path. We
// stash the unresolved focus here and let the first successful draw replay it.
void* g_bcmdPendingPanel = nullptr;
int   g_bcmdPendingIndex = -1;

// ---- WHOSE command menu is this? (Session 86) --------------------------------------------------
// With 2+ party members, left/right moves the battle command window between characters, and the mod
// said nothing -- the player heard "Attack" and had no idea who was about to do it.
//
// The controller is FUN_002778c0 (RVA 0x1578C0), the sole creator of the command panel and the only
// thing that reads the pad directly. Its cases 0xa/0xb turn pad RIGHT (0x20) into +1 and LEFT (0x80)
// into -1, gated on FUN_0035d4e0() -> *(int*)&DAT_022c8478 > 1 (i.e. "2 or more party members" --
// the tester's own words, and the game's own test), stash it at ctrl+0x2DB6, then send THEMSELVES
// message 0x23. Case 0x23 resolves the new character and writes it to parent+0x2FE0, which the
// controller's own msg 0x2d hands back as "the current battle-menu character".
//
// THE ID AT +0x2FE0 IS A SCENE HANDLE -- established from the game's own comparison, not a probe.
// FUN_0035bc50 builds the party record table and does:
//     DAT_022c806c = thunk_FUN_003590d0();              // the LEADER SCENE HANDLE accessor
//     if (*(int*)(record + 0x04) == DAT_022c806c) ...   // -> that slot is the leader
// It compares record+0x04 against the leader handle directly, and FUN_0027c280 returns exactly that
// field as the new character. So BattleState::ActorForHandle resolves it, and the name is a pure
// memory read of actor+0x18 -- no game call, safe from any thread. Conf 0.99.
constexpr uint32_t RVA_BCMD_CTRL   = 0x1578C0;  // FUN_002778c0(ctrl, msgStruct)
constexpr uint32_t OFF_CTRL_PARENT = 0xD0;      // ctrl+0xD0   -> parent registry object
constexpr uint32_t OFF_CUR_CHAR    = 0x2FE0;    // parent+0x2FE0 = current battle-menu char handle
constexpr int      MSG_CTRL_BUILD  = 0x01;      // controller construct (the menu is coming up)
constexpr int      MSG_CTRL_SWITCH = 0x23;      // "switch to the adjacent character", arg = +1/-1

typedef uint64_t (*Pfn_BcmdCtrl)(void*, void*);
Pfn_BcmdCtrl s_origBcmdCtrl = nullptr;

void* g_bcmdCtrl = nullptr;          // the live controller, cached so the focus path can reach +0x2FE0
// NOT a dedup -- a TRANSITION latch, in the sense CLAUDE.md carves out. `g_bcmdNeedName` is armed by
// the controller's construct so the name is spoken once when the menu comes up, and `g_bcmdQueueNext`
// makes the command announcement that follows QUEUE behind the name instead of interrupting it.
// Neither suppresses an event; both exist so two announcements arrive in the order the tester asked
// for: who is acting, then what is highlighted.
bool g_bcmdNeedName  = false;
bool g_bcmdQueueNext = false;

// Entry-announce stash for the FIELD pane (FUN_00280de0) -- the exact analogue of g_bcmdPending*.
// FUN_00244830 fires at the START of menu construction, so speaking the entered row there lands it in
// the player's ear before the menu is up. Instead we stash it here and let the menu's own SHOW
// message (cat 0x13, HookedFieldPaneWnd) release it, so speech coincides with the menu appearing.
void*    g_panePendingOwner  = nullptr;
uint32_t g_panePendingRowOff = 0;
int      g_panePendingIndex  = -1;
uint64_t g_panePendingArmMs  = 0;    // arm time, for the "shown after Nms" log only

// First-seen category logging for ONE open: log each cat once (with Δt from the arm) so the whole
// SHOW sequence is visible without a per-frame flood. Reset by ArmPaneEntry, appended in the wnd hook.
// Keyed on category alone (not msg) so pointer-valued msgs cannot flood it -- there are ~10 cats.
constexpr int kPaneSeenMax = 24;
uint32_t g_paneSeen[kPaneSeenMax];
int      g_paneSeenCount = 0;

// True + records `cat` if this is the first time it has been seen since the last arm; false otherwise
// (already seen, or the set is full). Caller holds g_mutex.
bool MarkPaneSeen(uint32_t cat) {
    for (int i = 0; i < g_paneSeenCount; ++i) if (g_paneSeen[i] == cat) return false;
    if (g_paneSeenCount >= kPaneSeenMax) return false;
    g_paneSeen[g_paneSeenCount++] = cat;
    return true;
}

// The focused row's NAME codec pointer, or null. POD-only under __try (no objects), so the SEH
// guard is legal; decoding happens outside. The game always sends a valid focus index.
const uint8_t* ReadRowName(void* owner, uint32_t rowOff, int index) {
    if (!owner || index < 0) return nullptr;
    __try {
        char* w = reinterpret_cast<char*>(owner);
        char* rowArray = *reinterpret_cast<char* const*>(w + rowOff);
        if (!rowArray) return nullptr;
        char* row = rowArray + static_cast<size_t>(index) * ROW_STRIDE;
        return *reinterpret_cast<const uint8_t* const*>(row + OFF_ROW_NAME);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// The highlighted command id at row `index` on the panel (u16 @ panel+0x510+index*8), bounded by the
// count at panel+0x500. POD-only under __try. Returns -1 on fault / out of range.
int ReadBcmdCmdId(void* panel, int index) {
    if (!panel || index < 0) return -1;
    __try {
        char* p = reinterpret_cast<char*>(panel);
        int count = *reinterpret_cast<int*>(p + OFF_BCMD_CNT);
        if (count <= 0 || count > 64 || index >= count) return -1;
        return *reinterpret_cast<uint16_t*>(p + OFF_BCMD_ARR + static_cast<size_t>(index) * BCMD_STRIDE);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Per-row flag byte at panel+0x513+index*8 (bit 2 = Technicks category in the M&T chooser). 0 on fault.
uint8_t ReadBcmdRowFlag(void* panel, int index) {
    if (!panel || index < 0) return 0;
    __try {
        char* p = reinterpret_cast<char*>(panel);
        int count = *reinterpret_cast<int*>(p + OFF_BCMD_CNT);
        if (count <= 0 || count > 64 || index >= count) return 0;
        return *reinterpret_cast<uint8_t*>(p + OFF_BCMD_FLAG + static_cast<size_t>(index) * BCMD_STRIDE);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// The per-row cost word at panel+0x512+index*8 (MP, or mist charges -- see OFF_BCMD_COST). Bounded
// by the same row count as the id and flag readers. POD-only under __try; false on fault.
bool ReadBcmdCost(void* panel, int index, uint16_t* out) {
    if (!panel || index < 0) return false;
    __try {
        char* p = reinterpret_cast<char*>(panel);
        int count = *reinterpret_cast<int*>(p + OFF_BCMD_CNT);
        if (count <= 0 || count > 64 || index >= count) return false;
        *out = *reinterpret_cast<uint16_t*>(p + OFF_BCMD_COST + static_cast<size_t>(index) * BCMD_STRIDE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// During FUN_00276be0's draw of row `index`: the command id (panel+0x510+index*8) and the name codec
// the game just resolved into panel+0x1578. POD-only under __try.
bool ReadBcmdDraw(void* panel, int index, int* outCmdId, const uint8_t** outCodec) {
    if (!panel || index < 0) return false;
    __try {
        char* p = reinterpret_cast<char*>(panel);
        int count = *reinterpret_cast<int*>(p + OFF_BCMD_CNT);
        if (count <= 0 || count > 64 || index >= count) return false;
        *outCmdId = *reinterpret_cast<uint16_t*>(p + OFF_BCMD_ARR + static_cast<size_t>(index) * BCMD_STRIDE);
        *outCodec = *reinterpret_cast<const uint8_t* const*>(p + OFF_BCMD_NAME);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Speak a focused row. NO dedup: FUN_00247510's 0x8000 is event-driven (one message per genuine
// focus change), so every fire is a real move the player needs to hear -- including landing back on
// the row they left when they re-enter a pane. If this ever speaks twice for one keypress, a second
// call path is firing; find it, don't filter here.
void SpeakRow(void* owner, const std::wstring& text, const char* tag) {
    if (text.empty()) return;
    Log::WriteW("INGAME", tag, owner, text);
    Speech::Output(text, /*interrupt=*/true);
}

// FUN_00276be0 draws one battle-command row and resolves its name codec into panel+0x1578. Cache the
// DECODED name per cmdId (memory-only — the game's own localized text) for the 0x8000 focus lookup.
// The codec pointer can point into a shared static buffer, so we decode immediately and cache the
// string (never the pointer).
void SpeakBattleCommand(void* panel, int index);   // defined below; replayed from here
void HookedBcmdDraw(void* panel, void* geom, int row) {
    if (s_origBcmdDraw) s_origBcmdDraw(panel, geom, row);
    STALL_SCOPE("IngameMenu::HookedBcmdDraw");
    int cmdId = -1; const uint8_t* codec = nullptr;
    if (!ReadBcmdDraw(panel, row, &cmdId, &codec) || !codec || cmdId < 0 || cmdId >= 256) return;
    std::wstring text = GameText::Decode(codec, 256);   // SEH-guarded inside GameText
    if (!GameText::IsMostlyPrintable(text)) return;
    void* replayPanel = nullptr; int replayIndex = -1;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_bcmdName[cmdId] = text;
        // Consume a pending entry focus for THIS panel. `exchange`-style one-shot: FUN_00276be0 is a
        // PER-DRAW hook, so without clearing the pending slot here it would re-announce every frame.
        // (This is the sanctioned per-frame guard, not a dedup -- see the no-dedup rule in CLAUDE.md.)
        if (g_bcmdPendingPanel == panel && g_bcmdPendingIndex >= 0) {
            replayPanel = g_bcmdPendingPanel; replayIndex = g_bcmdPendingIndex;
            g_bcmdPendingPanel = nullptr; g_bcmdPendingIndex = -1;
        }
    }
    // Outside the lock: the speak path re-enters BattleCommandName, which takes g_mutex itself.
    if (replayPanel) SpeakBattleCommand(replayPanel, replayIndex);
}

// FUN_00280de0(window, packet): the field pause menu's own message proc. OBSERVE ONLY -- runs the
// original first, then reads; alters nothing. Two jobs:
//   (1) release the stashed entry announce on the SHOW message (cat 0x13), so the first row speaks
//       WITH the menu -- the field-pane analogue of HookedBcmdDraw releasing the battle stash;
//   (2) log each category once per open (Δt from the arm) so the open sequence stays visible.
// Both are gated to the pane we are actually waiting on (window == g_panePendingOwner), so this is a
// cheap pointer compare on every message when nothing is armed.
uint64_t HookedFieldPaneWnd(void* window, void* packet) {
    const uint64_t ret = s_origFieldPaneWnd ? s_origFieldPaneWnd(window, packet) : 0;
    STALL_SCOPE("IngameMenu::HookedFieldPaneWnd");

    uint32_t cat = 0; uint64_t msg = 0;
    if (!MemRead::SafeReadU32(packet, PKT_CAT_OFF, &cat)) return ret;
    MemRead::SafeReadU64(packet, PKT_MSG_OFF, &msg);

    // Pause-menu teardown -> invalidate any `o` description the menu left behind. The `o` help
    // (TextCapture::CurrentHelpText) is valid only while its generation matches the current focus
    // generation; that generation is bumped on every menu FOCUS but nothing bumps it when the menu
    // CLOSES, so the last row's description stayed "current" and `o` spoke it out in the field. This
    // is FUN_00280de0 (the pause-menu ROOT command column), so its close is the whole pause menu
    // going away -- bumping the generation here makes CurrentHelpText() return empty once we are back
    // in the field. Ungated (not tied to a pending entry): it must fire on every close.
    // NOTE: MenuState::IsAnyMenuOpen() is NOT usable as an `o` gate -- DAT_0208ebc0 is never nulled
    // (it holds the last-focused window forever) so it reads "open" in the field too.
    if (cat == WND_CAT_CLOSE) TextCapture::NotifyFocusChanged();

    void* releaseOwner = nullptr; uint32_t rowOff = 0; int idx = -1;
    bool logSeen = false; uint64_t waited = 0;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_panePendingOwner != window) return ret;   // only the pane whose entry we stashed
        waited  = GetTickCount64() - g_panePendingArmMs;
        logSeen = MarkPaneSeen(cat);
        if (cat == WND_CAT_SHOW) {
            // Consume one-shot under the lock (the sanctioned per-frame guard -- 0x13 may repeat).
            releaseOwner = g_panePendingOwner;
            rowOff = g_panePendingRowOff; idx = g_panePendingIndex;
            g_panePendingOwner = nullptr; g_panePendingRowOff = 0; g_panePendingIndex = -1;
        }
    }
    if (logSeen) {
        char m[128];
        snprintf(m, sizeof(m), "wnd-first: cat=0x%X msg=0x%llX +%llums",
                 cat, (unsigned long long)msg, (unsigned long long)waited);
        Log::Write("INGAME", m);
    }
    if (releaseOwner) {
        char m[80];
        snprintf(m, sizeof(m), "pane entry announce: shown after %llums", (unsigned long long)waited);
        Log::Write("INGAME", m);
        // Outside the lock. Speaks only if the row resolves -- if it never does, stay silent (no
        // fallback), exactly like the battle menu only replays on a successful draw.
        IngameMenuReader::OnRowChainFocus(releaseOwner, rowOff, idx);
    }
    return ret;
}

// The panel's per-row draw callback (*( *(panel+0x1510) + 0x120 )) — identifies the list type.
void* BattleDrawCallback(void* panel) {
    if (!panel) return nullptr;
    __try {
        void* lw = *reinterpret_cast<void* const*>(reinterpret_cast<char*>(panel) + OFF_LISTWIDGET);
        if (!lw) return nullptr;
        return *reinterpret_cast<void* const*>(reinterpret_cast<char*>(lw) + OFF_DRAW_CB);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Def-table name codec (commands / magicks / technicks): FUN_0035d330(cat,id) -> record;
// record+0x18 -> Resolve58b0 -> name codec. The game call runs on our (game) thread. SEH-guarded.
// Delegates to BattleState::DefName -- the SAME chain, hoisted there in S87 so the combat log can
// use it too (AbilityName was walking a different, broken one). Returns the decoded string rather
// than a codec pointer; callers below adapt.
std::wstring ResolveDefNameText(uint32_t cat, uint32_t cmdId) {
    return BattleState::DefName(cat, cmdId);
}

// Item name codec: FUN_00272cb0(id) returns it directly (CONFIRMED working). Centralized in
// core/item_names.cpp, because the field loot scanner needs the same lookup for ground drops.
const uint8_t* ResolveItemName(uint32_t itemId) { return ItemNames::ResolveCodec(itemId); }

// Resolve the highlighted command's name by the panel's list type (draw callback). Top-level uses
// the memory-only cache (no game call); sub-lists resolve on focus. Empty if unknown/unresolved.
std::wstring BattleCommandName(void* panel, int index, int cmdId) {
    void* cb = BattleDrawCallback(panel);
    if (cb == Hooks::ResolveRva(RVA_DRAW_TOPCMD)) {           // top-level command list (CONFIRMED)
        if (cmdId < 0 || cmdId >= 256) return std::wstring();
        std::lock_guard<std::mutex> lk(g_mutex);
        return g_bcmdName[cmdId];
    }
    if (cb == Hooks::ResolveRva(RVA_DRAW_CHOOSER)) {         // Magicks/Technicks category chooser
        uint32_t cat = (ReadBcmdRowFlag(panel, index) & 4) ? CAT_CHOOSER_TECH : CAT_CHOOSER_MAG;
        return ResolveDefNameText(cat, static_cast<uint32_t>(cmdId));
    }
    if (cb == Hooks::ResolveRva(RVA_DRAW_MAGICK)) {          // spell / technick list
        return ResolveDefNameText(CAT_MAGICK, static_cast<uint32_t>(cmdId));
    }
    if (cb == Hooks::ResolveRva(RVA_DRAW_ITEM)) {            // item sublist (CONFIRMED)
        const uint8_t* codec = ResolveItemName(cmdId);
        if (!codec) return std::wstring();
        std::wstring text = GameText::Decode(codec, 256);
        return GameText::IsMostlyPrintable(text) ? text : std::wstring();
    }
    return std::wstring();                                   // unmapped list type — stay silent
}


// Resolve and speak the highlighted battle command. Returns FALSE when the name is not resolvable
// yet -- on menu OPEN that is the normal case, not an error: the 0x8000 arrives before FUN_00276be0
// has drawn any row, so nothing is cached to look up.
// The character whose command window is currently up. Empty when it cannot be resolved -- the caller
// then says nothing about it, which is the correct answer rather than a guess.
std::wstring CurrentBattleCharName() {
    void* ctrl = nullptr;
    { std::lock_guard<std::mutex> lk(g_mutex); ctrl = g_bcmdCtrl; }
    if (!ctrl) return std::wstring();
    void* parent = MemRead::PtrAt(ctrl, OFF_CTRL_PARENT);
    if (!parent) return std::wstring();
    uint32_t handle = 0;
    if (!MemRead::SafeReadU32(parent, OFF_CUR_CHAR, &handle) || handle == 0) return std::wstring();
    return BattleState::NameForActor(
        BattleState::ActorForHandle(static_cast<int32_t>(handle)));
}

// Announce who is acting, and make whatever speaks next queue behind it.
void SpeakBattleCharName(const char* why) {
    std::wstring nm = CurrentBattleCharName();
    if (nm.empty()) return;                       // silence beats a guess at who is about to act
    Log::WriteW("INGAME", why, nm);
    Speech::Output(nm, /*interrupt=*/true);
    std::lock_guard<std::mutex> lk(g_mutex);
    g_bcmdQueueNext = true;
}

// The acting character's scene handle, straight off the panel (panel+0x4C8 == list+0x08, the field
// the game's own confirm handler uses). 0 when unreadable.
uint32_t ReadBcmdCharHandle(void* panel) {
    uint32_t h = 0;
    if (!panel || !MemRead::SafeReadU32(panel, OFF_BCMD_CHAR, &h)) return 0;
    return h;
}

// ": on" / ": off" for the Gambits row, empty for everything else. Empty is also the answer when the
// state cannot be read or the row is greyed out -- a blind player is better served by the bare name
// than by a state that might be wrong. The words are mod-emitted (the game draws an icon frame, not
// text); see speech/phrasebook.h.
std::wstring GambitStateSuffix(void* panel, int index, int cmdId) {
    if (cmdId != BCMD_ID_GAMBIT) return std::wstring();
    // Top-level list only: the same draw-callback identity BattleCommandName gates on.
    if (BattleDrawCallback(panel) != Hooks::ResolveRva(RVA_DRAW_TOPCMD)) return std::wstring();
    if (ReadBcmdRowFlag(panel, index) & BCMD_FLAG_DISABLED) return std::wstring();

    bool resolved = false;
    const bool on = BattleState::GambitsEnabled(ReadBcmdCharHandle(panel), &resolved);
    if (!resolved) return std::wstring();
    return std::wstring(L": ") + Phrase::Get(on ? Phrase::Id::On : Phrase::Id::Off);
}

// Name the surface ONCE per distinct (draw callback, list kind, cost gate) triple, so a log from
// any play pass says which list was on screen and which branch below owned it. Log-only volume
// control, and the reason it exists: the draw callback ALONE said "items" and "magicks" were the
// same surface, and one line of this would have caught the wrong label before it shipped.
void LogSecondColumnSurface(void* cb, uint32_t listKind, uint32_t costKind) {
    struct Seen { void* cb; uint32_t list; uint32_t cost; };
    static Seen s_seen[12] = {};
    static int  s_n = 0;
    for (int i = 0; i < s_n; ++i)
        if (s_seen[i].cb == cb && s_seen[i].list == listKind && s_seen[i].cost == costKind) return;
    if (s_n >= 12) return;
    s_seen[s_n++] = Seen{cb, listKind, costKind};
    const uintptr_t base = reinterpret_cast<uintptr_t>(Hooks::ResolveRva(0));
    const uintptr_t c    = reinterpret_cast<uintptr_t>(cb);
    char m[160];
    const char* means = (costKind == 0 || costKind == COSTKIND_MIST) ? "nothing"
                      : (costKind == COSTKIND_ITEMS)                 ? "owned count"
                                                                     : "MP cost";
    snprintf(m, sizeof(m), "second column: draw RVA=0x%llX listKind=0x%X costGate=%u -> %s",
             static_cast<unsigned long long>(c >= base ? c - base : c),
             listKind, costKind, means);
    Log::Write("INGAME", m);
}

// The number the game draws to the RIGHT of a row: " 31" for an item, ", MP 6" for an ability.
//
// ONE reader for both, because the game draws ONE column. Only the label forks, on the gate word the
// builder set beside it -- see OFF_BCMD_COST above for the two discriminators that looked right and
// were not. Empty (SILENT) whenever the game itself shows no number there.
std::wstring SecondColumnSuffix(void* panel, int index, void* cb) {
    uint32_t costKind = 0, listKind = 0;
    if (!MemRead::SafeReadU32(panel, OFF_BCMD_COSTKIND, &costKind)) return std::wstring();
    MemRead::SafeReadU32(panel, OFF_BCMD_LISTKIND, &listKind);     // for the log line only
    LogSecondColumnSurface(cb, listKind, costKind);

    // The game's own display test, verbatim (FUN_0027ce70:61) -- NOT "is the number non-zero", which
    // would read a Technick's stale word and a Quickening's mist charges as a figure.
    if ((costKind & ~COSTKIND_MIST) == 0) return std::wstring();

    uint16_t word = 0;
    if (!ReadBcmdCost(panel, index, &word) || word == 0) return std::wstring();
    if (word > MAX_SECOND_COLUMN) {
        // The column is showing something that is not a small number. Report it rather than putting
        // it in the player's ear -- this line is the falsifier for the offsets above, exactly like
        // the macro writer's first-write log.
        char m[128];
        snprintf(m, sizeof(m), "second column out of range: listKind=0x%X costGate=%u raw=%u -- not spoken",
                 listKind, costKind, static_cast<unsigned>(word));
        Log::Write("INGAME", m);
        return std::wstring();
    }

    if (costKind == COSTKIND_ITEMS) {
        // ITEM list -- "Potion 31", word for word what the field item list says, on the tester's
        // instruction. Above 1 only, which is that list's rule too: a row exists only because you
        // own at least one, so a bare name already means exactly one.
        if (word <= 1) return std::wstring();
        return L" " + std::to_wstring(word);
    }

    // ABILITY list. The label is the mod's existing "MP " gauge word, so the status screen and this
    // row read alike.
    return std::wstring(L", ") + Phrase::Get(Phrase::Id::MPPrefix) + std::to_wstring(word);
}

// The extra thing the game draws to the RIGHT of a row's name, dispatched on the same draw-callback
// identity BattleCommandName resolves the NAME with. One function so the cases cannot drift apart,
// and so an unmapped list type stays silent by construction rather than by omission.
//
// FUN_0027ce70 covers the item list as well as the ability lists -- see OFF_BCMD_COST. There is
// deliberately NO branch for FUN_0027e530: it draws its count from its own FUN_00272c80(entry) call
// rather than from +0x512, and no play pass has ever landed on it, so reading it would be shipping
// an unverified number. Silence on a surface we have never seen is the correct answer.
std::wstring RowDetailSuffix(void* panel, int index, int cmdId) {
    void* cb = BattleDrawCallback(panel);
    if (cb == Hooks::ResolveRva(RVA_DRAW_TOPCMD)) return GambitStateSuffix(panel, index, cmdId);
    if (cb == Hooks::ResolveRva(RVA_DRAW_MAGICK)) return SecondColumnSuffix(panel, index, cb);
    return std::wstring();
}

bool TrySpeakBattleCommand(void* panel, int index) {
    int cmdId = ReadBcmdCmdId(panel, index);
    if (cmdId < 0) return false;
    std::wstring text = BattleCommandName(panel, index, cmdId);   // resolves by list type; locks internally
    if (text.empty()) return false;
    // "Gambits: on" / "Potion 32" / "Cure, MP 6" -- the NAME is still the game's own text.
    text += RowDetailSuffix(panel, index, cmdId);

    // THE MENU JUST BECAME ACTIVE -> say whose it is first. The tester asked for this explicitly:
    // the command highlight is meaningless until you know which character is about to obey it.
    bool needName;
    { std::lock_guard<std::mutex> lk(g_mutex); needName = g_bcmdNeedName; g_bcmdNeedName = false; }
    if (needName) SpeakBattleCharName("battle menu active, character:");

    // Queue rather than interrupt when a name was just spoken, so the two land in order instead of
    // the command cutting off the name the player needs to hear.
    bool queue;
    { std::lock_guard<std::mutex> lk(g_mutex); queue = g_bcmdQueueNext; g_bcmdQueueNext = false; }

    Log::WriteW("INGAME", "command:", reinterpret_cast<void*>(static_cast<uintptr_t>(cmdId)), text);
    if (queue) Speech::SpeakQueued(text);
    else       Speech::Output(text, /*interrupt=*/true);
    return true;
}

// FUN_0027c3d0(outSel, list, row): the battle menu's CONFIRM handler. Hooked for exactly one case --
// `case 0xd`, the Gambits toggle -- so the player hears the new state the moment they flip it. The
// tester asked for just the word here ("on" / "off"), not the whole row again.
//
// Runs AFTER the original and RE-READS the flag rather than decoding the return code (0x17 = now on,
// 0x19 = now off). Both would work under the same gate, but reading the value the game just wrote is
// the measurement; the return code is an inference about it. Gate first, so this costs two compares
// on every other confirm in the game.
uint32_t HookedBcmdConfirm(int16_t* outSel, void* list, int row) {
    const uint32_t ret = s_origBcmdConfirm ? s_origBcmdConfirm(outSel, list, row) : 0;
    STALL_SCOPE("IngameMenuReader::HookedBcmdConfirm");
    if (!list || row < 0) return ret;

    uint32_t kind = 0;
    if (!MemRead::SafeReadU32(list, OFF_LIST_KIND, &kind)) return ret;
    if (kind != 6 && kind != 9) return ret;              // not the top-level command list
    int count = 0;
    if (!MemRead::SafeReadInt(reinterpret_cast<char*>(list) + OFF_LIST_CNT, &count)) return ret;
    if (count <= 0 || count > 64 || row >= count) return ret;
    uint16_t cmdId = 0;
    if (!MemRead::SafeReadU16(list, OFF_LIST_IDS + static_cast<uint32_t>(row) * BCMD_STRIDE, &cmdId)) return ret;
    if (cmdId != BCMD_ID_GAMBIT) return ret;

    uint32_t handle = 0;
    if (!MemRead::SafeReadU32(list, OFF_LIST_CHAR, &handle)) return ret;
    bool resolved = false;
    const bool on = BattleState::GambitsEnabled(handle, &resolved);
    if (!resolved) return ret;                            // silence beats guessing at the new state

    const wchar_t* word = Phrase::Get(on ? Phrase::Id::On : Phrase::Id::Off);
    Log::WriteW("INGAME", "gambits toggled:", word);
    Speech::Output(word, /*interrupt=*/true);
    return ret;
}

// FUN_002778c0(ctrl, msgStruct): the battle command menu controller.
// msgStruct: +0x00 u32 message id, +0x08 i64 arg0.
uint64_t HookedBcmdCtrl(void* ctrl, void* msg) {
    STALL_SCOPE("IngameMenuReader::HookedBcmdCtrl");
    int msgId = -1;
    if (ctrl && msg && MemRead::SafeReadInt(msg, &msgId)) {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_bcmdCtrl = ctrl;
        // Construct: arm the name announce for the first focus, so it is spoken as the menu appears
        // rather than before it exists.
        if (msgId == MSG_CTRL_BUILD) { g_bcmdNeedName = true; g_bcmdQueueNext = false; }
    }

    const uint64_t r = s_origBcmdCtrl ? s_origBcmdCtrl(ctrl, msg) : 0;

    // AFTER the original: case 0x23 is where parent+0x2FE0 is written, so the new character only
    // exists on the way out.
    if (msgId == MSG_CTRL_SWITCH) SpeakBattleCharName("battle char switch:");
    return r;
}

void SpeakBattleCommand(void* panel, int index) { TrySpeakBattleCommand(panel, index); }

} // namespace

namespace IngameMenuReader {

bool Init() {
    bool ok = Hooks::InstallTyped(RVA_FIELD_PANE_WND, &HookedFieldPaneWnd, &s_origFieldPaneWnd);
    ok     &= Hooks::InstallTyped(RVA_BCMD_DRAW,     &HookedBcmdDraw,    &s_origBcmdDraw);
    ok     &= Hooks::InstallTyped(RVA_BCMD_CTRL,     &HookedBcmdCtrl,    &s_origBcmdCtrl);
    ok     &= Hooks::InstallTyped(RVA_BCMD_CONFIRM,  &HookedBcmdConfirm, &s_origBcmdConfirm);
    // The character-chooser hook moved to char_select_reader.cpp (Session 93) -- see that file's
    // header for why one reader owns Party/Status/Equipment/Gambits together.
    Log::Write("INGAME", ok ? "IngameMenuReader: field-pane show + battle command-draw + battle char "
                              "switch hooks installed"
                            : "IngameMenuReader: a field/battle hook FAILED to install");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_BCMD_CONFIRM);
    Hooks::Uninstall(RVA_BCMD_CTRL);
    Hooks::Uninstall(RVA_BCMD_DRAW);
    Hooks::Uninstall(RVA_FIELD_PANE_WND);
    std::lock_guard<std::mutex> lk(g_mutex);
    g_bcmdPendingPanel = nullptr;
    g_bcmdPendingIndex = -1;
    g_bcmdCtrl = nullptr;
    g_bcmdNeedName = false;
    g_bcmdQueueNext = false;
}

uint32_t RowChainOff(void* owner) {
    if (!owner) return 0;
    void* cls = Obj0(owner);
    for (const auto& rc : ROW_CHAIN)
        if (cls == Hooks::ResolveRva(rc.rva)) return rc.rowOff;
    return 0;
}

void OnRowChainFocus(void* owner, uint32_t rowOff, int index) {
    STALL_SCOPE("IngameMenu::OnRowChainFocus");
    const uint8_t* codec = ReadRowName(owner, rowOff, index);
    if (!codec) return;
    std::wstring text = GameText::Decode(codec, 256);   // SEH-guarded inside GameText
    if (!GameText::IsMostlyPrintable(text)) return;
    SpeakRow(owner, text, "menu:");
}

// True if `owner` is the battle command panel (window class FUN_0027ad70).
bool IsBattleCommandOwner(void* owner) {
    return owner && Obj0(owner) == Hooks::ResolveRva(RVA_BCMD_PANEL);
}

// THE LIVE-SURFACE FLAG. See the header for why `o` needs this and `IsBattleCommandOwner` will not
// do: a hotkey has no owner pointer to ask about, only "where am I".
//
// Stored as a plain pointer and RE-VALIDATED against the window class on every read, so a panel that
// has been freed or handed to another class stops answering true on its own. That liveness check is
// the load-bearing half -- the explicit clears below are belt and braces.
std::atomic<void*> g_bcmdLivePanel{nullptr};

bool BattleCommandActive() {
    void* p = g_bcmdLivePanel.load(std::memory_order_relaxed);
    return IsBattleCommandOwner(p);
}

void ClearBattleCommandActive() {
    g_bcmdLivePanel.store(nullptr, std::memory_order_relaxed);
}

// True if `owner` is the field pause menu's command column (window class FUN_00280de0). This is the
// ONE class whose entry announce is deferred to the SHOW message; every other pane speaks on entry.
bool IsFieldPaneOwner(void* owner) {
    return owner && Obj0(owner) == Hooks::ResolveRva(RVA_FIELD_PANE_WND);
}

// Stash the field pane's entry focus, released by HookedFieldPaneWnd on cat 0x13. Mirrors the battle
// menu's g_bcmdPending* handoff. Overwrites any un-released prior arm (each open is a fresh pane
// object, so a lingering one can never match a new window's pointer anyway) and resets the per-open
// first-seen category set.
void ArmPaneEntry(void* owner, uint32_t rowOff, int index) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_panePendingOwner  = owner;
    g_panePendingRowOff = rowOff;
    g_panePendingIndex  = index;
    g_panePendingArmMs  = GetTickCount64();
    g_paneSeenCount     = 0;
}

// Battle command focus (0x8000): read the highlighted command id from the panel (owner+0x510+
// index*8) and speak its name — cached by the FUN_00276be0 draw hook (the game's own localized text).
//
// NO dedup. This used to compare against the last spoken text, which made backing out of the Attack
// list and reopening it SILENT (same first command, same string). 0x8000 is event-driven, so every
// fire is a real highlight the player must hear.
//
// INITIAL FOCUS ON ENTRY. When the menu opens, this fires before any row has been drawn, so the
// name cache is empty and the highlighted command resolves to nothing -- the menu used to open
// silently and only start speaking on the first cursor MOVE. The field pause menu gets its entry
// announce from the FUN_00244830 pane replay; the battle menu is a separate system with no such
// path, so an unresolved focus is stashed here and replayed by the first FUN_00276be0 draw that
// caches a name.
void OnBattleCommandFocus(void* owner, int index) {
    g_bcmdLivePanel.store(owner, std::memory_order_relaxed);
    if (TrySpeakBattleCommand(owner, index)) {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_bcmdPendingPanel = nullptr; g_bcmdPendingIndex = -1;   // spoken -> drop any stale pending
        return;
    }
    std::lock_guard<std::mutex> lk(g_mutex);
    g_bcmdPendingPanel = owner;
    g_bcmdPendingIndex = index;
}

} // namespace IngameMenuReader

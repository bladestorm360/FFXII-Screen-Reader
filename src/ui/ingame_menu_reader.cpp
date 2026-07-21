#include "ui/ingame_menu_reader.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "speech/speech.h"
#include "core/logger.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
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
    { 0x445E00, 0xC8 },   // FUN_00565e00 — gambits
    { 0x44F810, 0xE0 },   // FUN_0056f810 — sub-panel list
    { 0x45B890, 0xD0 },   // FUN_0057b890 — equip-type screen list
};
constexpr uint32_t ROW_STRIDE   = 0x20;   // row record size
constexpr uint32_t OFF_ROW_NAME = 0x10;   // NAME codec* (built by FUN_002cd3c0)

// ---- Battle command menu (CONFIRMED 2026-07-10 via probe) ------------------------------------
// The in-battle command list (Attack / Magicks & Technicks / Items / ...) routes cursor moves
// through the SAME FUN_00247510 msg-0x8000 dispatch the field menu uses. Its `owner` is the command
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
constexpr uint32_t OFF_DEF_CODEC   = 0x18;     // FUN_0035d330 record +0x18 = codec source
constexpr uint32_t OFF_BCMD_FLAG   = 0x513;    // panel+0x513 + row*8 = per-row flag byte (bit2 in chooser)
constexpr uint32_t RVA_DRAW_TOPCMD = 0x156BE0; // FUN_00276be0 -- SAME function as RVA_BCMD_DRAW
                                              // above; two names on purpose, one is the hook target,
                                              // the other the draw-callback identity we compare against.
constexpr uint32_t RVA_DRAW_CHOOSER= 0x15D240; // FUN_0027d240 (Magicks/Technicks category chooser)
constexpr uint32_t RVA_DRAW_MAGICK = 0x15CE70; // FUN_0027ce70 (spell/technick list, cat 0x14)
constexpr uint32_t RVA_DRAW_ITEM   = 0x15E530; // FUN_0027e530 (items) — CONFIRMED working
constexpr uint32_t RVA_RESOLVE_DEF = 0x23D330; // FUN_0035d330(cat, id) -> &record
constexpr uint32_t RVA_RESOLVE_ITEM= 0x152CB0; // FUN_00272cb0(id) -> item name codec — CONFIRMED
constexpr uint32_t CAT_MAGICK      = 0x14;     // FUN_0035d330 category for the spell/technick list
constexpr uint32_t CAT_CHOOSER_TECH= 0x18;     // chooser category when flag bit2 set (Technicks)
constexpr uint32_t CAT_CHOOSER_MAG = 0x15;     // chooser category otherwise (Magick schools)

typedef const uint8_t* (*Pfn_ResolveDef)(uint32_t, uint32_t);  // FUN_0035d330(cat, id)
typedef const uint8_t* (*Pfn_ResolveItem)(uint32_t);           // FUN_00272cb0(id)

// Battle target-selection readout lives in battle_target_reader.cpp now (hooks the vitals builder
// FUN_00329220 + the current-target index ctx+0xde0). The old reticle hook (FUN_005528c0) was
// removed: probing proved it never fires for normal Foes/Party/Allies selection (it is the
// free-aim/area mode only), which is why targeting was silent.

// ---- Status screen party-member chooser (FIELD menu; the shared "Select a character" grid) -----
// ⚠️ DEFERRED / NOT WORKING YET (Session 31, 2026-07-11). This reader is SILENT and does not ship a
// usable feature — do not treat it as done. Two issues, both decompile-confirmed:
//   1. FUN_00285a10 (the chooser cursor-set) does NOT fire for the highlight on menu ENTRY. Trace of
//      FUN_00285290 case 1: it calls FUN_002858a0(1), then FUN_00285a10(0xffffffff) (CLEARS — we bail on
//      negative slot), then sets ctrl+0x117=0 (highlights slot 0) by a DIRECT WRITE, not via FUN_00285a10.
//      FUN_00285a10 only gets a valid slot from the nav FUN_00285190 on an actual d-pad MOVE to another
//      valid portrait; the prologue tutorial party is one character (Reks) so there is nothing to move to
//      and it never fires. => needs the broader "speak initial focus on menu entry" work + a different
//      hook event (likely read ctrl+0x117 after entry / on the controller's own event). Revisit post-tutorial.
//   2. RVA_PAUSE_CTX below was miscalculated (0xE9AC30); corrected to 0x1F7AC30 (DAT_0209ac30 abs 0x209AC30
//      − 0x120000; add-back 0x1F7AC30+0x120000=0x209AC30 ✓; sibling DAT_0209be80→0x1F7BE80). Latent — it
//      would fault the read chain, but issue #1 means the hook never fires, so this alone changes nothing.
// The read design below (once a firing event is found) resolves the highlighted character exactly as the
// game's own portrait draw FUN_00283e40 does: ctx = *DAT_0209ac30; controller = *(ctx+0xf8);
// portrait = *(controller+0xc0 + slot*8); block = *(ctx+0xac8 + *(int)(portrait+0xc0)*8); fields are plain
// loads off `block`; name via FUN_0035d330(2,charId). Labels (LEVEL/HP/MAX/MP/MAX) still to be sourced.
constexpr uint32_t RVA_STATUS_CURSOR   = 0x165A10; // FUN_00285a10(slot) — chooser cursor-set (per highlight)
constexpr uint32_t RVA_PAUSE_CTX       = 0x1F7AC30; // DAT_0209ac30 (ptr) — pause-menu context (was 0xE9AC30, wrong)
constexpr uint32_t OFF_CTX_CTRL        = 0xF8;     // ctx+0xf8 = active chooser controller (FUN_00285290)
constexpr uint32_t OFF_CTX_BLOCKS      = 0xAC8;    // ctx+0xac8 + blockIdx*8 = per-character HUD block ptr
constexpr uint32_t OFF_CTRL_PORTRAITS  = 0xC0;     // controller+0xc0 + slot*8 = portrait child ptr
constexpr uint32_t OFF_PORTRAIT_BLKIDX = 0xC0;     // portrait+0xc0 = index into ctx+0xac8 (int)
constexpr uint32_t OFF_BLK_CHARID      = 0x60;     // block+0x60 = char id (i16; < 0 = empty slot)
constexpr uint32_t OFF_BLK_CURHP       = 0x20;     // block+0x20 = current HP (i32)  [FUN_00283e40 puVar3[8]]
constexpr uint32_t OFF_BLK_MAXHP       = 0x24;     // block+0x24 = max HP (i32)      [puVar3[9]]
constexpr uint32_t OFF_BLK_CURMP       = 0x2C;     // block+0x2c = current MP (i32)  [puVar3[0xb]]
constexpr uint32_t OFF_BLK_MAXMP       = 0x30;     // block+0x30 = max MP (i32)      [puVar3[0xc]]
constexpr uint32_t OFF_BLK_LEVEL       = 0xBA;     // block+0xba = level (u8)
constexpr uint32_t CAT_CHARNAME        = 2;        // FUN_0035d330 category for character names

typedef void (*Pfn_StatusCursor)(int);
Pfn_StatusCursor s_origStatusCursor = nullptr;

std::mutex   g_mutex;
std::wstring g_bcmdName[256];              // top-level cmdId -> decoded name (cached from FUN_00276be0)
// Initial-focus replay for the battle command menu. When the menu OPENS, its 0x8000 fires before
// FUN_00276be0 has drawn (and therefore cached) any command name, so the highlighted command
// resolves to nothing and the entry is silent -- the field pause menu gets its entry announce from
// the FUN_00244830 pane replay, but the battle menu is a separate system with no such path. We
// stash the unresolved focus here and let the first successful draw replay it.
void* g_bcmdPendingPanel = nullptr;
int   g_bcmdPendingIndex = -1;

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

// Replicate FUN_002b58b0(src, 0): a 2-byte-marker-prefixed codec block; index 0 -> src+2 if it
// starts with the 0x0000 marker, else src. Memory-only, SEH.
const uint8_t* Resolve58b0(const uint8_t* src) {
    if (!src) return nullptr;
    __try {
        if (src[0] == 0 && src[1] == 0) return src + 2;
        return src;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
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
const uint8_t* ResolveDefName(uint32_t cat, uint32_t cmdId) {
    auto fn = reinterpret_cast<Pfn_ResolveDef>(Hooks::ResolveRva(RVA_RESOLVE_DEF));
    if (!fn) return nullptr;
    __try {
        const uint8_t* rec = fn(cat, cmdId);
        if (!rec) return nullptr;
        return Resolve58b0(*reinterpret_cast<const uint8_t* const*>(rec + OFF_DEF_CODEC));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Item name codec: FUN_00272cb0(id) returns it directly (CONFIRMED working). SEH-guarded.
const uint8_t* ResolveItemName(uint32_t itemId) {
    auto fn = reinterpret_cast<Pfn_ResolveItem>(Hooks::ResolveRva(RVA_RESOLVE_ITEM));
    if (!fn) return nullptr;
    __try { return fn(itemId); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Resolve the highlighted command's name by the panel's list type (draw callback). Top-level uses
// the memory-only cache (no game call); sub-lists resolve on focus. Empty if unknown/unresolved.
std::wstring BattleCommandName(void* panel, int index, int cmdId) {
    void* cb = BattleDrawCallback(panel);
    if (cb == Hooks::ResolveRva(RVA_DRAW_TOPCMD)) {           // top-level command list (CONFIRMED)
        if (cmdId < 0 || cmdId >= 256) return std::wstring();
        std::lock_guard<std::mutex> lk(g_mutex);
        return g_bcmdName[cmdId];
    }
    const uint8_t* codec = nullptr;
    if (cb == Hooks::ResolveRva(RVA_DRAW_ITEM)) {            // item sublist (CONFIRMED)
        codec = ResolveItemName(cmdId);
    } else if (cb == Hooks::ResolveRva(RVA_DRAW_CHOOSER)) {  // Magicks/Technicks category chooser
        uint32_t cat = (ReadBcmdRowFlag(panel, index) & 4) ? CAT_CHOOSER_TECH : CAT_CHOOSER_MAG;
        codec = ResolveDefName(cat, cmdId);
    } else if (cb == Hooks::ResolveRva(RVA_DRAW_MAGICK)) {   // spell / technick list
        codec = ResolveDefName(CAT_MAGICK, cmdId);
    } else {
        return std::wstring();                              // unmapped list type — stay silent
    }
    if (!codec) return std::wstring();
    std::wstring text = GameText::Decode(codec, 256);
    return GameText::IsMostlyPrintable(text) ? text : std::wstring();
}

// Read the highlighted Status-chooser slot's vitals + the active controller (log tag only). Returns
// false on empty slot / fault. POD-only under __try (decode happens outside). Mirrors FUN_00283e40.
struct StatusVitals { int charId; int curHP; int maxHP; int curMP; int maxMP; int level; };
bool ReadStatusSlot(int slot, StatusVitals* out, void** outCtrl) {
    *outCtrl = nullptr;
    if (slot < 0) return false;
    __try {
        void* ctx = *reinterpret_cast<void* const*>(Hooks::ResolveRva(RVA_PAUSE_CTX));
        if (!ctx) return false;
        char* c = reinterpret_cast<char*>(ctx);
        void* ctrl = *reinterpret_cast<void* const*>(c + OFF_CTX_CTRL);
        *outCtrl = ctrl;
        if (!ctrl) return false;
        void* portrait = *reinterpret_cast<void* const*>(
            reinterpret_cast<char*>(ctrl) + OFF_CTRL_PORTRAITS + static_cast<size_t>(slot) * 8);
        if (!portrait) return false;
        int blkIdx = *reinterpret_cast<int*>(reinterpret_cast<char*>(portrait) + OFF_PORTRAIT_BLKIDX);
        if (blkIdx < 0) return false;
        void* block = *reinterpret_cast<void* const*>(
            c + OFF_CTX_BLOCKS + static_cast<size_t>(blkIdx) * 8);
        if (!block) return false;
        char* b = reinterpret_cast<char*>(block);
        int charId = *reinterpret_cast<int16_t*>(b + OFF_BLK_CHARID);
        if (charId < 0) return false;                              // empty portrait slot
        out->charId = charId;
        out->curHP  = *reinterpret_cast<int32_t*>(b + OFF_BLK_CURHP);
        out->maxHP  = *reinterpret_cast<int32_t*>(b + OFF_BLK_MAXHP);
        out->curMP  = *reinterpret_cast<int32_t*>(b + OFF_BLK_CURMP);
        out->maxMP  = *reinterpret_cast<int32_t*>(b + OFF_BLK_MAXMP);
        out->level  = *reinterpret_cast<uint8_t*>(b + OFF_BLK_LEVEL);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// FUN_00285a10(slot): the Status chooser's cursor-set. Fires on each highlight (and on open) with
// `slot` = the highlighted portrait (< 0 = cleared). Speak name + Level + HP + MP.
//
// NO dedup. All 8 call sites in the decompile live in FUN_00284ec0 / FUN_00285190 / FUN_00285290 /
// FUN_00285b20 -- open, cursor-set and close handlers, none of them per-frame -- so every fire is a
// real highlight change. Re-opening the chooser on the same slot therefore re-announces, which is
// the point. If a single highlight ever produces TWO `status:` lines, two of those handlers are
// firing for one input: narrow the hook to the one that owns the event, do NOT re-add a filter.
//
// NOTE: "Level"/"HP"/"MP" are mod-emitted labels matching the on-screen columns (English for now).
void HookedStatusCursor(int slot) {
    if (s_origStatusCursor) s_origStatusCursor(slot);              // let the game set +0x114/+0x117 first
    if (slot < 0) return;

    StatusVitals v;
    void* ctrl = nullptr;                                          // used for the log tag only
    if (!ReadStatusSlot(slot, &v, &ctrl)) return;

    const uint8_t* codec = ResolveDefName(CAT_CHARNAME, static_cast<uint32_t>(v.charId));
    if (!codec) return;
    std::wstring name = GameText::Decode(codec, 256);
    if (!GameText::IsMostlyPrintable(name)) return;

    std::wstring line = name;
    line += L", Level " + std::to_wstring(v.level);
    line += L", HP " + std::to_wstring(v.curHP) + L"/" + std::to_wstring(v.maxHP);
    line += L", MP " + std::to_wstring(v.curMP) + L"/" + std::to_wstring(v.maxMP);
    Log::WriteW("INGAME", "status:", ctrl, line);
    Speech::Output(line, /*interrupt=*/true);
}

// Resolve and speak the highlighted battle command. Returns FALSE when the name is not resolvable
// yet -- on menu OPEN that is the normal case, not an error: the 0x8000 arrives before FUN_00276be0
// has drawn any row, so nothing is cached to look up.
bool TrySpeakBattleCommand(void* panel, int index) {
    int cmdId = ReadBcmdCmdId(panel, index);
    if (cmdId < 0) return false;
    std::wstring text = BattleCommandName(panel, index, cmdId);   // resolves by list type; locks internally
    if (text.empty()) return false;
    Log::WriteW("INGAME", "command:", reinterpret_cast<void*>(static_cast<uintptr_t>(cmdId)), text);
    Speech::Output(text, /*interrupt=*/true);
    return true;
}

void SpeakBattleCommand(void* panel, int index) { TrySpeakBattleCommand(panel, index); }

} // namespace

namespace IngameMenuReader {

bool Init() {
    bool ok = Hooks::InstallTyped(RVA_BCMD_DRAW,     &HookedBcmdDraw,    &s_origBcmdDraw);
    ok     &= Hooks::InstallTyped(RVA_STATUS_CURSOR, &HookedStatusCursor,&s_origStatusCursor);
    Log::Write("INGAME", ok ? "IngameMenuReader: battle command-draw + status-chooser hooks installed"
                            : "IngameMenuReader: a battle/status hook FAILED to install");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_STATUS_CURSOR);
    Hooks::Uninstall(RVA_BCMD_DRAW);
    std::lock_guard<std::mutex> lk(g_mutex);
    g_bcmdPendingPanel = nullptr;
    g_bcmdPendingIndex = -1;
}

uint32_t RowChainOff(void* owner) {
    if (!owner) return 0;
    void* cls = Obj0(owner);
    for (const auto& rc : ROW_CHAIN)
        if (cls == Hooks::ResolveRva(rc.rva)) return rc.rowOff;
    return 0;
}

void OnRowChainFocus(void* owner, uint32_t rowOff, int index) {
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

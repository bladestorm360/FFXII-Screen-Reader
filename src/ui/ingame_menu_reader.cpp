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

// ---- Battle command menu ---------------------------------------------------------------------
// The in-battle command list (Attack/Magicks/Technicks/Items/Gambits) does NOT route cursor moves
// through FUN_00247510. Its list FUN_002c59d0 calls FUN_00293110(0xe, cmdId) on each highlight, and
// its cell painter FUN_002c5900 resolves + STORES each command's name codec on the cell text widget.
// So we cache cmdId->codec from the painter (reading the game's own resolved text — no game call),
// and look it up on the highlight trigger.
constexpr uint32_t RVA_CMD_CELL_PAINT = 0x1A5900;  // FUN_002c5900(dataCtx, cellCtx, geom, cellIdx)
constexpr uint32_t RVA_CMD_HILITE     = 0x173110;  // FUN_00293110(kind, id)
constexpr uint32_t OFF_CMD_ENTRIES    = 0xC8;      // dataCtx + cellIdx*4 + 0xC8 = cmdId byte
constexpr uint64_t CMD_KIND_BATTLE    = 0xE;       // FUN_00293110 kind for the root command menu

typedef uint64_t (*Pfn_CmdCellPaint)(void*, void*, void*, int);
typedef void     (*Pfn_CmdHilite)(uint64_t, int);
Pfn_CmdCellPaint s_origCmdCellPaint = nullptr;
Pfn_CmdHilite    s_origCmdHilite    = nullptr;

// ---- Battle target selection (menu-style: after picking a command that needs a target) --------
// Each target node carries the pre-resolved name codec at node+0x48 (set at list build; mirrors
// Libra "????"). The hovered node follows the cursor at reticle+0x9f40. Pure memory read. Gate to
// menu-style selection (the target window exists AND mode != 3 = not a passive preview) so it never
// fires for the field/free-roam auto-target reticle (which never touches DAT_02ca8f38).
constexpr uint32_t RVA_RETICLE        = 0x4328C0;  // FUN_005528c0 (reticle child handler)
constexpr uint32_t RVA_TARGET_WIN     = 0x2B88F38; // DAT_02ca8f38 (ptr) — target-select window
constexpr uint32_t RVA_BATTLE_STATE   = 0x1F7BE80; // DAT_0209be80 (ptr) — battle state
constexpr uint32_t OFF_TARGET_MODE    = 0x10FA2;   // *(u8)(battleState+0x10fa2) = select mode (3=passive)
constexpr uint32_t OFF_RETICLE_NODE   = 0x9F40;    // *(reticle+0x9f40) = hovered target node
constexpr uint32_t OFF_NODE_NAME      = 0x48;      // *(node+0x48) = name codec ptr

typedef uint64_t (*Pfn_Reticle)(void*, void*);
Pfn_Reticle s_origReticle = nullptr;

std::mutex   g_mutex;
void*        g_lastOwner = nullptr;
std::wstring g_lastText;
const uint8_t* g_cmdCodec[256] = {};       // cmdId -> resolved name codec (from the cell painter)
int          g_lastCmdId = -1;             // dedup for the battle command highlight
void*        g_lastNode = nullptr;         // dedup for the target reticle (announce on hover change)

void LogLine(const char* tag, void* owner, const std::wstring& text) {
    char utf8[512] = {};
    if (!text.empty())
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    char line[640];
    snprintf(line, sizeof(line), "%s owner=%p \"%s\"", tag, owner, utf8);
    Log::Write("INGAME", line);
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

// From a battle-command cell paint: the cmdId (dataCtx+cellIdx*4+0xC8) and the codec the painter
// stored on the cell text widget ( *( *(cellCtx+0x10)+8 ) + 0x18 ). POD-only under __try.
bool ReadCmdCell(void* dataCtx, void* cellCtx, int cellIdx, int* outCmdId, const uint8_t** outCodec) {
    if (!dataCtx || !cellCtx || cellIdx < 0) return false;
    __try {
        int cmdId = *reinterpret_cast<char*>(
            reinterpret_cast<char*>(dataCtx) + static_cast<size_t>(cellIdx) * 4 + OFF_CMD_ENTRIES);
        void* p1 = *reinterpret_cast<void* const*>(reinterpret_cast<char*>(cellCtx) + 0x10);
        if (!p1) return false;
        void* p2 = *reinterpret_cast<void* const*>(reinterpret_cast<char*>(p1) + 8);
        if (!p2) return false;
        *outCmdId = cmdId & 0xff;
        *outCodec = *reinterpret_cast<const uint8_t* const*>(reinterpret_cast<char*>(p2) + 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Hovered target's name codec during MENU-STYLE selection, or null (not targeting / passive mode /
// no hover). Memory-only. Returns the node ptr via *outNode for dedup. POD-only under __try.
const uint8_t* ReadHoveredTargetName(void* reticle, void** outNode) {
    *outNode = nullptr;
    if (!reticle) return nullptr;
    __try {
        void* win = *reinterpret_cast<void* const*>(Hooks::ResolveRva(RVA_TARGET_WIN));
        if (!win) return nullptr;                                   // target selector not open
        void* bs = *reinterpret_cast<void* const*>(Hooks::ResolveRva(RVA_BATTLE_STATE));
        if (bs && *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(bs) + OFF_TARGET_MODE) == 3)
            return nullptr;                                         // passive preview, not player-driven
        void* node = *reinterpret_cast<void* const*>(reinterpret_cast<char*>(reticle) + OFF_RETICLE_NODE);
        if (!node) return nullptr;
        *outNode = node;
        return *reinterpret_cast<const uint8_t* const*>(reinterpret_cast<char*>(node) + OFF_NODE_NAME);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Speak once per (owner, text). Mirrors menu_reader's focus dedup.
void SpeakIfNew(void* owner, const std::wstring& text, const char* tag) {
    if (text.empty()) return;
    bool dup;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        dup = (owner == g_lastOwner && text == g_lastText);
        if (!dup) { g_lastOwner = owner; g_lastText = text; }
    }
    if (dup) return;
    LogLine(tag, owner, text);
    Speech::Output(text, /*interrupt=*/true);
}

// Cell painter for the battle command list — cache cmdId -> resolved name codec (memory-only read
// of the codec the game just stored on the cell).
uint64_t HookedCmdCellPaint(void* dataCtx, void* cellCtx, void* geom, int cellIdx) {
    uint64_t r = s_origCmdCellPaint ? s_origCmdCellPaint(dataCtx, cellCtx, geom, cellIdx) : 0;
    int cmdId = -1; const uint8_t* codec = nullptr;
    if (ReadCmdCell(dataCtx, cellCtx, cellIdx, &cmdId, &codec) && codec && cmdId >= 0) {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_cmdCodec[cmdId] = codec;
    }
    return r;
}

// Battle command highlight changed → speak the focused command's cached name. kind 0xe = the root
// command menu; (0,0) = cleared. Descriptions ride the FUN_00293170/FUN_00292b70 capture (`o` key).
void HookedCmdHilite(uint64_t kind, int id) {
    if (s_origCmdHilite) s_origCmdHilite(kind, id);
    if (kind != CMD_KIND_BATTLE || id == 0) return;
    const uint8_t* codec = nullptr;
    bool changed;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        changed = (id != g_lastCmdId);
        g_lastCmdId = id;
        codec = g_cmdCodec[id & 0xff];
    }
    if (!changed || !codec) return;
    std::wstring text = GameText::Decode(codec, 256);   // SEH-guarded inside GameText
    if (GameText::IsMostlyPrintable(text)) {
        LogLine("command:", reinterpret_cast<void*>(static_cast<uintptr_t>(id)), text);
        Speech::Output(text, /*interrupt=*/true);
    }
}

// Target reticle handler — on each call read the hovered target and, when it changes, speak its
// name (node+0x48). Gated to menu-style selection inside ReadHoveredTargetName.
uint64_t HookedReticle(void* reticle, void* msg) {
    uint64_t r = s_origReticle ? s_origReticle(reticle, msg) : 0;   // updates reticle+0x9f40 first
    void* node = nullptr;
    const uint8_t* codec = ReadHoveredTargetName(reticle, &node);
    bool changed;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        changed = (node != g_lastNode);
        g_lastNode = node;
    }
    if (node && changed && codec) {
        std::wstring text = GameText::Decode(codec, 256);
        if (GameText::IsMostlyPrintable(text)) {
            LogLine("target:", node, text);
            Speech::Output(text, /*interrupt=*/true);
        }
    }
    return r;
}

} // namespace

namespace IngameMenuReader {

bool Init() {
    bool ok = Hooks::InstallTyped(RVA_CMD_CELL_PAINT, &HookedCmdCellPaint, &s_origCmdCellPaint);
    ok     &= Hooks::InstallTyped(RVA_CMD_HILITE,     &HookedCmdHilite,    &s_origCmdHilite);
    ok     &= Hooks::InstallTyped(RVA_RETICLE,        &HookedReticle,      &s_origReticle);
    Log::Write("INGAME", ok ? "IngameMenuReader: battle command + target-reticle hooks installed"
                            : "IngameMenuReader: a battle/target hook FAILED to install");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_RETICLE);
    Hooks::Uninstall(RVA_CMD_HILITE);
    Hooks::Uninstall(RVA_CMD_CELL_PAINT);
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
    SpeakIfNew(owner, text, "menu:");
}

} // namespace IngameMenuReader

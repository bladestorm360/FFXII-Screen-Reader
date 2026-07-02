#include "ui/title_reader.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "speech/speech.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

// ---------------------------------------------------------------------------
// Title command-menu reader.
//
// Read path (derived offline, validated by the live dump 2026-07-02):
//   FUN_003939b0(window, packet) is the title window handler. A cursor move
//   arrives as a notify packet {u32 category@0 == 0xc, u64 msg@8 == 0x8000,
//   u64 val@0x10 == focus index}. On that, the focused row's LABEL is the atlas
//   cell it samples: cellTable[focusIndex] = {u16 x@0, u16 y@2, u32 (w:12|h:12|
//   pal:8)@4}; the y (÷70 = atlas row) identifies the baked label. We read the
//   ACTUAL focused cell (index-agnostic) — e.g. focus index 4 -> cell.y=350 ->
//   row 5 -> "Exit", NOT "Press Start". cellTable is obtained from the title's
//   per-row decorator FUN_00393950 (disp = *(*(param2+0x10)); cellTable = disp+0x20).
// ---------------------------------------------------------------------------

namespace {

// RVAs into FFXII_TZA.exe (abs = RVA + 0x120000). Validated live 2026-07-02.
constexpr uint32_t RVA_TITLE_HANDLER = 0x2739B0;  // FUN_003939b0(window, packet)
constexpr uint32_t RVA_ROW_DECORATE  = 0x273950;  // FUN_00393950(a, drawCtx, c, cellIndex)
constexpr uint32_t RVA_LOGO_HANDLER  = 0x274070;  // FUN_00394070(logoObj, packet)

// Notify-packet layout (built by FUN_00247510): category@0, msg@8, val@0x10.
constexpr uint32_t PKT_CAT = 0x00;
constexpr uint32_t PKT_MSG = 0x08;
constexpr uint32_t PKT_VAL = 0x10;
constexpr uint32_t CAT_INIT   = 0x1;    // window init (new menu session)
constexpr uint32_t CAT_NOTIFY = 0xc;    // input notify; msg@8 carries 0x8000/0x8001/...
constexpr uint64_t MSG_FOCUS  = 0x8000; // cursor moved to item N (val = index)
// Logo handler messages (its switch scrutinee is at packet+0).
constexpr uint32_t MSG_LOGO_READY   = 0x10;  // press-start prompt visible
constexpr uint32_t MSG_LOGO_DESTROY = 0x12;

// Struct offsets (all validated live).
constexpr uint32_t OFF_WINDOW_LIST    = 0xC8;  // window -> W_LIST (list widget)
constexpr uint32_t OFF_LIST_COUNT     = 0xE8;  // W_LIST -> u16 item count
constexpr uint32_t OFF_DRAWCTX_A      = 0x10;  // FUN_00393950 param_2 (+0x10) -> A; *A -> disp
constexpr uint32_t OFF_DISP_CELLTABLE = 0x20;  // disp -> cellTable ptr
constexpr uint32_t CELL_STRIDE        = 8;     // cellTable entry size
constexpr uint32_t OFF_CELL_Y         = 2;     // u16 texture-atlas y within a cell
constexpr int      ATLAS_ROW_PITCH    = 70;    // px per atlas row in title_logo.tm2

// Baked-sprite label catalog (title_logo.tm2 atlas rows, y ÷ 70). The game
// supplies NO text string for these — they are pre-rendered glyph images, so
// these labels are read from the game's own art (evidence-based, not invented)
// and hardcoded here as the documented last-resort for baked UI text.
const wchar_t* AtlasRowLabel(int row) {
    switch (row) {
        case 0: return L"New Game";
        case 1: return L"Load Game";
        case 2: return L"Trial Mode";
        case 3: return L"Credits";
        case 4: return L"Press Start";   // press-⊗-to-start prompt cell (not a selectable row)
        case 5: return L"Exit";
        default: return nullptr;
    }
}

// SEH-guarded reads — title menu objects can be destructed asynchronously.
template <typename T>
bool SafeRead(const void* p, T* out) {
    if (!p) return false;
    __try { *out = *reinterpret_cast<const T*>(p); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* PtrAt(void* base, uint32_t off) {
    if (!base) return nullptr;
    void* v = nullptr;
    return SafeRead(reinterpret_cast<char*>(base) + off, &v) ? v : nullptr;
}

std::atomic<void*> g_cellTable{nullptr};   // title list's shared cell node cell-table
std::atomic<int>   g_lastRow{-1};          // last atlas row spoken (edge-trigger)
std::atomic<bool>  g_pressStartSaid{false};
bool g_initialized = false;

typedef uintptr_t (*Pfn_TitleHandler)(void*, void*);
typedef int       (*Pfn_RowDecorate)(void*, void*, void*, int);
typedef uintptr_t (*Pfn_LogoHandler)(void*, void*);
Pfn_TitleHandler s_origTitle = nullptr;
Pfn_RowDecorate  s_origRow   = nullptr;
Pfn_LogoHandler  s_origLogo  = nullptr;

// Map focus index -> the focused cell's baked label and speak it (on change).
void OnTitleFocus(void* window, int index) {
    void* wlist = PtrAt(window, OFF_WINDOW_LIST);
    uint16_t count = 0;
    if (!wlist || !SafeRead(reinterpret_cast<char*>(wlist) + OFF_LIST_COUNT, &count)) return;
    if (index < 0 || index >= static_cast<int>(count)) return;  // drop OOB wrap-transients

    void* cellTable = g_cellTable.load();
    uint16_t y = 0;
    if (!cellTable ||
        !SafeRead(reinterpret_cast<char*>(cellTable) + index * CELL_STRIDE + OFF_CELL_Y, &y)) {
        return;
    }
    int row = (y + ATLAS_ROW_PITCH / 2) / ATLAS_ROW_PITCH;  // nearest atlas row
    const wchar_t* label = AtlasRowLabel(row);

    char dbg[144];
    if (!label) {
        snprintf(dbg, sizeof(dbg), "focus idx=%d y=%u row=%d -> (no catalog label)", index, y, row);
        Log::Write("TITLE", dbg);
        return;
    }
    if (g_lastRow.exchange(row) == row) return;  // state-change detection: speak only on change
    snprintf(dbg, sizeof(dbg), "focus idx=%d y=%u row=%d -> \"%ls\"", index, y, row, label);
    Log::Write("TITLE", dbg);
    Speech::Output(label, /*interrupt=*/true);
}

uintptr_t HookedTitle(void* window, void* packet) {
    uintptr_t ret = s_origTitle ? s_origTitle(window, packet) : 0;
    uint32_t cat = 0;
    if (SafeRead(reinterpret_cast<char*>(packet) + PKT_CAT, &cat)) {
        if (cat == CAT_NOTIFY) {
            uint64_t msg = 0, val = 0;
            if (SafeRead(reinterpret_cast<char*>(packet) + PKT_MSG, &msg) && msg == MSG_FOCUS &&
                SafeRead(reinterpret_cast<char*>(packet) + PKT_VAL, &val)) {
                OnTitleFocus(window, static_cast<int>(static_cast<int64_t>(val)));
            }
        } else if (cat == CAT_INIT) {
            g_lastRow.store(-1);  // new menu session -> re-announce the first focused option
        }
    }
    return ret;
}

int HookedRow(void* a, void* drawCtx, void* c, int cellIndex) {
    int ret = s_origRow ? s_origRow(a, drawCtx, c, cellIndex) : 0;
    // Cache the shared cell node's cell-table pointer (stable per menu instance;
    // this callback is title-specific, so it only fires while the title is drawn).
    void* dispHolder = PtrAt(drawCtx, OFF_DRAWCTX_A);
    void* disp = PtrAt(dispHolder, 0);
    void* cellTable = PtrAt(disp, OFF_DISP_CELLTABLE);
    if (cellTable) g_cellTable.store(cellTable);
    return ret;
}

uintptr_t HookedLogo(void* logoObj, void* packet) {
    uintptr_t ret = s_origLogo ? s_origLogo(logoObj, packet) : 0;
    uint32_t msg = 0;
    if (SafeRead(reinterpret_cast<char*>(packet) + PKT_CAT, &msg)) {
        if (msg == MSG_LOGO_READY) {
            if (!g_pressStartSaid.exchange(true)) {
                Speech::Output(L"Press Start", /*interrupt=*/true);
            }
        } else if (msg == MSG_LOGO_DESTROY) {
            g_pressStartSaid.store(false);
        }
    }
    return ret;
}

} // namespace

namespace TitleReader {

bool Init() {
    if (g_initialized) {
        Log::Write("TITLE", "TitleReader::Init called twice — ignoring");
        return true;
    }
    bool ok = true;
    ok &= Hooks::InstallTyped(RVA_ROW_DECORATE,  &HookedRow,   &s_origRow);
    ok &= Hooks::InstallTyped(RVA_TITLE_HANDLER, &HookedTitle, &s_origTitle);
    ok &= Hooks::InstallTyped(RVA_LOGO_HANDLER,  &HookedLogo,  &s_origLogo);
    g_initialized = true;
    Log::Write("TITLE", ok
        ? "TitleReader initialized (title menu: 0x8000 focus -> cellTable sprite-catalog labels)."
        : "TitleReader: one or more hooks failed to install — see Hooks log.");
    return ok;
}

void Shutdown() {
    if (!g_initialized) return;
    Hooks::Uninstall(RVA_TITLE_HANDLER);
    Hooks::Uninstall(RVA_ROW_DECORATE);
    Hooks::Uninstall(RVA_LOGO_HANDLER);
    g_cellTable.store(nullptr);
    g_initialized = false;
    Log::Write("TITLE", "TitleReader shut down");
}

} // namespace TitleReader

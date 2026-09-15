#include "ui/reward_panel_reader.h"
#include "ui/message_reader.h"
#include "battle/battle_state.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "speech/phrasebook.h"
#include "speech/speech.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

// HOW A HUNT PAYS OUT (abs = RVA + 0x120000).
//
// The petitioner's script calls native 0x37C, `questresultwindow`. Two independent sources name that
// slot: action_binding_tables.txt binds id 892 = 0x37C to `FUN_00344c60`, and the .dbg name join at
// the measured delta 5140 lands 892 on `questresultwindow`. Its body does what the name says:
//
//   FUN_00344c60 -> FUN_00290130(questId, ...)
//     FUN_003f8c60 / FUN_003f4c30  fill a 0x28-byte reward block from the quest's reward data:
//                                  +0x00 bit 0 = has gil, +0x04 gil, +0x08 u16 count A, +0x0A u16
//                                  count B, +0x0C A x (u16 id, u16 qty), +0x1C B x u16 id
//     FUN_003f4aa0                 GRANTS it -- adds the gil, adds every item (A with its qty, B x1)
//     FUN_003f4bd0 -> FUN_003f4e70 the result window; resolves the TITLE from the hunt table
//                                  (FUN_0037e8a0, DAT_02aed490+0xC8 -- the table the Clan Primer's
//                                  Hunts list reads, confirmed verbatim there)
//       -> FUN_003f4840            a sequencer that opens two windows in turn:
//            stage 0: FUN_003f4330  THIS PANEL -- title, gil, list-A items          (hooked here)
//            stage 1: FUN_003f4060  list B -> FUN_002a5b90 -> FUN_002a59c0 -> the "You obtain X!"
//                                   toast, which message_reader already speaks
//
// List B is exactly the KEY ITEMS: FUN_003f4c30 routes an id there iff FUN_0030baf0 is true, and that
// function's whole body is `(id & 0xF000) == 0x8000`. So a hunt's key item speaks through the toast
// and everything else speaks here -- no reward is covered twice, and none is left out.
//
// FUN_003f4840 is also opened from a menu (FUN_003f47e0 <- FUN_0057a4e0) with its own title and block.
// It is the same panel, so it speaks the same way; nothing here assumes the title is a hunt.
namespace {

using MemRead::PtrAt;
using MemRead::SafeReadInt;
using MemRead::SafeReadU16;
using MemRead::SafeReadU32;

constexpr uint32_t RVA_REWARD_PANEL = 0x2D4330;  // FUN_003f4330(win, msg)

// FUN_003f4330 case 1 fills these from its construct packet, and the row paint callback FUN_003f41c0
// reads them back by row index -- producer and consumer agree on every offset below.
constexpr uint32_t OFF_TITLE     = 0xC0;   // char* codec -- the hunt / bill name
constexpr uint32_t OFF_ROW_COUNT = 0xC8;   // i32 -- rows built; the fill loop stops at 3
constexpr uint32_t OFF_ROW_ID    = 0xCC;   // u16 per row, stride 8 -- item id, or 0xFFFF for gil
constexpr uint32_t OFF_ROW_VALUE = 0xD0;   // u32 per row -- the gil amount, or the item quantity
constexpr uint32_t ROW_STRIDE    = 8;
constexpr int      MAX_ROWS      = 3;
constexpr uint16_t ROW_ID_GIL    = 0xFFFF; // FUN_003f41c0 draws message 0x7D3 with the amount for it

// FUN_0031c5d0 category 1 takes ANY item id in the high half: it re-dispatches on `id >> 12` through
// FUN_00309440's table, which is why the toast resolves a key item and license_reader resolves gear
// through this one category. BattleState::DefName is the mod's choke point for it.
constexpr uint32_t CAT_ITEM = 0x01;

constexpr int MSG_BUILD   = 1;
constexpr int MSG_DESTROY = 0x12;   // FUN_0035e070 / FUN_003f4840 / FUN_003f4e70 each clear their
                                    // own live-window global on it; this proc forwards it upward

typedef uintptr_t (*Pfn_Panel)(void* win, void* msg);
Pfn_Panel s_origPanel = nullptr;

// Written on the game thread, read by MessageReader's `t` gate on the input thread.
std::atomic<bool> g_live{false};
bool g_initialized = false;

// GAME THREAD ONLY -- DefName is a game call. Runs after the original, so the rows are filled.
void OnBuilt(void* win) {
    const uint8_t* titleCodec = static_cast<const uint8_t*>(PtrAt(win, OFF_TITLE));
    std::wstring title = titleCodec ? GameText::Decode(titleCodec, 256) : std::wstring();
    if (!GameText::IsMostlyPrintable(title)) title.clear();

    int rows = 0;
    SafeReadInt(static_cast<char*>(win) + OFF_ROW_COUNT, &rows);

    std::wstring line = title;
    char diag[384];
    char utf[96];
    Log::ToUtf8(title, utf, sizeof(utf));
    int n = snprintf(diag, sizeof(diag), "reward panel: title=\"%s\" rows=%d |", utf, rows);

    int spokenRows = 0;
    for (int i = 0; i < rows && i < MAX_ROWS; ++i) {
        uint16_t id = 0;
        uint32_t value = 0;
        const bool ok = SafeReadU16(win, OFF_ROW_ID + i * ROW_STRIDE, &id)
                     && SafeReadU32(win, OFF_ROW_VALUE + i * ROW_STRIDE, &value);

        std::wstring row;
        if (ok && id == ROW_ID_GIL) {
            row = std::to_wstring(value) + Phrase::Get(Phrase::Id::GilSuffix);
        } else if (ok) {
            // Same quantity rule as the inventory rows: a bare name already means one.
            row = BattleState::DefName(CAT_ITEM, static_cast<uint32_t>(id) << 16);
            if (!row.empty() && value > 1) { row += L" "; row += std::to_wstring(value); }
        }

        Log::ToUtf8(row, utf, sizeof(utf));
        if (n > 0 && n < static_cast<int>(sizeof(diag)))
            n += snprintf(diag + n, sizeof(diag) - n, " [id=0x%04X value=%u \"%s\"]", id, value, utf);
        if (row.empty()) continue;          // unresolvable: silent, and the diag line says which
        if (!line.empty()) line += L", ";
        line += row;
        ++spokenRows;
    }
    Log::Write("REWARD", diag);

    if (spokenRows == 0) {
        // Different from a hook that never fired, and without this line the two are one silence.
        Log::Write("REWARD", "reward panel: no row resolved, staying silent");
        return;
    }
    MessageReader::NoteSpoken(line);
    Log::WriteW("REWARD", "reward panel: ", line);
    Speech::Output(line, /*interrupt=*/true);
}

uintptr_t HookedPanel(void* win, void* msg) {
    uintptr_t ret = s_origPanel ? s_origPanel(win, msg) : 0;
    STALL_SCOPE("RewardPanelReader::Panel");
    int msgCase = 0;
    if (!win || !msg || !SafeReadInt(msg, &msgCase)) return ret;
    if (msgCase == MSG_BUILD) {
        g_live.store(true, std::memory_order_relaxed);
        OnBuilt(win);
    } else if (msgCase == MSG_DESTROY) {
        g_live.store(false, std::memory_order_relaxed);
    }
    return ret;
}

} // namespace

namespace RewardPanelReader {

bool Init() {
    if (g_initialized) return true;
    const bool ok = Hooks::InstallTyped(RVA_REWARD_PANEL, &HookedPanel, &s_origPanel);
    g_initialized = ok;
    Log::Write("REWARD", ok
        ? "RewardPanelReader initialized (questresultwindow panel FUN_003f4330: title + gil + items)"
        : "RewardPanelReader: hook failed to install -- see Hooks log.");
    return ok;
}

void Shutdown() {
    if (!g_initialized) return;
    Hooks::Uninstall(RVA_REWARD_PANEL);
    g_initialized = false;
    g_live.store(false, std::memory_order_relaxed);
}

bool IsLive() { return g_live.load(std::memory_order_relaxed); }

} // namespace RewardPanelReader

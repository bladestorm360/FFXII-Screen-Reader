#pragma once
#include <cstdint>
#include <string>

// Reader for the in-game "row-chain" menu family — the field pause menu + ALL its submenus
// AND the battle command menu, which the universal focus signal FUN_00247510 (msg 0x8000)
// reaches but the title/config decode path cannot handle.
//
// One structure covers the whole family. On 0x8000, `val` is the focused row INDEX. The row
// array base lives at owner+ROW_OFF (ROW_OFF differs per owner class); each row is a 0x20-byte
// record built by the shared builder FUN_002cd3c0(cmdId, descId):
//   row+0x00 = command id (u32)
//   row+0x10 = NAME codec*        <- the short label we speak ("Equipment", "Attack", …)
//   row+0x18 = DESCRIPTION codec* (the game feeds this to the description-bar setter
//              FUN_00291d80, which text_capture already captures for the `o` key)
//
// The 5 owner classes (obj[0] RVA -> ROW_OFF): FUN_00280de0/0xD8 (field command column),
// FUN_002c2320/0xC8 (magicks/technicks + battle command), FUN_00565e00/0xC8 (gambits),
// FUN_0056f810/0xE0 (sub-panel), FUN_0057b890/0xD0 (equip-type list).
//
// menu_reader owns the single FUN_00247510 hook and delegates row-chain owners here. It also
// owns the battle command-draw + status-chooser hooks (installed by Init below). All reads are
// memory-only + SEH-guarded; text is the game's own (decoded via GameText). (The battle
// target-selection readout is a separate reader — see battle_target_reader.{h,cpp}.)
namespace IngameMenuReader {

// Installs the battle command-draw + status-chooser hooks. Called from MenuReader::Init; the
// row-chain focus path needs no install (menu_reader's dispatch hook delegates to it).
bool Init();
void Shutdown();

// If `owner` is a row-chain menu class, return its ROW_OFF (non-zero); else 0.
uint32_t RowChainOff(void* owner);

// Speak the focused row's NAME (owner+ROW_OFF row array, row = base + index*0x20, name at
// row+0x10). Rows are pre-built at menu open, so this reads immediately — no dispatch deferral.
void OnRowChainFocus(void* owner, uint32_t rowOff, int index);

// Battle command menu (owner class FUN_0027ad70): recognise it, and speak the highlighted command
// (owner+0x510+index*8 -> cmdId -> name cached from the FUN_00276be0 draw). Same FUN_00247510 0x8000
// dispatch as the party menu, just an unmapped owner class.
bool IsBattleCommandOwner(void* owner);

// IS THE BATTLE COMMAND MENU THE LIVE SURFACE RIGHT NOW? (Session 156)
//
// `IsBattleCommandOwner` answers about a pointer you already have; this answers about the CURRENT
// state, which is what a hotkey needs. It exists because `o` must never be the Libra key while the
// player is choosing a Magick or Technick -- reading the ability's description is the whole point of
// the key there, and a committed target from the previous action is not a question about a monster.
//
// Self-clearing three ways: the stored panel is re-validated against the window class on every read
// (a freed or repurposed object stops matching), any focus on a different owner clears it, and
// battle teardown clears it. Safe from any thread.
bool BattleCommandActive();

// Drop the live-surface flag. Called when focus lands anywhere that is not the battle command menu.
void ClearBattleCommandActive();
void OnBattleCommandFocus(void* owner, int index);

// Field pause menu command column (owner class FUN_00280de0). Its entry announce is DEFERRED, unlike
// every other pane: FUN_00244830 fires at the start of construction, so MenuReader::HookedFocusSet
// stashes the entry focus via ArmPaneEntry instead of speaking, and the menu's own SHOW message
// (cat 0x13, handled by this reader's window hook) releases it -- speech then lands with the menu.
bool IsFieldPaneOwner(void* owner);
void ArmPaneEntry(void* owner, uint32_t rowOff, int index);

} // namespace IngameMenuReader

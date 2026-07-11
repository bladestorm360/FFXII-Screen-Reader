#pragma once
#include <cstdint>

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
// owns the battle target-reticle name hook (installed by Init below). All reads are memory-only
// + SEH-guarded; text is the game's own (decoded via GameText).
namespace IngameMenuReader {

// Installs the battle target-reticle name hook (FUN_003bfe10). Called from MenuReader::Init;
// the row-chain focus path needs no install (menu_reader's dispatch hook delegates to it).
bool Init();
void Shutdown();

// If `owner` is a row-chain menu class, return its ROW_OFF (non-zero); else 0.
uint32_t RowChainOff(void* owner);

// Speak the focused row's NAME (owner+ROW_OFF row array, row = base + index*0x20, name at
// row+0x10). Rows are pre-built at menu open, so this reads immediately — no dispatch deferral.
void OnRowChainFocus(void* owner, uint32_t rowOff, int index);

// Battle command menu (owner class FUN_0027ad70): recognise it, and speak the highlighted command
// (owner+0x510+index*8 -> cmdId -> name cached from the FUN_00276be0 draw). Same FUN_00247510 0x8000
// dispatch as the field menu, just an unmapped owner class.
bool IsBattleCommandOwner(void* owner);
void OnBattleCommandFocus(void* owner, int index);

} // namespace IngameMenuReader

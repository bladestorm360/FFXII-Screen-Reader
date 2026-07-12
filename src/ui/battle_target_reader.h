#pragma once

// Reader for the battle TARGET-SELECTION readout — announces the unit the target cursor is on
// when you aim a battle action (Attack / Magick / Item) at a foe or ally.
//
// Target selection lives on the battle-state object P (DAT_0209be80): the highlighted target
// handle at *(int)(P+0x9FD8), gated by *(P+0x10f78)!=0 while selection is active. The reticle
// path (FUN_005528c0) and the acting-char controller (DAT_0209ac30+0xde0) were both DISPROVEN.
// Two hooks drive it: FUN_002bfd20 (the target nameplate render, which tells us the current unit)
// and the nested FUN_00329220 (per-unit vitals snapshot); we announce the current target on change.
//
// Speech mirrors the top-left TARGET INFO panel (NEVER the lower-right HUD):
//   - enemy = "<name>, HP <pct> percent"  (percentage from real cur/max; no MP, no numbers)
//   - ally  = "<name>, HP <cur> of <max>" — "ally" includes the player-controlled leader
//     (self-target, def+5==0), not only party-side scene-kind==3.
// Memory-only, SEH-guarded reads; the name is the game's own (decoded via GameText).
namespace BattleTargetReader {

// Installs the vitals-snapshot hook (FUN_00329220). Call once from MenuReader::Init.
bool Init();
void Shutdown();

} // namespace BattleTargetReader

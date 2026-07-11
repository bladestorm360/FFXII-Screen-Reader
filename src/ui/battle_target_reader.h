#pragma once

// Reader for the battle TARGET-SELECTION readout — announces the unit the target cursor is on
// when you aim a battle action (Attack / Magick / Item) at a foe or ally.
//
// The shipped mod used to hook the wrong function (the reticle FUN_005528c0), which never fires
// for normal Foes/Party/Allies selection, so targeting was silent. Live probing established the
// real, reliable path on the command controller DAT_0209ac30 (RVA 0x1F7AC30):
//   - current target index = *(s16)(ctx + 0xde0)  (0..8; slots 0-3 party, 4-8 foes; 0xffff = none)
//   - per-unit vitals are built by FUN_00329220 (RVA 0x209220), which fires reliably during
//     targeting; we hook it, cache slot->BtlChr, and announce the current target on change.
//
// Speech mirrors the top-left TARGET INFO panel (NEVER the lower-right HUD):
//   - enemy = "<name>, HP <pct> percent"  (percentage from real cur/max; no MP, no numbers)
//   - ally  = "<name>, HP <cur> of <max>" (context MP branch is a later stage)
// Memory-only, SEH-guarded reads; the name is the game's own (decoded via GameText).
namespace BattleTargetReader {

// Installs the vitals-snapshot hook (FUN_00329220). Call once from MenuReader::Init.
bool Init();
void Shutdown();

} // namespace BattleTargetReader

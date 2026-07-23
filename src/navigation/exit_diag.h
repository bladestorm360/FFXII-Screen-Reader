#pragma once

// Exit-coverage diagnostic (tag NAV-DIAG, file-only, never speaks).
//
// WHY THIS EXISTS: the mod sourced exits from ONE table -- the `__MJ_CTRL` map-jump controllers in the
// map's compiled script -- and paired each controller to a door by the rule "`__MJ_CTRL<N>` owns `+0x54`
// slot N+1". Both halves were derived on Nalbina, whose maps have two or three doors. Session 55 measured
// Rabanastre East End and found the rule is not enough on a dense map:
//
//   * `+0x54` slot 3 has a controller and NO `+0x70` field-sign record.
//   * `+0x54` slot 7 has a field-sign record and NO controller -- a transition the game itself files as
//     an exit that the mod cannot see at all.
//   * The interior doorways (five shops, The Sandsea, Stair to Lowtown) are a third class again: field
//     signs bound by `setfieldsignlocationjumpinfo`, no controller, destination only in `+0x8c`.
//
// So neither table alone is the exit list, and the pairing is confirmed for exactly one door per map --
// the one the tester walked through. This dump prints the two tables side by side, flags every slot only
// one of them covers, and prints BOTH candidate pairings for each controller, so a single walk-through
// discriminates them instead of another session of inference.
namespace ExitDiag {

// Dump the coverage matrix, both candidate pairings, and the `+0x8c` destination records. Memory-only and
// SEH-guarded; safe to call whenever a field map is loaded. Called from the `'` diagnostic key.
void DumpCoverage();

} // namespace ExitDiag

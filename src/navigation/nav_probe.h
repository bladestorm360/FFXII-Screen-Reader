#pragma once

#include <cstdint>

// The `'` diagnostic, rebuilt (Session 74) around the three questions the 3D pathfinder rebuild has to
// answer before any graph code is written. It replaces the old dump wholesale -- the move-frame
// snapshot, the wall self-test, the grid cross-check, the +0x54/+0x70 legacy exit tables,
// ExitDiag::DumpCoverage and MapExits::DiagScanScriptMapjumps (which alone emitted a 0x9000-byte blob
// at ~1,152 lines). None of that measures anything still open.
//
// WHAT IT MEASURES
//
//   SPAN-PROBE   Every walkable floor layer in a block of columns around the player and around each
//                map-jump seam, next to the engine's own GroundAt answer for the same point.
//                Closes two gates:
//                  GATE A -- does MapQuery::AllFloorsAt EVER report >= 2 layers? Across every log so
//                    far it never has (`layers>=2` x0), which is why Session 73 struck the layered
//                    grid. But that diagnostic only ever sampled the player's column and each listed
//                    object's column, and Upper Apartments' two seams sit in DIFFERENT columns 3.1 m
//                    apart -- so the stacking in that room was never sampled. This sweeps a grid.
//                    If every column still reports one span, the span graph would be isomorphic to
//                    today's flat grid and the rebuild must be re-derived, not built.
//                  GATE B -- span height is only trusted at 100% walkability agreement; the existing
//                    cross-check reports exactly that for WALKABILITY (416/416, 404/404, 520/520) but
//                    as low as 41/45 for HEIGHT. Span Y is what the whole rebuild keys on, so the
//                    disagreements have to resolve to a rule (topmost / nearest / lowest) first. This
//                    reports which span index GroundAt actually lands on, per column.
//
//   INTERACT-REACH  The engine's horizontal interaction reach, replicated from FUN_003da5a0 and
//                checked against the engine's own arithmetic in the same breath -- see
//                InteractTarget::ReadReachFor. This is the number `kApproachRadius = 4.0f` was
//                invented to stand in for, and the routing goal set must not be narrowed with the
//                replica until the two agree.
//
//   EXIT-AIM     Per map-jump group: the seam's centroid (what an exit currently aims at) beside its
//                nearest vertex to the player (what it should aim at), in metres AND steps. The gap
//                is the overshoot the tester reports as "25 steps north" for a transition that fires
//                after five.
//
// THREADING. The whole probe runs on the GAME THREAD, drained from the field-frame hook. `'` only
// raises a flag. That is not incidental: Gate B needs MapQuery::GroundAt, which is a game call, and
// `Docs/debug.md:1608` records that calling it from the old input-thread dump was a bug. Everything
// else here is memory-only, but keeping one thread for the whole probe means no half-and-half.
namespace NavProbe {

// INPUT THREAD. Queue a probe; speaks nothing. Overwrites any pending request.
void Request();

// GAME THREAD. Called once per field frame from the field-frame hook. O(1) (one atomic load) when
// nothing is pending. Retries for a bounded number of frames while the field is not yet nav-safe,
// then gives up out loud rather than leaving the key looking dead.
void OnGameFrame();

} // namespace NavProbe

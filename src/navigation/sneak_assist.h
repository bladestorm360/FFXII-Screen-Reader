#pragma once

#include <cstdint>

// SNEAK ASSIST — neutralise the "a guard noticed you" fail condition on stealth-minigame maps.
//
// ⚠ THIS IS THE MOD'S SECOND WRITE-CATEGORY EXCEPTION, after auto-walk (CLAUDE.md: the mod is
// strictly read-only on input and game memory). USER-AUTHORIZED 2026-07-31, explicitly, as an
// accessibility skip for a hard progress block. Boundaries, all non-negotiable:
//   1. DEFAULT OFF, gated on the ModMenu "Sneak assist" toggle (F10 is the shortcut);
//   2. with the toggle off the hook's FIRST branch tail-calls the original and returns — the write
//      is unreachable, not merely skipped, so an unarmed build is byte-identical in behaviour;
//   3. effective ONLY on maps PathDanger has a row for (`PathDanger::MapHasRow`) — everywhere else
//      the hook is a pass-through even with the toggle on;
//   4. NO PERSISTENT GAME STATE IS EVER WRITTEN. The only write is to the script VM's own return
//      slot for the call being serviced, inside that call. Toggling off restores vanilla behaviour
//      on the very next call; nothing is left behind for a save to capture.
//
// WHAT IT DOES, and why it is not a flag write. Map 568's sneak sequence has NO "guards let him
// pass" flag: `とおしてあげる` ("let him pass") is a ROUTINE, and the fail condition is a watcher
// that calls the script native `distance` and branches to `ヴァン捕獲` ("Vaan captured") when the
// result is small. So the honest override is at the measurement: while armed, the native's result
// is replaced with a large value, and the watcher's "too close" branch never fires.
//
// THE NATIVE (Session 106, resolved from the BYTECODE, not from the name table):
//   * `rrp_a02.ebp` (map 568) contains exactly 8 `CALLACT 0x0290` sites and ZERO for `0x028f`.
//   * The `0x0290` slot's handler is `FUN_003448f0` (RVA 0x2248F0): it pops two coordinates and an
//     actor id, resolves the actor (`FUN_00264010` → `FUN_00265060` → the actor's `+0xB8` world
//     transform — the SAME offset this mod already reads for scene objects), then calls
//     `FUN_004686d0` = `sqrtf(dx*dx + dz*dz)` — a HORIZONTAL distance, which is exactly the
//     proximity semantics the script needs.
//   * `mapctrl.dbg` names index `0x028f` "distance": OFF BY ONE SLOT. The bytecode and the handler
//     body agree with each other and outrank the name table — this is the standing "resolve natives
//     by BEHAVIOUR, never by index arithmetic" rule paying for itself again.
//
// KNOWN LIMIT, accepted by the user: `distance` is a GENERIC native, so v1 clamps every call on a
// table map while armed. If a map ever gates progression on the party APPROACHING something, that
// gate would stall while this is on — hence default-off, the instant toggle, and a log line for
// every clamp so a stall is diagnosable in one grep. Map 569's catch is capture RECTS, not
// distance, so this does nothing there until 569 gets its own mechanism.
namespace SneakAssist {

// Installs the native hook. Non-fatal on failure: the feature simply never arms.
bool Init();
void Shutdown();

// True when the toggle is on AND the current map has a PathDanger row. Cheap; used by the F10
// command to tell the player whether the setting can do anything where they are standing.
bool ArmedHere();

} // namespace SneakAssist

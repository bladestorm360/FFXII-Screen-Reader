#pragma once

#include <cstdint>

// SNEAK ASSIST — neutralise the "a guard noticed you" fail condition on stealth-minigame maps.
//
// ⚠ THIS IS THE MOD'S SECOND WRITE-CATEGORY EXCEPTION, after auto-walk (CLAUDE.md: the mod is
// strictly read-only on input and game memory). USER-AUTHORIZED 2026-07-31, explicitly, as an
// accessibility skip for a hard progress block. Boundaries, all non-negotiable:
//   1. DEFAULT OFF, gated on the ModMenu "Sneak assist" toggle (F10 is the shortcut). It is forced
//      off at STARTUP and on EVERY MAP CHANGE (S109), and `F10` is a NO-OP on any map without a
//      PathDanger row -- so the only way it can be on is that the player deliberately armed it,
//      this session, while standing on the guarded map it acts on;
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

// Does the CURRENT map have a stealth sequence this feature covers? **`F10` is a NO-OP when this is
// false** (S109, user instruction): the key does not toggle, does not speak, and only logs. The
// setting is for getting past guards, so it can only be armed while standing on a map that has
// them — a player cannot leave it switched on somewhere it was never meant to act.
bool AvailableHere();

// True when the toggle is on AND the current map is covered — i.e. the clamp would actually fire.
bool ArmedHere();

// GAME THREAD, once per field tick. Refreshes the snapshot of WHICH scene objects are this map's
// guards, which is what lets the touch-test override answer per OBJECT instead of per map. Costs a
// table lookup and one store while the toggle is off or the map has no row — i.e. almost always.
void OnFieldFrame();

// GAME THREAD, from the field-teardown hook. **Forces the toggle OFF on every map change** (S109,
// user instruction), silently and persistently, so the feature can never carry into a map it was
// not authorized for because a player forgot to switch it off. Arming is therefore always a
// deliberate act on the map it applies to.
void OnMapTeardown();

} // namespace SneakAssist

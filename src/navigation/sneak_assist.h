#pragma once

#include <cstdint>

// SNEAK ASSIST — neutralise the "a guard noticed you" fail condition on stealth-minigame maps.
//
// ⚠ THIS IS THE MOD'S SECOND WRITE-CATEGORY EXCEPTION, after auto-walk (CLAUDE.md: the mod is
// strictly read-only on input and game memory). USER-AUTHORIZED 2026-07-31, explicitly, as an
// accessibility skip for a hard progress block. Boundaries, all non-negotiable:
//   1. THE MAP TABLE IS THE ONLY GATE (Session 115, user-authorized in that conversation). There is
//      no toggle, no mod-menu row and no hotkey: on a map `PathDanger` names, the overrides are
//      always live; on every other map they are unreachable. This REPLACES the S107/S109 arming
//      rule (default off, F10, forced off at startup and on every map change) -- see below for what
//      the play evidence was and why the arming stopped buying anything;
//   2. off a table map the hooks' FIRST branch tail-calls the original and returns — the write is
//      unreachable, not merely skipped, so on every map but the table's the build is byte-identical
//      in behaviour to the read-only mod;
//   3. the table is `PathDanger::MapHasRow`, today maps 568 and 569 only, and that pair is a
//      MEASUREMENT of the palace's own scripts, not a guess (path_danger.cpp holds the census);
//   4. NO PERSISTENT GAME STATE IS EVER WRITTEN. The only write is to the script VM's own return
//      slot for the call being serviced, inside that call. Walking off the map restores vanilla
//      behaviour on the very next call; nothing is left behind for a save to capture.
//
// WHY THE TOGGLE WENT (Session 115, tester's decision). S113 shipped armed and PLAY-CONFIRMED: the
// tester crossed 568 with it on for three minutes and the servant chain, the shout and the
// transition all worked, with the falsifier silent throughout. The arming existed to contain a risk
// that the suppression might silence the map's EVENTS as well as the guards' catch; play showed it
// silences only the catch. What the arming was left doing was making a blind player re-arm a fix
// for a puzzle they cannot see, on every entry to the map that needs it. So the table became the
// gate outright.
//
// WHAT THAT COSTS, stated rather than buried: the accepted limit below -- `distance` is a GENERIC
// native, so every call on a table map is clamped -- no longer has a player-side escape hatch. On
// 568 that risk is retired by play. On 569 it is not yet, so the `clamp ACTIVE` line and the
// non-guard falsifier both still print: if a 569 gate ever stalls, the fix is to drop 569's row.
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
// KNOWN LIMIT, accepted by the user: `distance` is a GENERIC native, so this clamps every call on a
// table map. If a map ever gates progression on the party APPROACHING something, that gate would
// stall — hence the table being as small as the evidence allows, and a log line for every clamp so
// a stall is diagnosable in one grep. Map 569's catch is documented as capture RECTS as well as the
// distance native; the rects carry no npcdic name, so if one ever catches the player the falsifier
// below names the object and it becomes one more table entry.
namespace SneakAssist {

// Installs the native hooks. Non-fatal on failure: the feature simply never acts.
//
// There is no `Shutdown()` (removed Session 115). It only ever cleared an "are the hooks installed"
// flag whose sole reader was `ArmedHere()`, which the toggle's removal deleted, and
// `Navigation::Shutdown` never called it -- so it was a lifecycle stub that recorded nothing and
// ran never. The hooks themselves are torn down with the rest by `Hooks::`.
bool Init();

// GAME THREAD, once per field tick. Refreshes the snapshot of WHICH scene objects are this map's
// guards, which is what lets the touch-test override answer per OBJECT instead of per map. Costs a
// table lookup and one store on any map with no row — i.e. almost always.
void OnFieldFrame();

// GAME THREAD, from the field-teardown hook. Prints the leaving map's native census and clears the
// guard snapshot and the log latches, so a stale scene-object pointer can never be matched against
// an object on the next map.
void OnMapTeardown();

} // namespace SneakAssist

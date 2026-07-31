# FFXII-Screen-Reader — Session Log (Sessions 101–current)

Continues `sessions_051_100.md`, which is closed at **Session 100** (the adjacency march +
auto-walk build).

Entry format is mandatory: `## Session N — YYYY-MM-DD — [track] <title>`, where `N` is a single,
global, monotonically increasing integer shared by all parallel tracks. Never a date-only header,
never a letter sub-session. Before appending, grep this file for the highest `## Session N` AND
check `git log` for an unlogged session after it; the next session takes `N+1`. Split again after
Session 150 (`sessions_101_150.md` + `sessions_151_current.md`). Every entry carries a KEYWORDS
line for grep.

## Session 102 — 2026-07-31 — [exits] The binding is the CALLS, not the NAME: event-bound transitions

KEYWORDS: event transfer staircase dungeon yes no prompt third transition kind map 313 north spur
sluiceway group 1 unclaimed NO CONTROLLER CLAIMS THIS GROUP __MJ_CTRL name filter setmapjumpgroup
mapjump flags presentation bitfield FUN_00355350 FUN_00314440 FUN_003145e0 routineIndex viaController

**The report:** a staircase on the map after 315 transfers the party into a dungeon behind a yes/no
prompt, and the mod lists nothing for it. It was framed as a **third transition kind**, after S64's
TRANSITIONS (walk onto a `setmapidmj` surface) and DOORS (press Enter on a `+0x70` field-sign object).

**It is not a third kind, and the mod's own log had been saying so** (`FFXII-Screen-Reader-Latest.log`,
23:18:27 on map 313):

```
exits: controllers=1 surfaces=2 listed=1 | dropped: nogroup=0 notused=0 unreachable=0
surface g1: 2 polys at (32.0,17.0,-0.8) box x[30.0..34.0] z[-5.8..4.2]
                                       <== NO CONTROLLER CLAIMS THIS GROUP -- unreachable exit
surface g2: 22 polys at (182.7,9.3,56.1)          (the 315 exit -- listed, routed, walked)
```

An ordinary walk-onto map-jump surface, group 1, two polys, sitting at Y=17 while the rest of the map
is at Y≈9 — up a flight of stairs. **The seam sweep already finds it.** Only the destination was
missing, so the S64 drop rule ("destination unresolved ⇒ it leads nowhere") deleted it, and a
progress-critical dungeon entrance was invisible to a blind player. The diagnostic line naming the
defect had been printing on every scan and nothing consumed it.

### Root cause: one `continue`

`map_script.cpp` rejected every routine whose NAME did not parse as `__MJ_CTRL<NNN>` **before its code
span was ever read**. Map 313's routine table has 31 entries and exactly one controller (back to 315);
among the rest is a routine whose Shift-JIS name begins **イベント** ("event"). The S64 rule is about
the CALLS a routine makes — `setmapjumpgroup(K)` for WHERE, that same routine's `mapjump` literal for
WHERE TO. The name was how they were found, and it hardened into the rule.

Second, narrower filter: only `mapjump` with `flags == 0` was accepted. From the decompile,
`FUN_00355350` (the native) → `FUN_00314440(dest, entrance, flags, 1)` → `FUN_003145e0`, and in
`FUN_00314440` `flags & 1` picks the no-fade path while `(flags >> 1) & 1` feeds `FUN_002efa70`.
**`flags` is a presentation bitfield, not a transition kind** (conf 0.99).

### What shipped

- **Every routine's code span is scanned** for the `setmapjumpgroup` + `mapjump` pair.
- **Admission for a non-controller routine requires `setmapjumpgroup`.** A routine that arms no group
  claims no surface — that keeps out the Director's world-map teleport run and every story-move
  `mapjump`. Without it, every map would sprout phantom exits with nowhere to stand, which is the
  failure S83/S84 spent two sessions deleting.
- **Controllers keep their exact previous behaviour**, including the `flags == 0` test and the
  group-less record that downstream still counts as `nogroup`. Non-controllers are admitted on any
  flags except the world-map menu's `0x0A`.
- **The arrival pairing is protected.** `ResolveControllerArrivals` binds controller *i* to arrival
  *i* by POSITION, so event-bound entries are held in a second vector and appended only after it has
  run, with no arrival of their own. Appending them first would have shifted every existing exit onto
  the wrong doorway on every map in the game.
- `ExitDest` gains `routineIndex` / `viaController` / `routineName` / `jumpFlags`; `exit_scan.cpp`
  keys the cursor id on a disjoint band for event-bound exits (`-(2000 + routineIndex)`), because
  their `ctrlIndex` is -1 and two on one map would collide.

### State

**BUILT, NOT DEPLOYED** (Session 101's pathfinding build holds the game's `dinput8.dll`; this is a
separate worktree so the two can be reverted independently). NOT play-confirmed.

Falsifier, already written and already printing — on map 313:
`NO CONTROLLER CLAIMS THIS GROUP` for g1 must be **gone**, `listed=1` must become `listed=2` with a
real destination name, and the new entry must be routable at ~(32.0,17.0,-0.8). **On every other map
the listed count must not move** — a new exit appearing on a map that was already correct is a
regression, not a win. Taking the staircase should then produce a `CROSSING ORACLE … <== MATCH`.

If instead the log shows no routine claiming group 1, the binding is outside the map-control blob and
the EVENT script domain is next (`notes\dbg_symbols_evctrl.csv` carries its own `mapjump`,
`setmapjumpmode`, `setposparty_mapjump`, `lockmapjump`; confirm prompt is `FUN_002a6190`).

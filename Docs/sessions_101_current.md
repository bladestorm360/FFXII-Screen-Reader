# FFXII-Screen-Reader — Session Log (Sessions 101–current)

Continues `sessions_051_100.md`, which is closed at **Session 100** (the adjacency march +
auto-walk build).

Entry format is mandatory: `## Session N — YYYY-MM-DD — [track] <title>`, where `N` is a single,
global, monotonically increasing integer shared by all parallel tracks. Never a date-only header,
never a letter sub-session. Before appending, grep this file for the highest `## Session N` AND
check `git log` for an unlogged session after it; the next session takes `N+1`. Split again after
Session 150 (`sessions_101_150.md` + `sessions_151_current.md`). Every entry carries a KEYWORDS
line for grep.

## Session 101 — 2026-07-31 — [navigation] The goal is a SURFACE: the route now ends where walking first touches the seam

KEYWORDS: surface goal seam poly set map jump transition endpoint portal entry point path_surface_goal
PathSurfaceGoal::Route surfaceTouch first pop pass=surface-goal 315 northern sluiceway replan frontier
suppressed no path 16.4m short vertex boundary S98 diagnosis S99 circularity InsetCorners kArrivalTol

**The one residual defect from the S100 breakthrough.** Map 315 routes and the tester has walked it
(`319c0bb`), but a mid-route replan from certain approach directions still failed: the route target
is `MapQuery::NearestPointOnSurface` — the nearest tagged seam **vertex** in XZ — and on a 27 m seam
that is the corner the walkable approach reaches LAST. The final leg then runs along the exit surface,
validation fails, `PathSearch::Run` returns `Plan::Frontier` 16.4 m short, the planner suppresses it
and speaks "No path", and the beacon stops until the player presses `\` again.

### What shipped

`src/navigation/path_surface_goal.{h,cpp}` — a route that ends where walking first TOUCHES the
transition surface:

- **The endpoint is the PORTAL the corridor crosses onto the surface**, not a distance to anything.
  `EdgeClearSpan` (falling back to `EdgePortal`) → midpoint → step `kStepIn = 0.5 m` toward the
  surface poly's centroid so the arrival poly is unambiguously the transition → `ClosestPointOnPoly`
  to clamp back onto the triangle and take Y from its own plane.
- **The corridor is rebuilt for that end poly** via the existing `PathCorridor::Build`, the same
  helper `BuildFrontier` uses and for the same reason: funnelling one poly's portal sequence toward a
  point on another is not a path the funnel can repair.
- **Validation is the ordinary one, not a lighter one.** `BestPolarity` → `InsetCorners` →
  `DropPassedWaypoints` → `PathValidate::CheckLegs(poly, budget, kArrivalTol)`, in that order, with
  the corner footprint tests, the adjacency march and the pinned-corner acceptance all included.
  Accepted **only** on `ok && !truncated && size >= 2`; anything else falls through to today's
  frontier. No partial acceptance, no relaxed tolerance, no repair-ladder shortcut.

`path_search.cpp` grew by ~15 lines (it is already 789 and over the 500-line cap, which is why the
work went in its own file): an `unordered_set` built once per `Run` from `seamPolys`; `surfaceTouch`
recorded on the **first pop of any member**, reset beside `came.clear()` so the touch and the parent
links can never come from different passes; and the consumer replacing the `(void)seamPolys;` line.

### Why this cannot change a route that works today — the property, not the promise

1. `seamPolys` is `nullptr` for every request that is not a walk-onto transition
   (`path_planner.cpp` only fills it when `seamGroup != 0`), so the set is empty, `surfaceTouch`
   stays `kNoPoly`, and the whole block is unreachable.
2. The block sits **below** the `if (best.reachedGoal && bestReport.ok && !truncated) return
   Plan::Route`, so a route that validates never reaches it.
3. Observing the set costs one hash probe per pop and changes no A\* decision.

Only two outcomes are reachable: a validated `Route` where there is a suppressed Frontier / "No path"
today, or the frontier path exactly as it runs today.

### The S99 rule, and why this is not S98 again

**A route may never be validated against a point derived from that same route's own progress.** S98
aimed at the seam member nearest the banked proven prefix's end; the prefix already ended on a seam
poly, so the aim point WAS the reference (`0.0m from ref` ×18) and the re-run validated the prefix it
came from. Here the endpoint is a portal between two mesh triangles — geometry that exists whether or
not this search ran — and the proof is a body walk, not a distance comparison. `pass=seam` remains
grep-dead; this pass logs **`pass=surface-goal`**.

### State

**BUILT, DEPLOYED, NOT PLAY-CONFIRMED.** Regression gate to check first in the next tester log, before
anything else: `pass=seam` absent; the working 315 route still `pass=mesh` with `inset=` non-zero,
`validate: … OK` and ~20/20 legs; and **no `surface-goal:` line on it** — a validated route must never
reach the new block. Then the fix itself: replan from mid-bank should log `surface-goal: … ACCEPTED`
and `plan=Route` where it logged `FRONTIER SUPPRESSED … 16.4m short`.

Done on the `nav/surface-goal` worktree so it can be reverted without touching Session 102's
event-bound transition work. `debug.md`'s Sluiceway section had its stale "STILL BLOCKED" title struck
and rows 11-14 added — the anatomy of what actually unblocked the map.

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

**BUILT AND MERGED INTO `combat-system` alongside Session 101, NOT play-confirmed.** Both sessions
were developed on their own branches (`nav/surface-goal`, `nav/event-transfer`) and merged as two
DISTINCT commits at the tester's instruction, so one build carries both while `git revert 83c77f0`
still backs out the pathfinding change on its own without losing this one. `nav/event-transfer`
stays as a live branch for further non-pathfinding exit work until pathing is thoroughly tested.

Falsifier, already written and already printing — on map 313:
`NO CONTROLLER CLAIMS THIS GROUP` for g1 must be **gone**, `listed=1` must become `listed=2` with a
real destination name, and the new entry must be routable at ~(32.0,17.0,-0.8). **On every other map
the listed count must not move** — a new exit appearing on a map that was already correct is a
regression, not a win. Taking the staircase should then produce a `CROSSING ORACLE … <== MATCH`.

If instead the log shows no routine claiming group 1, the binding is outside the map-control blob and
the EVENT script domain is next (`notes\dbg_symbols_evctrl.csv` carries its own `mapjump`,
`setmapjumpmode`, `setposparty_mapjump`, `lockmapjump`; confirm prompt is `FUN_002a6190`).

## Session 103 — 2026-07-31 — [navigation] The remainder counts LEGS; and S102 found nothing on 313

KEYWORDS: route wording then 14 more legs remainder step total dropped path_directions Describe
ThenJoiner MoreSuffix StepsSuffix kMaxSpokenLegs S102 field result map 313 group 1 unclaimed
orphaned diagnostic always-print summary event script blob evctrl

### 1. The route sentence (tester request, both parts)

`PathDirections::Describe` used to emit
`"North 19, East 5, Southeast 22, South 28, Southeast 20, then 225 more. 319 steps"`. The `225` was
the leftover STEP count and `319` the route total. **Every number before it is glued to a direction
word**, so "225 more" reads as 225 more *legs* — which is exactly how the tester read it ("it sounds
like it's counting legs of the route not steps"). The route was 19 legs / 239 m, 34% over the
straight line; **the pathfinder was blameless and the sentence was the defect.**

Two changes, both the tester's wording:

- the remainder now counts **unspoken legs** (`legs.size() - kMaxSpokenLegs`), which is what its
  position in the sentence already promised;
- the **route total is dropped entirely** — per-leg counts are what you act on, and the total was the
  least actionable number in the line while landing last, where it sticks.

New form: `"North 19, East 5, Southeast 22, South 28, Southeast 20, then 14 more."`
No new phrasebook string — `ThenJoiner` (`", then "`) and `MoreSuffix` (`" more"`) are reused
verbatim. `StepsSuffix` is no longer used by this function but stays live in `nav_common`'s
crow-flies phrases and in `NextInstruction`; do not delete it.

Settled and **closed — do not re-propose**: `NavCommon::g_unitsPerStep = 0.75 m` stays. It measures
*steps to walk*, which is what the tester wants; a recalibration would move every spoken distance in
the mod on every map.

### 2. Session 102 was tested and found NOTHING — and my instrumentation cannot say why

Map 313, on the merged build (deployed DLL verified as the build containing the change):

```
exits: controllers=1 surfaces=2 listed=1 | dropped: nogroup=0 notused=0 unreachable=0
surface g1: 2 polys at (32.0,17.0,-0.8)  <== NO CONTROLLER CLAIMS THIS GROUP -- unreachable exit
```

Byte-identical to before. **No `EVENT-BOUND transition` line on any map in the log.**

**NO REGRESSION** (the gate that mattered): 315 still lists 2 of 2 surfaces, 313 still lists its 1
real exit. No map gained or lost an exit.

**THE CONCLUSION IS NOT AVAILABLE, ONLY THE ABSENCE OF ONE.** The scan's only output is a POSITIVE
finding, and S102 made the span-empty / span-unreadable logs controller-only — so *"no routine
outside `__MJ_CTRL` arms a group"* and *"the scan silently skipped those routines"* print the
identical nothing. **This is the S77 orphaned-diagnostic failure, repeated by the person who wrote
the S77 entry.** B1 is not refuted; it is untested.

### 3. THE DIAGNOSTIC NEEDED (next session, first item)

One **always-printing** line from `ReadExitDests`, log-only, no behaviour change:

- routines scanned / spans read / spans skipped, with the skip reason counted;
- **every `setmapjumpgroup(K)` value found and which routine armed it** — this is the line that says
  whether anything claims group 1;
- every `mapjump` found: routine name, dest, entrance, flags.

One load of map 313 then settles B1 vs B2 outright. It is the falsifier S102 should have shipped
with, and it costs ~15 lines.

### 4. Where the evidence now points

**The yes/no prompt was the tell and S102 under-weighted it.** A confirm prompt means walking onto
the surface fires an EVENT and only "yes" performs the jump — so S64's one-routine-holds-both-halves
model does not describe this shape at all. The 2-poly surface at (32.0,17.0,-0.8) is most likely a
**trigger volume**, with the destination in the EVENT script: a different blob the mod has never
read. That is B2 (`notes\EBP2_DBG_format.md`, `notes\ebp_routines_evctrl.csv`,
`notes\dbg_symbols_evctrl.csv`; the event VM has its own `mapjump`, `setmapjumpmode`,
`setposparty_mapjump`, `lockmapjump`; confirm window `FUN_002a6190`). **Prove B1 dead with the line
above before spending a session on B2.**

## Session 104 — 2026-07-31 — [exits] NOT the event script: five script containers, and a placard in a group nobody reads

KEYWORDS: B2 refuted event ebp setmapjumpgroup zero of 346 evt_t debug warp mrm_f0100 five script
containers DAT_02098e10 stride 0x288 FUN_0026c8c0 FUN_00263ff0 FUN_00266d10 DAT_02099d70 container
census map_script_census field-sign group 1 areaId 32 destIdx 2 SAKIYOMI grm_a0380 map 313 staircase

The instruction was to stop proving B1 dead and go straight at B2 — *"find whatever event fires that
yes/no prompt or at least the dungeon transition; we're looking for a very different script."*
**B2 as it was written is refuted, from the game's own shipped data, and the search moved twice.**

### 1. An event script NEVER arms a walk-onto transition — 0 of 346, measured

`plan_master\us\event\<area>\<unit>\<unit>.ebp` is extracted: **346 event scripts**. Swept for the two
calls the exit reader already knows (`4f K K 5d 1e 01` = `setmapjumpgroup(K)`, `4f/4f/4f 5d 8d 00` =
`mapjump`):

- **`setmapjumpgroup`: 0 files.** Not one event script in the game arms a map-jump group.
- **`mapjump`: 14 files — and 13 of them are `evt_t00NN`**, the developers' test-warp events (long
  runs of `mapjump(dest, entrance=0, flags=0x0/0x4)` over unrelated maps). The single real one is
  **`mrm_f0100.ebp` → `mapjump(dest=612, entrance=2, flags=0)`**: a story move fired by a cutscene,
  which is exactly what an event transfer looks like and is NOT a surface you walk onto.

So "the surface is a trigger and the jump lives in the event blob" cannot be the mechanism: the two
halves of a walk-onto transition (S64) are `setmapjumpgroup` + `mapjump`, and **the first half never
appears in an event script.** Map 313's own routine list did point at the event domain —
`SAKIYOMI_grm_a0380` is "pre-read event grm_a0380", and `grm_a\grm_a0380\grm_a0380.ebp` is a real
7,104-byte file — but that file contains **neither** call. (Event `.ebp` headers also differ from
`ctrl.ebp`: routine table not at `+0x18`. Not pursued; it does not matter now.)

### 2. The mod has been reading ONE of FIVE script containers (conf 0.99)

Not the answer to 313, but a real architectural hole and it is now instrumented. `MapScript::
ReadExitDests` reads the pointer at `DAT_02098e10 + 0x00`. That is **script container 0**, and there
are five:

| fact | evidence |
|---|---|
| 5 containers, stride `0x288`, base `DAT_02098e10` (= `NavRva::HANDLE_TABLE_BASE`) | `FUN_00266d10` memsets `0x288` bytes five times from that base |
| each stamped with its own index at `+0x28` | same loop: `*(int*)(p+0x18) = i` on `&DAT_02098e20` |
| `FUN_00263ff0(i)` → the i-th container | `return &DAT_02098e10 + i*0x51` (0x51 qwords = 0x288) |
| `FUN_0026c8c0(i, blob, entry)` INSTALLS a blob into container i | writes `(&DAT_02098e10)[i*0x51] = blob` and sets the current-context global `DAT_02099d70` |
| every container's blob shares the header layout the exit reader parses | `FUN_00264b90(idx, c)` reads `container[c]->blob + 0x54` for ANY `c` |
| field objects carry their container index | `obj+0x15` indexes the same array (`FUN_00263880`, `FUN_002675c0`, `FUN_00263050`) |

Container 0 is special-cased throughout (`FUN_0026c8c0`'s `param_1 == 0` branch does the full map
reset), and the object-enable sweeps walk containers `0..2`.

### 3. What shipped — `map_script_census.{h,cpp}`, ALWAYS-PRINTING, LOG-ONLY

The falsifier Session 103 demanded, widened from one container to five. Per container, on every map,
whether or not anything is found:

- blob pointer, `routineTable=+0x…`/`namePool=+0x…` (`(ABSENT)` when the slot is empty), routine count;
- the four map tables' counts (`+0x54` / `+0x84` / `+0x70` / `+0x8c`), so a container holding
  something that is not a map blob is visible as that rather than as a silent zero;
- **routines scanned / spans read / spans empty / spans unreadable** — the line whose absence made
  S102 untestable;
- every `setmapjumpgroup(K)` and every `mapjump(dest, entrance, flags)` with the routine index and
  name that holds it (ALL occurrences, not the first), destination resolved to its area name;
- routine names for containers 1-4 (container 0 already dumps them);
- and the closing line **`MAP-JUMP GROUPS ARMED ANYWHERE: …`**.

Caps are counted and printed, never silent. Its latch only sticks once a container actually held a
readable blob, so the one printing is not spent on the frames where the map is still streaming in.

**It changes no behaviour.** It returns nothing, nothing consumes it, it adds no exit to any list, and
`ReadExitDests` is untouched — deliberately not refactored to share code with a diagnostic (S101: make
the new path unreachable from the working one rather than testing your way to confidence). The blob
format constants come from the shared `map_script_internal.h`, which is what stops the two drifting.

### 4. THE LEAD THAT REPLACED B2 — it is not a script at all

The `+0x70` field-sign table, which the mod has printed on every map for sessions and reads only
**group 0** of:

```
map 315:  6 records, ALL group 0.  Live: (10.99,4.29,116.00) and (180.00,9.21,54.26)
          -- the map's two seam surfaces, x[11.0..16.3] z[116.0..123.5] and x[153..180] z[52..62].
map 313:  7 records.
   g0[0]  (174.00, 9.20,54.97)  areaId=65535            -> the 315 exit's surface x[174..189] z[52..62]
   g1[5]  ( 30.16,13.00, 4.25)  areaId=32  destIdx=2    -> the UNCLAIMED staircase seam,
                                                           x[30.0..34.0] z[-5.8..4.2], centroid Y=17
```

Group 0's live records land 1:1 on the ordinary walk-onto transitions on both maps. Map 313's ONE live
**group-1** record sits inside the unclaimed seam's x-range, 0.05 m off its z-edge, 4 m below its
centroid — the foot of the staircase — and it is **the only field-sign record in any log this project
has ever taken whose `areaId` is not `0xFFFF`**. A live record in a higher group is therefore a second
CLASS of transition placard, and the doorway test throws it away by group before anything reads what
it says.

**Open, and printed rather than guessed:** `areaId = 32` has **no** `planmapname` name — its offset word
is 0 in `planmapname.bin`, so `ResolveFullAreaName(32)` cannot name it. Either word[5] of a `+0x8c`
record is not a planmapname map id for this record class, or this destination genuinely has no area
name. `entity_postscan.cpp`'s sign line now prints the RESOLVED NAME beside the id and flags the record
`NOT GROUP 0 BUT CARRIES A DESTINATION -- a second transition class`, so one log answers it.

### 5. State and what to read in the next log

**BUILT AND DEPLOYED on `combat-system`. Log-only; there is nothing to play-confirm and nothing that
can regress** — no exit, name, route or announcement can move, because no consumer exists.

On map 313, in order:

1. `MAP-JUMP GROUPS ARMED ANYWHERE:` — if it lists `2` only, **B1 is dead**: the group-1 staircase is
   armed by nothing in any loaded script container, and the binding is the field-sign record.
   If it lists `1` as well, name the container and routine that armed it and the reader follows it.
2. `container N:` lines for `N != 0` — the first time this project has seen what else is loaded.
   A container with `routineTable=+0x0 (ABSENT)` holds something that is not a map blob.
3. `sign g1[5] … dest="…"` — whether area 32 names anything.

Do NOT re-attempt: sweeping event `.ebp` files for a walk-onto binding (0/346, above), and offline
`.mpk` map-script analysis (the name pool is packed on disk — already recorded in `GameArchitecture.md`).

## Session 105 — 2026-07-31 — [exits] An event-fired transition arms NO group: the staircase is bound by elimination

KEYWORDS: map 313 staircase Royal Palace Cellar Stores 567 routine 4 event routine mapjump flags 0x1
setmapjumpgroup absent group 1 unclaimed elimination binding BindUnclaimedSurface groupInferred
field-sign group 1 areaId 32 Pharos at Ridorana STRUCK container census

### The census answered it on the first load

```
c0 routine[1] "__MJ_CTRL000": setmapjumpgroup(2) @+0x6
c0 routine[1] "__MJ_CTRL000": mapjump(dest=315 "Garamsythe Waterway: Northern Sluiceway", entrance=2, flags=0x0)
c0 routine[4] "?C?x???g????": mapjump(dest=567 "Royal Palace: Cellar Stores", entrance=1, flags=0x1)
container 1..4: NO BLOB INSTALLED
CENSUS: 1/5 container(s) hold a blob | 25 routine span(s) read | MAP-JUMP GROUPS ARMED ANYWHERE: 2
```

Routine 4 is the `イベント…` ("event") routine S102 named and discarded. **Its span was read, its
`mapjump` was found, and the destination resolves: map 567, Royal Palace: Cellar Stores.** It arms no
group, and nothing anywhere arms group 1.

> **An event-fired transition supplies only HALF of S64's binding.** The walkmap's group tag is static
> map data; `setmapjumpgroup(K)` is what a DOOR CONTROLLER does at runtime. An event never calls it,
> because the event decides whether the party moves. The surface and the destination are authored in
> the same blob and nothing joins them.

**The exit was never listed in any build**, and the tester said so before the log did: the pre-S102
`__MJ_CTRL` name filter rejected routine 4, and S102's replacement group filter rejected it again.
S102 did not delete an exit; it failed to add one, twice. *"It wasn't in builds going back before
session 90"* — correct, and the earlier draft of this entry blamed S102 for a deletion that never
happened.

### The join: elimination, not proximity

`BindUnclaimedSurface` (`exit_scan.cpp`), run before `PublishClaims`:

> exactly ONE swept surface that no routine's group claims **and** exactly ONE group-less candidate
> whose destination resolves to a real area name ⇒ they are each other's. **Any other count binds
> nothing** and logs the counts.

Arithmetic over the game's own two lists. No proximity test, no invented geometry — the failure mode
of every refuted exit model, and of this session's own first hypothesis (below). It is **unreachable
on a map that is already correct**: such a map has zero unclaimed surfaces, so the first count is 0 and
the function returns before deciding anything. `ExitDest::groupInferred` carries the provenance so no
log line can present an inference as a reading (`routine[4] "…" [group INFERRED]`).

The gate in `map_script.cpp` that replaced "must arm a group", since S83/S84's phantom exits are the
real risk: `flags == 0x0A` still excluded; a group-less candidate **must resolve to a real area name**
(with no walkmap tag vouching for it, the destination must); and it is bound to nothing until the 1:1
test above passes.

**Falsifier, already shipped:** the binding is published to `g_claims`, so walking the staircase makes
NavTrace's `CROSSING ORACLE` compare mod-claimed against actually-arrived and print `MATCH`/`MISMATCH`
by itself.

### STRUCK, same day it was raised — the field-sign group-1 destination (S104)

313's live group-1 `+0x70` record sits at `(30.16,13.00,4.25)`, inside the staircase seam's x-range,
0.05 m off its z-edge, and carried the only non-`0xFFFF` `areaId` this project had ever logged. It
looked like the destination for exactly the surface that lacked one.

**`areaId = 32` resolves to "Pharos at Ridorana". The script says 567, Royal Palace.** Word[5] of a
`+0x8c` record is not this class's destination. It survived less than one map load — because the
diagnostic printed the RESOLVED NAME rather than the id it was tempting to ship.

### State — PLAY-CONFIRMED

The tester walked it, and the oracle confirmed the inferred binding without being asked to:

```
[NAV-DIAG] elimination binding: the ONE unclaimed surface (group 1) is routine[4] "?C?x???g????"'s
           -> dest=567 ("Royal Palace: Cellar Stores") -- INFERRED, no script arms this group
[NAV-DIAG] exits: controllers=2 surfaces=2 listed=2 | dropped: nogroup=0 notused=0 unreachable=0
[NAV-TRACE] CROSSING ORACLE: left map 313 via seam g1 (3.5m from its near edge)
            | mod claimed 567 ("Royal Palace: Cellar Stores")
            | ACTUALLY ARRIVED 567 ("Royal Palace: Cellar Stores")  <== MATCH
```

`NO CONTROLLER CLAIMS THIS GROUP` is gone from 313. The next map on (567) also matched
(`left 567 via g1 | claimed 568 "Royal Palace: Cellars" | ARRIVED 568`).

### OPEN — the 2-against-2 case, and the trap not to fall into

**Map 568 (Royal Palace: Cellars) has TWO event-bound transitions and TWO unclaimed surfaces**, so
elimination correctly declines and the map lists nothing:

```
routine[12] "door1"        arms NO group -> dest=567 ("Royal Palace: Cellar Stores") entrance=2 flags=0x0
routine[13] "door_gunbit"  arms NO group -> dest=569 ("Royal Palace: Lower Halls")   entrance=1 flags=0x0
elimination binding: 2 unclaimed surface(s) vs 2 group-less destination(s) -- not 1:1, nothing bound
surface g2: 4 polys at (43.7,-0.0,118.0)    <== NO CONTROLLER CLAIMS THIS GROUP
surface g1: 10 polys at (12.6,-8.0,183.3)   <== NO CONTROLLER CLAIMS THIS GROUP
```

**No regression** — those two were dropped by the old `group <= 0` gate as well, so the map lists
exactly what it always did. But two real doors are still invisible there, and the routine names
(`door1`, `door_gunbit` — plain ASCII, plainly doors) say this class is COMMON, not exotic to 313.

**DO NOT pair them by authoring order** (routine[12]→g1, routine[13]→g2). That is the S46/S58
"`__MJ_CTRL<N>` owns slot `N+1`" rule wearing a new hat, and it was refuted twice. The discriminator
has to come from the game's own data. Untried candidates, in order of promise: the `+0x70` doorway
SCENE OBJECTS this map has two of (`Door` x2, one at `(17.42,-8.06,184.00)`, ~4.9 m from surface g1) —
but that is proximity, which S92 caught wrong in both directions on one map; and the `entrance`
literal read against the DESTINATION map's arrival table, which needs cross-map data the reader has
always refused to depend on.

### (superseded by the above) What to check on map 313

On map 313 expect:
`elimination binding: the ONE unclaimed surface (group 1) is routine[4] "…"'s -> dest=567 ("Royal
Palace: Cellar Stores") -- INFERRED, no script arms this group`, then `exits: … listed=2`, the
`NO CONTROLLER CLAIMS THIS GROUP` line **gone**, and a routable Exit at ~(32.0,17.0,-0.8).
**On every other map the listed count must not move** — a new exit on a map that was already correct
is a regression, not a win. Then walk it for the `CROSSING ORACLE … MATCH`.

### The scan hole, diagnosed: a routine under 0x40 bytes is DROPPED WITHOUT BEING READ

313 reported `spans read=25, empty=0, unreadable=6`. Those six were never read. In `map_script.cpp`
(and the census, which copies it):

```cpp
size_t span = spanWanted;                 // = min(end - start, CODE_SPAN_MAX)
while (span >= 0x40 && !BlobBytes(blob, start, code.data(), span)) { span /= 2; ... }
if (span < 0x40) { /* dropped as SPAN UNREADABLE */ }
```

When `spanWanted < 0x40` the loop body **never runs** — no read is attempted — and the routine is then
dropped by the `< 0x40` test as "unreadable". **The 0x40 floor was written for the LAST routine
only**, whose span is an unbounded guess that must be halved until it lands in mapped memory. It
became a minimum routine size for every routine on every map.

- A `setmapjumpgroup(K)` is 6 bytes and a `mapjump` is 12, so a 40-byte routine can hold either.
  **This is in the shipped reader, not only the diagnostic.**
- `spansUnreadable` conflates "too short to attempt" with "the read faulted" — the S103
  orphaned-diagnostic shape again, in the very code written to end it.

**Fix, deliberately NOT made this session** (it changes the shipped reader on every map and the
tester's build is confirmed working): read whatever the span says however short, halve **only** after
a read has actually failed, and count the two causes separately. Do not just lower the floor — the
floor is not the mechanism, the missing read attempt is. Full entry in `debug.md`.

### NEXT SESSION, FIRST ITEM

The 2-against-2 maps above. Map 568 is the worked example and its two doors are real and currently
invisible. Whatever discriminator is chosen must come from the game's own data, and **the scan hole
must be fixed first or measured around** — a routine the reader never read cannot be counted, and both
of this problem's counts (unclaimed surfaces, group-less candidates) are counts.

## Session 106 — 2026-07-31 — [navigation] The palace sneak: danger zones + the midpoint-attribution defect

**KEYWORDS: Royal Palace Cellars 568 sneak minigame guards Imperial npcdic 694 servant door_gunbit 569
danger zones penalty disc path_danger target-conditional tight quarters z=121 lane start poly edge
midpoint attribution sweep stop re-attribute no-frontier false No path capture distance scene-gap
controls overlay tutorial re-arm capture restart 0x0f glyph selector 0x48 action 0x0f DAT_01f80f90
binding bank FUN_002b5b60 menuhandbook hctgf hdatg probe_key_bindings probe_controls_overlay DEFERRED**

Commits: `1a6b9dc` (danger zones) + `01b7746` (start-edge re-attribution), separate so either reverts
alone. Built, NOT yet play-confirmed.

**The tester's palace session (log 12:35) diagnosed end to end.** Map 568 lists 0 exits (the S105
2-against-2, unchanged); the tester routed to the raw "Door 2" object — which works and is the
approved approach for this puzzle. Three findings:

1. **The "controls overlay" mystery is CLOSED, by the tester**: the panel re-appears because getting
   too close to a guard fires a capture ("Vaan captured" routine, `distance` native x8 in
   rrp_a02.ebp) and RESTARTS the minigame — nothing on 568 records success (story flags are written
   on 569), so the instructions re-arm every time. The overlay itself is engine-fired (the map script
   calls no tutorial native) and is very likely the menuhandbook image viewer — its `.bin` (`hctgf`)
   carries per-panel TITLE STRING IDS (0x116e9..0x116f7), its `NNN.dat` (`hdatg`) are baked page
   graphics. **DEFERRED with W1 below** (user instruction: pathing only this session).

2. **12 of 17 spoken "No path"s were false** — search collapse from the z≈121 lane, NOT the guard:
   the breaching first leg crossed the room-sized start triangle, midpoint attribution landed on the
   seed's own (protected) edge, the loop broke with no retry, no banked prefix (`firstBad=1`), and
   `BuildFrontier` aimed past the pinch and failed → `pass=no-frontier`. **Fix (`01b7746`)**:
   re-attribute by the sweep's STOP point before conceding; the seed rule holds only when the stop's
   portal is also the seed's edge. Falsifier in the log: the new `re-attributed by the sweep stop`
   line, and lane starts producing `plan=Route` when the way is open.

3. **Danger zones (`1a6b9dc`)**: the script's notice radius exists only as literals in story-script
   code, so it is priced, never cut — soft discs (r=9m est., w=2000) around npcdic-694 actors, LIVE
   positions per replan, **armed ONLY when the route target is the 568 door at (38.60,0.00,117.85)**.
   Routing to the Palace Servant (3.4 m from the guards) or anything else passes null — search
   byte-identical (S101 unreachable-not-skipped). USER AUTHORIZED map-specific data; user chose
   SILENT SHAPING ONLY (no proximity speech/tone). A log-only `DANGER scene-gap` diagnostic on table
   maps records pre-gap player position + actor distances so every real capture measures the radius.
   Map 569 (capture rects, eight soldiers) is known and deliberately NOT entered yet.

**Binding user decisions this session:** partial-route speech REJECTED ("a hard progress block —
partial route is worse than no route"); FRONTIER SUPPRESSED stays. Glyph speech = BOUND KEY NAME.
Danger pathing may be map-specific but arms per target only.

**DEFERRED, groundwork done (do not re-derive):** the 0x0f button-glyph chain is fully mapped —
`0f 48 80` in rrp_a02 msgs 23/24/25/28 → idx 8 → action 0x0f → DIK byte at `DAT_01f80f90[action]`
(RVA 0x1E60F90, stride 0x1c, col 0 = Main; builder FUN_002b5b60 abs 0x2b5b60; Confirm Type
`DAT_01f82d20+0x64` swaps only actions 0x0d/0x0e; DIK→glyph tables DAT_01df1030/01df0e80; keyboard
glyph table DAT_01e0ce30 rebuilt from LIVE bindings). Predicted palace call key: F (DIK 0x21).
Probes authored and user-run pending: `probe_key_bindings.js`, `probe_controls_overlay.js`.
menuhandbook_* extracted to `..\FFXII-Decompile\extracted\...\handbook\`.

**Verify next play (568):** `zones armed:` on a Door-2 route and `danger=` in its `costed:` line —
and NEITHER on a servant route; a z≈121-lane start giving `plan=Route` with the way open; honest
"No path" only with the guard at post; `DANGER scene-gap` lines on any capture; CROSSING ORACLE
568 → 569 on success. Regression gate: no `pass=seam`; working routes still `pass=mesh`.

## Session 107 — 2026-07-31 — [navigation] Sneak assist (F10): clamp the guards' notice check

**KEYWORDS: sneak assist F10 toggle default off clamp script distance native 0x0290 FUN_003448f0
RVA 0x2248F0 off-by-one name table 0x028f FUN_0026b4c0 return slot ctx+0xA8 ctx+0x11 stride 0x28
tag 3 FUN_004686d0 sqrtf horizontal distance actor +0xB8 write-category exception danger table
MapHasRow map 568 capture Vaan captured mod menu sneak_assist phrasebook Controls.md README**

Commit: `309e5ce`. Built, NOT play-confirmed. **Second write-category exception in
the project, after auto-walk — USER-AUTHORIZED explicitly this conversation.**

**The user's idea, corrected by the data.** Proposal was "set the guards to *let him pass* once the
shout has been made". There is **no such flag**: `とおしてあげる` is a ROUTINE, and the fail condition
is a watcher calling a distance native and branching to `ヴァン捕獲`. So the override belongs at the
MEASUREMENT, not at a flag — while armed, the native's result is replaced with 9999, and the "too
close" branch can never be taken.

**RE, and a name-table correction worth keeping.** The generated table pairs "distance" with native
`0x028f` → `FUN_003482f0`, which is a two-line wrapper round a bitmask setter — plainly not a
distance. The bytecode settles it: `rrp_a02.ebp` has **8 `CALLACT 0x0290` sites and zero `0x028f`**,
and the `0x0290` handler `FUN_003448f0` (RVA `0x2248F0`) pops two coords + an actor id, resolves the
actor's `+0xB8` transform, and measures `sqrtf(dx²+dz²)`. **Off by one SLOT** — the standing "resolve
natives by BEHAVIOUR" rule paying for itself. Full offsets in GameArchitecture.md.

**One honest gap, handled rather than guessed:** the result word's representation (float bits vs
converted int) is NOT settled by the decompile — `FUN_004686d0` returns sqrtf's float and
`FUN_0026b4c0` stores an undefined4, with the XMM→GPR move invisible in the decompile. Rather than
ship a guess, the clamp **decides per call from the stored value's own magnitude** (a real distance
reads as 0.001..100000 as a float; as an int the same bits read ~1e9) and **logs which reading it
saw on the first clamp per arming**. One log line settles it permanently.

**Boundaries (all in `sneak_assist.h`, none negotiable):** default OFF; the unarmed path is the
hook's FIRST branch, so the write is unreachable rather than skipped; effective ONLY where
`PathDanger::MapHasRow` (the S106 table — map 568 today); and **no persistent game state is written**
— the only write is the VM's return slot for the call being serviced, so toggling off restores
vanilla on the very next check. Accepted limit, documented for the tester: `distance` is generic, so
v1 clamps every call on a table map while armed; if a sequence ever stalls, F10 off. Map 569's catch
is capture RECTS, so F10 does nothing there until 569 gets its own mechanism.

Wiring reused rather than reinvented: `Hooks::InstallTyped` + a constexpr in `nav_rva.h` (no
`mod_config.ini` involvement — that file belongs to the RVA validator); `ModMenu` row + `F10` through
`ModMenu::CycleSetting`, so the value has exactly ONE place it changes, persists and is announced;
phrasebook rows for the new strings (plan-approved wording — flag before rewording). `README.md`,
`Docs\Controls.md` updated.

**Verify next play:** F10 anywhere speaks the new state; on map 568 with it ON, the log shows
`[SNEAK] clamp ACTIVE on map 568: script distance <d> -> 9999 (float|int slot)` once per arming and
walking the corridor near a guard no longer triggers a capture; with it OFF, captures still happen
(vanilla preserved) and no `SNEAK` clamp lines appear. S106's own verification greps still outstanding.

### Addendum — WHAT the 8 call sites are, asked by the tester ("could this softlock me?")

Disassembled all 8 `CALLACT 0x0290` sites in `rrp_a02.ebp` (opcode table: `notes\athena_opcodes.md`;
`PUSHDBG` operands run sequentially at each site, which is the alignment check):

- **2 sites (`0x248e3`, `0x24b2e`) are the CAPTURE TEST, and they are unambiguous:**
  `CALLACT(0x290) → PUSH[0xe](0x79) → OPLSE(<=)` then a branch body of
  `CALLPOPA(0x15|0x19) CALLACTPOPA(0x415) (0x416) (0x273) (0x3b7) (0x553) (0x3ed)` — the "he noticed
  you" reaction. **The comparison is `distance <= X`, so it fires when the number is SMALL: a large
  clamp makes it FALSE and can only ever SUPPRESS the capture, never trigger anything.**
- **6 sites are three IDENTICAL pairs** (variable sets `0x22`/`0xb`, `0x28`/`0xd`, `0x2e`/`0xe` —
  three copies of one block, matching the map's three `先行促し01/02/03` "urge-to-advance" actors).
  Their results are POPped into variables and compared elsewhere, so the direction is NOT established
  offline. **Not resolved, and deliberately not guessed.**

**The tester's actual worry — the "no going upstairs until you talk to the servant" gate — is NOT
distance-driven.** That gate and its siblings are RECT/TOUCH events: `seteventwakerect` ×9,
`setrect` ×6, `rectdisable` ×5, `settouchuconly` ×6, `istouchuc`/`istouchucsync` ×25, with the named
rect actors `地下イベントレクト`, `練習用レクト`, `引き返し禁止`, `先行促し01-03`. Sneak assist
changes the return value of ONE native and touches no rect, flag, or touch test, so story gates,
trigger volumes and dialogue fire exactly as they do in vanilla.

**Standing guidance to the tester, and the intended usage: leave it OFF and switch it ON only for
the sneak run itself.** That is not a workaround for a weakness — it is the design, and it bounds
the six unresolved sites to the seconds they are needed.

**Available hardening if the six ever misbehave (NOT built — would need approval):** clamp only when
the ORIGINAL distance is small (below ~25 m). Any check of the form "too far ⇒ force something" fires
on a LARGE value, so leaving large values untouched removes that entire class from the blast radius,
while the capture test (which fires small) is still defeated.

## Session 108 — 2026-08-01 — [navigation] REVERT the S106 pathing; the toggles stay

**KEYWORDS: revert danger zones penalty disc Door 2 unroutable corridor paid terrain=8000 no route
map 568 sneak assist F10 kept MapHasRow whitelist re-attribution reverted d18943e 1a6b9dc partial
revert one-corridor room a penalty is only a detour when a detour exists**

Tester deployed S106+S107 and reported: **no path to Door 2, where it had worked before.** Both S106
pathing commits are OUT. **Sneak assist (F10, S107) is UNAFFECTED and stays.**

**The log named the mechanism, and it is worth keeping.** On every armed request:

```
[DANGER] zones armed: map 568 target-door matched; 2 disc(s) r=9.0 (20.8,-8.0,125.1) (19.4,-8.0,126.2)
[NAV-ROUTE] costed: ... terrain=457 foreignSeam=7 danger=29 ... | corridor paid terrain=8000 other=500
[NAV-ROUTE] drain seq=1: target="Door 2" ... plan=Frontier legs=4
[NAV-ROUTE] FRONTIER SUPPRESSED for "Door 2" -- 4 legs reaching 4.8m short; spoken as No path
```

> **A PENALTY IS ONLY A DETOUR WHEN A DETOUR EXISTS.** The guards stand in the ONLY corridor to the
> stair, so the discs did not push the route wide — there was no wide. The search did what a soft
> cost tells it to do and bought its way out through the next-cheapest thing available, which was
> **ground the party's own floor class cannot stand on** (`corridor paid terrain=8000` = four
> kTerrainPenalty crossings). That corridor then failed validation, and the honest frontier was
> suppressed as designed. **The S96 lesson ("nothing severs the graph; everything difficult is
> expensive") does not license pricing the only way through: in a one-corridor room a price and a
> cut are the same move with extra steps.** The 9 m radius estimate made it worse, but the radius
> was not the defect — the placement was.

Also measured: **the S106 re-attribution never fired** (`re-attributed by the sweep stop`: zero
occurrences). The late `pass=no-frontier` failures show `attempts=1 banned=0`, i.e. they break
BEFORE the re-cost step, so that fix addressed a case the log does not actually contain. Reverted
too — an unexercised change on a hot path is cost without evidence.

**What was done.** `d18943e` = `git revert 01b7746` (clean, self-contained). The danger zones were
PARTIALLY reverted rather than `git revert 1a6b9dc`, because S107's sneak assist depends on the
table: `Disc`/`ActiveZones`/`PenaltyAt`, the `PathSearch::Run` parameter, the pricing block, the
`danger=` counter and the planner's arming are all gone; `PathDanger::MapHasRow` (the F10 map
whitelist) and the log-only `scene-gap` capture diagnostic remain, with the table trimmed to
`{mapId, nameIdx}`. `EntityList::CollectPositionsByNameIdx` stays — the diagnostic uses it.

**Verify next play:** map 568 Door 2 routes again as it did before S106; no `zones armed:` or
`danger=` lines anywhere; `[SNEAK] script-distance hook installed` still present at startup and F10
still speaks its state. The S106 memory entry and MEMORY.md were corrected — the danger zones must
not be remembered as shipped.

## Session 109 — 2026-08-01 — [navigation] Sneak assist can only be armed where it is meant to act

**KEYWORDS: sneak assist F10 no-op unsupported map auto-off map transition teardown forced off
startup safety net SetSilently silent persist ModMenu one choke point AvailableHere OnMapTeardown
never carries across a map arming is deliberate**

Tester instruction, on the S107 feature: **auto-off on a map transition** so a forgotten toggle
cannot affect other maps, and **make `F10` a no-op off the maps it is meant for** — "it should only
be for getting around guards". Both shipped; the clamp itself is unchanged.

**Three gates now stand between the feature and an unintended map**, and the point of having all
three is that no single one of them has to be perfect:

1. **`F10` is a NO-OP where the feature does not apply.** `SneakAssist::AvailableHere()` (a
   `PathDanger::MapHasRow` test) gates the key in `nav_commands.cpp`: off-table it does not toggle,
   does not speak, and writes one log line. **Silent by design** — this is the same silence `;` and
   `7` already use for "nothing here to report", and speaking "not available here" on a key the
   player pressed on the wrong map would be exactly the filler the standing rule forbids.
2. **Forced OFF on every map teardown**, from `NavHooks::HookedTeardown` beside
   `PathPlanner::OnMapTeardown` — unconditional, not "only when leaving a covered map", because the
   state that matters is *armed while the player walks somewhere new*.
3. **Forced OFF at startup**, because a crash or an old settings file could otherwise hand a launch
   a `sneak_assist=1` nobody chose this session.

**The auto-off is SILENT, and that needed a new choke point rather than a raw write.**
`ModMenu::SetSilently` sets + persists + logs through the same path `Adjust` uses (CLAUDE.md's "one
place a setting's value changes"), minus the speech. Announcing a setting nobody touched, on every
single map load, is filler; the log records it instead.

**Net effect: the only way the toggle can be on is that the player deliberately armed it, this
session, while standing on the guarded map it acts on.** `README.md` and `Docs\Controls.md` updated
to say so in the tester's terms.

**Verify next play:** `F10` on any ordinary map says nothing and logs
`F10 ignored: this map has no guarded sequence`; on map 568 it speaks On/Off as before; walking
through any transition while it is on logs `map change: sneak assist forced OFF` and the next `F10`
press on the new map is a no-op unless that map is covered.

## Session 111 — 2026-08-01 — [navigation] The search gave up after ONE attempt while a route existed

**KEYWORDS: Door 2 map 568 No path attempts=1 banned=0 final approach leg index vs distance
kFinalApproachDist two-corner route re-cost start poly edge re-attribution sweep stop restored
S106 01b7746 reverted on contaminated evidence positional not temporal 0x07A01000 search breadth**

**The tester rejected the previous diagnosis and was right to.** I had claimed a barrier appeared
when the guard was called. It does not exist. The `refused eff-flags: 0x07A01000 x305` I built that
on is a count over a search that expanded **363** polys; the SUCCESSFUL route in the same minute
(seq 44, `expands=22`) shows the identical flag **x1**. **That number measures how far the search
wandered, not what blocked it** — a symptom read as a cause, which is the S93 lesson ("a report
about BEHAVIOUR is not a report about DATA") in a new costume.

### What the log actually says

The failure is **POSITIONAL, not temporal**. The ramp mouth is at ~(16,-8,120.4):

| start x | outcome |
|---|---|
| ≤ 16.1 (seq 32-35, 44, 45) | `plan=Route legs=4` — including AFTER the guard returned |
| ≥ 17.4 (seq 40-43, 46-52)  | `NoPath` |

Calling the guard coincided with the tester walking northeast, out of the working region — which is
what made it look like the call caused it. **And a route from the failing region demonstrably
exists**: seq 38 returned `plan=Route legs=18` from (20.73,-8.00,127.99), the exact coordinate that
answers `NoPath` four seconds later.

**Every single failure carries `attempts=1 banned=0`** — the attempt loop broke before re-costing
anything. Two guards did it, and both are now keyed on measurements instead of indices:

1. **The final-approach rule was an INDEX test.** `firstBad >= poly.size() - 1` makes leg 1 of a
   TWO-corner route simultaneously the first and the last, so *any* breach broke the loop. seq 51:
   `bad=1 len=19.22m reached=1.79m stop=(21.5,-8.00,121.3)` with the target at (38.6,117.9) — the
   walk died **17 m** from the goal and was treated as an arrival problem. Now the test is the
   STOP's distance to the target against `kFinalApproachDist = 2 * kArrivalTol`, so the 2.55 m stop
   its evidence came from is still protected and a corridor problem is not.
2. **Start-poly re-attribution RESTORED** (S106's `01b7746`, reverted in S108). The revert's stated
   reason — "it never fires" — was measured on a log whose routes the danger zones had already
   deformed, so the branch never got the chance. In the clean log it fires: seq 40 is
   `len=8.00m reached=7.04m` with the midpoint on the seed's protected edge. **A revert justified by
   absence needs a log in which the change COULD have fired.**

### Blast radius — why other maps cannot move

Both edits live **after** `rep.ok` has already failed AND all three `PathRepair` rungs have failed,
on the path to `break`. Every request reaching them is one that today ends as `No path` or a
suppressed frontier, so **a route that validates never executes a line of this**. Work stays bounded
by the existing `kMaxAttempts=4` / `kMaxTotalExpand`. `path_funnel`, `path_validate`, `path_corridor`,
`path_repair`, `path_march`, `nav_mesh`, `nav_footprint`, `map_query`, `path_surface_goal` remain
**byte-identical to pre-S106** — verified by diff, not by assertion.

**Verify next play:** from the east side of 568, `\` produces a route (expect the long way round,
`legs≈18`) instead of "No path"; the log shows `attempts` > 1 with a `replan:` line naming the
re-costed portal, and where applicable `re-attributed by the sweep stop`. Regression gate unchanged:
315 still `pass=mesh`, no `pass=seam`, and no new `attempts>1` on routes that already validated.

## Session 112 — 2026-08-01 — [input] The game owns F9; the beacon moves to F11 (bare press only)

**KEYWORDS: on-screen keyboard overlay F9 Hide On-Screen Keyboard Space Close controls panel
audio beacon F11 bare press Shift+F11 NVDA modifier guard DIK_F11 0x57 config screen not evidence
unclaimed pane census obj0 class RVA menu_reader HookedFocusSet map 568 pane-entry spam**

**The tester photographed the panel that keeps interrupting them.** It is the game's own
**on-screen keyboard**, and its footer reads `F9  Hide On-Screen Keyboard` / `Space  Close`.

**The mod had `F9` bound to the audio beacon.** The mod is strictly read-only on input and cannot
swallow a key, so every beacon toggle ALSO flipped that full-screen panel, and every dismissal of
the panel flipped the beacon. The tester pressed F9 three times in the log that came with the
screenshot (`set: audio_beacon=1/0/1`).

> **"THE CONFIG SCREEN DOES NOT LIST IT" IS NOT EVIDENCE THAT A KEY IS FREE.** Every mod F-key row
> in `Controls.md` carried "free — game binds F1/F2/F3 only", derived from the game's **Controls
> configuration screen** — which lists only REBINDABLE actions. The game has bindings it never shows
> there. The on-screen-keyboard overlay is the game showing its own layout, and it is the instrument
> that should have been consulted. Struck in `Controls.md`, with the overlay's real bindings recorded
> — including `1 Game Speed/Change Group`, `2 Lock On`, `3 Change Group`, which **contradict this
> file's own Session 44 correction**. Neither is struck: one of them is reading a different profile
> and it has not been measured which. **Flagged as unresolved rather than silently overwritten.**

### What shipped

- **Audio beacon `F9` → `F11`**, and **BARE PRESS ONLY**: Shift, Ctrl or Alt held suppresses it,
  because **Shift+F11 is an NVDA command the tester uses while playing** (their instruction). The
  guard is deliberately LOCAL to that one edge registration — every other hotkey keeps the exact
  behaviour it was tested with. `DIK_F9` is now unused by the mod; F9 belongs to the game.
- **Unclaimed-pane census** (`menu_reader.cpp`, LOG-ONLY): when a pane takes the cursor and no
  reader speaks for it, log its `obj[0]` class RVA — one line per DISTINCT class, capped at 12, so
  the per-frame re-opens on 568 cannot flood the file.

**Why the announce did NOT ship with it.** Recognising a surface here is always `obj[0]` against a
known RVA, and this project has never measured one for this panel. Announcing every unclaimed pane
would talk over surfaces that are deliberately silent, and **a wrong guess is a regression in a
working reader** — so the RVA is measured first and the announce ships gated on it. One session with
the panel open settles it.

### Also confirmed this round (from the tester's log, not asserted)

- **The S110 census answered its question: `[SNEAK] native FIRED on map 568`** — the hook IS on a
  function the script calls. The earlier zero was the sequence not having reached the watcher.
- **The clamp works and the representation is settled:**
  `clamp ACTIVE on map 568: script distance 5.25 -> 9999 (float slot)`. **FLOAT**, as the per-call
  discriminator determined at runtime rather than by guess.
- **S111's retry fix works**: `drain seq=21: target="Door 2" from=(20.73,-8.00,127.99) plan=Route
  legs=18` — a full route from the exact coordinate that answered `NoPath` before it. Other start
  positions still fail; that work is not finished.

## Session 113 — 2026-08-01 — [navigation] The guards' own trigger volume, silenced per OBJECT

**KEYWORDS: sneak assist touch test FUN_002677f0 RVA 0x1477F0 native 0x26D 0x525 FUN_0033fa40
FUN_00340bc0 FUN_003407c0 leader slot bit object+0xB8 +0x60 +0x100 +0x228 per-object suppression
guard npcdic 694 scene object identity falsifier capture rect soldier map 568 writes no flags**

**The tester's requirement, and every alternative was already refuted in play:** hear where the
guards are (impossible — stereo panning, camera-relative), route around them (refuted — S106's
zones made Door 2 unroutable), avoid bumping them (impossible — two moving guards in a room of
servants). **So the guards' notice has to go.**

**The distance clamp was necessary but NOT sufficient.** It works — the log proves the write
(`script distance 5.25 -> 9999 (float slot)`) and the window went from **4–7 s to 3 m 02 s** — and
then the sequence still ended. Map 568 has a second catch path.

### The second path, measured

Besides `distance` (`0x290` ×8) the script uses exactly two trigger natives — the instant touch test
(**`0x26D`** ×4, `FUN_0033fa40`) and the waiting one (**`0x525`** ×21, `FUN_003407c0`/`FUN_00340bc0`)
— and **both funnel into ONE function, `FUN_002677f0(object, mode)` (RVA `0x1477F0`)**: leader via
`FUN_003590d0`/`FUN_003588b0`, party-slot bit `1 << leader[0x12]`, tested against a mask at
`*(object+0xB8) + 0x60 | 0x100 | 0x228` — **"is the leader inside THIS object's volume?"** The map's
`recttocircle` ×3 makes those volumes circles around actors. **No `distance3d` / `checkdistance3d` /
`waitdistance3d` anywhere in the file**, so `0x290` + this function are the entire detection surface.

> **THE OBJECT UNDER TEST IS `param_1`, AND THAT IS WHAT MAKES THIS SAFE.** The guards are silenced
> by IDENTITY — their scene objects, resolved from the game's own npcdic name index in the existing
> per-map table — while every other trigger volume on the map answers truthfully: doors, event
> rects, the servant's conversation triggers, the advance-urging rects. **A map-wide suppression
> would have risked stopping the sequence from starting; per-object cannot.**

### Why no event flag can be lost

**Map 568 writes NO story flags.** The sequence's flags are `setquestscenarioflag(4,40)` and `(4,70)`
and both live in `rrp_a03.ebp` — **map 569, after arrival**. Nothing on 568 records success or
failure, so the only thing that matters is reaching the door.

### The falsifier ships with it (tester chose the conservative rule)

The capture volume may belong to an **unnamed rect actor** rather than the guard NPCs — 569's own
names include `捕獲レクト兵士` ("capture rect soldier") and rect actors carry no npcdic name, so the
identity rule would miss them. Every OTHER object that reports the leader inside it, while armed, is
**logged once and left alone**. An unnamed object may be a story rect; silencing one could stop the
sequence. If a capture still happens, the log names the culprit and it becomes one table entry.

### Verify next play (armed on 568)

`touch SUPPRESSED … for a danger actor` naming the guard object; walking into a guard during the
distraction produces **no** capture and no retry line ("You...weren't the swiftest pup in the
litter"); `Wait here until I've further directions` does not re-fire while the window is open; the
servant still starts the chain and the shout still works; `CROSSING ORACLE` prints `568 -> 569`.
With the toggle OFF, captures happen exactly as vanilla. Routing untouched this session.

## Session 114 — 2026-08-01 — [navigation] S113 PLAY-CONFIRMED; and what maps 569 + the corridor still need

**KEYWORDS: sneak assist PLAY-CONFIRMED touch SUPPRESSED Imperial 1 map 568 -> 569 CROSSING ORACLE
seam g2 identical retries kBreachPenalty 500 vs kTerrainPenalty 2000 reachability oracle 569 Lower
Halls npcdic 694 fourteen guards exits controllers=3 surfaces=3 listed=0 nogroup=3 tight quarters**

### S113 IS PLAY-CONFIRMED — the tester got through

```
[SNEAK] touch SUPPRESSED on map 568 for a danger actor (obj=...2D099AC0, mode=0) ...
[SNEAK]   suppressed object is: "Imperial 1"
[NAV-TRACE] TRANSITION FIRED: mapId 568 -> 569
```

**The falsifier stayed silent** — not one `touch REPORTED` line — so the guard NPCs really were the
whole remaining catch, and the conservative object-scoped rule was sufficient. The servant's own
triggers and the shout kept working, which is the property object-scoping was chosen for. Both
halves were needed: `clamp ACTIVE ... 3.50 -> 9999` fired first, then the touch suppression.

### FIX FOR MAP 569 (Royal Palace: Lower Halls) — a one-line table row

> **⚠ STRUCK by Session 116's play test — "a one-line table row" WAS WRONG.** The row and
> `kMaxGuards` shipped in S115 and the tester reports the guard suppression **does not work on 569**.
> The log shows the distance clamp firing there (`27.80 -> 9999`) and **zero** `touch SUPPRESSED`
> **and zero** `touch REPORTED` lines — the touch hook never reaches a guard at all. Why:
> **`rrp_a03` contains ZERO `0x26D` and ZERO `0x525`**, the two natives that funnel into
> `FUN_002677f0`, so per-object suppression has nothing to suppress on 569. It uses
> `seteventwakerect` (`0x3DF`) ×70 and an unresolved `0x26E` ×77 instead — the `捕獲レクト兵士`
> mechanism. **S115's own script census contained this fact and the entry claimed 569 was covered
> anyway.** The row is harmless and stays; 569 needs its own hook. See Session 116.

The tester ended the log on 569, and it is the same problem with more of it:

- **14 "Imperial" actors, ALL `nameIdx=694`** — the SAME npcdic id as 568's pair, at
  `(57.99,0,78.03)`, `(57.97,0,97.98)`, `(95.91,0,118.02)`, `(103.25,0,84.15)`,
  `(102.17,0.79,106.08)`, `(102.18,0.80,141.93)`, `(103.59,1.00,179.93)` and more — several guard
  POSTS along the halls.
- **`native FIRED on map 569`** — the distance native is live there too.

=> **`PathDanger`'s table needs exactly one new row, `{569, 694}`**, and `kMaxGuards` must rise from
8 to at least 16 for this map. No new mechanism: S113's suppression is keyed on npcdic identity.
**Do NOT widen anything else** — 569's catch is documented as capture RECTS (`捕獲レクト兵士` in
`rrp_a03.ebp`), so if a capture still happens there the S113 falsifier will name the rect object and
it becomes a second table entry, exactly as designed.

**Also on 569, and SEPARATE: it lists ZERO exits.** `exits: controllers=3 surfaces=3 listed=0 |
dropped: nogroup=3` — three controllers, three surfaces, all three dropped for arming no group. Same
family as 568's 2-against-2, one size larger. Not fixed here.

**Ground truth banked for the S105 2-against-2:** the oracle measured
`left map 568 via seam g2 ... -> arrived 569`, so 568's **g2 = `door_gunbit` -> 569** and therefore
**g1 = `door1` -> 567**. That is a MEASUREMENT, not a rule — the mod must still DERIVE the pairing;
this is the answer any future discriminator has to reproduce, and the falsifier for it.

### THE "NO PATH" ROUTING DEFECT — diagnosed, NOT fixed (tester deferred it)

S111's two fixes fire correctly (`re-attributed by the sweep stop`, `attempts=4 banned=1`). **The
retries are useless because they are IDENTICAL:**

```
attempt 1  BREACH bad=1 len=8.00m reached=7.04m stop=(23.4,-8.00,121.5)
attempt 2  BREACH bad=1 len=8.00m reached=7.04m stop=(23.4,-8.00,121.5)
attempt 3  ... same ...      attempt 4  ... same ...
```

> **THE RE-COST CANNOT WIN AN ARGUMENT IT IS PRICED OUT OF.** `kBreachPenalty` adds a flat **500**
> per attempt (500->1000->1500->2000) while every candidate corridor `pays terrain=4000` —
> `kTerrainPenalty` is **2000 per crossing**. Four attempts cannot outbid one terrain crossing, so
> A* returns the same disproved corridor every time and 1,445 expansions are spent to say "No path".

**The tester's framing, and it is the right one: this is the standing "stuck in corners / tight
quarters" path-invalidation defect that also happens in the WATERWAYS.** Treat it as ONE defect
class, not a palace bug. (Treasure routing on 568 is explicitly NOT a concern — the tester's issue
is routing round into the corridor that leads to the door.)

Candidate fixes when this resumes, all failure-path-only so a validating route cannot reach them:

1. **Escalate MULTIPLICATIVELY when a retry reproduces an identical breach** (same portal, same
   stop, same leg) rather than adding a flat 500 — directly targets "the four attempts are the same".
2. **A reachability ORACLE (log-only):** on total failure, a pure adjacency flood start->goal, so
   "No path" is separable into *genuinely unreachable* vs *the search gave up*. This is what makes
   the whole class diagnosable instead of anecdotal.
3. **Only then** question whether `0x07A01000` is over-refused. **This one has cross-map blast
   radius (Waterways, Giza) and must not be attempted casually.**

## Session 115 — 2026-08-01 — [navigation] The palace sneak becomes automatic; and a re-cost that can actually win

**KEYWORDS: sneak assist always on no toggle F10 unbound PathDanger 569 Lower Halls npcdic 694
kMaxGuards 16 rrp_a01..a06 palace script census capture routine CALLACT 0x290 0x26D 0x525
SetSilently removed phrasebook ids removed census map label off by one kBreachEscalation
kIdenticalStopTol identical breach same stop terrain guard S108 reachability oracle FloodFrom
corridor openings dump full-corridor rung fails**

Two tracks, two commits, deliberately separately revertable.

### Track 1 — sneak assist is automatic on the palace maps (`0fceed1`)

**The tester's call, on S113's play evidence.** The suppression turned out to silence only the
guards' catch — the servant chain, the shout, the doors and the transition all worked through three
minutes of armed play, with the falsifier silent throughout. So the arming was no longer containing a
risk; it was making a blind player re-arm a fix for a puzzle they cannot see, on every entry to the
map that needs it. **The `PathDanger` map table is now the feature's only gate**, and F10 went back
to the game.

**Map 569 got the row S114 predicted.** Fourteen "Imperial" actors, all `nameIdx=694` — the same
npcdic id as 568's pair — so S113's identity-keyed mechanism covers it with one line. `kMaxGuards`
8 → **16**, because a snapshot that truncated at fourteen would silently leave real guards answering
truthfully, which is this array's only failure mode.

> ### THE PALACE TABLE IS COMPLETE, AND THAT IS A MEASUREMENT
>
> The question "are there other palace maps?" was answered from the game's own scripts rather than
> guessed. The palace is maps 567–572 = `rrp_a01`..`rrp_a06`; each `.ebp` was scanned for
> capture-routine name strings AND for the detection natives' `CALLACT` operands, and the two
> signals agree:
>
> | map | script | `捕獲` routines | `0x290` distance | `0x26D` touch | `0x525` wait-touch |
> |---|---|---|---|---|---|
> | 567 Cellar Stores | `rrp_a01` | **0** | 1 | – | – |
> | **568 Cellars** | `rrp_a02` | 1 (`ヴァン捕獲`) | 8 | 4 | 21 |
> | **569 Lower Halls** | `rrp_a03` | **12** (`捕獲レクトＡ/Ｂ/Ｃ`, `捕獲レクト兵士０１..０７`, `捕獲監視監督`) | 4 | – | – |
> | 570 Secret Passage | `rrp_a04` | 0 | – | – | – |
> | 571 Treasure Room No. 8 | `rrp_a05` | 0 | – | – | – |
> | 572 The Garden Stairs | `rrp_a06` | 0 | – | – | – |
>
> **Only 568 and 569 run a capture sequence at all**, which is exactly the pair the tester asked
> for. Confidence 0.99 — two independent signals, no inference in between. **Sweep the corpus
> before theorising about one member of it** (S104's lesson, paying again).

**What it costs, recorded rather than buried.** The accepted limit — `distance` is a GENERIC native,
so every call on a table map is clamped — **no longer has a player-side escape hatch**. On 568 that
risk is retired by play. On 569 it is not, so the `clamp ACTIVE` line and the non-guard falsifier
both still print and the fix, if a 569 gate ever stalls, is dropping 569's row.

Gone with the toggle: the ModMenu row and `SneakAssistOn`, **`SetSilently` (its only caller)**, the
four phrasebook ids, the `F10` binding and its DirectInput edge, and `AvailableHere` / `ArmedHere` /
`Shutdown` — the last of which recorded a flag nothing read and was never called by
`Navigation::Shutdown` anyway. `Adjust` is once again the ONLY place a setting's value changes.

Also fixed, both log-only and both wrong in a way that would mislead the next reader:

- **The census line named the wrong map.** `OnMapTeardown` printed `MapNames::CurrentMapId()`, which
  the engine has ALREADY advanced by then — so 568's two `distance` calls were logged as
  `map 569 census`. Every census line in the file was one map late. The id is latched on the field
  tick now and the line says which map it means.
  > **⚠ STRUCK by Session 116's play test — THE LATCH DID NOT FIX IT.** The log still reads
  > `map 567 census` when leaving 313 and `map 569 census` when leaving 568, because **the field
  > tick has already run for the NEW map by the time `OnMapTeardown` fires**, so the latch advances
  > too. Needs an id that cannot advance first (the teardown hook's own map argument, or a latch
  > that updates only when the id CHANGES and reports the PREVIOUS value). **A fix for an ordering
  > bug must be verified against the ORDERING, not against the read.**
- **`Controls.md` still told the tester to press `F9` for the beacon**, in the mod-menu paragraph
  S112 did not reach. That is the key the game uses for *Hide On-Screen Keyboard*.

Also recorded in `GameArchitecture.md`: **`FUN_0033f680` is a THIRD native handler funnelling into
`FUN_002677f0`** (`FUN_00267e10` → `FUN_002677f0` → `FUN_0026b4e0`), with no callers of its own.
Hooking the shared choke point covers every native that reaches it, whichever slot dispatches it —
which is why 569 needed no new mechanism, only a row.

### Track 2 — the re-cost can finally outbid the corridor it disproved (`path_search.cpp` only)

**The tester's hard requirement for this half was "do not break pathing on other maps", so the
containment is argued from the code and verified by diff, not asserted.**

#### Two corrections to what S114 recorded, both material

S114 wrote this up from the summary; the log itself (seq 32, start poly 207, target Door 2) says
something sharper:

1. **The "priced out by `terrain=4000`" story does not hold.** That failure logs
   `corridor paid terrain=0 other=1000` — **there is no terrain price in it at all.** The retries
   are identical because **+500 on one portal is simply not enough to change A\*'s answer**, not
   because a terrain crossing outbids it.
2. **The whole corridor is unwalkable, not just the taut chord.** `repair[full-corridor]` re-inserts
   *every portal midpoint* (4 → 10 points) and **still breaches**. The `retreat` rung never even
   runs: it needs `badReached > BodyRadius()` and the body got **0.07 m**.

```
attempt 1  BREACH bad=3 len=22.60m reached=0.07m stop=(16.2,-7.72,120.4) why=sweep
           repair[unpull] / [unpull-departure] / [full-corridor]  -> still breaching
           replan: portal (poly 449, edge 1) re-costed to 500
attempt 2  IDENTICAL corridor, IDENTICAL breach   -> 1000
attempt 3  different corridor, breach ~the same place -> portal 453:1 -> 500
attempt 4  BACK to attempt 1's corridor, IDENTICAL breach -> 1500
           frontier: 22.6m short. attempts=4 banned=2
```

#### What shipped

1. **`kBreachEscalation` (×3) on a REPEATED IDENTICAL breach.** `BannedEdge` now carries the stop
   that earned its last price. When the same `(poly, edge)` breaches again and the body stopped
   within `kIdenticalStopTol` (0.5 m) of last time, the price **multiplies** — 500 → 1500 → 4500 —
   instead of crawling 500 → 1000 → 1500. A retry whose breach MOVED is progress and keeps the
   gentle additive step it has always had.

   > **THE DISTINCTION IS A MEASUREMENT, NOT AN INDEX** — the same correction S111 applied to the
   > final-approach rule. An identical stop is not new information about the route; it is a
   > measurement that the price was too low. And it is still a PRICE: even the escalated figure is
   > one A\* will pay when the portal is genuinely the only way through, which is the property S96
   > bought and S108 re-bought.

2. **A terrain guard, because escalation is exactly what could buy S108's regression back.** S106
   priced the ground round the guards, the discs sat across the ONLY corridor, and the search did
   not take a wider berth — it bought its way onto ground the party's class cannot stand on and
   validation killed the route. So: **the moment a retry's corridor starts paying terrain where its
   predecessor paid none, the loop stops** rather than spending its remaining attempts proving it.
   A corridor that paid terrain from attempt 1 is untouched — nothing got worse there.

3. **A reachability ORACLE, log-only** (S114's candidate 2). On a request that is about to answer
   Frontier or NoPath, `NavMesh::FloodFrom` — the flood that already exists for `nav_probe`, pure
   adjacency plus the party's walkability test, no rays, no costs — says whether the goal poly is in
   the start's component at all.

   > **Every "No path" in this project has been argued about from two words that cannot be told
   > apart in the log**: *genuinely unreachable* and *the search gave up* are opposite defects that
   > print identically. One line now separates them, on every map — which is what turns the tester's
   > "stuck in corners, also in the Waterways" into a decidable question instead of an anecdote.

4. **The corridor itself, dumped on failure.** `corners(xyz)` prints the string-pulled polyline —
   what the body was asked to walk — but never what A\* actually found. When `full-corridor` fails,
   that distinction IS the question: if the corridor's own openings do not walk either, the chord
   was never the problem and no amount of un-pulling will help.

#### Containment — why no other map can move

1. **Every line of 1 and 2 sits after `rep.ok` failed AND all three `PathRepair` rungs failed**, on
   the path to `break`. A route that validates on attempt 1 — which is every route the tester walks
   today — never executes one instruction of it.
2. **The escalation differs from today only on the SECOND breach of the SAME portal at the SAME
   stop.** That needs attempt ≥ 2 of a request whose attempt 1 already failed. Those requests end
   today as `No path` or a suppressed frontier, so the set of requests whose OUTCOME can change is
   exactly the set that currently produces no route.
3. **The terrain guard can only make the loop stop EARLIER.** It cannot refuse a walkable route:
   validated routes have already broken out above it.
4. **3 and 4 change nothing** — log output on a path that has already failed.
5. **`git diff --stat` shows ONE file: `path_search.cpp`.** `path_funnel`, `path_validate`,
   `path_corridor`, `path_repair`, `path_march`, `nav_mesh`, `nav_footprint`, `map_query`,
   `path_surface_goal`, `path_planner` verified unchanged **by diff, not by assertion**.
6. **Bounds untouched**: `kMaxAttempts=4`, `kMaxTotalExpand=40000`, `kProbeBudget=1600`. No request
   can cost more frames than it does today.

**Explicitly NOT done:** `0x07A01000` is not touched (S114's candidate 3 — cross-map blast radius
into the Waterways and Giza). The frontier stays suppressed and "No path" stays honest; partial-route
speech was rejected by the user in S106 and is not re-opened.

### Verify next play

**Sneak assist:** on 568 AND 569, with nothing switched on, `clamp ACTIVE on map 568|569` and
`touch SUPPRESSED ... for a danger actor` naming an `"Imperial"` object; no capture walking past
569's Imperial posts; `touch REPORTED ... by a NON-guard object` is the falsifier and names any
capture rect that still catches you; `map <n> census:` now names the map the count belongs to; `F10`
does nothing anywhere; the `F8` menu walks six settings and never says "Sneak assist"; **no `SNEAK`
lines at all on any map that is not 568 or 569.**

**Routing — this is the half that must not regress:** map 315 still `pass=mesh`, `pass=seam` still
grep-dead, and routes that work today still answer `attempts=1` (the new lines cannot appear on
them). From the east side of 568 (start x ≥ 17.4), look for `re-cost ESCALATED x3` and whether a
route appears where `Frontier` was. **Every total failure now carries an `oracle:` line — that line
is the deliverable even if the escalation does not fix 568.**

## Session 116 — 2026-08-01 — [navigation] A* certifies crossings, never the travel between them

**KEYWORDS: corridor march MarchCorridor CorridorMarch path_corridor EdgePassable at SOME parameter
along it repair ladder skipped measured vs inferred crossing re-cost full-corridor rung fails on both
maps 568 palace ramp no water Garamsythe Northern Sluiceway oracle SEARCH giving up probe budget
kProbeBudget surface-goal REJECTED budget ran out tight corners replan from a bad position**

### The tester's challenge, and the correction it forced

The first read of the Waterway log put **water** at the root: 7 of 7 requests whose corridor paid
`terrain` failed, 15 of 15 spoken routes were dry, and the proposed fix was a dry-first search pass.
**The tester rejected it on one sentence: map 568 has the identical symptom and there is no water in
the palace.** They were right, and the correlation was hiding the cause.

> ### THE FACT THAT KILLS THE WATER THEORY
> `repair[full-corridor]` — the rung that rebuilds the polyline from EVERY portal midpoint in the
> corridor — **fails on both maps**:
>
> ```
> 568 palace   repair[full-corridor]: leg 3, 8   -- 4->10  points, probes=6   -> still breaching
> waterways    repair[full-corridor]: leg 2, 164 -- 22->166 points, probes=362 -> still breaching
> ```
>
> That rung pulls nothing taut. If walking the corridor's own openings in order still fails, then
> **the corridor A\* returned is not walkable as a corridor**, and whatever is sitting in the way —
> water, a ramp lip, a pillar, a party-only volume — is incidental. 568's failing corridor pays
> `terrain=0`.

### The root cause, and it is in this project's own header

`nav_mesh.h`, on `EdgePassable`:

> 2. the body can be swept across the shared edge **at SOME parameter along it**.

**A\* CERTIFIES CROSSINGS. IT NEVER CERTIFIES THE TRAVEL BETWEEN TWO CONSECUTIVE CROSSINGS.** Every
opening in a corridor can be individually passable while the hop from one opening to the next is not.
Nothing in the search asks about those hops, and the first thing that does is the body walk — by
which time it is a breach with a repair ladder pointed at it.

That one gap produces every symptom on both maps:

| | 568 palace | Waterway seq 21 |
|---|---|---|
| corridor pays terrain | **0** | 2000 |
| breach leg | last (3/3) | **2 of 21** |
| leg / body reached | 22.60 m / **0.07 m** | 16.42 m / **0.23 m** |
| every repair rung fails | yes | yes |
| `full-corridor` fails | yes | yes |

- Every rung fails **correctly** — the ladder reshapes a path *within* a corridor, and the corridor
  is the problem.
- The re-cost then prices "the portal nearest the failing chord's midpoint" — **a guess** — so A\*
  returns another corridor with the same untested hop, and S115's escalation re-prices the same guess
  harder.
- It correlates with the player being somewhere awkward because from a ledge or a corner the cheapest
  corridors thread pinches a clean start routes around. That is the tester's own description of the
  defect, and it is now explained rather than restated.

### What shipped: ask the corridor, before deciding what is wrong with it

`PathCorridor::MarchCorridor` walks the corridor's own openings, opening to opening, with the
adjacency march — which **spends no probes** (`path_march.h`: "probes price SWEEPS, and this makes
none"). It reuses `PathFunnel::FullCorridor` to build the very polyline the last repair rung builds,
so the check and that rung can never describe different things. Run **after a breach, before the
ladder**, it decides which tool the failure needs:

- **CLEAR** → the corridor walks and only the taut chord did not. *That is exactly what the ladder
  was built for.* Run it, unchanged. **The waterways/tight-corner repairs the ladder was introduced
  for keep working, untouched.**
- **BREACH** → the corridor is not walkable. **Skip the ladder** (no rung can help; today it burns
  ~1,000 of the 1,600 `kProbeBudget` proving that) and **re-cost the crossing the march named**
  instead of the chord-midpoint guess.

The re-cost now has two sources and says which one it used: `[MEASURED by the corridor march]` or
`[inferred from the chord]`. The measured crossing keeps **both** protections the inferred one has
always had — never price the final approach, never price the seed's own edge — applied to the better
measurement rather than dropped because it is better. `killStop` (the march's hit point, or the
chord's stop) is what S115's escalation compares against, so an identical breach still reads as
identical whichever source named it.

**FAILS OPEN.** A hop `MarchLeg` cannot decide is counted (`noVerdict=`) and skipped, never promoted
to a breach — `MarchLeg`'s own contract, and the S96 rule that inventing a wall on a working map is
the one regression this project cannot afford.

### Containment

1. It runs **only after `rep.ok` has already failed.** A route that validates on the chord never
   executes a line of it.
2. When the march says CLEAR, behaviour is byte-identical to before plus one free march.
3. When it says BREACH, the ladder is skipped — and every such case ends as `No path` today anyway,
   because `full-corridor` is already the last rung and it already failed in every one of them.
4. Net CPU on the failure path goes **down**: the march makes no sweeps, and skipping the ladder
   saves the ~1,000 probes it was spending. Failing requests currently log
   `STALL PathPlanner::OnGameFrame 50.7ms`.
5. `git diff --stat` = **3 files** (`path_corridor.{h,cpp}`, `path_search.cpp`). `path_funnel`,
   `path_validate`, `path_repair`, `path_march`, `nav_mesh`, `nav_footprint`, `map_query`,
   `path_surface_goal`, `path_planner` verified unchanged **by diff, not by assertion**.
6. `kMaxAttempts` / `kMaxTotalExpand` / `kProbeBudget` / every penalty untouched. Full path
   validation is untouched: every leg of every spoken route still gets the same march plus sweep.

### The falsifier is the log line itself

`corridor march: CLEAR over N hop(s)` vs `corridor march: BREACH at hop k/N -- crossing P:E -> nbr=N
nbrEff=0x…`. **If these failures come back CLEAR, this diagnosis is wrong**, the ladder runs exactly
as today, and the cost was one free march. If they come back BREACH it names the exact crossing on
both maps — the first time we would know whether 568's ramp and the Waterway channel refuse for the
same reason.

### Also confirmed this round — S115's oracle paid for itself immediately

Every one of the 13 failures: `oracle: goal poly 324 is IN the start poly's adjacency component
(1997 polys) -- the SEARCH giving up, not an unreachable goal`. That question is now closed on this
map class and never has to be argued again.

### Deliberately NOT done

- **No dry-first search pass.** The corridor march subsumes it (a bit-23 crossing is one of the
  things the march refuses) *and* it covers the palace ramp, which a dry-first pass would not. Less
  change, wider coverage.
- **No cost-model change.** `kTerrainPenalty` untouched — S96/S108 territory, and not needed.
- **The budget hygiene is held back**: skipping rungs the remaining budget cannot validate, logging
  truncation as truncation (`probes=7` on a 27-leg candidate is currently reported as "still
  breaching", which is a lie), and reserving `PathSurfaceGoal::Route`'s probes up front — it is
  `REJECTED (budget ran out -- NOT verified)` in 10 of 13 failures with exactly its 128-probe floor
  spent on a 145-corner polyline. With the ladder skipped on the corridor-is-broken branch most of
  that pressure disappears, so it gets re-measured before it gets sized. Three changes in one build
  is what S96 records as the cause of the damage it spent a session undoing.

### Verify next play

On **map 568** approaching Door 2 from off the corridor, and in the **Garamsythe Waterway** heading
for the North Spur Sluiceway: every failure now carries a `corridor march:` line. Expect BREACH on
both, with a named crossing, followed by `re-costed … [MEASURED by the corridor march]` and — the
actual test — a route where there was `No path`. Regression gate unchanged and non-negotiable: **map
315 still `pass=mesh`**, `pass=seam` still grep-dead, and routes that work today still answer
`attempts=1` with no `corridor march:` line at all.

### PLAY RESULTS (2026-08-01, same session) — and S116's falsifier fired AGAINST it

**KEYWORDS ADDENDUM: corridor march CLEAR x81 zero BREACH falsifier refuted InsetCorners dense
polyline 569 guard suppression does not work 0x26D 0x525 absent from rrp_a03 seteventwakerect 0x26E
twin filter no distance test 90.06m hasNameSign Shop misclassified door census label still off by one**

#### 1. Sneak assist is PLAY-CONFIRMED on map 568, automatically, with no key

```
[SNEAK] script-distance hook installed (sneak assist acts on the danger-table maps only; nothing to switch on)
[SNEAK] clamp ACTIVE on map 568: script distance 3.04 -> 9999 (float slot)
[SNEAK] touch SUPPRESSED on map 568 for a danger actor ... suppressed object is: "Imperial 1"
```

The tester reports it "works perfectly". No toggle, no `F10`, nothing to arm. **The S115 track is
confirmed for 568.**

#### 2. ⚠ IT DOES NOT WORK ON MAP 569 — and S115's own census predicted this

The tester reports the guard suppression does not work on 569. The log agrees and says exactly why:

```
[SNEAK] clamp ACTIVE on map 569: script distance 27.80 -> 9999 (float slot)     <- the CLAMP fires
                                                                                <- ZERO touch lines
```

**Zero `touch SUPPRESSED` and zero `touch REPORTED` on 569.** Not one. The falsifier cannot fire
either, which means the touch hook is never reaching a guard at all.

> **THE ANSWER WAS ALREADY IN S115's SCRIPT CENSUS AND I UNDER-WEIGHTED IT.** `rrp_a03` (map 569)
> contains **ZERO `0x26D` and ZERO `0x525`** — the two natives that funnel into `FUN_002677f0`. 568
> has 4 and 21 of them. **The touch test is never called on 569**, so per-object suppression has
> nothing to suppress. 569 instead uses **`seteventwakerect` (`0x3DF` → `FUN_0034d470`) ×70** and an
> unresolved **`0x26E` ×77**, which is what the `捕獲レクト兵士` ("capture rect soldier") actors are
> driven by. That is the mechanism to hook, and it was written down as "the first two places to look"
> — the census answered this before the play test did, and the entry claimed 569 was covered anyway.
>
> **A CENSUS THAT SAYS A MECHANISM IS ABSENT IS NOT A DETAIL — IT IS THE ANSWER.** Same shape as
> S107's "when a user proposes a state change, check whether the state exists".

Next session: resolve `0x26E`'s handler and whether `seteventwakerect`'s rects test the leader through
a different choke point, then hook that. `kMaxGuards` 16 and the `{569,694}` row are harmless and
stay; the distance clamp on 569 is live and correct, it is simply not the whole catch there either.

#### 3. S116's corridor march is REFUTED BY ITS OWN FALSIFIER

```
corridor march: CLEAR   x81
corridor march: BREACH  x0
```

**The BREACH branch never executed.** So S116 changed nothing observable this session, the repair
ladder ran exactly as it always did, and — stated plainly — **568's pathing behaving well is NOT
attributable to the corridor march.** (It is also unconfirmed: the tester walked straight to the
door and did not stress it.)

> **THE DIAGNOSIS WAS WRONG, AND THE FALSIFIER IS WHY WE KNOW IN ONE SESSION RATHER THAN FOUR.** The
> S116 reasoning was: `repair[full-corridor]` fails, that rung walks the corridor's own openings,
> therefore the corridor is unwalkable. The march says the corridor IS walkable. Both were measured;
> the inference between them was the error. **Two instruments that walk "the same" polyline and
> disagree are not measuring the same thing** — which is S97's lesson arriving from the other side.

#### 4. What the falsifier BOUGHT — a new and much sharper suspect

On the Sluiceway failure, the same request, the same polyline:

```
corridor march: CLEAR over 139 hop(s) (grazes=73 noVerdict=0)
repair[full-corridor]: leg 20, 138 -- 21->140 points, probes=310 -> still breaching
```

Adjacency-CLEAR and body-BREACH on the corridor's own openings. There are exactly two differences
between what those two walked:

1. **`tryPoly` runs `PathFunnel::InsetCorners(cand)` and `MarchCorridor` does not.** The inset steps
   EVERY interior corner by `BodyRadius + kClearanceMargin` along its angle bisector. That is right
   for a 20-corner route through open rooms. **On a 140-point polyline of portal midpoints in a
   narrow channel, every point is a "corner" with a near-straight bisector, and stepping all of them
   can push points off the corridor they were sampled from.** PRIME SUSPECT.
2. `CheckLegs` also runs the body sweep; the march makes none.

**And this predicts the pattern already in the log:** the ladder REPAIRED 9 routes this session
(`unpull` ×4, `unpull-departure` ×4, `full-corridor` ×1) — all short — and failed on every long dense
one. `probes=310` for 139 legs is ~2.2/leg, a real validation, not a truncation.

Cheapest next test, log-only and free: after `InsetCorners` on a repair candidate, count how many
points left the poly they were sampled from (`NavMesh::FindPolyAt` before vs after). If that number
is large on the failing routes and zero on the repairing ones, the suspect is confirmed and the fix
is to bound or skip the inset on dense polylines.

Also worth weighing: `grazes=73` of 139 hops. The corridor is only "clear" thanks to graze rescues,
so the march's CLEAR here is not a comfortable one.

#### 5. Still failing, unchanged: all 8 remaining failures are the same exit

`target="Exit, Garamsythe Waterway: North Spur Sluiceway"` ×8, all with the identical breach
(`bad=last len=61.16m reached=17.25m stop=(116.4,9.75,86.2) why=march from=632:2 nbr=939
nbrEff=0x17B00000`, `corner: poly=324 clear=0 margin=-0.27m`) and all with
`surface-goal ... probes=128 -> REJECTED (budget ran out -- NOT verified)`. The tester adds that it
is **easier to get loose of** than before, but notes the confound: **there are enemies at that
location and a fleeing one drags the player out of the stuck spot** — so a recovery in the log may be
the enemy's doing, not the router's. Do not credit any fix for it.

#### 6. ⚠ CORRECTION — the S115 census map-label fix DID NOT WORK

It still names the map being ENTERED: `map 567 census` prints when leaving 313, `map 569 census` when
leaving 568. Latching the id in `OnFieldFrame` was not enough, because **the field tick has already
run for the NEW map by the time `OnMapTeardown` fires**. The S115 entry claims this was fixed; it was
not. Needs the id latched somewhere that cannot advance first (the teardown hook's own map argument,
or a latch that only updates when the id CHANGES and reports the previous value).

#### 7. NEW DEFECT — a bare unlabelled door on 569 is classified as a SHOP

Tester: *"there is definitely no sign that says what this door is for and it shouldn't be falling
into the shop category, it's just a bare unlabelled door."* Map 569 lists `Door=0 Shop=1`.

**Root cause found, and it is two faults compounding** — `src\navigation\entity_postscan.cpp`:

```
[NAV-DIAG] twin dropped (sign repeats a doorway) "Door": [0:57] (83.92,0.00,38.48)
                                            <- keeping [0:56] (41.65,0.00,118.00) 90.06m away
```

1. **THE TWIN FILTER HAS NO DISTANCE TEST ON INTERACTABLES.** The NPC branch guards with
   `kStackedDist` on both the horizontal distance and the Y (lines ~357-358). The interactable branch
   (~368-376) matches on `label` and `doorway` alone and will pair two objects **90 metres apart**. A
   shop SIGN stands BESIDE its doorway — that is the entire premise of the filter, and it is the one
   thing not being checked.
2. **`hasNameSign` was derived from a GENERIC FALLBACK LABEL.** Both objects were called `"Door"` —
   the mod's own fallback naming, not game-supplied sign text. Two objects sharing a generic label is
   no evidence of a shopfront. The drop then sets `out[twin].hasNameSign = true` (line ~406), and
   line ~435 turns that into `Category::Shop`.

So the mod invented a shop out of two unrelated doors at opposite ends of the map. Fix is a bounded
proximity rule on the interactable branch plus a requirement that the shared name be DISTINCTIVE
rather than a generic fallback — **and note this can only ever have made things worse where it
fired, because it also DELETED the other door**: 569 lists zero Doors.

## Session 117 — 2026-08-01 — [navigation] Map 569's catch is the ENGINE's, not the script's; and a twin filter with no distance test

**KEYWORDS: sneak assist map 569 event fire FUN_003dbb60 RVA 0x2BBB60 FUN_0025c830 trigger volume
update ENTER LEAVE routine index FUN_003dbcf0 routine count bound capture routine name 捕獲 Shift-JIS
95 DF 8A 6C action_binding_tables validated script_native_table dbg join unreliable seteventwakerect
STRUCK 0x3DF FUN_0034dca0 setter 0x26E FUN_00341000 0x409 0x40A twin filter kTwinNearDist
kTwinNameMaxObjects hasNameSign Shop misclassified door 90.06m InsetCorners leftHome instrument
census latch write-once consumed by the print**

Three open defects from S116's play test, all with root causes already recorded. All three fixed; one
of them needed the mechanism found first.

### 1. THE 569 CATCH — no script native is involved at any point

S116 established that `rrp_a03` calls **zero** `0x26D` and **zero** `0x525`, so `FUN_002677f0` — the
choke point the S113 suppression hooks — is never reached on 569, and the play log's zero touch lines
of either kind agreed. What it left open was what the catch actually IS. It is the engine's own
trigger-volume update:

> **`FUN_0025c830(container, object)`** zeroes the object's inside-mask, walks the FOUR party actors
> at `DAT_0209a1f0` against the volume, ORs each slot bit into `*(u32*)(*(object+0xB8) + 0x60)` — **the
> same mask `FUN_002677f0` reads** — and then, on the frame the mask goes from empty to occupied,
> calls **`FUN_003dbb60(object, 4, routineIdx, 0)`: START THIS OBJECT'S ROUTINE.** That ENTER branch is
> the one that goes on to `FUN_002e1cd0(0,0xd)` / `FUN_00268530(2)` and hands the field to a scripted
> scene. Kind 2 is ON LEAVE, kinds 3/6 the in-volume tests.

**The suppression is keyed on the ROUTINE, not on the object, and that is forced rather than chosen.**
569's volumes are the `捕獲レクト兵士01..07` ("capture rect soldier") actors — rect actors carry no
npcdic name, so the entity scan cannot see them and `IsGuardObject` could never match them. The
routine, by contrast, names itself. `FUN_003dbcf0` rejects a record whose index is `>=`
`**(u32**)(object+0x48)` — it bounds the fired index against the object's script container's ROUTINE
COUNT, which is what establishes that the index addresses the same table `MapScript` already reads. So
`MapScript::RoutineNameAt(idx)` resolves it, and the mod declines any fire whose routine the map's own
author named `捕獲` ("capture").

That is the **global** rule the per-map table never was: it matches 568's one capture routine
(`ヴァン捕獲`) and 569's twelve with no map id, offset or index in it. The danger table stays as the
containment gate. **It FAILS OPEN by construction** — unreadable blob, out-of-range index, or an
unmatched name all take the original path — and it logs every fire on a table map, matched or not,
with the raw name bytes, so "the index space is not this table's" and "this volume is not a capture"
cannot print identically.

**ONLY A TRIGGER VOLUME'S FIRE MAY BE DECLINED, and that is made a fact rather than inferred from the
kind byte.** `FUN_003dbb60` has ~20 call sites and they are not all volumes — `FUN_00269640` /
`FUN_00269860` start conversation events, `FUN_00269a90` / `FUN_00269ba0` / `FUN_00266530` are
script-side event calls reached with a `param_5` the volume path never passes. **A routine the SCRIPT
asks for must run:** declining a `捕獲監視監督` ("capture watch supervisor") that the map's own setup
starts would stall the sequence rather than save it. So `FUN_0025c830` (RVA `0x13C830`) is hooked as a
pure SCOPE MARKER — two thread-local stores around a passthrough, no behaviour of its own — and only
fires issued inside its extent are candidates. The log line carries `src=trigger-volume` or
`src=script/other` either way, so a capture routine that arrives by the other path is visible instead
of being silently suppressed or silently missed.

### 2. THE NATIVE TABLE THAT WAS BEING QUOTED IS THE WRONG ONE — `seteventwakerect` STRUCK

`GameArchitecture.md` and S115's census carried "**569 also uses `seteventwakerect` (`0x3DF` →
`FUN_0034d470`) x70 and an unnamed `0x26E` x77** — the first two places to look". Both halves are
wrong, and re-deriving the census this session is what caught it:

- **The handler is wrong.** `output/script_native_table.txt` is anchored on `mapjump` and joins names
  from the `.dbg` symbols through a *measured* delta; its own self-check prints
  `VERDICT: NOT COHERENT -- do not use any id above`. **`output/action_binding_tables.txt` is the
  validated table** — it independently reproduces all three previously-established facts (`0x08D` exec
  `FUN_00355350` mapjump, `0x290` poll `FUN_003448f0` distance, `0x525` enter/exec
  `FUN_003407c0`/`FUN_00340bc0`) — and it maps `0x3DF` to **`FUN_0034dca0`**, not `FUN_0034d470`.
- **The name is wrong, and so is the lead.** `0x3DF` → `FUN_0034dca0` → `FUN_0026a660` sets bit 5 of
  `object+0xC`. `0x26E` → `FUN_00341000` sets bit 5 of `object+0x8`. `0x409`/`0x40A` (also x70 — the
  same three-natives-per-rect signature) → `FUN_0033f1e0`/`FUN_0033f650` set bits 1 and 0 of
  `object+0xB`. **All four are SETTERS that configure a volume; not one of them tests anything.** Two
  of those bits are read by `FUN_0025c830` itself: `+0x8` bit 5 restricts the test to the LEADER, and
  `+0xC` bit 5 gates the volume on an event being active.

> **A NAME FROM AN UNVALIDATED JOIN IS A GUESS WEARING A LABEL.** "seteventwakerect" was quoted across
> three documents as the mechanism to hook, and it is a flag setter whose name came from a table that
> says in its own output that its mapping is wrong. The census COUNTS were right all along (this
> session reproduced 568's 1 and 569's 12 capture-routine names, and the `0x290`/`0x26D`/`0x525`/
> `0x26E`/`0x3DF` counts, exactly from the `.ebp` files); only the resolution was fiction.

### 3. THE TWIN FILTER HAD NO DISTANCE TEST — and one of the two recorded faults was wrong

The interactable branch matched on `label` + `doorway` and nothing else, so on 569 it paired two
generic `"Door"` objects **90.06 m apart**, deleted one, and promoted the survivor to `Category::Shop`.
The map listed a bare unlabelled door as a shop and listed **no doors at all**.

Both halves now gate the DROP, not just the Shop promotion — deleting a real door because it shares
the game's word for "door" with a bound one costs a blind player a way out of the room:

- **`kTwinNearDist` = 20 m.** From the measurement the filter was built on: East End's shop pairs sit
  **6-15 m apart**, so the bound must admit 15 and reject 90.
- **`kTwinNameMaxObjects` = 2.** A shopfront's name is carried by exactly the two objects that make it
  up; `"Door"` is carried by every door on the map (568 has three). No word list, nothing assumed
  generic — the map's own data answers it.
- **A refusal is now logged too** (`twin KEPT ...`, one line per label per map). The drop line has been
  unconditional since it deleted a story NPC; its refusals were invisible in exactly the same way.

> **CORRECTION TO THE S116 WRITE-UP:** it recorded fault 2 as "`hasNameSign` was derived from a GENERIC
> FALLBACK LABEL — the mod's OWN fallback naming". **That is wrong.** `ApplyFallbackLabels` runs
> *after* `TagDoorwaysAndDropSignTwins`, and `gameNamed` is set from whether the game supplied text at
> all — so at filter time `"Door"` is the map's own `fieldsignmes` string, not a mod word. The defect
> was the missing distance test, once. A second fault asserted beside a real one gets fixed twice and
> believed forever.

### 4. THE INSTRUMENT S116 ASKED FOR — free, log-only

`PathFunnel::InsetCorners` now fills an optional `InsetStats{corners, moved, leftHome}`, and
`repair[...]` prints it: **`inset M/N corner(s) moved, K LEFT their poly`**. `leftHome` counts moves
whose accepted point landed in a **different mesh poly** from the corner it came from — i.e. a corner
pushed out of the corridor A\* certified. It costs nothing: both `FindPolyAt` results were already
computed to decide the move, and the function's behaviour is byte-identical.

The prediction to read off the next log: **`K` stays 0 on the rungs that report `OK` and grows on the
dense candidates that report `still breaching`.** Confirmed => bound or skip the inset on dense
polylines. Small on both => the suspect is wrong and the next one is `CheckLegs`' own body sweep.
Nothing was changed in the cost model, the ladder or the tolerances.

### 5. The census map label, third attempt — and this one is not an ordering bet

S115 latched the map id on the field tick; the play log still said `map 569 census` for 568's two
calls, because the field tick has already run for the NEW map by the time teardown fires. The latch is
now **write-once per map and consumed by the print**: the tick fills it only when empty, teardown
prints it and clears it. Whichever runs first, the id printed is the one the counter was counting on.

### Containment

Ten files, all in `src/navigation/`, and the other twenty-odd nav sources verified unchanged by diff.
The event-fire hook's FIRST branch is the danger-table gate, exactly like the two hooks beside it, so
on every map but 568/569 the build is behaviourally identical to today's — and trigger fires happen on
every map in the game, which is why that early-out is what keeps the hook from having a blast radius.
No cost-model, tolerance or bounds change anywhere in the pathfinder.

### Verify next play

**Map 569, nothing switched on.** `event-fire hook installed`; walking into a guard's rect produces
`event fire on map 569: ... CAPTURE, SUPPRESSED` and **no capture**. The falsifier is a capture that
happens with no `CAPTURE, SUPPRESSED` line — then the index space is not the routine table's, and the
`passed through` lines name every index and raw name the fire carried. 568 must still behave exactly
as S116 play-confirmed (`clamp ACTIVE`, `touch SUPPRESSED ... "Imperial"`). **Zero `event fire` lines
on any map that is not 568/569.**

**Map 569 exits:** the rescan should read `Door=2, Shop=0` — `[0:57]` survives and `[0:56]` is no
longer promoted — with a `twin KEPT "Door" ... 90.06m ...` line saying why. Rabanastre East End's shops
must still list as Shops: that pairing is 6-15 m with exactly two objects per name.

**Routing gate, unchanged and non-negotiable:** map 315 still `pass=mesh`, `pass=seam` grep-dead,
working routes still `attempts=1`, and every `repair[...]` line now carries its `inset`/`LEFT` counts.

## Session 118 — 2026-08-01 — [navigation] The fired index is OBJECT-LOCAL: S117's falsifier fired, and the name was one indirection away

**KEYWORDS: sneak assist 569 capture survived falsifier fired object-local event index object+0x48
event table name-pool offset FUN_00263e40 blob+0x4C FUN_00263ff0 HANDLE_TABLE_BASE 0x288 obj+0x15
FiredRoutineName RoutineNameAt removed verdict cache removed NoteFireOnce object key dedup hid the
capture fire consecutive indices setup resident director impossible names**

Tester: the guard still detected them on 569 (568 untested). **The log answered before any theory
did, exactly as S117 built it to** — every fire printed, and the fires were impossible:

```
obj=2CFD9C80 kind=3 routine=1 src=trigger-volume name="setup"            -- passed through
obj=2CFD9C80 kind=4 routine=2 src=trigger-volume name="常駐ディレクター"   -- passed through
obj=2CFD4050 kind=3 routine=2 ... obj=2CFD7F00 kind=3 routine=3 ...
```

A trigger volume does not start `setup`, and no volume's ENTER is the map's resident director. And
every object fired **consecutive small indices** — 1,2,3 on one object, 2,3,4 on the next, 3,4,5 on a
third. That is not what routine-table indices look like; it is what **object-local slots** look like.

### The corrected model — each step a verbatim decompile read

S117 claimed `FUN_003dbcf0`'s bound `**(u32**)(object+0x48) <= idx` proved the index addressed the
blob's routine table. **Wrong table.** `object+0x48` is the object's OWN EVENT TABLE:

```
tbl   = *(u64*)(object + 0x48)         [count:u32][8-byte records]
entry = *(u32*)(tbl + 4 + idx*8)       a NAME-POOL OFFSET, not a routine index:
                                       FUN_00263e40(blob, x) = blob + x + *(u32*)(blob + 0x4C)
                                       and +0x4C is HDR_NAME_POOL
blob  = *(u64*)(HANDLE_TABLE_BASE + obj[0x15]*0x288)     FUN_00263ff0 -- the object's own container
```

The dispatch (`FUN_003db7a0`) compares the fired index against the object's active-slot table at
`+0xA0` at the same index — object-local throughout. So S117's resolver looked index 2 up in the
routine table and got `常駐ディレクター`, while the object's event table's entry 2 named something
else entirely — on a capture rect, one of the `捕獲` routines. **The name never matched, the fail-open
path passed the fire through, and the player was caught.** Fail-open did its job: vanilla behaviour,
plus the log that named the defect.

### What shipped

- **`MapScript::FiredRoutineName(object, eventIdx)`** replaces `RoutineNameAt(index)` (no other
  caller ever existed). Walks the chain above, SEH-guarded at every read, container id read from the
  object rather than assumed 0.
- **The per-index verdict cache is GONE, not fixed** — an object-local index makes "is routine N a
  capture" unanswerable without the object, so a cache keyed on the bare index was wrong within a
  single map. Fires are event-driven; resolving per fire is a handful of guarded reads.
- **`NoteFireOnce` now keys on (object, kind, index), 96 slots** — S117's object-less key
  deduplicated DIFFERENT objects' fires into one line, which is precisely how the capture rect's own
  fire went unlogged while the player was being caught. **A dedup key that omits part of the identity
  does not reduce volume, it deletes evidence.**
- Suppression rule unchanged: decline a trigger-volume fire whose resolved name contains `捕獲`.
  Scope marker, danger-table gate, fail-open — all unchanged.

### Verify next play (569)

Fires now print the name the OBJECT's table holds. Walking into a capture rect must produce
`event fire on map 569: ... src=trigger-volume name="\x95\xDF\x8A\x6C..." -- CAPTURE, SUPPRESSED`
and no capture. The falsifier is unchanged: a capture with no `CAPTURE, SUPPRESSED` line, with the
per-object log now naming what actually fired. The impossible names (`setup` as a volume's kind-3)
must be gone — if they persist, the chain is still wrong and the log says where. 568: unchanged
expectations (`clamp ACTIVE`, `touch SUPPRESSED`), zero `event fire` lines off 568/569.

## Session 119 — 2026-08-01 — [navigation] Skip the volume at the WRITER; a census that cannot miss; and the event-table join names a door

**KEYWORDS: sneak assist 569 third round trigger update skip FUN_0025c830 class +0x18==1 never fires
FUN_003dbb60 notification registers FUN_003df760 DAT_02b59c40 mask node+0x60 writer not reader
trigger census per object event names nearest guard log budget tiers spam init main 96 slots
exhausted event-door nameOff join ExitDest object+0x48 category Door twin KEPT play-confirmed**

Third round on the 569 capture. The S118 resolver is CONFIRMED (names now real: `init`/`main`), and
the twin filter is PLAY-CONFIRMED (`twin KEPT "Door" … 90.06m`, `Shop=0`, `[0:56]` cat=Door). But the
guard still caught the player, and the log had — again — no line for it: **all 96 fire-log slots were
spent on load-time `init`/`main` spam inside 18 seconds.** Two plays, two different logging failures,
and the capture's mechanism has STILL never been observed.

### What a closer read of `FUN_0025c830` settles

**Objects of class `+0x18 == 1` (script-created — rects) NEVER reach `FUN_003dbb60`.** The ENTER
branch returns before the call; the kind-3/6 branches guard it out. Their trigger update's only
outputs are the inside-mask at `node+0x60`, status bits on `object+0xC`, and the notification
registers `FUN_003df760` writes (`DAT_02b59c40/c80/cc0/ce0`, slots 1/2) for the script to poll. **A
capture rect of that class is invisible to the fire hook by construction** — no decline there can
ever reach it. The S117/S118 fire-hook design could only have worked for `+0x18 != 1` volumes.

### The fix — suppress at the WRITER, which every read path shares

`HookedTriggerUpdate` (already installed as the scope marker) now SKIPS the original for exactly two
measured identities, danger-table maps only:

1. **A guard's own object** (`IsGuardObject`, the npcdic snapshot) — S113's user-approved "silence
   the guards' own trigger volume, per object", moved from the reader (`FUN_002677f0`, which 569's
   script never calls) to the writer. Covers vision volumes attached to the guards themselves.
2. **An object whose own event table names a `捕獲` routine** — the same author's-own-word rule the
   fire hook applies, evaluated over the identity a rect actually carries.

A skipped volume's mask is never written, bits never set, registers never posted, fires never issued
— inert on every downstream path at once, whichever one the script polls. Fail-open: unreadable
table/names ⇒ no skip. The fire-hook decline stays as the second net for `+0x18 != 1` volumes.
**NOTE for 568:** guards there are also skipped now (same table, same identities) — S113 established
568's sequence lives in servant routines + the distance watcher, but this is a behaviour change on
the working map; the gate below covers it.

### The census that cannot miss

One line per object the trigger update touches on a danger map, first touch, file-only:
`trigger census obj=… class=0x… f8/fB/fC=… pos=… nearestGuard=…m evt=N names:a|b|c` (+
`<== NAMES A CAPTURE ROUTINE`). Whatever catches the player next play IS in this table — class,
flags, event names, position, distance to the nearest guard — and turning it into a rule is one
read. 160 objects, overflow logged loudly.

**Log budgets are now tiered by what a line can prove:** capture-named fires always log;
trigger-volume fires get the per-object dedup and the 96 slots; script/other spam gets 16. The S118
play was the second in a row where the logging design deleted the one line that mattered — first by
an under-keyed dedup, then by an unpartitioned budget. **A shared budget is a dedup key with the
same failure mode: whatever fills it first decides what evidence survives.**

### The door under Interactables (tester's report, same play)

`[0:57]` — the door the old twin filter used to DELETE — survived and listed under Interactables:
`doorway` comes from the `+0x70` sign table alone, and the map's only group-0 record claims the
OTHER door. The binding that does exist is the map's own: **`ExitDest` now carries its routine's
name-pool offset, and a scene object's event table (`object+0x48`) holds name-pool offsets — offset
== offset says "this object's events run that transition routine."** S102's "the binding is the
CALLS", object-side; an integer compare in one pool, no authoring order (S46/S58), no label text, no
locale. New pass in `TagDoorwaysAndDropSignTwins` promotes such objects to `Category::Door` (never
Shop; `doorway` stays false; no destination is spoken — the exits work stays open). **This join —
object ↔ transition routine via nameOff — is also the first measured object↔routine binding this
project has had; the 569-lists-zero-exits backlog should start from it.**

### Verify next play (569 focus)

- Walking into a guard/capture volume: `trigger update SKIPPED for obj=…` and **no capture**. If a
  capture still happens, the census table + the tiered fire log now name the mechanism — read them
  before proposing anything.
- `trigger census` lines: expect the ~70 rects with `class=0x01`, their event names, and
  `NAMES A CAPTURE ROUTINE` on the capture rects. If NO census object names a capture routine, rule
  2 never fires and the census says what to key on instead.
- `[0:57]` lists under **Doors** (`Door=2`), `event-door:` line on map change, Shop still 0.
- 568 (when tested): sequence still completes; `clamp ACTIVE` + `touch SUPPRESSED` still print;
  `trigger update SKIPPED … a guard's own object` ×2 is EXPECTED there now.
- Off 568/569: zero `SNEAK` lines beyond install + census prints.

## Session 120 — 2026-08-01 — [navigation] The census named the catcher: a talkless WAKE rect riding the guard; and doors are template siblings

**KEYWORDS: sneak assist 569 fourth round census delivered catch rect 同期 sync 兵士全停止 soldiers
stop wake flag fC bit5 0x20 seteventwakerect vindicated guard-riding rect separate object talk
exclusion 8m radius template signature door fieldsign ＯＫ ＮＯＴ ＯＮ ＯＦＦ anchored inference
[0:57] template-door catch rect suppressed at writer and fire**

The S119 census worked on its first play. The capture moment is in the log, named:

```
trigger census obj=2CFD18D0 ... fC=0x28 pos=(58.0,-0.0,98.0) evt=7 names:init|touch|touchon|touchoff|SET_RECT|同期
[00:33:10] event fire ... obj=2CFD18D0 kind=3 routine=1 src=trigger-volume name="touch"   -- passed through
[00:33:10] event fire ... obj=2CFD18D0 kind=4 routine=2 src=trigger-volume name="touchon" -- passed through
                                                          <- capture; log ends
```

**The catcher is a rect at (58.0, 98.0) — riding the patrolling Imperial at (57.97, 97.98).** The
guards' vision volumes are SEPARATE rect objects that move with the guards. Both S119 identities
missed by construction: `IsGuardObject` (they are not the npcdic actors) and the `捕獲` name rule
(their event names are template-generic — `init|touch|touchon|touchoff|SET_RECT|同期`). Also
refuted: the S119 `+0x18==1` theory — every trigger object in the census is `class=0x00`, and the
catch DID go through the fire hook; we simply had no rule that matched it.

### What the census measured, and the rule built from it

Exactly SEVEN objects in the whole 569 census carry `fC` bit 5 — **the WAKE-EVENT flag, `0x3DF`'s
own bit, the one `FUN_0025c830`'s ENTER branch requires before handing the field to a scene**: three
`同期` ("sync") rects on the patrolling guards and four `兵士全停止` ("all soldiers stop") rects
between the stationary pairs (3.8–4.9 m from them). The capture machinery, complete, and nothing
else. (So `seteventwakerect` — the name S117 struck as a wrong lead — turns out to be a fair
description after all: it flags the rects whose ENTER wakes an event. The HANDLER resolution was
still wrong; the lead died for the right reason and the flag came back as the discriminator.)

**The rule (S120), all three facts measured and ANDed:** on a danger-table map, an object is a catch
rect when it (1) carries the wake flag (read live), (2) sits within **8 m** of a snapshotted guard
(read live — fact 2 moves every frame), and (3) has **no `talk` event** in its event table. The talk
exclusion is what protects 568: its servant stands 3.4 m from a guard — inside any radius that
admits the stop-rects — and interactables' rects carry the template's `talk` event; the seven catch
rects carry none. Excluding talk-rects can only widen safety.

Catch rects are skipped at the trigger update (mask/bits/registers/fires all dead) AND declined at
the fire hook (second net; logs `CATCH RECT, SUPPRESSED`). Guard positions now ride along with the
pointer snapshot each field tick. Census line gains `talk=`/`wake=` columns.

### The door — template siblings of a proven doorway

The census also explained the event-door failure: a door object's events are NOT the map's
transition routines. `[0:56]` and `[0:57]` both run the FIELD-SIGN TEMPLATE
(`init|talk|フィールドサインＯＫ/ＮＯＴ/ＯＮ/ＯＦＦ`) in its own container — the location jump
lives inside that machinery, so the S119 nameOff join could never fire (kept: it is correct where
objects do live in container 0). New rule, ANCHORED: an object whose event-table SIGNATURE
(container id + exact sorted event name-pool offsets) equals that of an object the map's own `+0x70`
table proved to be a doorway is a Door. No anchor on a map, no promotion — fail closed. Signets and
walls run different templates and cannot match. Logged as an inference (`[inferred from the
anchor]`), same honesty rule as `groupInferred`.

### Verify next play (569)

- Walking at a patrolling guard: `trigger update SKIPPED … a talkless WAKE rect within 8m of a
  guard` and **no capture**. Walking between the stationary pairs on the east side: same, via the
  `兵士全停止` rects.
- Census: the seven rects show `wake=1 talk=0`; every other object `wake=0` or `talk=1`.
- `[0:57]` under **Doors** (`Door=2 Shop=0`), `template-door:` line on map change.
- 568: the servant chain must still fire (its rects carry `talk`); watch for any
  `SKIPPED … WAKE rect` line there and check what it names before judging it wrong.
- Falsifier: a capture with the seven rects logged as skipped means a catch path that does not run
  through `FUN_0025c830` on those objects — the census + tiered fire log will carry it.

## Session 121 — 2026-08-01 — [navigation] The mechanism is the scope: 568's row is script-native, and the S120 machinery no longer exists there

**KEYWORDS: mechanism gate MapUsesEngineCatch engineCatch row scoped 568 untouched vanilla trigger
update fire hook passthrough tester instruction working map never widened onto**

Tester's instruction, verbatim requirement: S119/S120 must not have changed 568 — "if you need map
specific behavior then it should be map specific." They were right that it had: the guard-object
skip (S119) and the catch-rect rule (S120) were gated on the danger TABLE, and 568 has a row, so
its guards' trigger volumes were being skipped on the play-confirmed map.

The row now records WHICH catch the map runs — a measurement, not a switch: 568 = script-native
(`0x290` watcher + `0x26D`/`0x525` touch tests; S115's census), 569 = engine-trigger (zero touch
natives; wake rects riding the guards; S119's census). `PathDanger::MapUsesEngineCatch` exposes it,
and ALL of the S119/S120 machinery — catch-rect skip/decline, guard-object skip, event-fire
declines, the trigger census — is reachable only on an engine-catch row. On 568 the trigger update
and every event fire run VANILLA: its behaviour is the play-confirmed set (distance clamp + per-
guard touch suppression) and nothing newer. The only residue is inert: the scope-marker bracket
(two thread-local stores) and the fire hook's one boolean test.

> **RULE (tester's, now structural): a working map is never widened onto. New machinery starts
> scoped to the map whose defect bought it, and earns each additional map with play evidence.**

Verify: on 568 — zero `trigger census` / `SKIPPED` / `event fire` lines; `clamp ACTIVE` and
`touch SUPPRESSED … "Imperial"` exactly as S116's confirmed play. On 569 — everything S120 listed.

### PLAY RESULTS (2026-08-01, same day) — 569 IS CLEARED; and map 572 has an exit with no surface

**KEYWORDS ADDENDUM: 569 PLAY-CONFIRMED all seven catch rects skipped progressed 570 571 572
template-door confirmed Door=2 map 572 Garden Stairs exits missing controllers=1 surfaces=0 listed=0
nogroup=1 movie start rect event transition flags=0x1 dest=314 East Spur Stairs entrance=4 zero
map-jump surfaces elimination rule has nothing to pair**

**THE SNEAK TRACK IS DONE — the player made it through.** All seven catch rects logged
`trigger update SKIPPED … a talkless WAKE rect within 8m of a guard` at 00:59:47, no capture, and
the census trail reads 569 → 570 → 571 → 572. Five rounds, and the S119 census was the turn: the
rule that held is the one built from a measured catcher, not a model. 568 was not visited this play;
its row is script-native (S121), so its verification stands as scheduled.

**The template-door rule is PLAY-CONFIRMED too:** `template-door: obj [0:57] … container 0, 6
events … -> Category::Door`, and 569's rescan reads `Door=2 Shop=0`. (Note the template lives in
container 0 on this map — the fieldsign names are in the map's own pool; the anchor rule did not
care either way.)

#### NEW, FOR NEXT SESSION — map 572 (Royal Palace: The Garden Stairs) lists NO exits

```
exits: controllers=1 surfaces=0 listed=0 | dropped: nogroup=1 notused=0 unreachable=0
routine[7] "ムービー開始位置" arms NO group -> dest=314 ("Garamsythe Waterway: East Spur Stairs")
                                              entrance=4 flags=0x1 -- EVENT-BOUND
seams: swept 0 group(s) for map 572 in epoch 5
```

The map's ONE way out is an event transition (a "movie start" rect — walk to the stairs, cutscene,
arrive in the Waterway; `flags=0x1`, the same presentation bits as 313's staircase) and **the
walkmap carries ZERO map-jump surfaces**, so S105's elimination rule has nothing to pair — that rule
needed exactly one unclaimed surface, and here there are none. The tester's read is right: this is
the surface-goal / event-transfer class of work (S101/S102, the Sluiceway-era track; worktrees
`nav/surface-goal` and `nav/event-transfer` are still parked).

What next session has that S101 did not: **the S119/S120 event-table join.** The trigger RECT that
fires routine[7] carries that routine in its own event table (`object+0x48` ↔ `ExitDest::nameOff`,
`ObjectEventNameOffsets`) — so the transition can be bound to its trigger VOLUME by the map's own
data and the volume's position becomes the route target, no surface required. Also on the map: the
door object `[0:8]` "Door" at (84.0, 0.0, 40.4), `door=1 cat=2` — the +0x70 record claimed it, so
the map DOES have a field-sign door; whether the movie rect and that door are the same place is the
first thing to measure. `Exit=0` is the defect; the Door listing is the workaround the tester has
meanwhile.

## Session 122 — 2026-08-01 — [navigation] Map 572's exit has no surface: the event-table join binds it to its trigger rect

**KEYWORDS: map 572 Garden Stairs zero exits event transition movie start rect ムービー開始位置
dest 314 East Spur Stairs no map-jump surfaces exit_event_bind AppendEventBoundExits nameOff join
object event table container 0 handle table walk 1:1 or nothing candidates empty gate never widen
route to rect isTransition seamGroup 0 wake flag logged not gated nearest door measurement 569
backlog same family**

The S121 play ended on map 572 (Royal Palace: The Garden Stairs) reading `exits: controllers=1
surfaces=0 listed=0 | nogroup=1`: the map's ONE way out is routine[7] "ムービー開始位置" (mapjump →
314 "Garamsythe Waterway: East Spur Stairs", entrance=4, flags=0x1), an EVENT transition. It arms
no group, so S64's binding has no first half — and the walkmap carries ZERO map-jump surfaces, so
S105's elimination has nothing to pair either. `Exit=0` for the player; the `[0:8]` "Door" listing
was the tester's interim workaround.

### The fix — `exit_event_bind.cpp`, the S119 join object-side, route target = the RECT

The trigger rect that fires routine[7] carries that routine in its OWN event table
(`object+0x48`, entries are name-pool offsets), and `ExitDest::nameOff` is the same routine's
offset in the same pool — the join S119 shipped for door categorisation, now consumed by the exit
scan. `AppendEventBoundExits` (called from `ScanExits` between the candidate loop and the
reachability filter, so its entries face the routability table and the fail-open filter like every
other exit) walks the handle table for container-0 objects with event tables and binds each
DROPPED group-less event dest to the ONE object whose table names it. The rect's position becomes
the exit entry: `fixed`, `isTransition=true` (arriving IS crossing), `seamGroup=0` (no surface —
the seam machinery stays out), label = "Exit, <destName>", id band `-(2000+routineIndex)` as the
surface path already used for event-bound dests.

**The never-widen guarantee is structural, not promised (the S101 shape):**
1. gate `!candidates.empty() → return` — a map that lists ANY exit never reaches the join;
2. only dests the surface path already dropped qualify (`!viaController && group <= 0`);
3. **1:1 or nothing** (S105's honesty): zero or multiple matching objects bind NOTHING and log
   both counts plus each match's identity and position;
4. `haveSurfaces` gate — before this map's seam sweep lands, every map's candidates are empty and
   deciding then would open the pass map-wide on first frames.
No map id anywhere in the file; the gate is the mechanism. **Map 569's backlog
(`controllers=3 surfaces=3 listed=0 | nogroup=3`) is the same family and passes the same gate** —
expect three join attempts there; its three unclaimed-surface lines keep printing, honestly, since
this pass never touches surfaces.

**Deliberately NOT gated: the wake flag** (`+0xC` bit 5, S120's ENTER-scene handoff bit).
Requiring it would be a model of what a movie rect must look like — five rounds on 569 died to
models. It is LOGGED (`f8/fB/fC wake=`) so a bound rect that never fires carries its own diagnosis.

**The tester's open measurement ships in the log:** each bound rect prints its distance to the
nearest Door/Shop object the scan admitted — on 572 that answers "is the movie rect the same place
as door [0:8] at (84.0, 0.0, 40.4)?" from the first scan, before any design leans on the answer.
`dropNoGroup` is decremented on a successful bind so the exit inventory reports outcomes
(`listed=1 | nogroup=0`), not intermediate states.

There is no CROSSING ORACLE line for this class (the oracle keys on map-jump groups; this exit has
none) — the play is the oracle.

### Verify next play (572)

- `event-exit: routine[7] "…" -> dest=314 … bound to trigger obj [c:s] at (x,y,z) via event-table
  join` + the rect's flag line (`wake=` is the number to read) + the nearest-Door line.
- Exits list shows **Exit, Garamsythe Waterway: East Spur Stairs**; inventory reads
  `controllers=1 surfaces=0 listed=1 | nogroup=0`.
- `routable? "Exit, Garamsythe …"` line: `walk=`/`reach=` say whether the rect's centre sits on
  the walkmap; if `reach=0` the fail-open rule (would-drop > half) keeps it listed and says so.
- Route `\` to it: the route should end ON the rect and the cutscene fire → arrive 314. **Falsifier:
  arriving at the rect with no cutscene — then read the logged `f8/fB/fC` flags; a dormant rect
  (event-gated) is the first suspect and the flags are the evidence.**
- 569 (if visited): three `event-exit:` lines, bound or refused with counts — either way the
  backlog item gains its first measurement.
- Any OTHER map: **zero `event-exit:` lines** — one on a map that lists exits is the never-widen
  gate failing and is a bug regardless of what it says.

## Session 123 — 2026-08-01 — [navigation] The join measured ZERO on its first play; the +0x70 table had the answer all along — field-sign elimination

**KEYWORDS: map 572 event-exit join refuted nameOff 0x39A matches 0 objects event table names own
handlers not fired routines S120 lesson repeated rects field-sign elimination unclaimed live g0
record placard 85.95 32.00 61.08 usable shown exit arrow drawn doubled placard g1 same position
313 staircase pattern kSignMatchDist claim test 1:1 or nothing scan hole lower bound**

S122's build went straight to a play and the join's own falsifier fired: **`event-exit:
routine[7] … nameOff=0x39A matches 0 container-0 object(s)`, every scan, all visit.** No object's
event table names "ムービー開始位置". The S120 door lesson, now measured for rects too: **an event
table names the object's OWN handlers (`init|touch|touchon|SET_RECT|…`), never the routines those
handlers FIRE** — the movie routine is started from inside the rect's template machinery, so the
nameOff join misses by construction. The join has now missed for BOTH object classes it was
proposed for (doors S120, rects S123). It stays in the pass as the per-map measurement — its
match-count line is what settled this in one play — but it is not the binder.

### What the same log had been printing all along (the S101 instrument lesson, again)

The `+0x70` field-sign table on 572 — printed once per map since the sign-table diagnostic
shipped — reads:

```
sign g0[0] pos=(82.00,0.00,39.05)  … | nearest "Door" 2.43m  | CLAIMED -> doorway   (the way BACK)
sign g0[1] pos=(85.95,32.00,61.08) usable=1 shown=1 | nearest "Door" 20.75m | TOO FAR, unclaimed
sign g1[2] pos=(85.95,32.00,61.08) — same position, higher group
```

**`g0[1]` is a LIVE group-0 doorway placard at the top of the Garden Stairs (Y=32) that no object
claims — and `shown=1` means the game is drawing its exit arrow from that record this instant**
while the mod lists nothing. The doorway it describes is the EVENT rect, which the entity scan can
never list (nameless, non-interactive). And `g1[2]` doubles it at the same spot — the exact
doubled-placard pattern S105 measured at 313's event staircase. The room's one way out has been in
the log, with `shown=1` beside it, since the first scan.

### The fix — source 2 in `exit_event_bind.cpp`: field-sign elimination

After the join binds nothing: **exactly ONE unclaimed live group-0 record and exactly ONE
still-unbound group-less event dest ⇒ they are each other's**, and the record's position is the
exit. Any other count binds nothing and prints both counts — S105's elimination over the game's
placard table instead of the walkmap. The claim test reuses the S92 rule verbatim (non-NPC scene
object within `kSignMatchDist`), so this pass and `TagDoorwaysAndDropSignTwins` cannot disagree
about "claimed". All S122 gates unchanged: `candidates` empty (unreachable from a working map),
`haveSurfaces`, dropped dests only. On a correct map every live g0 record is claimed by its door
object — the unclaimed count is 0 and the rule cannot fire. Early in a visit even the back door's
record reads unclaimed → count 2 → declines until the handle table streams in. Fail closed,
self-heals.

**Printed limit, not hidden:** 572 reads `spans read=9, unreadable=6` — the 0x40 scan hole bites
on this exact map, so the dest-side count is a LOWER BOUND. 1:1 protects the day the hole is
fixed: a second dest surfacing flips the rule to decline, never to a different guess.

### Verify next play (572)

- `field-sign elimination: the ONE unclaimed live g0 record [1] at (85.95,32.00,61.08) usable=1
  shown=1 is routine[7] "…"'s -> dest=314 … INFERRED` + the `corroboration: g1[…] sits at the same
  position` line.
- Exits list speaks **Exit, Garamsythe Waterway: East Spur Stairs**; inventory
  `controllers=1 surfaces=0 listed=1 | nogroup=0`; `routable?` line says whether (85.95,32,61) is
  on the reach flood (233 polys from poly 184 — the stairs should be in it).
- Route `\` to it: up the stairs, movie fires → arrive 314. **Falsifier: standing AT the placard
  with no cutscene** — then the rect is elsewhere/dormant and the next instrument is a census-style
  dump of every container-0 object's position + event table on a gated map.
- The join line still prints `matches 0` first — expected, it is the measurement, not the binder.
- Any map that lists exits: **zero `event-exit:` and zero `field-sign elimination:` lines.**

### PLAY-CONFIRMED (2026-08-01, same day) — 572 IS CLEARED

**KEYWORDS ADDENDUM: 572 PLAY-CONFIRMED field-sign elimination works placard exit listed routed
movie fired arrived 314 tester confirmed**

Tester's verdict: **works.** The field-sign elimination listed the exit, the route led up the
Garden Stairs to the placard at (85.95, 32.00, 61.08), and progression continued into the
Garamsythe Waterway (314). Source 2 is the binder for this class; the nameOff join stays as the
per-map measurement only (its refutation is recorded in debug.md, Tried & Failed). The never-widen
gate held: no `event-exit:` / `field-sign elimination:` lines on any exit-listing map. Still open
from this track: the 0x40 scan hole (dest-side counts are lower bounds until it is fixed), and the
569 zero-exits backlog — both sources will print their counts on any 569 visit.

## Session 124 — 2026-08-02 — [navigation] The funnel kept the mirrored string: 315's replan "No path" was the S86 comparison outliving its fix

KEYWORDS: funnel polarity FLIPPED as-labelled BestPolarity mirrored shorter flood chord 61m map 315
northern sluiceway south bank replan no path MESH LABELLING ANOMALY POLARITY SELF-CHECK retired S86
winding authoritative repair ladder truncation three outcomes still breaching where surface-goal 128
floor starved kSurfaceGoalMinProbes 512 gate

**The tester's report:** two runs of 315 in one session. Beacon from the entrance crossed clean
(including resuming its old route after combat); walking off-route and re-requesting from the south
bank answered "No path" — a valid path became invalid with no game-state change. Tester's
requirement: surgical, this map, and understand WHY the route builder fails at specific points.

### The evidence chain (all from `FFXII-Screen-Reader-Latest.log`, seq=30–32, plus the archive)

- The three failing requests — and ONLY they, out of 32 — funnel `polarity=FLIPPED … <== POLARITY
  SELF-CHECK FAILED`. Every working route in the session is `as-labelled`.
- The mirrored funnel "wins" by 1.6% (`kept=223.6m other=227.2m`) because its final leg is a 61 m
  taut chord (102,95.8)→(153,62) across the terrain-refused flood — **shorter BECAUSE invalid**, the
  S86 mechanism verbatim (`debug.md`: "a funnel that accepts a bound on the wrong side cuts through
  the wall"). Its corner list shows the mirrored zigzag signature (string bouncing z=78↔82) — 35
  corners where the neighbouring successful request produced 17.
- Validation catches it honestly: `BREACH bad=34 … why=march … nbrEff=0x17B00000` (bit 23, flood).
  `corridor march: CLEAR over 153 hop(s)` — the CORRIDOR walks; the polyline is the lie.
- The ladder burns ~1150 probes 0-for-4; `inset 0/150 moved, 0 LEFT their poly` — **S117's inset
  suspect answers 0/0 on both outcomes and is exonerated**; the chord-inferred re-cost wanders
  (poly 503→504 on a byte-identical stop) so the S115 escalation's same-portal identity never
  engages; two attempts drain `kProbeBudget=1600`.
- `surface-goal: … corners=16/147 probes=128 -> REJECTED (budget ran out -- NOT verified)` — the
  S101 designed endpoint TOUCHED the surface and built a route, then starved at the shared
  `kFrontierMinProbes=128` floor. Measured cost of successful seam validations on this map:
  416–460 probes. `oracle:` confirms the goal is in the component — the search gave up.
- **The archive sweep that settled scope:** in every log since the S86 sign fix, `polarity=FLIPPED`
  count == `POLARITY SELF-CHECK FAILED` count, per file (34, 3, 4, 12, 20, 22, 6) — **zero
  legitimate flips exist anywhere**, all on 315's south bank (y≈13, x≈38–90), all ending
  Frontier→"No path". This is also the true identity of the S115/S116 "Waterway No path" replans,
  which were filed under the destination exit's name (North Spur Sluiceway). The defect printed in
  the log for four sessions — S101's lesson, again.

### What shipped (built + deployed, NOT play-confirmed)

1. **`path_funnel.{h,cpp}` — `BestPolarity` returns the AS-LABELLED polyline unconditionally** (the
   one behavior change). The winding is engine-forced (`GameArchitecture.md`, VERTEX WINDING), so
   the labelling is the authority and the length comparison is demoted to an instrument: both runs
   still execute, and a mirrored-shorter measurement on a ≥2-portal corridor sets an `anomaly` flag
   the callers log as **`MESH LABELLING ANOMALY`**. All four outputs (polyline, both lengths,
   portal indices) switch together — a polyline with the other run's indices would mis-address
   `Unpull`'s splices. The mirrored polyline never leaves the function. This finishes `debug.md`'s
   S86 entry ("do not reintroduce keep-the-shorter"), which the code had kept as a live selector.
2. **`path_search.cpp`** — funnel log line: non-anomaly format byte-identical to before (that is
   what proves working-route invariance by diff); anomaly appends the marker. `POLARITY SELF-CHECK
   FAILED` and `polarity=FLIPPED` are now unprintable.
3. **`path_corridor.cpp` / `path_surface_goal.cpp`** — the two `BestPolarity` call sites that had NO
   polarity visibility now log the anomaly (log-only; frontier routes are never spoken/beaconed,
   and the surface-goal line gains a suffix).
4. **`path_repair.cpp` — rung honesty (log-only; `good`, budget flow, rung order untouched):** three
   outcomes instead of two — `OK` / `still breaching @ leg N/M stop=(…) why=…` / `ran out of probes
   (NOT verified)`. Closes the logging half of the S116 holdback (`debug.md` entry updated); the
   re-measure condition was met: the 315 failures marched CLEAR, took the ladder branch, and the
   pressure did not disappear.

### Why this cannot change a route that works today

On every logged working route on every map, as-labelled already wins the comparison, so the returned
polyline is **bitwise identical** — the anomaly condition is itself the scope (today: only 315's
south bank), with no map id anywhere (the tester asked for map-specific containment; this is
containment by defect signature, which a mapId gate could not improve on other maps and would
under-cover on future ones). The two previously-unlogged call sites are failure-path-only. Review
caveat, recorded honestly: `repair[full-corridor]`'s candidate is polarity-INDEPENDENT (portal
midpoints) and still breached with what looks like a real validation (338 of ~776 available probes)
— so "the bank now routes" is a hypothesis under test, and the new rung breach-location line is the
instrument that names the residual if one remains.

### Pre-designed, NOT shipped — the surface-goal floor (gate in `debug.md`, S116 holdback entry)

If the confirming log still contains `surface-goal: … REJECTED (budget ran out` → ship
`kSurfaceGoalMinProbes = 512` at `path_search.cpp`'s surface-goal call site only (frontier keeps
128; sized from the 416–460 measurement). Zero hits → do not ship.

### Verify next play (map 315, south bank x≈38–45 and x≈70–90, y=13; request the North Spur exit)

- `FRONTIER SUPPRESSED for "Exit, Garamsythe Waterway: North Spur Sluiceway"` count == 0; bank
  presses end `plan=Route` (`pass=mesh` or `pass=surface-goal`).
- `MESH LABELLING ANOMALY` ≥ 1, every hit inside a 315 epoch, every hit on an `as-labelled` line.
- `polarity=FLIPPED` == 0, `POLARITY SELF-CHECK` == 0, `pass=seam` == 0 (S99 rule); the entrance
  route still `pass=mesh`, working routes still `attempts=1`, and repeated from/target pairs print
  byte-identical `funnel:`/`say=`/`legs=` lines against the previous log.
- `repair[…]` lines may newly read `ran out of probes (NOT verified)` — rung ORDER per Mend must
  match the old log for identical requests.
- Build-2 gate: grep `REJECTED (budget ran out` — see above.

### ✅ PLAY-CONFIRMED (2026-08-02, same day) — every gate passed on the first play

**KEYWORDS ADDENDUM: S124 play-confirmed map 315 south bank poly 1406 routed 24 requests zero no
path anomaly fired 11 times mirrored 47.6m vs 140.2m two-thirds Build 2 NOT NEEDED gate closed
surface-goal never invoked invariance byte-identical**

Tester walked the bank from x≈26 to x≈127 requesting directions continuously. **"I was able to
request directions continuously and the path never fell back to invalid."**

**The problem ground was covered, measured against the pre-fix log's own failure coordinates:**
start poly **1406** — the poly that answered "No path" three times — was the start poly for two
requests here and **both returned full routes**; `from=(37.34,13.00,79.94)` sits **0.86 m** from the
pre-fix `from=(38.20,13.00,79.98)` failure. The second failure band from the 08-01 logs
((69.98,78.70), (77.14,79.61)) was covered by `(70.55,13.00,78.50)` and `(82.25,13.28,77.75)`.

| Gate | Result |
|---|---|
| `say="No path"` / `FRONTIER SUPPRESSED` / `[BEACON] stop` | **0 / 0 / 0** across 24 requests |
| `polarity=FLIPPED`, `POLARITY SELF-CHECK`, `pass=seam` | **0 / 0 / 0** — all three grep-dead |
| `MESH LABELLING ANOMALY` | **11**, every one on map 315 (single epoch), every one on an `as-labelled` line, every one ending `plan=Route pass=mesh` |
| Search health | 21 of 24 validated `checked=N/N … OK` on attempt 1; **no request ever reached attempt 2**; zero re-costs; ladder ran 3× and rung 1 repaired all three |
| Invariance vs pre-fix log | entrance route identical (`startPoly=39 expands=690 touched=993 volumeRays=1156`); the repaired route's line character-for-character identical (`repair[unpull]: leg 2, 25 -- 18->42 points … probes=345 -> OK`) |

**THE ANOMALY IS FAR LARGER THAN THE 1.6% IT WAS DIAGNOSED AT.** Walking east the mirrored funnel's
claimed saving grows monotonically: `235.5/231.8` (1.6%) → `195.1/160.2` (18%) → **`140.2/47.6`
(66%)**. The last one is the bug in one line: the mirrored string claimed a **47.6 m** path where
the real walk is **140.2 m**, by cutting across the flooded channel — and under the old code that
number WON and became the route. Corner counts confirm the mechanism: the corrected string-pull
yields `corners=17/152` where the pre-fix mirrored run yielded 35 (the zigzag signature).

### ❌ BUILD 2 IS NOT NEEDED — the gate returned zero, it is CANCELLED

`REJECTED (budget ran out` == 0, and `surface-goal:` appears **zero times** — the surface-goal path
was never invoked at all, because the ordinary mesh route validated on attempt 1 every time. The
128-probe starvation was a SYMPTOM of the ladder burning the budget on an illegal polyline, not an
independent defect. **`kSurfaceGoalMinProbes = 512` is withdrawn; do not ship it without a NEW log
that shows the REJECTED line.** (S116's holdback discipline — re-measure before sizing — is what
stopped a 512 that would have been fixing nothing.)

### Scope of this confirmation — stated exactly, not rounded up

The tester **walked to the problem ground and a short way past it, then stopped**; the log ends at
x≈127 with the exit at x=153. So what is confirmed is **the defect**: continuous re-requests across
the ground that produced every recorded "No path" now produce valid routes, with the anomaly
instrument proving the defect was live at those coordinates. What is **not** in this log is a
complete traverse to the exit and the map transition. Tester's own read: *"I'm 99% sure it would
continue to work"* — recorded as their expectation, not as a measurement. A future 315 run that
reaches the North Spur transition closes that last gap; nothing is owed before then.

### Residual, downgraded

The `repair[full-corridor]` caveat from the build notes (its candidate is polarity-independent and
still breached pre-fix) **did not materialise**: the ladder ran only 3 times, rung 1 repaired all
three, and `full-corridor` was never reached. The three leg-2 breaches at (22.98–25.50, y≈10) are
PRE-EXISTING and unchanged — the identical breach at the identical 63.4 m appears in the pre-fix
log and repairs identically.

## Session 125 — 2026-08-02 — [menus] The element was a codec escape all along; Game Over's silence is structural

**KEYWORDS: element sprite 0F 3F 81 8A-91 attribute_data.bin word.bin chunk 4 idx 23 pool 0x2017
ElementName ElementNames AbilityElements action row+0x13 damage type SetElementSpriteResolver
adjacency suppression no-list popup win+0x3C4 bit0 win+0x1B0 FUN_00241d40 Game Over gameover_c.tm2
DAT_022c83e8 0x21A83E8 leader select FUN_00298250 0x4C6 ResolveStringById probe_equip_compare**

Three tester-reported gaps, all of the same shape: **the game draws a picture where a word belongs,
or raises a window shaped so that no reader ever sees it.**

### 1. Element affinity — the "picture" is `0F 3F 81 <XX>`, and it was already framed correctly

`Bugs.txt`: *"all of them like said half damage and then like you know immunity to like poison or
blind but they don't do just like half damage… I believe the element is a picture."* Exactly right,
and the diagnosis explains the asymmetry: in an equipment detail panel the STATUS list is real
words (`FUN_0035d330(0x1A, bit) + 0x18`), which is why "poison"/"blind" always read, while the
ELEMENT is an inline sprite escape.

**`0F 3F 81 <XX>`, XX = `0x8A`..`0x91` = fire, lightning, ice, earth, water, wind, holy, dark.**
Selector `0x3F` already returned 2 parameter bytes in `EscapeParamCount`, so the FRAMING was never
wrong — the escape was consumed and nothing emitted. The fix is additive.

Established three independent ways, all offline, **conf 0.99**:

1. `menu_expansion.bin` string ids `0x4B27+n` — the very strings `FUN_00293ce0:41` appends per set
   element bit — each hold exactly `0f 3c c1 fe | 0f 3f 81 <8A+n> | 0f 3c 81 80`.
2. `help_action.bin` names every sprite in its OWN PROSE: `"Deal <0F 3F 81 8A> fire damage to one
   foe."`, once per element. The game put the byte and the word side by side in shipped data.
3. `word.bin` chunk 4 idx 23..30 = Fire, Lightning, Ice, Earth, Water, Wind, Holy, Dark, and
   `attribute_data.bin` (st2e, 8 x u16) holds pool ids `0x2017..0x201E` **in bit order** — the
   game's own binding, so `PoolString(0x2017 + bit)` is not an invented constant.

A scan of all 27 US st2e master-data files found `0x3F` used **only** for these eight, plus one
stray `0F 3F 46 00` in `battle_pack.bin` that fails the `0x81` test and is left alone.

**NO WIKI AND NO HARDCODED NAMES WERE NEEDED.** The plan allowed a wiki-derived table as a
fallback; the game supplies every name, so none was written.

Shipped: `BattleState::ElementName(bit)` / `ElementNames(mask)` beside `StatusName`;
`GameText::SetElementSpriteResolver` (a registered callback, because `core/` must not depend on
`battle/`), wired once in `dllmain.cpp`; the decoder parks a private-use marker and
`ResolveSprites` turns markers into words once per page, so `Decode` and `DecodePages` cannot
diverge.

**ADJACENCY SUPPRESSION, and why it is not the banned dedup.** In ability help the game writes BOTH
the sprite and the word, so naive substitution gives "fire fire damage"; in the item panel the
sprite stands alone. A caller-based discriminator is impossible — both strings come out of the same
formatter, `FUN_00292b70`. So the rule lives in the decoder: emit the name unless the immediately
adjacent word already IS that name. **This is a within-string RENDERING decision, not cross-event
suppression** — nothing is cached, no window, no last-spoken state, re-entry always re-speaks in
full, so the silence-on-re-entry failure the NO-DEDUP rule exists to prevent is structurally
impossible. The default is to EMIT. **User approved the carve-out explicitly this session.** In a
locale that declines the noun the match fails and the element is said twice — a stumble, never
silence.

Written as a general seam because `debug.md:1287-1330`'s `0F 2E <80 90>` tutorial name-slot is the
same family and should plug in here, not into a second path.

### 2. Damage type in battle — the sprite is NOT there, the action record is

The tester asked for the damage type in combat. **`battle_message.bin` contains zero `0F 3F`
escapes**, and `combat_system.md` §5.5 already recorded why: no element survives to the apply site
(the damage math's scratch bank is zeroed per calculation, only the integer is kept). So Task 1's
decoder buys nothing here.

**Action record `+0x13` is the element mask** — same bit order, **conf 0.99**:
15/15 named spell families match (Fire/Fira/Firaga `0x01`, Thunder family `0x02`, Blizzard family
`0x04`, Aero/Aeroga `0x20`, Holy `0x40`, Dark family `0x80`); correct negatives (Cure, Shock,
Scathe, Bio all `0x00`); and **across all 543 rows the byte only ever holds zero or ONE bit** —
values are exactly `{0, 1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80}`, distribution 447 non-elemental then
19/11/10/8/13/13/9/13. A byte meaning something else would not fall out that way. That property is
also the runtime self-check: `ElementNames` logs any multi-bit mask.

Nearly free: `AbilityCategory` already does this exact pure read on this exact table at `+0x1E`, and
`HookedApply` already carries the action id. `DamageLine` gained an `elementMask` parameter and
appends the element to both the number branch and the outcome branch ("Immune. Fire" is arguably
the more useful of the two). Suppressed when the action's own name already spells the element.

**KNOWN GAP, deliberate:** a basic Attack (category 0) takes its element from the WEAPON, not the
action record, so a flame-sword hit reports none. Better silent than wrong.

> **DEFERRED, not built — the Verbose charge announce.** The plan called for suffixing the element
> to the charge announce, which is where the warning is actually actionable (before the hit). The
> charge announce is a **Tier-1 game message** through `HookedSprintf` on `FUN_00536410`, and that
> hook carries only the **message id** (`argBlock+4 & 0x7FFF`) and the finished string — **not the
> action id**, so there is nothing to look the element up with. The arg-block layout past `+4` is
> undocumented; inferring it would be exactly the sub-0.98 guess the project bans. Reverse-matching
> the decoded action NAME across 543 rows is fragile and non-unique, and hooking the charge site
> `FUN_00304850` is a new hook that risks double-speaking against the game's own announce.
> **Consequence: the element is readable in the combat log, but nothing new is spoken mid-fight.**
> Reopening this needs the arg-block layout established first.

### 3. Game Over was silent for a STRUCTURAL reason, not a missing string

`FUN_0035c660` → `FUN_00241c90` → `FUN_002465f0(win, 0x3D0, FUN_00241d40, ...)` — **the same class
`PopupReader` already reads at `+0x1B0`.** The creation packet's `+0x2C` lands at `win+0x3C4`, and
`FUN_00241d40:88` shows the branch: `if ((*(byte*)(win+0x3c4) & 1) == 0) { ... *(win+200) = <list> }`.

**Bit 0 SET means the window builds NO list**, so `win+0xC8` stays 0, no `0x8000` focus is ever
emitted, `OnFocus` never runs and `BodyText` is never consulted. That is the whole reason Game Over
says nothing while the Load Game menu reached from it works fine. The two shapes are **disjoint by
the game's own branch**, so the new construction hook can never double-speak with the focus path
and needs no latch. **This fixes every no-list prompt in the game, not just Game Over.**

The body text id Ghidra dropped from `FUN_0035c660` turned out to be **unnecessary** — the body is
readable at `win+0x1B0` for every no-list prompt.

`PopupReader::SpeakBody` is now the single emit point; `menu_reader.cpp`'s existing focus-driven
body branch was repointed at it so the two cannot drift on wording or interrupt policy.

**"GAME OVER" is baked art** (`PS2Data\...\Tm2_Menu\gameover_c.tm2`), like the title logo — no codec
text exists. The announce is therefore gated on the game's own STATE, not the picture:
`DAT_022c83e8 & 0x02` (the body of `FUN_0035c740`, party wipe). Leader-down and guest-wipe variants
fall through to their own bodies with nothing added. `Phrase::Id::GameOver` — user-approved.

### 4. Leader-select — SHIPPED ON BEST GUESS, no probe (user decision)

Two surfaces can present "choose a new leader" and it is **not settled** which one the player sees:
`FUN_00298250` hangs off the BATTLE context (`DAT_0209be80`) while the list the mod already reads
hangs off the PAUSE context (`DAT_0209ac30`). The user declined a probe for it, so **both are wired
and whichever never appears never fires**:

- **Body:** hook `FUN_00298250` (RVA `0x178250`) case 1, resolving `FUN_002f9860(0x4C6)` (=
  `menu_command` 222 "Select Leader", used nowhere else in the binary) with `0x4A49` as fallback.
  Resolving the ids beats chasing the conditional widget chain the function threads them through.
- **List:** `HookedStatusCursor` gained a leader-down arm gated on a pure read of
  `DAT_022c83e8 & 0x10` (the body of `FUN_0035c750`), speaking `Phrase::Id::Leader` for the row that
  is already leader.

**TWO WITNESSES before believing the row flags.** `row+0xFC` bit 3 = in party / bit 4 = leader is
documented, but the pointer we read it through is the PORTRAIT child and "portrait == row" was never
measured. Bit 3 has an independent witness the mod already trusts (`ctx+0xB10+charId`, the byte the
game's own toggle writes), so the leader bit is only believed when the two agree on in-party.
Otherwise: say nothing extra.

A `LEADER` log line names whichever hook fired, or reports that neither did — that is what the next
pass tunes against, with the existing log-only unclaimed-pane census (`menu_reader.cpp:452-485`) as
the fallback instrument if both stay quiet.

### 5. Centralization — `TextCapture::ResolveStringById`

`status_reader.cpp` and `inventory_reader.cpp` had each grown a private `ResolveMsgCodec` + decode
pair, and the leader prompt was about to add a third. One resolver now: cache-first, falling back to
the `FUN_002f9860` **trampoline** (so it neither recurses through our own hook nor pollutes the cache
with ids nothing drew). Game-thread only.

Note `TextCapture::StringById` caches only ids `1000/1001` and `0x46dc..0x4882` — **`0x4A90` and
`0x4C6` are both outside that band**, so anything keyed on those ids must use the resolving variant.
The Session-71 confirmation of the five attribute labels came from `status_reader`'s own game call,
not from the cache.

### Confirmation strategy — ONE probe, for the shop only (user decision)

The user declined probes for Tasks 1 and 3 as too much setup. Both ship on their offline evidence
plus **log lines the mod writes itself**, and every uncertain path degrades to SILENCE:

| Instead of a probe criterion | The mod now logs |
|---|---|
| pool `0x2017..0x201E` = the eight names | `ELEM 0=Fire 1=Lightning ...` once, at first resolve. A blank means that sprite is dropped. |
| the bytes either side of a sprite | (covered by play: a doubled or missing element word is audible) |
| Game Over `win+0x3C4 & 1`, `win+0x1B0` | `POPUP nolist flags=... wipe=... spoke=0/1`; `spoke=0` = the body read is wrong and we stayed silent |
| which leader surface | `LEADER` line naming the hook that fired |
| action `row+0x13` | any mask with more than one bit set — never happens across the 543 shipped rows |

`frida/probe_equip_compare.js` authored for the SHOP work (Task 2), not yet run. Its criterion 1 is
the make-or-break one: **`FUN_002ca7c0` computes every delta as `current - new`, so a BETTER item
should report NEGATIVE.** An inverted "Attack Power up 12" is worse than silence — the player buys
the wrong sword. Criterion 8 measures whether the keys `8`/`9` are free by watching the game's own
pad words (`DAT_02f97368/6a/6c`, filled by `FUN_002498b0`) — sight-free, because S112 established
the Controls config screen is not evidence and the tester cannot read the on-screen keyboard.

**Task 2 (the shop equip comparison) is NOT built** — it is gated on that probe by the approved
plan's own sequencing.

### RVA arithmetic caught in review

`FUN_002ca7c0` is RVA **`0x1AA7C0`**, not `0x1AC7C0` — an easy digit slip against its neighbours
`0x1AC4F0` / `0x1AC780`. Every RVA in the probe was re-derived against the decompile filenames
before it shipped.


### Session 125 (continued) — probe results, the Fire-only bug, and the shop comparison

**KEYWORDS: ResolveSprites find bug kSpriteMark range Fire-only probe_equip_compare polarity
cur-new equip_compare equip_target_reader keys 4-9 collision watch DAT_02ca97a0 null first fire**

#### ⛔ STRIKE — "the element sprite fix works": it worked for FIRE ONLY

`ResolveSprites` early-outed on `s.find(kSpriteMark) == npos`. `kSpriteMark` is `0xE000`, the
marker for element **bit 0**, so a string carrying any other element (0xE001..0xE007) matched
nothing and the function **returned before resolving anything**. Fire read; the other seven were
dropped exactly as before the fix.

Caught from the DESC dump, and the evidence is as clean as it gets — two weapon descriptions whose
raw bytes differ in exactly one place:

```
desc #10  ... 0f 3c c1 fe 0f 3f 81 8a 0f 3c 81 80 ...   ->  "Element: Fire"
desc #11  ... 0f 3c c1 fe 0f 3f 81 8c 0f 3c 81 80 ...   ->  "Element: "
```

Same shape, same surroundings, `8a` vs `8c`. Fixed by testing the whole mark RANGE
(`IsSpriteMark`) instead of the base character.

**LESSON: a range encoded as `base + n` cannot be searched for with the base.** The bug survived a
build, a deploy and a report because the ONE element anybody happens to test first — Fire — is the
only one that worked. The tester's "not half damage" was the symptom; "half damage" items in that
save happened to halve something other than Fire.

**And it was invisible without the raw bytes.** The decoded string alone (`"Element: "`) says only
"nothing came out"; it took the hex beside it to show the sprite WAS present and correctly framed.
That is what the DESC dump was for.

#### The DESC dump's other finding — budget by gate state, not one shared cap

All 12 of the first run's dumps logged `gated=0`: `FUN_00292b70` also runs for OFF-SCREEN WIDTH
MEASUREMENT, and those calls exhausted the shared budget before a single display-path call was
seen. Split into separate caps (10 gated / 4 ungated). **A shared cap silently favours whichever
path fires first, which is exactly the path you are not investigating.**

#### Probe A results — shop equipment comparison

Full detail in `GameArchitecture.md`; the load-bearing points:

- Structure confirmed exactly as derived: 1 header + 9 column calls per refresh, **zero stray delta
  targets**, `canEq`/`eq` agreeing with the equip screen's own witness, labels readable straight off
  the header widgets.
- **`DAT_02ca97a0` is NULL on the first `FUN_0057b1a0` fire.** A reader keyed on that global misses
  the screen's entry announcement; `EquipTargetReader` validates the window the hook is handed.
- **Polarity is `cur - new`, so NEGATIVE = better** — settled at 1.00 from Ghidra's own local
  declarations (`local_b8[10]` then `local_ae`; `local_8c[10]` then `local_82`), NOT from the log.
  The log genuinely cannot decide it: delta went 80 -> 0 when the tester equipped the item, which
  fits both signs. Worth remembering — **a before/after pair that collapses to zero calibrates
  magnitude, never direction.**
- Only kind-1 (weapon) passes were captured, so the two-label kinds rest on code alone.

#### Shipped — the shop equipment comparison

`src/ui/equip_compare.{h,cpp}` (two hooks: `FUN_002cc4f0` brackets a refresh, `FUN_002cc780`
attributes each delta by pointer-matching `target` against the nine columns) and
`src/ui/equip_target_reader.{h,cpp}` (`FUN_0057b1a0`). `EquipCompare` speaks nothing — it snapshots
on the game thread and owns the wording; each surface has its own single emit point.

Keys **`4`-`9`** address columns 1-6, context-gated on a live, class-validated panel. Outside that,
`4`-`7` keep their party-slot meaning and `8`/`9` are silent. The gate is structural, so leaving a
shop restores party status with no state to unstick.

#### `8` / `9` are NOT proven free — and the probe could not prove it

Criterion 8 was designed wrong: its pass condition was "no new pad value appears", which is
**indistinguishable from "the key was never pressed"**. That is the S111 lesson again — *an absence
cannot prove a negative*. The capture shows only ordinary menu-navigation pad values, so it says
nothing either way.

Rather than assert freedom, `input_tracker.cpp` now WATCHES: on the poll where `8` or `9` goes
down it reads the game's own pad words (`DAT_02f97368/6a/6c`) and logs `COLLISION?` if a bit moved.
Absence of that line still is not proof of freedom, but its presence IS proof of collision — and
that is the direction that costs the player something. The mod cannot swallow keys, so an unnoticed
collision is exactly the F9 failure of S112.


### Session 125 (continued) — the status cap, the field Equipment preview, and two bleed bugs

**KEYWORDS: equip_detail ExpandCollapsedStatuses listhelp_common base 0x2328 StatusName
includeSuppressed field equipment preview panel+0xEC menuCtx+0x138 FUN_003fe5d0 0x4A90 item desc
vs description bar g_itemDesc shop surface gate DAT_02ca9798 stale snapshot**

#### The ">=4 statuses" cap — removed

`src/ui/equip_detail.cpp`. The game popcounts the mask and only enumerates below four
(`FUN_00293310:157-212`), printing "Immune: Various status effects" otherwise — which on an
accessory is the entire point of the item. The mask is still at record `+0x44`/`+0x48`, so the
names are recoverable.

**Substitution, not appending.** The needle is the game's own `0x2335`/`0x2336` string, so the
replacement is locale-correct by construction and reads `"Immune: Sleep, Confuse, Silence, Blind"`
rather than saying "Immune" twice. If the phrase is not found, nothing is patched and a `DESC` line
records the disagreement — **never patch a string you cannot locate.**

**listhelp_common string ids: base `0x2328`, verified 18/18** against the shipped file (full table
in `GameArchitecture.md`). The earlier positional guess happened to be right, but it was a guess;
this is measured, and it is what makes the substitution safe.

`BattleState::StatusName` gained `includeSuppressed`. KO / Invisible / HP Critical / X-Zone carry
`rec+0x02 == 0xFF` and are hidden on the battle HUD as noise — but the item panel lists them (it
reads the master name with no suppression check), and an accessory that blocks KO is exactly what a
buyer needs to hear. **A suppression rule that is right for one surface is not a property of the
data.**

STILL HIDDEN, deliberately: when both masks are non-zero the game shows only the immune one. Not
expanded — that would invent a row the panel never had. Logged when it happens.

#### The field Equipment screen has a preview too — and it is a DIFFERENT mechanism

Confirmed from a tester screenshot: the pause menu's Equipment screen shows
`Attack Power 108 > 14`, `Evade 30 > 5` — one character, `current > preview`, an **absolute new
value**, not the shop's per-character signed delta.

That is the S71 attribute panel: `menuCtx+0x138` (class `FUN_003fe5d0`), current at
`panel+0xC8 + row*4`, preview at `panel+0xEC + row*4`, labels `FUN_002f9860(0x4A90 + row)`, rows
0-8. `status_reader` reads `+0xC8` and has never read `+0xEC`.

`EquipCompare` now has TWO sources behind one interface. Only one can be showing, and the shop's is
tested first because its surface test is stricter. On the field screen `ColumnCount()` is 1 (one
character), so key `4` reads it: `"Balthier: Attack Power 14, down 94, Evade 5, down 25"` — the new
value plus direction and size, in the shop's vocabulary. Unchanged rows say nothing.

"Is a preview showing" needs no extra state: it is `current != preview`, the same test
`FUN_003fe490:31` uses to decide whether to draw the arrow at all. On the Status screen the two are
always equal, so this reports nothing there — which is why that screen shows no arrows either.

#### ⛔ BUG — the shop comparison bled into the pause menu

The compare panel is **parked in menuCtx and survives leaving the shop**, so `IsLive()` (a class
check on `menuCtx+0x2E0`) stayed true afterwards and the keys kept answering from the last shop
item while the player browsed their own inventory. A stale comparison against something the player
is no longer looking at is worse than silence.

Fixed by requiring a surface that actually DRIVES the panel: the live shop list container
(`DAT_02ca9798`, class-validated) or the equip-target screen. `LineForMember` got the same gate.

**The approved plan had this exact gate written down and the implementation dropped it** — it kept
only the two cheap checks (`!= 0` and the class test) and lost the third. *An object still existing
is not the same as its surface still being on screen*, and the plan said so before the code did.

#### ⛔ BUG — `o` read the pane help instead of the item description

On the field Equipment screen, highlighting a weapon formats its detail panel and then the pane
re-sets the description BAR to its own static help ("Change equipment."). **Both wrote
`g_helpText`**, so it was last-writer-wins and the bar won on every row — the `o` key read the same
sentence forever, which sounded stale because it was.

The DESC dump is what separated the two candidates: `gated=1` fires once per highlight, seconds
apart, with the correct per-item bytes. So capture was never the problem — *the text was being
captured and then overwritten*, which no amount of staring at the capture path would have shown.

Fixed with a second slot (`g_itemDesc`) and arbitration in `CurrentHelpText`: the ITEM detail wins
over the BAR when both belong to the current focus, falling back to the bar so every surface with
only one of the two behaves exactly as before. Two texts that mean different things must not share
one variable.

#### Keys 8 and 9 — confirmed free in play

Tester report, and zero `COLLISION?` lines in the log. The watcher stays in: it costs nothing and
its evidence runs the other way (a line proves collision; silence never proved freedom).


### Session 125 — PLAY RESULT: all confirmed working

Tester, 2026-08-03: element sprites, the battle damage type, Game Over, the leader popup, the shop
comparison on `4`-`9`, the equip-target screen, the expanded status lists, the `o` fix, the
shop-context gate, and the field Equipment preview on highlight — **all confirmed working in one
pass**. Keys `8` and `9` confirmed free; zero `COLLISION?` lines in the log.

**Two surfaces, two behaviours, deliberately.** The shop comparison stays on KEYPRESS (six
characters x two stats is too much for every cursor move); the field Equipment screen volunteers its
line automatically (one character, usually one or two changed stats). Tester: *"you put the delta
reader on keypress which is perfect."*

**AUTODETAIL (F7) now has its first defined consumer** — written up in `Docs/Controls.md` under
"Equipment comparison — and the AUTODETAIL mode it is waiting for". The short version: when the
toggle is built, ON makes the SHOP comparison volunteer itself per highlight, queued behind the item
name; OFF is today's behaviour. `EquipCompare::LineFor` already produces the text, so only the
trigger and the toggle are missing. Two constraints recorded there because both are easy to get
wrong: the keys must keep working in BOTH modes (a toggle that removes a way to ASK is a regression,
not a setting), and the announce must go through the surface's existing choke point rather than a
new speaker — two speakers on one surface race, and that is exactly how the notice board lost its
Status column.

**The Equipment screen's automatic line is NOT autodetail already existing.** It is one line about
one character on a screen whose whole purpose is that comparison. Recorded explicitly because a
future session will otherwise read it as the feature and skip building the real one.

### Session 125 — the charge-announce element: OFFERED AND DECLINED

Recorded because it was carried as "deferred/blocked" for most of the session and that framing is
now wrong. **It is not blocked and not unfinished — it is unwanted.** User decision 2026-08-03.

**The load-bearing reason is that it is NOT ACTIONABLE.** Charge windows are short, the abilities
that matter are usually buffs and debuffs prepared in advance, and there is no response a player can
execute in the window available. A warning nobody can act on is just more words mid-fight. That is
the tester's judgement from play, and it is a better argument than the one this entry first
recorded.

Two secondary facts, kept so neither is mis-remembered:

* **The game does NOT draw the element in the charge message.** `battle_message.bin` has zero
  `0F 3F` sprite escapes; ids `0x0D`/`0x0E`/`0x0F` are plain `"{0} begins casting {1}."`. So unlike
  the item panel — where the game really was drawing something a sighted player could see and the
  tester could not — this was never surfacing hidden art. It would have been the mod ADDING
  information the game never shows, which is a much weaker proposition.
* The mod COULD still have supplied it: the element is in the action record at charge time, so
  `Belias begins casting Pain Flare` would have read as fire. **"The data is unavailable" is
  therefore not the reason — the data exists and the feature was still not worth having.** Do not
  re-open this on the strength of rediscovering the field.

Process note: the original pitch called the charge announce "where the warning is actionable". That
was an INFERENCE about how it would be used, never checked against play, and it was wrong. The
objection that killed it was about the GAME, not the code — and it came from the person who plays it.

The unblock recipe (pair the Tier-1 message with the charge site `FUN_00304850`, RVA `0x1E4850`,
which holds the action id) stays in `GameArchitecture.md` so it need not be re-derived if the call
is ever reversed. **Do not put it back on a to-do list.**

## Session 126 — 2026-08-03 — [nav+menus] Four defects that had been printing in the log for sessions

**KEYWORDS: entity_list kFieldContextBits 0x07 0x03 NavSafeFailMask bit2 CondAreaId failMask 0x0C
areaId areaManifest S93 Bhujerba 805 806 announce mapId map_script MAPJUMP_FLAGS_FIELD_DOOR
MAPJUMP_FLAGS_WORLDMAP_MENU jumpFlags 0x2 __MJ_CTRL001 __MJ_CTRL002 REJECTED Lhusu Mines 357 Oltam
Span 358 NO CONTROLLER CLAIMS THIS GROUP shop_reader OnListRefreshed FUN_005655f0 g_lastContainer
recycled dialogue_reader PageKey widget ForgetLivePages clerk re-entry**

Four tester reports from one Bhujerba / Lhusu Mines session. **Every one of them was already in the
log, and had been for sessions.** Nobody had grepped for the anomaly lines the mod prints about
itself — the same lesson as S101/S102 and S124, now on its third repeat.

### 1. A whole city that never announced its sections — and it was NOT the name dedup

Report: *"in this city, entering another section of the city does not fire the map transition
announcement."*

The obvious suspect is `s_lastArea`, the sanctioned per-frame dedup in `entity_list.cpp`. **The log
refutes it outright:** Rabanastre announces section-to-section perfectly — Southgate, Southern
Plaza, East End, Muthru Bazaar, Migelo's Sundries, Amal's Weaponry, all consecutive, all spoken.

The real discriminator was sitting in the routing log the whole time. Across 21 logs, with zero
exceptions:

* `failMask=0x00` → announced. **11/11** (Rabanastre 290/291/292/295/297/306, Giza 236,
  Garamsythe 315, Lhusu Mines 357).
* `failMask=0x0C[areaId(log),areaManifest(log)]` → silent. **3/3** (Bhujerba 805, 806, 806).

`kFieldContextBits = 0x07` includes bit 2, `CondAreaId`. **Session 93 already established that bits
2/3 are not a readiness signal** — `0x0C` is the engine's TERMINAL "this area has no such resource"
state written by `FUN_003ea820`, which nothing ever retries — and removed them from
`IsFieldNavSafe()`. The NAV-ROUTE gate line has been printing *"(bits 2/3 are log-only; routing runs
regardless)"* ever since. **The announce gate was simply never updated.** Bhujerba is a second family
of maps in the same terminal state as Ridorana/Pharos, which is the map S93 measured.

Fix: `kFieldContextBits = 0x03`. This is finishing an S93 change, not a new widening.

*Process note:* the tester's phrasing ("in this city") invites the same-region-suppressed
hypothesis, which is what I formed first. It survived about ninety seconds — until the log was asked
whether any other city announced. **A report describes a symptom, not a mechanism.**

### 2. No way onward in the mines — a presentation bit read as a kind

Report: *"in the mines, there is no exit to continue on. another cutscene? another exit type we
missed somehow?"* Neither. The exits were found, then thrown away.

On map 357 "Lhusu Mines: Shaft Entry", **three instruments contradicted the exit builder and all
three were right**:

1. the container census read all three controllers — `__MJ_CTRL000` group 1 → 806, and
   `__MJ_CTRL001` group 2 / `__MJ_CTRL002` group 3, both → 358 "Lhusu Mines: Oltam Span";
2. the blob's own `+0x84` edge table: `door binding: 3 edge records vs 1 controllers`;
3. `exit_scan`: `surface g2: 30 polys` and `surface g3: 30 polys`, each
   `<== NO CONTROLLER CLAIMS THIS GROUP -- unreachable exit`.

The builder listed one exit. The two it dropped carry `flags=0x2`, and the controller branch tested
`jumpFlags != MAPJUMP_FLAGS_FIELD_DOOR` — i.e. `!= 0`. **`map_script_internal.h` has documented since
S64 that flags is a PRESENTATION bitfield** (bit 0 = no-fade path, bit 1 feeds `FUN_002efa70`) — "how
a jump looks, not what it is" — and the code filtered on it anyway. `0x2` is an ordinary field door
with a different fade.

New rule: the world-map teleport MENU value is refused for both classes; beyond that a controller
that ARMED A GROUP is admitted on any value, which is the standing the non-controller branch has had
since S105. A controller with no group keeps the strict `== 0`.

**Why this cannot widen onto a working map — structural, not a hope.** `exit_scan` lists a dest only
when a swept walkmap surface carries that group tag and drops it as `nogroup` otherwise, and
`map_script.cpp` already asserts that a map which is correct today has ZERO unclaimed surfaces. A
newly admitted controller can therefore only land on a surface already logged as unclaimed.

**The diagnostic that would have caught this is now written.** Both silent-drop paths — every
mapjump in the span refused, or no mapjump found at all — now emit an always-logged
`__MJ_CTRL%03d REJECTED` line beside the existing `SPAN EMPTY` / `SPAN UNREADABLE`. The old
`span=... | setmapjumpgroup ... | mapjump at` line lives INSIDE the accepted branch, **so a refused
controller read exactly like a routine that does not exist.** That is the whole reason this took two
scanners to see.

Maps still logging unclaimed surfaces, for the next play to attribute: **318, 319, 321, 322, 568**.
**Map 569 is RULED OUT** — it has no `__MJ_CTRL` routines at all and all three of its jumps are
`flags=0x0`, so the existing `nogroup=3` backlog item has a different cause and stands.

### 3. Shops said nothing until you moved the cursor

`ShopReader` had two hooks and **neither is an open event**: `FUN_0056e5d0` is a highlight/refresh
handler reached from the cursor mover and the L/R tab handler, and the quantity panel proc returns
immediately while browsing. `debug.md` recorded the gap outright: *"the shop-open hook point is still
unestablished."*

Rather than establish one, the open event is **borrowed from the list family the shop already belongs
to**: `InventoryReader` hooks `FUN_005655f0`, the unified refresh the game runs on screen OPEN as
well as on category change, and now calls `ShopReader::OnListRefreshed` once the original has rebuilt
the rows. That is the same mechanism the party-menu lists got for the identical defect ("entering a
one-item list moves no cursor").

Second half: the guard cleared only `g_lastItemId`, never `g_lastContainer`, which was reset at DLL
unload alone. The header claimed *"it resets when the surface (container) changes"* — a test on an
address **the engine is free to hand back**, exactly as `menu_reader.cpp:102` records for pop-up
windows. Both halves now clear on the refresh event. Comment corrected rather than left standing.

### 4. Re-entering a shop did not re-speak the clerk

`dialogue_reader.cpp`'s `g_lastPage[]` names its per-frame function (`FUN_002a8c50`), so it passes the
letter of the no-dedup rule. **It fails on its own stated justification:** the comment claimed "the
key is dropped the moment the message ends", but the `+0xC0` latch is read PRE-call and is therefore
only visible on the call AFTER the message ended — and when the shop tears the box down, that call
never comes. The key then collides by construction on the way back: recycled slot, same `base` (the
map's message data is still resident), `off == 0` for page 1 both times.

Three drops now, where there was one: the `+0xC0` latch; a different widget appearing in the same
registry slot (`widget` is now part of `PageKey`); and `DialogueReader::ForgetLivePages()` from the
same `FUN_005655f0` hook — an event, not a poll. All three only ever ADD speech.

Also raised the reject diagnostic's hard cap 16 → 64: one session had already burned 13, and once
spent it is dead for the process, so the rejection you want to see hours in is the one it cannot
report.

### 5. The paint replay got exactly one shot — and that lost announcements outright

Found while diagnosing #3, initially deferred, then fixed on the tester's instruction ("if they are
genuinely bugs, then fix the bugs"). It is genuine, and **the evidence was in the same log as
everything else here**:

```
focus owner=…2C2A14C0 index=7 (new surface)
  (text not ready — awaiting paint)
focus owner=…2C2A14C0 index=7          <- no "(new surface)"
  (text not ready — awaiting paint)     <- and never stashed again
```

`OnFocus` assigns `g_focusOwner = owner` **before** the `text.empty()` test, so on the paint replay
`ownerChanged` is already false; the re-stash was gated on it, and `OnMenuPainted` had cleared the
pending slot on its way in. **One paint is not always the right paint** — the paint that fires the
callback need not be the one that fills THAT owner's item map. When it wasn't, the row was gone for
good and the player had to move the cursor. Affects **every** menu, not just the save list.

Now retries up to 8 replays. The budget lives in `g_retryOwner`/`g_retryCount`, which deliberately do
NOT reuse the pending pair — `OnMenuPainted` clears that before re-invoking, so it cannot also serve
as "have I already retried this". Budget released when text arrives; exhausting it logs
`TEXT NEVER PAINTED ... this surface is MUTE` **once**, because a surface that never paints is a real
defect elsewhere and silence is exactly how it stayed invisible.

### 6. The option cursor had no re-arm at all

`choice_reader.cpp`'s guard was two function-local statics that **nothing ever cleared, not even
`Shutdown()`**. Its comment claimed "re-entering the prompt re-announces, because the widget is
rebuilt and the remembered cursor no longer matches" — the same false pointer-identity assumption as
#3 and #4, in a session that now has three instances of it.

Given a real re-arm, `ChoiceReader::ForgetLastCursor()`, from two events: `DialogueReader`'s
end-of-message latch (the choice widget IS the box's embedded list block — both readers reach it at
`window+0xD0`, so a finished message retires the cursor on it) and `ForgetLivePages()`. The tick has
no way to notice either by itself: `FUN_002a9980` simply stops being called.

### The through-line

Defects 1 and 2 were both **fully described in the log for four-plus sessions**. Defect 2 in
particular had three independent instruments disagreeing with a fourth in the same file, printed
side by side. The missing piece was never analysis — it was that nobody ran a grep for the mod's own
anomaly lines before theorising. **Grep the log's own anomaly lines FIRST.**

Second through-line, and it turned up **three** times once looked for: **a guard whose comment
asserts something false about pointer identity.**

| where | the claim | why it is false |
|---|---|---|
| `shop_reader.cpp` | "resets when the container (surface) changes" | compared an address the engine pools and hands back |
| `dialogue_reader.cpp` | "the key is dropped the moment the message ends" | the latch is read pre-call and is never seen when something else tears the box down |
| `choice_reader.cpp` | "the widget is rebuilt and the remembered cursor no longer matches" | recycled address + cursor back at its starting index = equal key |

None had ever been tested, and `menu_reader.cpp:102` has recorded the counter-evidence — **the engine
recycles these addresses** — since S51. All three now re-arm on a real game event instead.
**A comment is not a measurement. If a guard's safety argument rests on an address changing, prove
it changes.**

Third, from #5: **a retry budget of one is not a retry.** The replay looked like it handled the
late-paint case and handled only the first frame of it, and the failure mode was silence, which is
the one outcome this project treats as worse than any repeat.


## Session 127 — 2026-08-03 — [menus] Save slots and the Clan Primer, and the bit-2 replacement

**KEYWORDS: save slot FUN_0057fe80 0x45FE80 win+0x1C0 rowmap +0xEF record 0xA0 playtime +0x18 +0x1A
+0x1B frame counter +0x10 mapId +0x4C party +0x20 leader bit0 gil ambiguous +0x08 +0x0C no date
clan rank +0x44 Hedge Knight Knight of the Round primer FUN_00576300 FUN_00573be0 FUN_0056f2a0
pageCount +0x03 pages +0x08+i*8 mode 4 tips 5 bestiary 6 hunts FUN_002f9920 VirtualBuffer
SetPrimerNavCallback kFieldContextBits 0xA3 CondLeaderPtr CondLeaderObj**

### 1. The bit-2 removal reinstated the pre-title announcement — fixed properly this time

S126 dropped fail-mask bit 2 from the announce gate because it was suppressing all of Bhujerba. It
was ALSO the only thing suppressing the boot-map announcement the original comment warned about, and
the log caught the regression within seconds:

```
[+5468ms] announce: mapId=12 sub="" region="Pharos at Ridorana"
```

— eight seconds before "Load Game", on boot map 12, with an empty sub-area and a garbage region.

**The gate wants "is a party standing on a field map", and the LEADER ACTOR is that question.**
`kFieldContextBits` is now bits 0/1/5/7 — field sim live, field module started, leader ptr present,
leader resolves through the handle table. Unlike the area manifest that is a liveness property, not a
per-area resource lookup, so it has no terminal-false state to fall into. Bhujerba (`failMask=0x0C`)
has bits 5 and 7 clear, so the city that started this still announces.

**Lesson: when you delete a gate, find out what it was actually load-bearing for.** Bit 2 was doing
two jobs — one legitimate (boot suppression), one accidental (silencing Bhujerba) — and removing it
fixed the second by breaking the first.

### 2. Save slots — the probe settled four questions and refused to settle a fifth

The row paints only region + sub-area as codec text; playtime, levels and gil are SPRITE DIGITS via
`FUN_00393160` → `FUN_0029f6e0`, which `TextCapture` structurally cannot see. `SaveReader` claims the
row from the existing 0x8000 focus chain (no new hook) and reads the 0xA0 preview record.

**Confirmed live over six real saves spanning 22 minutes to 92 hours:**

| field | meaning | how it was confirmed |
|---|---|---|
| `+0x18` u16 / `+0x1A` u8 / `+0x1B` u8 | playtime h / m / s | cross-checked against the frame counter at `+0x10`, which is h:m:s × 60 on every record (26h34m07s → 5,738,828) |
| `+0x4C` u32 | map id | 357 → Lhusu Mines, 806 → Bhujerba, 306 → Rabanastre Southgate |
| `+0x20 + n*4` | charId / level / gauges / flags, **bit 0 = leader** | a 92-hour save reads lv 99 across the party; low-level saves read 5–12. Level is real |
| `+0x44` u32 | clan rank | rank 2 → the game resolved "Hedge Knight"; rank 11 → "Knight of the Round" |

**Then the tester sent a screenshot of the load screen and it settled all three open questions at
once** — cursor on `005 Rabanastre Southgate 092:34:00`, the very record the probe had dumped:

* **`+0x08` IS GIL.** Panel reads `GIL 2782150G`; the record holds 2,782,150. `+0x0C` (961,190) is a
  different counter. Now spoken.
* **`+0x40` IS clan points, `+0x44` IS the rank.** Panel reads `CLAN RANK Knight of the Round` /
  `POINTS 13422868` for a record holding 11 and 13,422,868. **I had struck `+0x40` an hour earlier on
  the grounds that 13.4 million was "absurd for clan points" — that was reasoning from taste rather
  than measurement, and it was simply wrong.** Un-struck.
* **The displayed slot number is the ARRAY INDEX**, not `+0x54`: the rows read 004, 005, 006, 007,
  `<icon>`, 008 in exactly the order the row map gives (`...04 05 06 07 00 08`), and the record at
  index 8 is the one whose `+0x54` holds 3. Now spoken; index 0 (the icon row) gets no number,
  because what that icon means is not established.

### 2b. The clan rank name — a second probe run, and the answer was that the question was wrong

The rank name was the one field left unspoken, because turning `11` into "Knight of the Round" looked
like it needed a rank->string-id map and the only evidence was two points (rank 2 -> 1245,
rank 11 -> 1254). Two points fit any line, so the probe was extended — **not to collect more points
and fit a better line, but to test whether `FUN_003153f0` is the rank->id function at all.**

It is not. It is better than that. The probe's own hook ordering gave it away:

```
[str] id=1254 -> "Knight of the Round"          <- FUN_002f9860 returns...
[clan] rank=11 -> 748685674  matches +0x44? YES <- ...INSIDE FUN_003153f0, which then returns
```

`748685674` is `0x2CA2C0EA` — a heap pointer, not a string id. **`FUN_003153f0(rank)` resolves the
name internally and hands back the codec string.** So the mod calls the game's own resolver and
passes the result to `GameText::Decode`; there is no table, no formula, and nothing to keep in step
with the game.

**Why calling an 0.98 function is acceptable here:** if the return were ever not the name, the decode
fails `IsMostlyPrintable` and the reader says nothing. There is no path from a wrong pointer to a
wrong LABEL — only to silence.

### 2c. Rank 0, from a third screenshot

A rank-0 save (`001 Nalbina Fortress, 22 minutes`) draws **no CLAN RANK row and no POINTS row at
all** — just `LEVEL` and `GIL`. So rank 0 is not "a rank whose name is blank", it is "this save has no
clan", and the mod now suppresses **both** fields on it.

That fixed a latent bug rather than a cosmetic one: points had been gated on `clanPts > 0`, which
gave the right answer on this save **by luck** (its points are 0) and would have spoken a stray
points figure on any rank-0 save that had accumulated some. **Gate on the thing the game gates on,
not on a value that happens to correlate.**

The same screenshot re-confirmed gil at the far end of the range: panel `GIL 9G`, record `+0x08` = 9
(`+0x0C` = 492). Two confirmations three orders of magnitude apart — 2,782,150 and 9.

**Two lessons, and the second is the useful one:**

* **"That number looks wrong for this field" is not a refutation.** One screenshot beat an hour of
  inference, and the tester had offered it up front.
* **When a mapping looks like it needs fitting, check whether the game already computes it.** The
  "two points fit any line" objection was correct and would have led to collecting a third point;
  the right move was to stop fitting and ask what the function actually does.

**THERE IS NO SAVE DATE.** Two saves 77 seconds apart in one playthrough differ only in playtime, the
counter at `+0x50`, the map and the party gauges. FFXII stores playtime, not a timestamp. Do not go
looking again.

### 3. The Clan Primer was never "baked assets"

`GameArchitecture.md` recorded primer/handbook content as OCR-only. **That was measured on TUTORIAL
PANEL PICTURES.** The prose is ordinary codec text on a pointer array, and the probe decoded a
bestiary entry end to end on the first run:

```
entry id=0 pageCount=3
  page 0  "Plant
Cactus"
  page 1  "Observations |PAGE| Being a mischievous, mean-spirited beastie, mercifully lack-ing..."
  page 2  "The Adventurer's Handbook |PAGE| Ye adventurers only beginning on your travels..."
```

One record serves every sub-screen: `+0x03` page count, `+0x08 + i*8` page *i* as `HEADER 0x03 BODY`
— the same page-break byte `GameText::DecodePages` already splits on. Sub-screen ids confirmed by the
strings each resolved: **4 = Traveller's Tips, 5 = Bestiary, 6 = Hunts**, 7 = seen but empty.

**Announcements are driven by the GAME's own navigation, not by reading keys.** `FUN_00573be0` is the
set-page call, so whatever turns the page — left/right in the Bestiary, a pad button, anything —
the announcement follows. That is what "vocalise what the nav keys do" needed, without the mod
guessing which screen binds which key.

`VirtualBuffer` holds the open page split into sentences (the game hard-wraps mid-word and
hyphenates, so raw lines would read " mean-" / "spirited beastie"; the hyphen is rejoined).
Up/Down/Home/End walk it through a third `SetPrimerNavCallback` slot — the pattern
`input_tracker.h` already documents for the mod menu, kept separate so neither displaces the other.
**Left/Right are declined outright**: they are the game's page turn and the hook already speaks the
result, so one keypress keeps one speaker.

**`FUN_002f9920` (`0x1D9920`) is a SECOND string resolver** the mod does not hook — every primer
title comes through it. If one ever needs resolving by id, EXTEND `TextCapture::ResolveStringById`.

The primer state is cleared on the sub-screen hook, deliberately: three guards in S126 went silent
because their state outlived the object it described. The nav callback also re-checks that the
latched viewer is still a page viewer before answering.

### Phrasebook

Two strings added — `" hours"`, `" minutes"` — on the tester's explicit request for playtime. The
game draws it as sprite digits with no words anywhere near it, so there is no game-supplied wording
to read instead; a number with no unit is unspeakable. No plural form, exactly as `" steps"` has none.


### S127 addendum — the Clan Primer round two

Four sub-screens, all measured before anything was written (`probe_primer_screens.js`):

* **Bestiary read one page behind.** `FUN_00573be0` offsets the record page by ONE when `viewer+0xC8`
  bit 0 is set; the Bestiary sets it and record page 0 is the CLASSIFICATION box, not a turnable page.
  Traveller's Tips has the bit clear — same function, two behaviours, and only one was implemented.
* **Traveller's Tips was silent, and it was never a primer bug.** `InventoryReader::IsEmptyCategory`
  claims windows by SHAPE (null row array, live scroll, live table) and the primer entry list has
  exactly that shape, so the inventory reader claimed Tips and went deliberately quiet on it. **A
  shape test cannot tell two structs apart; only identity can.**
* **Hunts** — name/petitioner from `FUN_0037e5b0`, and the COMPLETE flag as a PURE bitfield read
  (`FUN_0046b1a0`'s whole body is one bit test), so no game call. A screenshot of an incomplete mark
  settled the state count: the list renders COMPLETE or nothing, **two states, not three.**
* **Sky Pirate's Den** — no `0x8000`, no list painter. Cursor at `den+0xCB`, tooltip
  `FUN_00572600(idx + 0x1B68 / + 0x5DDE)`. **The cursor is a POSITION, not an achievement id**: the
  id lives in the per-achievement record at `den+0xD0 + cursor*0x80`. The tooltip read correctly while
  navigation read a Chocobo as "Fran", and that split is what named the bug.

**Three bugs this round were mine, found by the tester's ear rather than by the log:**

1. The hunt row logged the right line and SPOKE the wrong one — two speakers in the same millisecond,
   the generic path second with `interrupt=true`. **`SPEAK-OUT` records what was SENT to speech, not
   what was HEARD;** a later interrupt cancels an earlier line and the log still shows both.
2. Claiming rows in the dispatch chain instead of inside `OnFocus` caused the save list to announce
   twice AND every primer sub-screen to be silent on entry — opposite symptoms, one cause. The claim
   now sits below the pane gate, so an unfocused pane defers to the replay and a focused one does not.
3. The buffer liveness check accepted only the page-viewer class, so the Quest Progress and Den
   buffers were built correctly and then refused every arrow key. **A liveness check has to know every
   surface it guards.**


### S127 closing note — one thing shipped unfinished

The Clan Primer wrap-around settle **fixed the wrong-text read and replaced it with silence**;
the row that settles on screen is not announced. Tester-confirmed after the commit, accepted as the
better of the two behaviours and left for a later session. Full write-up, the four candidate causes,
and the warning not to fix it by reverting: `Docs\debug.md`, "OPEN -- the Clan Primer wrap-around
settle is SILENT instead of stale".

**Fix the diagnostic before the bug.** The deferral returns before any logging, so the log cannot
tell "deferred and never replayed" from "never got the focus". That is the third silent-drop path
this session -- after the refused `__MJ_CTRL` controllers and the one-shot paint retry -- and all
three cost time for the same reason.

## Session 128 — 2026-08-03 — [combat] "The combat log logs nothing" — the report was unfalsifiable, and that was the defect

KEYWORDS: combat log, combat_events, combat_log, silent drop, drop census, power-of-two logging,
HookedApply, HookedSprintf, empty-line, apply-not-valid, realhit, Enemy=0, absent log entry,
regression hunt, S125 element suffix, no battle in log

Tester report: the combat log is "completely broken, not logging anything", with a suspicion that a
recent session had touched it. It had been touched — but not in any way that could break it, and the
evidence available could not have told us either way. That second fact is what got fixed.

> ## ⚠ TWO CLAIMS BELOW ARE STRUCK (Session 130) — and the cause was found
>
> The instrumentation this session added is good and shipped. The **reasoning** rests on two
> statements that the tester's own logs refute, and both had the same root: **every log read here was
> the DEVELOPER's.**
>
> 1. ~~"The hooks install. All three combat RVAs appear in the newest log's install list … over all
>    66 installed RVAs"~~ — **STRUCK.** True of the dev machine. On the tester's machine all three
>    combat hooks **FAILED to install**, with the reason printed at startup:
>    `MH_CreateHook failed at RVA 0x416410 … MEMORY_ALLOC` ×3, then
>    `CombatEvents: a hook FAILED to install`. 63 installed, 3 failed. `66` is a number only the dev
>    machine produces; reading it as the tester's build was the whole error.
> 2. ~~"No battle ever happened. Every 08-03 log reports `Enemy=0` on every rescan"~~ — **STRUCK.**
>    `Enemy=` is emitted by `entity_scan.cpp`, the **field-object** rescan; it is not a battle
>    predicate and never was. The 07-28 tester log has `Enemy=0` on all **361** rescans *and* a
>    complete Dire Rat fight with kills, EXP/LP and speech. The 08-03 07:31 tester log has 248
>    `[COMBAT]` lines. Battles happened; this session simply never looked at a log in which the hooks
>    had failed.
>
> **THE LESSON, and it is the expensive one: a defect reported on another machine cannot be
> diagnosed from your own logs.** Everything checked here was checked correctly and proved the code
> innocent — which it is. The answer was sitting in a file nobody had opened. Session 130 has the
> root cause and the fix.

### What the evidence actually said

All 20 archived logs were checked, not just the newest. The split is clean:

| build era | COMBAT lines |
|---|---|
| 2026-08-01 15:07 | 880 |
| 2026-08-02 04:33 / 05:39 / 06:01 | 112 / 61 / 12 |
| **every 2026-08-03 session (14 of them)** | **2 — the init and the install, nothing else** |

That looks exactly like a regression on 08-03, and it is not one. Three independent checks:

* **`git diff ab99988 HEAD -- src/battle/`** — the last-known-working commit to HEAD. `combat_log.cpp`
  is BYTE-IDENTICAL. The only functional change to the producers is S125 passing
  `BattleState::AbilityElements(actionId)` into `DamageLine`. `ElementSuffix` returns `""` for the
  447 non-elemental rows and otherwise appends at most `" Fire"` — it is incapable of suppressing a
  line. S126+S127 (`7f90dff`) touched **nothing** under `src/battle/`.
* **The hooks install.** All three combat RVAs (`0x416410`, `0x1F12F0`, `0x1F2280`) appear in the
  newest log's install list at the same addresses as the working build, and `uniq -d` over all 66
  installed RVAs finds no collision with the 14 hooks S126/S127 added. `CombatEvents::Init` has one
  call site (`dllmain.cpp:116`), so the ring cannot be cleared mid-session.
* **No battle ever happened.** Every 08-03 log reports `Enemy=0` on every rescan, `[TARGET]` holds
  only its install line (743 readouts in the working 08-01 log), and no battle UI is drawn. The
  `CombatEvents::HookedApply calls=1340` burst that looked like combat is the per-frame STATUS TICK
  path — it runs on the FIELD, where party BtlChrs exist. The tester was navigating Lhusu Mines
  (map 357) at the time.

So the producers are intact, and not one logged session on 08-03 contains a fight. **An absent log
entry means nobody wrote one.** The report is neither confirmed nor refuted by anything on disk.

### The actual defect: four silent drops made the two cases identical

A battle that logged nothing and a battle that never happened produced the SAME file. Every early
exit in the combat path returned without a word:

* `HookedApply` — real action id whose `+0x1c` emission gate never armed: **silent**
* `OnRealHit` — genuine hit whose formatted line came back empty: **silent**
* `HookedSprintf` — no readable id / no destination buffer, and decoded-to-nothing: **silent, twice**
* `CombatLog::Append` — empty text, one step short of the file: **silent**

All five now report, plus a **positive control** (`realhit`) so "no drops and no entries" stops
having two readings — without it, "the gate rejected everything" and "nothing reached the gate" look
the same. The empty-line drop carries `atkNamed`/`tgtNamed` separately, because `DamageLine` only
returns nothing when BOTH names fail, and that distinguishes a broken name chain from a broken
formatter.

**Counted on the hot path, reported at powers of two.** `HookedApply` runs ~20×/sec per actor, so a
per-call line would be O(N) and would flood the file the console-output budget protects. `1` is a
power of two, so the first of each kind always prints — and that first line is the one that names
the cause.

### The lesson

This is the FOURTH silent-drop path recorded in two sessions (S127 had three: the refused
`__MJ_CTRL` controllers, the one-shot paint retry, the primer settle). The pattern is now
unmistakable: **an early `return` with no log is a defect in its own right**, because it destroys the
evidence that would name the next bug. It also cost this session the wrong first move — the log's
own shape invited "S125 broke it", and only a diff against the last-working commit killed that.

**Do not revert working code to chase a report the log cannot see.** A revert justified by ABSENCE
needs a log in which the change COULD have fired; there was no such log here. What was missing was
not a fix, it was a measurement.

**Still open:** whether the tester's report reflects a real in-battle failure. The next log from an
actual fight now answers it outright — `realhit xN` with no `DMG |` lines means the formatter or the
name chain; no `realhit` at all means the applier gate; `drop[...]` names the rest.

---

## Session 129 — 2026-08-03 — [menus] The shop crash: a four-argument detour on a six-argument function

KEYWORDS: crash, shop, sell menu, equip_compare, HookedDelta, FUN_002cc780, FUN_002ca7c0,
FUN_002cc4f0, FUN_0056e5d0, FUN_0057b890, arity, stack arguments, shadow space, x64 calling
convention, access violation, minidump, InstallTyped, hook audit, silent memory corruption

Tester report: using the sell menu in a shop crashes the game. Root-caused from the crash dump to a
hook the mod installed in S125, fixed, and the same class of defect audited out of the other 65
hooks.

### The log named the surface; the dump named the instruction

The mod log ends mid-menu with no error, so the last two lines are the whole of what it says:

```
[READER] pane owner=...2BE00E00 focus=...2BE00E00 focused=1 rowOff=0xD0
[PERF] thread: IngameMenu::OnRowChainFocus runs on tid=1
```

Two earlier logs (`05-19-46`, `06-10-14`) contain the same pane class `0x45B890` at `rowOff=0xD0`
followed by `[INGAME] menu: "Sell"` / `"Buy"` / `"Bazaar"`, which identifies it: **that class is the
shop's Buy/Sell/Bazaar list**, and it is ONE class for all three entries. The wall-clock in the log
is skewed, but uptime is not — session start `17:46:25` plus the last stamp `+99406ms` lands on
`17:48:05`, exactly the dump `FFXII_TZA.exe.20260803_174805.dmp`. Same event.

The dump was parsed directly (no debugger on this machine — a ~90-line Python minidump reader over
the `Exception`, `ModuleList` and `Memory64List` streams):

```
exception  c0000005  ACCESS VIOLATION WRITE at 0x118110a0
rip        FFXII_TZA.exe+0x245668       (ABS 0x365668)
rcx = rbx  0x118110a0   <- unmapped
rdx        0x10d810a0
r13        0x0000ffff
```

`0x365668` is in a gap Ghidra never made a function for, so it was read as bytes out of the dump:

```
0x365660  cmp  edx, 8
0x365663  jge  0x365668
0x365665  xor  eax, eax ; ret
0x365668  mov  dword [rcx], 'ex00'   <-- FAULT
0x36566e  test r8d, r8d
0x365673  mov  word [rcx+4], '+' ; mov eax, 8 ; mov word [rcx+6], 0 ; ret
```

A bounds-checked 8-byte writer: `if (size >= 8) { write 8; return 8; }`. So `rcx` is an output
buffer and `edx` is its size — and both are garbage.

### Who supplied the garbage

A raw scan of the stack from `rsp` gives a chain that is self-consistent (every return address was
checked back against the byte at its call site):

```
rsp+0x000  FUN_002cc780+0xE9
rsp+0x050  dinput8.dll+0x21714       <- OUR HookedDelta
rsp+0x0d0  FUN_002ca7c0+0x3F6
rsp+0x1f0  FUN_002cc4f0+0x7A
rsp+0x400  FUN_0056e5d0+0x10F
rsp+0x430  dinput8.dll+0x1EDE7       <- OUR HookedHilite
rsp+0x460  FUN_0056dd50+0x13F
rsp+0x4c0  FUN_0057b890+0x33E        <- the Buy/Sell/Bazaar proc
rsp+0x4f0  FUN_00247510+0x39
rsp+0x540  dinput8.dll+0x137BA       <- OUR HookedDispatch
```

`FUN_0056dd50` is the shop container's constructor: it builds the rows (`FUN_0056e410`) and then
calls `FUN_0056e5d0(container, 1)` for the initial highlight — which is why this fires on the
container BUILD, and why "the sell menu" is the visible trigger rather than the cause. `FUN_0056e5d0`
ends in `FUN_002cc4f0(uVar6)`, `uVar6` being the row's item id or `0xFFFF` when the row is empty
(`r13 = 0xffff` — the empty-row path, taken because the panel is mid-construction). `FUN_002cc4f0`
loops the nine compare columns through `FUN_002ca7c0`, which calls `FUN_002cc780`.

**`FUN_002cc780` takes six arguments; `equip_compare.cpp` declared four.** Ghidra's decompile of the
callee says six outright, but the CALL SITE hides it — outgoing stack arguments are rendered as
caller locals:

```c
local_f8 = param_2 + 0xf8;
local_f0 = 8;
FUN_002cc780(&local_e0, delta, param_2 + 0xd8, 0x10);   // four visible arguments
```

Disassembled, those "locals" are the argument slots:

```
mov  dword [rsp+0x28], 8      ; arg6 = buffer size  -> matches `cmp edx,8`
mov  [rsp+0x20], rcx          ; arg5 = buffer ptr
mov  r9d, 0x10                ; arg4
call FUN_002cc780             ; ret 0x2CABB6 = the stack value exactly
```

and the callee reads arg5 back as `[rsp+0x70]` after its prologue (`R-0x48+0x70 = R+0x28`, the arg-5
home). On x64 only args 1-4 are in registers. A four-argument detour reserves only the 32-byte shadow
space when it calls the trampoline, so `[rsp+0x20]` and `[rsp+0x28]` are **never written** and the
original reads whatever the previous call left there. Every register at the fault agrees: `rbx` =
stale pointer, `rdx` = stale size that trivially clears `>= 8`, and the write goes wherever the
stale pointer pointed.

### Why "it worked in earlier shop sessions" proves nothing

The outcome is decided by leftover stack: **unmapped -> crash; mapped -> eight bytes of unrelated
memory destroyed with no symptom at all.** The 08-03 logs that show shops working are not logs where
the bug did not fire — they are logs where it landed somewhere writable. That is the worse case, and
it is why this was fixed by passing the arguments rather than by guarding the crash.

### Fix

`Pfn_Delta` and `HookedDelta` now declare and forward all six; the two new arguments are passed
through untouched and never read. One file, three lines of code.

### The rule, and the audit

`Hooks::InstallTyped` cannot catch this — it is a template whose only constraint is that the detour
and the trampoline pointer have the same type as **each other**. Both were wrong together, so it
compiled clean.

All 66 hooks installed in the crash session were audited: each hooked RVA mapped to ABS, the detour's
parameter count parsed out of the mod source and compared against the callee's decompiled signature.
`HookedDelta` was the only under-declaration. One over-declaration exists — `HookedSprintf` declares
4 against `FUN_00536410`'s 3 — and is left alone: the extra argument is a REGISTER argument, so it is
read as garbage and forwarded to a callee that ignores it. Nothing is written and nothing can be
corrupted. **Under-declaring is a memory-corruption bug; over-declaring within the four register
slots is not.**

**Not verified in play.** The build is deployed; the crash is a single-instruction write through an
uninitialised stack slot and the fix supplies that slot, but no shop has been entered on this build.

---

## Session 130 — 2026-08-04 — [combat+text] The combat log was never broken: MinHook ran out of trampoline slots at 63

KEYWORDS: MinHook, MEMORY_BLOCK_SIZE, MEMORY_SLOT_SIZE, trampoline pool, MH_CreateHook MEMORY_ALLOC,
63 slots, hook census, build stamp, tester machine, font00.dat, glyph table, accented characters,
diacritics, n-tilde, codec table, game_glyphs.h, IsMostlyPrintable, t key, re-read, IsBoxLive,
message window registry, lifetime, ForgetLastLine

Three defects, all root-caused, all fixed. The first had been hunted as a code regression for two
sessions and was neither a regression nor in our code.

### 1. The combat log — 63 is not a coincidence

The tester's own log said it, at startup, in plain text
(`Tester Logs\Dylan\FFXII-Screen-Reader-2026-08-03_17-39-23.log`, lines 115-118):

```
[HOOKS] MH_CreateHook failed at RVA 0x416410 (abs 0x536410): MEMORY_ALLOC
[HOOKS] MH_CreateHook failed at RVA 0x1F12F0 (abs 0x3112f0): MEMORY_ALLOC
[HOOKS] MH_CreateHook failed at RVA 0x1F2280 (abs 0x312280): MEMORY_ALLOC
[COMBAT] CombatEvents: a hook FAILED to install
```

| log | attempted | installed | failed | combat log |
|---|---|---|---|---|
| dev, current | 66 | 66 | 0 | works |
| tester 07-28 | 43 | 43 | 0 | worked — full Dire Rat fight, kills, EXP/LP spoken |
| tester 08-03 07:31 | 55 | 55 | 0 | worked — 248 `[COMBAT]` lines |
| **tester 08-03 17:39** | **66** | **63** | **3** | **dead** |

`include/MinHook/buffer.c` had `MEMORY_BLOCK_SIZE 0x1000` and `buffer.h` has `MEMORY_SLOT_SIZE 64`;
the block header eats the first slot, so the free-list is **exactly 63 trampolines per block**. The
mod installs 66. The tester's process could not place a second block within ±1 GB of `0x536410` —
the exe's image base is `0x120000`, which is very low, so there is little room below and the search
window is largely the exe's own. Whether that second block lands is a function of the process's
virtual-address map: other injected DLLs, overlays, ASLR. It is machine-dependent, and nothing else
in the combat path is.

**`CombatEvents::Init()` is the LAST subsystem initialised** (`dllmain.cpp`), so when the pool ran
dry the three combat hooks were the only casualties. Everything else worked; the mod looked healthy.

The attempted RVA set is byte-identical on both machines, same image base `0x120000`. Not a
localization (the tester's logs are full of English game text; the Polish patch is a VBF **data**
repack that does not touch the exe), not a race (installs are sequential on one thread), not a
missing dependency (NVDA detected, SDL3 audio ready on both).

**The fix, and why it is free.** `MEMORY_BLOCK_SIZE` becomes `0x10000`. On x64 MinHook places blocks
with `FindPrevFreeRegion` / `FindNextFreeRegion`, which step by `si.dwAllocationGranularity` — 64 KB
— so **Windows already reserves 64 KB for every block** and MinHook was asking for one page of it.
Raising the request to the granularity it was already paying for costs zero additional address space
and yields **1023 slots per block** instead of 63. `FreeBuffer`'s block-base arithmetic still holds:
a 64 KB-aligned block is still aligned to the larger size. Marked `[LOCAL]` in the vendored file with
the tester's log lines quoted as the reason.

The dev machine was about three hooks from the same cliff.

### 2. Two things that made this invisible, both now fixed

* **No log this mod has ever written carried a version.** Identifying the tester's build meant
  counting hook-install lines (43 / 55 / 66). Every log now names itself on line 3:
  `Build: V0.6 (90975a4) compiled ...`, from a CMake-generated version plus git short hash.
* **No caller checked `Hooks::Install`'s return** except `CombatEvents::Init`, and `dllmain` threw
  that away. There was no total anywhere, so a partial-hook session looked completely normal unless
  somebody happened to grep for `MH_CreateHook`. `Hooks::LogInstallCensus()` now emits one line at
  the end of deferred init — attempted / installed / failed, the first failing RVA, and a plain-words
  note that `MEMORY_ALLOC` means the trampoline pool, not the machine's memory.

**A failure that only ever appears as a line nobody greps for is a failure with no report.**

### 3. Accented characters were not being flattened — they were being deleted

Reported as "a bare n instead of an n-tilde". The code cannot do that. `game_text.cpp` ended its
glyph handling with `default: break;  // unmapped extended glyph: drop`, so an unmapped byte emitted
**nothing**: "Senor" (with the tilde) decoded as "Seor", exactly the way "Cuchulainn" read as
"Cchulainn" before `0x81` = u-acute was added by hand. The speech path was cleared end to end and is
innocent — `Decode` returns `std::wstring`, `Speech::Output` passes `c_str()` straight to
`Tolk_Output(const wchar_t*)`; there is no `CP_ACP` anywhere in `src/`, no default-char argument, no
transliterate helper, and `/utf-8` is set. All refuted at 1.00.

**`font00.dat` is the byte-to-character map, shipped as data.** Beside the font texture: a record
count at `+0x04` and 36-byte records carrying a slot index at `+0x18` and the character at `+0x1C`
**as its UTF-8 bytes packed little-endian** — which reads as a codepoint for ASCII and then stops
making sense, and is the trap. `FUN_002ac2f0:75` computes `glyph slot = byte - 0x20`, so
**codec byte = slot + 0x20**.

Record 97 is codec byte `0x81` and decodes to u-acute — the one accented character the mod had
already derived by hand, from shipped text, for "Cuchulainn". Two independent methods, same answer.

**The validation gate is what carries this over the 0.98 bar.**
`FFXII-Decompile/tools/parse_font_dat.py --check` re-derives all **80** mappings `game_text.cpp` had
established independently (62 structural A-Z/a-z/0-9 plus 18 hand-won punctuation marks) and refuses
to emit if one disagrees. **0 disagree.** A separate old-versus-new diff over all 256 byte values:
**0 regressions, 0 losses, 144 bytes newly mapped** — the entire accented Latin block `0x54-0x84`
(A-grave through eszett; **`0x72` is n-tilde**) and a good deal of plain ASCII that had been silently
deleted from spoken text: `@ # $ ^ * _ ; \ " [ ] > { } |`.

The table is generated into `src/core/game_glyphs.h`. Two deliberate speech overrides stay in
`game_text.cpp` rather than the generated file, where they would look like parser bugs: `0x8F` (the
atlas draws U+2014 em-dash) speaks as `-`, and `0xC4` (atlas draws U+2267) speaks as U+2265, which is
the one screen readers voice. Both predate the table and are play-confirmed.

**`IsMostlyPrintable` had to widen in the same change, or the fix would have made things worse.** It
required 60% ASCII and at least one character in `A-Z`/`a-z` — which held only *because* accents were
being dropped, so whatever survived was ASCII by construction and the test could not fail on real
text. With accents decoding, a short heavily-accented string could fall under the threshold and be
judged garbage by the ~30 call sites that gate on it: not a wrong reading, **silence**. Letters are
now counted with `iswalpha` and the printable share admits Latin-1 / Latin Extended-A.

**The map is PER-LOCALE, and this matters.** There are five font directories, not twelve: `us`
(shared by the seven Western locales), `jp`, `cn`, `ch`, `kr`. All 224 single-byte slots differ
between them — in the `jp` atlas slot 0 is a mathematical symbol, not `A`. The mod has always been
implicitly built for the `us` atlas; this table does not change that, it makes the `us` case
complete. Locale detection is the clean future path and does not exist yet.

Also recorded, not built: the 2-byte extended banks (`0x10`-`0x1F` lead) are still consumed and
dropped. The arithmetic is now known — `bank[i] = 224 * (i + 1)` for `i = lead & 0xF`, from
`DAT_00916570` / `DAT_009165b0` — but in the `us` atlas slots 224+ are kana and kanji, which cannot
appear in Western text, and the two bank tables disagree from index 8 on a runtime flag we do not
read. See `GameArchitecture.md`.

### 4. `t` was already dialogue-scoped. What it had no notion of was a LIFETIME

Contrary to the report, `t` never repeated "whatever the screen reader last said" — only three call
sites ever wrote `g_lastLine` (the obtained-item toast, the menu system-message panel, and each
dialogue page, all through `MessageReader::NoteSpoken`). Menu rows, entity descriptions, the combat
log, gil, LP and party status never touched it. **The scope was right from the start.**

Nothing ever *cleared* it. Not end-of-message, not a map change, not `Shutdown()` (which clears
`g_confirmPrompt` and not this). So a conversation that ended an hour ago was still what `t` spoke —
in the field, in menus, mid-battle.

Two guards now, deliberately independent:

* **The gate.** `DialogueReader::IsBoxLive()` — the same registry `LiveMessageSlot` uses to decide
  whether a text widget is a dialogue page (`DAT_0215f200`, 8 slots, stride `0x68`), asked the other
  way round. OR-ed with `MenuState::IsChoicePopup` / `IsConfirmWindow` for prompts, and with a latch
  for the toast, which is the one surface with nothing left to interrogate afterwards — its widget
  self-destructs on the case-2 animation end, so it arms at case 1 and the destruct clears it.
* **The clear.** `MessageReader::ForgetLastLine()`, called from the SAME events `DialogueReader`
  already drops its page key on — the `+0xC0` end-of-message latch and `ForgetLivePages()` — plus
  `Shutdown()`. Tied to events already proven to bound a box's life rather than given a fourth guard
  of its own; three guards in this codebase have gone silent by outliving the object they described.

**`MenuState::IsAnyMenuOpen()` was NOT used and must not be.** It reads `*DAT_0208ebc0`, which the
whole decompile writes once and never clears, so after the first menu it answers "open" forever. A
gate built on it once killed the field object scan for an entire fight.

**The log says which guard fired.** `t` with nothing open reports whether the gate held, the clear
held, or both agree. That is deliberate: the gate's weak point is unmeasured — if the game leaves
stale window pointers in `DAT_0215f200` the way it leaves one in `DAT_0208ebc0`, the gate would
answer "live" forever and only the cleared store would be doing the work. One press in the field
after a conversation distinguishes them.

### Files

`include/MinHook/buffer.c` (`[LOCAL]`), `src/core/hooks.{h,cpp}`, `src/proxy/dllmain.cpp`,
`src/core/logger.cpp`, `CMakeLists.txt`, `src/battle/combat_events.h`,
**`src/core/game_glyphs.h` (generated)**, `src/core/game_text.cpp`, `src/ui/dialogue_reader.{h,cpp}`,
`src/ui/message_reader.{h,cpp}`, `FFXII-Decompile/tools/parse_font_dat.py` (new).

Also committed: Session 128's uncommitted drop-census instrumentation, which is good and was never
the cause — it simply never reached a machine where the hooks were installed.

### Not verified in play

**Item 4 (`t`) is PLAY-CONFIRMED, 2026-08-04.** The key reads the current dialogue, prompt or toast
and is silent otherwise.

Note what that does and does not settle: it confirms the BEHAVIOUR, not which of the two guards is
producing it. The open question — whether the game nulls its `DAT_0215f200` slots when a
conversation ends, or leaves them standing the way it leaves `DAT_0208ebc0` — is still open, and the
`t` log line is what answers it. If a later change ever needs `IsBoxLive()` on its own, read that
line first rather than assuming the gate is load-bearing.

Items 1 and 3 are built, deployed and compile clean, and both carry offline proofs (the 63-slot
arithmetic; 80/80 cross-check plus a 0-regression diff, and for the Polish table 1,351 names with a
zero residual). **Item 1's real proof is the tester's next log** — the dev machine never reproduced
it, and as of this entry we are waiting on it. Ship him a build: he has never run one containing the
S128 counters, and he does not have S129's shop-crash fix either.

### 5. The Polish fan translation — its font metadata lies, so the mapping came from its text

`PL_ff12_v1.3` repaints ~16 accented glyph slots to Polish letters. Its `font00.dat` differs from
stock in **20 bytes and not one is a character field** — all ten changed records changed only their
advance width. So every repurposed slot still claims the stock letter it used to draw, and the file
that is authoritative for a stock install is actively wrong for this one.

Autodetection is not available: `instaluj.bat` repacks the archive in place and patches the file-size
table, leaving no loose file and no marker. Hence a mod-menu row — **Text glyphs: Standard / Polish
translation**, default Standard, applied at Init and on every change so the player hears it
immediately.

The mapping was recovered by decoding the patch's own shipped text with the STOCK table and reading
the Polish: a repurposed slot shows up as a letter Polish orthography forbids in that position.
`"nie moêe dosiègnàç celu"` is *nie może dosięgnąć celu*; `"zamienia siè w kamieñ"` is *kamień*;
`"PÊ czèéciowo odnowione"` is *PŻ* (Punkty Życia); `BROŃ JEDNORĘCZNA`, `Bezimienne Źródło` and
`Pani Życia i Śmierci` fix five of the capitals outright.

**L-stroke is the trap.** Lowercase `ł` sits at `0x94`, a PUNCTUATION slot outside the accented
block, so the block rule cannot place its capital — and the obvious guess (`0x93`, the neighbouring
inverted-exclamation slot) is wrong. `Ł` is at **`0x81`**, the very byte the stock table maps to `ú`,
which this project hand-derived years ago for "Cúchulainn". Found by asking which unmapped byte
behaves like a word-initial capital: 173 hits, then 42 more in the name pool (`Arkadyjski Łucznik`,
`Cesarska Łuska`, `Deszcz Łez`).

Within the block the rule is capital = lowercase − `0x18`, holding across all five confirmed pairs
with no counterexample. `Ą` and `Ć` follow from it and are marked **rule-derived, not witnessed** —
capital A-ogonek is essentially unattested in Polish and capital C-acute is word-initial only in rare
proper nouns, so neither occurs anywhere in the corpus. Recorded as derived so a later session does
not mistake it for a measurement.

**Validation:** decoding the patch's name pool with the shipped table yields **1,351 clean Polish
item and enemy names** — `Adamantowy Żółw`, `Agatowy Pierścień`, `Anielska Pieśń` — with **zero**
still containing a stock accented letter. That residual count is the check that proves no repurposed
slot was missed.

### Designed, not built — four items carried forward

Written up in full in the session plan and summarised here so they are greppable:

* **Live positional soundscape.** `AudioEngine` is one voice (retrigger, no overlap, no looping, no
  distance gain) and needs a software mixer summing N sources into the existing stream. All ten
  categories now have a sound; durations run 0.251-1.834 s and two are stereo, so
  `debug.md`'s "all nine are mono" is stale. 15 steps = 11.25 world units
  (`g_unitsPerStep = 0.75`). Identity is the `sceneObj` pointer. Voices must be phase-offset from a
  hash of that pointer so they never start together, and same-category instances pitch-spread. Needs
  its own polled-monitor approval, and a **mod-menu submenu** — the menu is flat today and
  `g_cursor` IS the `SettingId`.
* **Autodetail (`F7`).** `Docs/Controls.md` already holds the design; the shop equipment comparison
  is the named first consumer.
* **Dialogue scrollback — the GAME already has one.** Triangle (keyboard `R`, the Party Menu key)
  opens its conversation log during a conversation. Step one is a MEASUREMENT, not code: the log
  probably reuses the message widget `FUN_002a8c50` already hooks.
* **Controller support.** The game reads the pad through **XInput**, so the dinput8 proxy is
  structurally blind to it; consuming a button needs an IAT hook on `XInputGetState`. Claimable
  buttons are Select, L1, R1, L3, R3 — L2 stays the game's for Lock On. `L3` toggles intercept and is
  the one button the mod never hands back.

## Session 131 — 2026-08-05 — Bhujerba shout minigame: spoken infamy meter, two puzzle keys, instant fill

The tester sent an autosave sitting on the Bhujerba "shout at the crowd" sequence and asked for
three things: the meter spoken as a percentage, a key to check for guards, and a one-shout instant
fill so they need not play a minigame built entirely out of things a blind player cannot see.

**The whole minigame was cracked from the bytecode; nothing here is inferred from play.** Findings
are in `GameArchitecture.md` (gauge system, script-VM variable storage, the EBP2 self-naming rule,
two archive errata). The census tool is `..\FFXII-Decompile\tools\ebp_shout_census.py`.

**What shipped**

* `shout_table.*` — 14 rows, one per shout map script, **keyed on the script's own `.src` name**
  rather than a map id. Every EBP2 image names itself at file offset `0x110`; that parses on all
  809 EBP2 files in the game, so the unproven mapId↔script join is simply not needed.
* `shout_script.*` — module-record walk (the same 5-slot `0x288`-stride array the entity scan uses)
  and the descriptor decode that turns a script variable index into an address.
* `shout_meter.*` — the `FUN_004085B0` (RVA `0x2E85B0`) hook, the burst coalescer, the one speech
  choke point, and both keys.
* `shout_fill.*` — the write, under a four-boundary charter modelled on `sneak_assist.h`.
* `speech/phrase_format.*` — `PhraseFormat::Percent`, now the single owner of the "<n> percent"
  convention; `battle_target_reader.cpp`'s two inline copies (`:184`, `:449`) now call it.
* `EntityList::CollectNearestNPCs` — new, beside the existing `Collect*ByNameIdx` pair.
* One new phrase, `Infamy` — the tester's own word. The direction of a change reuses `StatDown`.

**THE COALESCER IS NOT SPEECH DEDUP.** The Imperial penalty is `setgaugecounter(v-1)` **thirty
times, one per frame** (measured at `byu_a01:0x34C35`). Announcing per call would say thirty
numbers for one event. What ships detects the END of a change — a field frame with no new set — and
speaks the value it settled on. Every burst announces; nothing is suppressed for being a repeat.
Direction is measured WITHIN the burst, never against the last value spoken, because the idle decay
runs through a different native that never reaches this hook.

**THE FALSIFIER FOR THE WRITE IS A FACT, NOT A GUESS.** The increment loop updates its variable
AFTER the gauge call, so at hook time `storage == newValue - 1` is what the bytecode guarantees.
The fill declines unless that holds, logs all three numbers either way, and writes the script's OWN
success constant into the script's OWN variable — after which the GAME clamps, prints its own
messages, and runs its own success branch. **This is the mod's THIRD write-category exception**
(after auto-walk S100 and sneak assist S107), user-authorized this conversation.

**WHAT WAS NOT SHIPPED, AND WHY.** The guard identity is not in these scripts: a 911-instruction
native census of the shout region contains no `distance` native, and the "how many heeded" weights
come from variables set elsewhere rather than npcdic ids. So no earshot radius and no guard npcdic
id reached the 0.98 bar. Rather than invent either, `guardNameIdx` stays `-1`, `N` reports the
game's own NPC names with bearing and distance, and a per-map npcdic census goes to the log — the
first play pass through the sequence is what measures it.

**KEY CHOICE.** The tester proposed `,` and `.`; those are the combat log's timeline navigation,
which is required to work everywhere. **B** (meter) and **N** (nearest NPCs) instead — neither is
in the game's bindings nor claimed by the mod, and both joined the 8/9-style pad collision watch,
because S112's lesson is that the Controls screen omits bindings the game really has.

Built, deployed, **NOT play-confirmed.**

**KEYWORDS: shout minigame Bhujerba byu infamy meter gauge setgaugecounter instant fill script
variable descriptor EBP2 src name burst coalescer B key N key phrase percent helper third write
exception ebp_shout_census athena opcode erratum native table stride 32**

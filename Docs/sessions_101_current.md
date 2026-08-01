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

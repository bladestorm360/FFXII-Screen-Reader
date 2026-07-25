# FFXII-Screen-Reader — Debugging Log

This file is structured for keyword searching. **Always grep before proposing solutions.**

## Tried & Failed

Approaches that were attempted and did NOT work. Each entry tagged with `KEYWORDS:` for
grep. Check this FIRST to avoid repeating failed approaches.

### Session 68 — PATHFINDER ELEVATION root-caused: STEP-DISCONTINUITY, not slope; slope-gate idea STRUCK

**KEYWORDS: pathfinder elevation step discontinuity ledge no slope limit walk-type flags FUN_0022cc50
FUN_00231900 FUN_0033bc80 0.3 step kStepDiscont kEdgeSubStep kMaxStep too loose poly normal slope gate
dropped GroundInfoAt route-profile diagnostic shipped pending confirmation**

This CLOSES the reasoning of the S67 pathfinder entry below (which said "the mechanism is plausibly right;
the THRESHOLD is not pinned"). Root cause and the winning fix:

**Decompile trace (first-hand, ~0.9): the FIELD walkmap has NO walkable-slope limit.** `FUN_0022cc50`
(move-across-walkmap per-poly handler) gates walkability on baked **walk-type flags** (poly+0xC & 7: 0
walkable, 1/4 conditional; walls block) — no cosine / normal.y / angle anywhere. `FUN_00231900` /
`FUN_00231890` only guard `B > 0.001` (divide-by-~0, not a slope cap). The player can therefore walk
**any continuous slope** (why stairs/hills work). The only geometric movement blockers are (1) **walls**
— `SegmentClear` (mask=4) already tests them and they are CLEAR along the descent — and (2) a
**step-height DISCONTINUITY**: `FUN_0033bc80:23-28` samples `GroundAt` ahead and reacts at
`ABS(groundY − currentY) >= 0.3` world-units. So the real limit is a ~0.3 m step; our A* `kMaxStep = 1.5`
was ~5× too loose and stitched routes across a 0.3–0.6 m ledge/lip (invisible to the floor+0.9 m wall
feeler and to the prior 0.6 m dense check — which is exactly why fix #4 changed nothing).

**STRUCK — do NOT build the poly-normal SLOPE GATE.** Rejecting cells/edges whose floor poly is too steep
(from the plane normal `B/|(A,B,C)|`) would be WRONG: the engine imposes no slope limit, so a slope cap
rejects continuous slopes the player can legitimately walk. The fix is a step-**discontinuity** test.

**Shipped (built + deployed, pending one tester round):** `path_search.cpp` `passable()` now sub-samples
`GroundAt` every `kEdgeSubStep = 0.25 m` along any edge with `|Δ| > kStepTrigger = 0.15 m` and rejects a
sub-step jumping more than `kStepDiscont = 0.35 m`; `kMaxStep = 1.5` kept as the coarse cliff gate so
continuous slopes survive; flat edges skip (city fast-path). Same cap in the string-pull validator.
`map_query::GroundInfoAt` (slope-returning floor read) + `path_planner::LogRouteProfile` (dumps the
ROUTE's own per-leg max sub-step ΔY + slope) added as the decisive diagnostic. `kStepDiscont = 0.35` is
PROVISIONAL — the route-profile `WORST step` on the tomato route vs a walkable city/stairs route brackets
the shippable value. See Session 68 session log + Known Issues header below.

### Session 67 — PATHFINDER ROUTES THROUGH IMPASSABLE ELEVATION (OPEN, progress-blocking) — 4 fixes tried, all failed

**KEYWORDS: pathfinder routes into cliff pit ledge impassible terrain character not moving elevation
plateau Rogue Tomato Dalmasca Estersand The Stepping kMaxStep step check GroundAt walkmap hasWorld=0
Bullet absent physics dead dense edge check kEdgeSubMax slope limit char-controller walkable slope
directline route field diagnostic stuck wiggle progress blocking**

**SYMPTOM (tester, 100% progress-blocking):** on elevation-varied field terrain (confirmed on Dalmasca
Estersand "The Stepping", mapId 227, hunting the Rogue Tomato) the pathfinder routes the player onto a
descent/climb they **physically cannot traverse** — the character walks against impassable terrain and
**does not move** ("holds up-stick, footsteps but no movement, until I move sideways first"). A* reports
`pass=strict nearDist=0.0m` (claims it reached the target), but the produced route is unfollowable. The
tomato happens to sit in a hard spot; **the bug is general elevation handling, NOT tomato-specific and
NOT map-specific** (the walkmap loads on every field map). Both the `\` entity route and the `p` combat
route are affected (same `PathSearch::Run`/`passable`).

**CONFIRMED (>=0.98), rule out these dead ends first:**
- **No Bullet/physics is involved.** `hasWorld=0` on every route line and the Bullet world-builder never
  fired this session (`grep -c "world-builder FIRED"` = 0). Routing has ALWAYS used the SQEX walkmap;
  the physics-collision idea (Steps 1/2, filter 0xF) is **DEAD for the field** — do not retry it. (The
  player's own ground/slope resolve `FUN_006a5c00` uses the Bullet raycast at filter 0xf, but it is not
  exercised here because there is no Bullet world.)
- **`GroundAt` is NOT over-reporting walkability.** `GroundAt` (`FUN_003208c0` -> `FUN_0026e3c0` ->
  `FUN_00231900(ctx,pt,param_3=1)`) filters to **type-0 (walkable) floor only, topmost** — line-45 test
  `1 >> (polyType&7) & 1` accepts only type 0, identical to the direct reader `ReadCellFloor`. So the
  grid's per-cell walkability matches the game's navmesh; the false-positive is in EDGE connectivity, not
  cell walkability.
- **The wall test is correct.** `SegmentClear` (`FUN_00230b60` mask=4 -> `FUN_0022cc50`) filters the
  player's real collision classes (0/1/4); it works in the city. The blocking terrain shows **no `W`
  (wall)** on the direct line — it is an elevation/floor issue, not a missing wall.
- **No "different pathfinder for the overworld" is needed** — same walkmap, same core, everywhere.

**ROOT CAUSE (as understood):** `PathSearch::passable()` decides an edge from the floor-height delta
between cell CENTRES 1.5 m apart (`|ay-by| <= kMaxStep`, 1.5 m). That single number cannot tell a
walkable ramp from a 1.5 m ledge or a ~45 deg+ cliff face — all read as "<=1.5 m." So A* stitches a
"path" down a stepped pit wall / up a ledge the player cannot traverse. Directline proof (seq 116):
player on a plateau `.29.0 ... .31.2 ^25.1 ^21.4 .20.3 ... .14.6` — a ~6 m cliff (the `^`) down to a pit
at Y~15; A* refuses the direct cliff but routes AROUND into a descent that is still un-walkable.

**TRIED & FAILED / INSUFFICIENT:**
1. **Bullet/physics edge gate** (probe_menu... no — the `directline` physics ray at filter 0xF): refuted,
   `hasWorld=0` (no physics world). DEAD.
2. **"GroundAt over-reports non-walkable terrain":** refuted by the `FUN_00231900 param_3=1` type-0 filter.
3. **"Small up-step the player can't climb"** (footstep-count hypothesis): the up-step case was a FALSE
   ALARM — the tester simply could not hear footsteps; the north walk was moving correctly.
4. **Elevation-honest DENSE EDGE CHECK — DEPLOYED, DID NOT FIX (the current in-tree state).** Added to
   `passable()` (`path_search.cpp`): when `|ay-by| > kEdgeSubMax` (0.6 m), sub-sample the floor every
   `kEdgeSubStep` (0.5 m) along the edge and reject any sub-step > `kEdgeSubMax` (a ledge/cliff), while a
   smooth ramp passes. Result: **route UNCHANGED** (`expands`~270 same as before, same `say=` legs,
   still routes into the pit, tester still stuck). So the descent A* uses **passes a ~50 deg cap** — it is
   either a smooth-but-too-steep slope the player's REAL slope limit rejects (limit < ~50 deg, so
   `kEdgeSubMax` needs to be much tighter, e.g. matched to the game's actual walkable-slope) OR the check
   is not catching the right edges. The mechanism is plausibly right; the THRESHOLD/coverage is not
   pinned. **Left in the tree for the next session to tighten or rework — see Known Issues.**

**WHERE TO START NEXT SESSION (see Known Issues "pathfinder elevation" below).**

### Session 67 — `o` describe stale in field; `MenuState::IsAnyMenuOpen()` is NOT a field/menu gate

**KEYWORDS: o key describe stale help text field no menu active IsAnyMenuOpen DAT_0208ebc0 never
nulled FocusedOwner input focus window help generation g_helpGen NotifyFocusChanged pause menu close
FUN_00280de0 cat 0x12 teardown**

- **`MenuState::IsAnyMenuOpen()` cannot gate "is a menu open".** It returns `FocusedOwner() != null`
  = `*DAT_0208ebc0 != null`. `DAT_0208ebc0` (the input-focus window ptr) is **WRITTEN ONLY** — the sole
  write is `DAT_0208ebc0 = param_2` in `FUN_00244830`; **no path ever nulls it** (grep of the whole
  decompile: 1 write, 0 clears). After the first menu it holds the last-focused window forever (the
  field root once back in the field), so it reads "menu open" during field roam and battle. A prior
  session already hit this — `entity_list.cpp:254` removed an `IsAnyMenuOpen()` gate that "silently
  killed the field object scan for the entire fight." Do not use it as a menu/field discriminator.
- Also dead as menu-open signals: **`DAT_0209ac30`** is a fixed singleton (`= &DAT_0209ac60`, never
  null); **`PlayerState::IsFieldActive()`** stays TRUE under the pause menu (map still live);
  **`DAT_0228ea60`** menu registry is zeroed on close (`FUN_00241d40` case 0x12) but only tracks
  `FUN_00241d40`-driven pop-ups/type-1, not equip/license/gambit — so not a general "any menu" signal.

### Sessions 58–59 — exit positions and the route target

**KEYWORDS: +0x84 edge pairing door binding N+1 struck arrival table nearest-boundary probe seam
geometric edge detection building wall vs map edge void-beyond test trigger bearing SeamTarget
walkmap boundary is not the transition script zone 0x202d crossing direction At the exit Walk east
crossRad kAtExitDist arrival Y nominal blob dY**

0. **FINDING THE TRANSITION FROM THE WALKMAP IS IMPOSSIBLE — TWO ATTEMPTS, BOTH STRUCK. Do not try a
   third.** The transition trigger is a **script ZONE (VM native `0x202d`)**; the collision mesh does
   not model it, so no amount of probing the walkmap can locate it. This is not a tuning problem.
   - *Attempt 1 (S58, discarded before shipping):* probe 16 headings out from the arrival, take the
     direction whose floor ends soonest. Cannot work — an unwalkable sample is a building wall and a
     map edge alike, and the "floor stays absent 3 m beyond" refinement only rejects thin railings; a
     building footprint passes it exactly as the map edge does.
   - *Attempt 2 (S58, SHIPPED and refuted in play):* march along the trigger bearing to where the floor
     ends. On Muthru Bazaar it found a real boundary **0 times out of 2** — one exit ran its full 14 m
     cap without leaving the floor, the other "ended" 3 m out by dropping 8 m onto a lower tier (it had
     no floor-continuity check). The tester was then routed confidently into a wall: ten `\` presses,
     "East 3. 3 steps" every time, x pinned at 48.40–48.41.
   - **What works instead (S59):** the route target is the `+0x84` ARRIVAL, and the crossing DIRECTION
     is read from the paired trigger record — it sits off-mesh exactly along the crossing axis on every
     transition measured. The planner speaks it on arrival ("At the exit. Walk east."). The player never
     needed a better target; they needed to be told which way to step.

0a. **The `+0x84` edge record's BEARING is NOT the direction you cross the transition — REFUTED IN PLAY
   (S60).** S59 shipped it: the bearing is exactly axis-aligned on every measured transition, which made
   it look definitive. On Muthru Bazaar the tester was told "Walk East" on five consecutive presses with
   a stable camera (`cross=90.0deg`, ref −176.4/−177.9/−178.0/−176.4/−176.4) and **their x never once
   passed 48.41 all session** (walked span x∈[38.36,48.41], z∈[45.68,68.25]). The relative-frame math
   checks out and agrees with the route legs — the direction is just wrong. Whatever that record is, it
   is not the trigger's heading.

   **That is FIVE blob-derived trigger models refuted on the ground:** `+0x54[N+1]` (S46), the
   `+0x54`∪`+0x70` union (S55), the nearest walkmap boundary (S58, pre-ship), the trigger-bearing march
   (S58), the edge bearing as crossing direction (S59). The cause is structural and unchanging: **the
   trigger is a script zone (`0x202d`) whose geometry is not in any blob table we can read.** DO NOT
   PROPOSE A SIXTH MODEL FROM THE BLOB. `NavTrace` (S60) now records where transitions actually fire —
   build from that measurement, and make any new model reproduce the recorded crossings first.

0e. **Every LOCAL rule for binding a destination to a doorway is refuted (S62). Do not try another.**
   Blob table order (labelled Muthru's dead slot "East End" and the real corridor "NOT USED"); the
   routine's `0x011E` argument (it is `ctrlIndex + 1`); and `entrance` read as a local slot (three East
   End controllers would claim slot 2 -- it matched on Muthru only because that pair is co-indexed).
   **The only verified rule is the ARRIVAL RELATION and it is not local:** map M's slot S leads to D iff
   D's script has `mapjump(M, S, 0)` -- so you learn a map's doors from its NEIGHBOURS (`exit_links.h`,
   persisted). An unestablished destination leaves the exit unnamed; do not add a "best guess" fallback.

0i. **"Map M has no door to X" is a statement about M, not about X (S66).** "East End has no loader to
   305 (Eastgate)" sat open from Session 55 and was repeatedly treated as evidence that the exit reader
   was incomplete. It was not: **the Eastgate transition is off Southern Plaza (292)**, and East End
   correctly has no loader to it. Several sessions went into hunting a door on the wrong map. When an
   expected neighbour is absent, check the ADJACENT maps before doubting the reader. (This also softens
   the S64 claim that 305 "was never a door to find" — it is a real walk-through transition, just not
   from East End.)

0h. **SOLVED (S64) — and the lesson is where the answer was hiding.** Transitions are tagged in the
   WALKMAP: `group = (polyFlags >> 3) & 0x1F`, matching the owning routine's `setmapjumpgroup(K)`.
   That field sits in a structure the mod had parsed for **thirty sessions** — `ReadCellFloor` reads the
   same flags word and uses **three bits of it**. Six models were invented to replace data that was
   already in memory, unread. **Before inventing a model, dump the whole record: every unread byte of a
   structure you already parse is cheaper to look at than one hypothesis is to test.** The same applies
   to the `+0x54`/`+0x84` records (stride 0x20, only 16 bytes ever read).

0f. **STRUCK (S63): "the transition trigger is VM native `0x202d`."** `mapctrl` native ids run
   ~`0x0000`-`0x06BA`; `0x202d` = 8237 is outside the table and indexes past the end of the symbol
   file. It was asserted in S57 and then quoted as established fact in three documents across five
   sessions. **No `__MJ_CTRL` routine contains a zone test at all** — all 15 of its natives decode
   (fade out, move party, jump) and none reads player position. The real trigger natives are
   `istouchuc` / `istouchucsync` / `settouchwh` / `touchradius`, and the test lives in the map's
   **Director** routine, which had never been dumped. Lesson: an id with no name attached to it is a
   guess; name it from `dbg_symbols_mapctrl.csv` (`dbgIndex = nativeId + 5140`, anchored on `mapjump`).

0g. **STRUCK (S63): the S62 learned/persisted `ExitLinks` store.** It discovered exit destinations by
   visiting maps and writing them to disk. That is against the project's rules — discovery happens in
   the decompile, the mod reads the game's own data, and `GameArchitecture.md` already specified that
   the exit reader resolves "on the first frame of any map with no cross-map data, no cache and nothing
   learned by playing". Removed entirely, not kept as a fallback. **Do not reintroduce a learning cache
   for anything the game itself can be read for.**

0c. **Do NOT filter exits on whether the destination name resolves (S61).** The "drop placeholder
   destinations (NOT USED, ids 293/294)" rule looked right while we trusted the destination binding.
   We do not: it is by blob table order and it mis-assigns. On Muthru the REAL Rabanastre-East-End
   corridor carries the "NOT USED" label and was being deleted, while the phantom that kept its name
   had a wall 3 m past it. **A label cannot decide whether a doorway exists.** The passage test can:
   march the crossing axis with `MapQuery::SegmentTraversable`; a real doorway has somewhere to go
   (>= 4 m), a dead slot does not. An unresolvable name costs the entry its WORD, never its existence.

0d. **The player position reads exactly (0,0,0) for ~1 s after a map load, and passes
   `IsFieldNavSafe`.** The NavReach flood burned a full pass on it at every transition
   (`fill complete -- 1 cells reachable from (0,0)`) and NavTrace logged a phantom crumb. Reject the
   exact origin in any per-frame position consumer; no real field position is there.

0b. **An exit's blob Y is not necessarily its floor.** Muthru Bazaar's East End arrival carries `y=0.0`
   while the player walks that ground at `y=−9.0`, which made the describe announce "(above)" for a
   doorway at the player's feet. Project the arrival onto `GroundAt(x,z)` and keep the blob Y only as a
   fallback. Corollary: **never test "am I at this exit" in 3D** — X/Z only.

1. **`__MJ_CTRL<N>` owns `+0x54` slot `N+1` (the S46 door rule) — FINALLY STRUCK, with the replacement.**
   Not merely "unverified": measurably wrong on any map with a controller-less arrival. The correct
   binding is the `+0x84` edge-pairing (GameArchitecture.md, "Exit mechanism — SETTLED Session 58"). It
   survives ONLY as the shape-mismatch fallback. Do not re-derive it as a primary rule.

2. **Using the `+0x84` EDGE record itself as the route target — REJECTED (design, not a bug).** It is the
   trigger volume's reference point: off the walkable mesh and at a variable distance (~120 units past
   the arrival on East End's Southern Plaza exit). A* cannot route to an off-mesh goal, and the distance
   would be spoken as nonsense. Its *bearing* is used instead; its *coordinate* never is. Likewise,
   pairing controller→edge by adjacency or by nearest distance both fail on at least one real exit — the
   only correct pairing is ordinal (the record after the i-th edge).

3. **Finding the map seam geometrically — WRITTEN AND DISCARDED before shipping.** The idea was: probe
   16 headings out from the arrival, take whichever direction runs out of floor soonest, and route to the
   last walkable point. It cannot work, because **an unwalkable sample is a building wall and a map edge
   alike** — an arrival with a shopfront 1.5 m to one side and the seam 4 m ahead would send the player
   sideways into the shop. The refinement of requiring the floor to stay absent for ~3 m beyond the first
   miss ("void-beyond") only rejects thin railings; a building footprint passes it just as the map edge
   does. **There is no geometric discriminator.** The direction has to come from the data — the paired
   edge record's bearing. If you find yourself designing a walkability probe to *find* an exit's
   direction, stop: read the trigger.

4. **Do not cache a seam-march miss before `MapQuery::HasWorld()`.** The exit scan runs from the first
   frame on a new map, before the collision world streams in, so every probe misses. Caching that would
   pin every exit to its arrival point for the whole area — the exact class of bug as S56's
   arrival-point exclusion silently excluding nothing because the object list was still empty.

**KEYWORDS: direction reversed 180 flip camera DAT_02aedf30 movement basis camera-relative accepted
placeholder NOT USED multiplicity 3 ids struck Aerodrome exit union shop arrival point spawn +0x70
sign object __MJ_CTRL slot N+1 camera lock battle targeting travel anchor Session 55 56**

**SESSION 57 — the big one. STRUCK: `+0x54` is a set of exit trigger volumes.** Explore-agent decompile
trace (0.9–0.95): `+0x54` is the **party ARRIVAL table** (x/y/z/angle only, no destination), read by
`getmapjumppos(nowjumpindex)` / `FUN_00264b90` and used by party-spawn `FUN_00259b30` to place you ON
ARRIVAL. `mapjump(dest, jumpIndex, flags)`'s `jumpIndex` = the **arrival slot on the DESTINATION map**,
not the source. No engine loop tests `+0x54` vs the player; the transition trigger is a **script zone
test (VM native 0x202d)** inside the `__MJ_CTRL` routine. So **there is NO `+0x54`→destination binding**,
and S46's `__MJ_CTRL<N> owns +0x54 slot N+1` paired two independent tables — it only ever held on
Nalbina's tiny maps. This is the definitive cause of BOTH the "Southern Plaza loads the Bazaar" mislabel
AND the "exit lands a few steps short" (we route to the arrival point, which is set back from the edge;
same data everywhere, so it happens in Nalbina too). Fix = read each exit's transition-tile position from
`+0x84` (leading candidate; `FUN_00264b90` reads it as the parallel table) or the routine's `0x202d` zone
test — never `+0x54`. Do not re-attempt N+1 or any `+0x54`-position → destination pairing.

Closed in Sessions 55–56 (Rabanastre East End / North End; all from the mod log + `'` dump):

1. **"The pathfinder direction flip is arc-smoothing / a mod bug."** WRONG — it is the game's camera.
   `NAV-ROUTE` showed the same route with five legs, identical lengths, every word exactly 180°
   opposite between two drains; player Y went 0.00→0.50 (stepped onto the terrace) on that frame. The
   direction reference reads row 2 of `DAT_02aedf30`, which the game block-copies each frame from the
   active camera. We read it correctly. Movement is camera-relative, so this cannot be fixed by any
   read — accepted and documented in README. See the camera-lock and travel-anchor dead ends below.

2. **Camera lock (freeze `DAT_02aedf30` to world axes at `FUN_004742a0`'s entry).** Rejected: that
   matrix orients the character onto the battle target, and the rotator keeps cross-frame lock-on state
   (`param_1[2]` + a 22.5° threshold, `FUN_002ddcf0` reset flag). Freezing its input basis breaks
   targeting, an undiagnosable soft-lock in combat. Also a write to game memory for a read-only-solvable
   problem. DO NOT re-attempt.

3. **Travel-anchored direction frame** (reference = direction actually walked, first leg as a turn).
   Rejected: stops the WORDS flipping but the held stick input is already wrong the instant the camera
   swings, and the readout stays silent until the next query — a symptom swap, plus 8 invented phrases.

4. **Spoken "Camera angle changed." notice on a large reference delta.** Rejected: the battle camera
   reorients on every target change, so it would fire almost continuously in combat.

5. **Placeholder-name detection by multiplicity (a name shared by ≥3 map ids is filler).** Refuted by
   its own first run: real "Aerodrome" is shared by 5 ids, "No. 10/11 Channel" by 3, while the actual
   placeholder "NOT USED" is shared by only 2 (293/294). Multiplicity flags real names and misses the
   filler. The "4 rows" evidence came from `planmapname_areas.csv`, which concatenates the area+region
   tables (table A holds two). Replaced by an exact match against the shipped token `"NOT USED"`.

6. **"+0x70 field-sign table is empty on every map."** STRUCK — 25 records on East End (13 in group 0,
   12 in group 3). The old "empty" reading was a calling-convention bug (getters take the group in ECX).

7. **`__MJ_CTRL` controllers are the whole exit list.** STRUCK — East End slot 7 has a `+0x70` sign and
   no controller; the exit source is the UNION of both tables. (The `__MJ_CTRL<N> → slot N+1` pairing
   is separately still unverified beyond one door/map — pending a walk test.)

8. **Every `+0x54` slot near a field sign is an exit.** WRONG — over-matched shop *arrival* points
   (destIdx 20-26) and the party-arrival slot. A sign only makes its slot an exit if no controller
   already claims that sign AND no scene object sits on the sign (an interior doorway has the
   press-Enter object on it; a district sign does not).

9. **"kind==5 is the ACTION gimmick" (S54).** STRUCK for the field population — on East End kind 5 =
   NPC, kind 4 = shop doorway, kind 1 = player. Consequence: `IsInteractionAvailable` (gated on kind 5
   / talk-target) has never evaluated a real door, so F5's story-gate filter has never applied to exits.

**KEYWORDS: town gate Rabanastre interactables missing FLAG_TALK NPC misclassification kind nibble
0x0E story gate 0x10 npcdic odd slot yomi second name Rabanastran duplicate names BLOB_MAX 0x18000
snapshot window silent return false camera-relative 180 degree flip Session 54**

Four dead ends closed in Session 54, all resolved OFFLINE (decompile + extracted data), no runtime
discovery:

1. **"A talk flag means it is a person."** `ClassifyByNameKey` returned `Category::NPC` for anything
   with `FLAG_TALK` (`sceneObj+0x1C & 0x400`), ahead of the character test. **The engine never says
   that** — `FUN_0025b820` branches on TALK and ACTION *independently on the same object*. A town gate
   opens a confirm prompt, so it *is* a talk target, and every one was filed under NPC.
   **…but the replacement was wrong too, and that is the more useful lesson.** The fix put
   `kind == 5` (from `FUN_002675c0` / `FUN_0025bad0`, "1 = person, 5 = action gimmick") *ahead of* the
   scene-character test — and **every NPC became an Interactable**. Tester caught it in play. So:
   **"kind 1 ⇒ person" is REFUTED** (field NPCs do not carry kind 1); `kind == 5` survives only as a
   gate/switch test *among non-characters*; and the reliable person test is the one that was already
   there — the scene CHARACTER class `sceneObj+0x03 & 0x1f` in 5-7. Two decompiled functions agreeing
   on a shape is NOT the same as that shape being a classifier for the population you are scanning.
   Shipping order now: npcdic gimmick band → `isCharacter` ⇒ NPC → non-character `kind == 5` ⇒
   Interactables → `FLAG_TALK` ⇒ NPC → Interactables. The `'` dump gained `kind=`/`en=` per object —
   its absence is why nothing caught this before it shipped.
2. **Classifying or INCLUDING an object by `sceneObj+0x1C` at all.** Those bits are **mode state**:
   `FUN_0025ad10` / `FUN_0025ae00` set `0x400` and clear `0x004` on entering talk mode, and clear
   `0x400` when the talk id is invalid, so a disabled object has **zero** flags. The scan's inclusion
   test (`interactive || gimmick-band || named`) therefore *dropped story-gated gates entirely* —
   exactly when the player most needs to find one. Include by KIND; use the flags only for "what can I
   do with it right now".
3. **Mining a better NPC name out of npcdic's odd slot.** `FUN_00263990` picks `slot = id*2 +
   (FUN_0032a930(id) != 0)`, and that flag is a live per-id bitfield at `FUN_002ef2b0()+0x13B4` — so
   the odd slot is a *state-selected second name*, not the "yomi/reading" `tools/parse_npcdic.py`
   assumed. **But in the US build it is byte-identical to the even slot** (decoded straight from
   `npcdic.bin`: 2282 slots / 1141 ids; ids 0-11 and 433-469 all `even == odd`). There is no hidden
   name. The duplication is the game's own data: **109 ids all render "Rabanastran"**. Number them;
   do not invent descriptors.
   **Struck from this entry as an UNVERIFIED HYPOTHESIS:** "gate guards are already distinct — npcdic
   362 = Imperial Guard". Id 362 exists and reads "Imperial Guard" (a separate entry from id 1 and the
   41 other ids that render "Imperial"), but **nothing confirms any object at the Rabanastre gates
   carries it** — it came from grepping the dictionary for guard-ish words, which proves only that the
   string exists, not which scene object stamps it at `+0x102`. The tester, who plays these areas,
   reports the gate guards are **not** Imperials. Confirm properly by standing at a gate, pressing
   `'`, and reading `nameIdx` on the objects near the logged player position. No code keys off 362.
4. **Reading `__MJ_CTRL` out of the `.mpk` files offline.** Zero plaintext hits across all 20 extracted
   map archives — *including the Nalbina maps where it is proven present at runtime*. The name pool is
   packed on disk; only the loaded blob answers it. (Consistent with the older "offline `.mpk` mapjump
   literal extraction finds zero hits" entry below.)

Two live bugs with the same shape — **a silent fallback that made a failure look like a fact**:

- **`MapScript::SnapshotBlob`'s fixed `BLOB_MAX = 0x18000` window.** The reader copied 96 KB of the
  map-control blob and bounds-checked the header offsets *against the copy*, so any map whose routine
  table sat past 96 KB made `ReadExitDests` `return false` **before its first log line**. Every door
  then logged as `-> no controller (arrival point)` and `Exit=0` — on every Rabanastre map. The tell
  was an ABSENCE: no `==== field-script exits ====` header at all, while `EnumerateMapJumps` happily
  reported 14 doors from the same blob. Fixed by reading the live blob directly (SEH-guarded, sanity
  ceiling instead of a window) **and by logging a reason on every bail**. See `GameArchitecture.md`.
- **The camera-relative direction frame's 180° flip.** CERTAIN (read off the code, not inferred):
  every call site wrote `float facingRad = 0.0f; PlayerState::ReadCameraForward(facingRad);` and
  **ignored the bool return**; the getter leaves `outRad` untouched when the camera row is not
  refreshed that frame, and `CompassFaceDeg(0) == 180`, so those frames spoke **every direction
  reversed**. Fixed by `PlayerState::ReadCameraForwardStable` (live value, else last good) whose bool
  every caller now honours — no reference means **no direction word**, never a defaulted one.

**KEYWORDS: egocentric compass words confusing southwest becomes north follow-cam trails relative
frame absolute vocabulary ahead behind left right leg merging 7 north 1 northeast Session 54**

**"DIRECTIONS SNAP AROUND" — the cause was the ROUTE SMOOTHER, not the frame and not the words.**
Three reversals before it landed; the reasoning is what matters:

1. Tester reported routes "constantly switching directions" → the relative frame was withdrawn for
   world-absolute. **Wrong:** absolute is stable but useless, you cannot push "north".
2. Frame restored, but with literal ego words ("ahead", "behind-left"), on the theory that
   compass-words-on-a-relative-frame was the confusion. **Also wrong**, and the tester's preference
   is the opposite: *"most prefer north/south/east/west … essentially north=forward whether it's true
   north or not, east=right."* Vocabulary was a red herring.
3. **The actual cause**, in the tester's words: *"we're not telling the player to walk 30 northwest
   when the arc is actually 18 north, 7 west … then if the player walks 10 west by accident the
   directions flip to northeast because the player has gone past the north leg."* The planner's
   greedy string-pull (`path_planner.cpp`, keep a waypoint only when the straight span is NOT
   traversable) collapses an L across open ground into one diagonal chord. The player is sent along a
   line the route never takes, and any drift off that imaginary diagonal comes back inverted.

**THE DIAGONAL RULE (tester's, now enforced in `path_directions.cpp`):** a diagonal word may ONLY
describe a genuinely diagonal stretch — a fine alternation (1 north, 1 east, 1 north, 1 east …) whose
net line really is 45°. A route with any shape to it — an L, or a gradual bend — is spoken as its
actual legs.

**Both extremes are wrong.** Legs off the smoothed chord gave one invented diagonal; legs off the raw
cell path gave a **19-leg** readout, measured in play ("North 6, Northwest 6, North 4, Northwest 8,
West 10, …"). Shipping pipeline: raw path → **Ramer-Douglas-Peucker at one grid cell** (deviation-
bounded, so real corners survive and grid jitter collapses — unlike string-pull it cannot cross a
corner) → runs → staircase collapse → merge → absorb sub-2-step legs → **cap at 5 spoken legs, then
"then N more"**. Totals always equal the sum of what was spoken.

Also recorded: **`phyre_types.h`'s `KIND_DEAD = 5` is a misname** — 5 is the field-gimmick (ACTION)
kind. Left in place because the combat track owns the correction and the actor pool holds no gimmicks,
so the skip is a no-op there today. Do not build on the "dead" reading; the same header's "kinds 1, 2
and 7 are enemy" is suspect too (the live diag shows Vaan as kind 1).

**KEYWORDS: field party menu entry announce speaks-on-keypress menu-not-ready readiness signal
FUN_00280de0 0x160DE0 cat 0x11f ACTIVATE never-sent DrawCallback first-paint 31ms 400ms fallback
draw-callback timeout Session 52 SOLVED-by-0x13**

**FOUR "menu is ready" signals REFUTED before landing on `cat 0x13` (SHOW).** The field/party menu
announced its focused row on key-press, before the menu was visible. Four attempts to detect
"visible" failed BY MEASUREMENT — do not retry any:
1. **first UI string drawn** (`TextCapture` per-string DrawCallback) — the field HUD paints text
   every frame, so it fired the very next frame ≈ key-press. `99ce38a`.
2. **the focused row's OWN text drawn** — measured **31 ms** after the focus event; the row is
   rasterized long before the menu is presented. Drawing ≠ presentation. `eaea451`.
3. **window ACTIVATE `cat 0x11f / msg 0x8000`** on `FUN_00280de0` — the decompile makes it look like
   "menu is live", but it is **NEVER SENT** (0 occurrences in a full session). This SHIPPED AS
   SILENCE. `1600244`.
4. **a 400 ms timeout fallback** that speaks anyway — a band-aid that speaks at the wrong time and
   hides which signal is right; also parked a per-string DrawCallback on the game's paint path.
   `58071b2`.
**SOLVED** by triggering on `FUN_00280de0` `cat 0x13` = the SHOW/menu-visible message (see Solved
Problems below + `GameArchitecture.md`). The lesson: the trigger must be the game's OWN visible-open
event, and "drawn" is not "shown". A bounded first-seen `wnd:` log is what proved 0x11f's absence —
the earlier version capped at +110 ms and hid `0x13`, which lands after that.

**KEYWORDS: tutorial popup item name missing substitution slot 0x0f 2e escape dropped
Orrachea Armlet Try equipping button glyph icon insert rbn_a16 msg 156 OPEN ISSUE**

**OPEN — `0x0f` SUBSTITUTION SLOTS are dropped, so injected names go unspoken.** Reported
2026-07-21 for the blue tutorial box:

    Try equipping the Orrachea Armlet.
    Use the Licenses command in the Party Menu
    to obtain the Accessories 1 license,
    then use the Equip command to equip it.

The item name is not spoken. The user identified this as the same class as the button/control glyphs
being dropped in tutorial pop-ups, and that is exactly right.

**Root cause is already pinned, offline** -- `rbn_a16.ebp` message 156 (found via the extracted
scripts in `FFXII-Decompile/extracted/`). It decodes to:

    'Try equipping the 
Use the Licenses command in the Party Menu
...'
                      ^^^ the name is simply absent

and the raw codec bytes show why:

    4d 41 3e 04   0f 2e 80 90   a8 02
    t  h  e  SP   <-- HERE -->  .  


**`0x0f 2e` is a runtime substitution slot** and the template does not contain the name at all -- the
game fills it in at draw time. `EscapeParamCount` classifies `0x2e` under the generic
`0x20..0x70 -> 2 params` arm, so `GameText::Decode` skips all four bytes and emits nothing. Compare
`0x31`, which our own table already labels "the codec-sprintf STRING substitution slot" (3 params);
`0x2e` is a sibling we have never resolved.

Note `0x0f 29` also appears later in the same message -- that is the COLOUR escape (`FUN_003ffbc0`)
that tints "Accessories 1" cyan. It is harmless: that text is literal and does get spoken. Do not
confuse the two while fixing this.

The work is: determine what `0x2e`'s two parameter bytes (`80 90` here) select, and where the game
resolves them. Likely the same machinery behind the composed system banners (see the "system
notification" entry) -- both are templates with coloured, injected tokens. If the runtime string
handed to our telop hook is already substituted, the fix may instead be that we are reading the
template rather than the composed buffer; check that first, it is cheaper than decoding the slot.

DEFERRED by user instruction 2026-07-21: document now, fix in a later session.

**KEYWORDS: system notification banner added to the Party Menu Clan Primer full-screen overlay
desaturated blur icon escape not spoken st2e FUN_002f9860 OPEN ISSUE**

**OPEN — full-screen SYSTEM NOTIFICATION banners are not vocalized.** Observed 2026-07-21:

    <wing icon> Clan Primer has been added to the Party Menu. <wing icon>

drawn centred over the field with the whole scene desaturated and blurred behind it. The mod says
nothing. Other notifications of this shape ("X has been added to...", feature/menu unlocks) are the
same surface and equally silent.

What is already known, so the next session does not start cold:

- **It is NOT event dialogue.** Searched all 17,268 messages across the 617 extracted US `.ebp`
  scripts (`FFXII-Decompile/extracted/`, see `tools/ebp_find_pagebreak.py` for the walk) for both
  "added to the Party Menu" and "Clan Primer": **zero hits**. So the telop reader
  (`FUN_002e16b0`) and the `.ebp` message table can never see it -- do not go looking there.
- **So it is a system/UI string**, which points at the st2e tables the menu text uses --
  `FUN_002f9860(id) -> codec byte*`, already hooked by `TextCapture::HookResolve` (that hook
  currently only caches ids 1000/1001 and the key-binding block, so the id would be visible there
  with the filter widened). `tools/st2e_decode.py` is the offline counterpart.
- **The line is composed, not literal.** "Clan Primer" is coloured differently from the rest, so the
  banner is a template with a substitution slot -- almost certainly the codec-sprintf STRING slot
  `0x0f 31` (3 params) already in our table -- filled with the item/feature name.
- **The flanking wings are icon escapes**, the `0x0f 0x40-0x6B` glyph family (1 parameter) in
  `game_text.cpp`'s `EscapeParamCount`. They decode to nothing today, which is correct for speech.
- The display surface is NOT identified. It is not the item toast (`FUN_0035e070`, which is the
  "Obtained <item>" path and works), and not the menu system-message surface (`FUN_0057c480`, which
  is menu-only). The screen-wide desaturation suggests its own overlay/effect object -- that effect
  may be the easier thing to find first and work back from.

DEFERRED by user instruction 2026-07-21: document now, fix in a later session.

**KEYWORDS: telop burst ambient NPC lines area load 18 messages 140ms FUN_002e16b0 slot 0
ambient chatter spoken on entry OPEN ISSUE**

**OPEN — the telop setter fires for EVERY ambient NPC line at area load, and we speak them all.**
Found 2026-07-21 while fixing multi-page dialogue. At Rabanastre entry the log shows **18 telop
messages in 140 ms** (+28641 .. +28781), all `slot=0`, all different NPCs: *"There you are,
Kytes."*, *"Creature spotted in the Estersand."*, *"Ah, Vaan. Migelo send you, too, did he?"*,
*"Quite the affair, throwing a banquet..."* — the area's whole ambient chatter table.

Each is spoken with `interrupt=true`, so they cancel one another and the player hears fragments of
the last one. This is SEPARATE from the multi-page fix (that one was a single message containing
several pages, now split on codec 0x03).

Not yet known: whether these calls are the game registering the table or genuinely displaying each
line for a frame. `FUN_002e16b0` param_2 is the slot (0-7, clamped) and all 18 use slot 0, which
argues for sequential replacement rather than 18 simultaneous displays. Needs the display-vs-set
distinction settled before any gating — do NOT simply rate-limit it, that would be a dedup in
disguise (see the no-dedup rule).

**KEYWORDS: dialogue choice pop-up options not spoken Tomaj hunt bill yes no selectable
conversation branch cursor pointing hand R Log Space Confirm OPEN ISSUE**

**KEYWORDS: duplicate interactables every shop listed twice handle table entries array sub-range
grp0 grp1 0x1D2 0x1D6 0x1DA 0x1DE FUN_0025b820 shadow registration dup-label OPEN ISSUE Session 54**

**OPEN — every interactable is listed TWICE (reported in play, "at least all of the shops").** Not
the `+0x54` arrival-point duplication (that one is separate and already deduped by exact position
equality in `MapExits::EnumerateMapJumps`). NOT YET DIAGNOSED — do not guess-fix it.

What is known: `EntityScan::BuildLocked` dedupes by scene-object POINTER (`AlreadyListed`), so the
twins must be **distinct objects that share the game's own name**. Two candidate explanations, and
they need opposite fixes, which is exactly why this is not being patched on a hunch:
- **Shadow registrations.** The mod walks the container's whole entries array `[0, entries[0])`. The
  GAME does not — `FUN_0025b820` walks exactly two `(start, count)` spans of that slot space:
  **grp1 (talk AND action) start `container+0x1DE` count `+0x1D6`**, **grp0 (action only) start
  `+0x1DA` count `+0x1D2`** (container stride `0x288` from `DAT_02098e10`). Anything the mod lists
  from outside both spans is something the engine never treats as interactable. If the twins live
  there, the fix is to walk the game's spans.
- **Two genuinely distinct objects with one name** (a merchant and their counter; 109 npcdic ids all
  read "Rabanastran"). Then there is nothing to remove and the numbering is the right answer.

**Instrumentation shipped (Session 54), one session settles it:**
- `NumberDuplicateLabels` dumps every duplicate group **once per map** as
  `NAV-DIAG dup-label "<name>" xN:` followed by one line per member with `obj=` pointer, `handle=`,
  `kind=`, `nameIdx=`, `flags=`, `avail=` and `pos=`. **Same position + different pointers ⇒ shadow
  registration; different positions ⇒ two real objects.**
- The `'` dump's per-container line now prints the game's own spans:
  `container N: … | game spans: grp1(talk+act)=[a,b) grp0(act)=[c,d)`. Cross-reference a twin's slot
  (the `i` in the `[c:i]` object lines) against those to see whether it falls outside both.

**KEYWORDS: unplaced reserve slot world origin 0,0,0 NPC 1 NPC 2 no path handle table
ScanCombatants guard missing East End NPC=37 phantom Session 54 SOLVED**

**"NPC 1, NPC 2 … no path to them" — unplaced reserve slots at the WORLD ORIGIN.** Reported in play;
root-caused from the `dup-label` dump in one session. East End listed **37** entities, ALL at
`pos=(0.00,0.00,0.00)`, all `nameIdx=-1` with an unresolvable custom string (so the label fell through
to the category word "NPC", then to "NPC 1"…"NPC 37" once duplicates were numbered), split between
`kind=5 flags=00030004 avail=1` and `kind=1 flags=00034885 avail=0`.

The map allocates object slots at load and positions them later; one still at the origin was never
placed. Being off the walkable map, routing correctly answers "No path" — the log shows
`NPC 1. North, 197 steps` then `NPC 1. No path`, 197 steps being the distance to (0,0,0).

**`ScanCombatants` has always had this guard** ("unplaced reserve unit"); the handle-table walk simply
never got one. Fixed by the same exact-zero test. **This also retroactively explains East End's
"NPC=37 Object=0"** in every earlier log — not one of those 37 was a real NPC, which is why the area
looked simultaneously full of NPCs and empty of interactables.

**OPEN — dialogue CHOICE options are not vocalized.** Observed 2026-07-21 in the Rabanastre tavern
(Tomaj, hunt-bill conversation). On screen:

- Speaker nameplate **Tomaj** with a portrait icon.
- Body: *"Do you want to hear all the details?"* — believed already spoken by the existing reader.
- **Two selectable choices below it**, a pointing-hand cursor on the first:
  *"Yeah, that would help."* and *"No, I think I got it."*
  **Neither is announced when navigated.** This is the defect.
- Footer hints: `R` = Log, `Space` = Confirm.

What is known: this is a *choice* dialogue and is NOT any of the surfaces already handled.
It is not the `FUN_0057c480` menu-message surface (that one is classified + logged but deliberately
muted, and is menu-only — it never fires for field text), and it is not the title/new-game confirm
(`FUN_00241d40`, handled via the 0x8000 focus path). The read-point for the choice LIST and its
cursor index is **not yet identified** — that is the work. Start from the message-surface section of
`GameArchitecture.md` and from whatever draws the pointing-hand cursor; the body text arriving
correctly suggests the body and the choices come from different objects.

DEFERRED by user instruction 2026-07-21: document now, fix in a later session.

**KEYWORDS: pathfinding accuracy near target reroute sand sea legs increase expands zero
recalculation oscillation fineCell 1.5m string-pull OPEN ISSUE**

**OPEN — routing gets LESS accurate the closer you are to the target.** Reported 2026-07-21: while
approaching a target the player was rerouted around the Sand Sea several times as the route
recalculated. Log evidence, same target cell throughout (`tgtCell=(99,21)`):

| seq | from (x,z)    | legs | expands | rays |
|-----|---------------|------|---------|------|
| 80  | 138.9, 35.4   | 3    | 16      | 454  |
| 82  | 143.2, 38.3   | **4**| 7       | 182  |
| 85  | 142.6, 39.8   | **4**| 8       | 210  |
| 86  | 143.8, 32.9   | 2    | 4       | 140  |
| 87  | 148.0, 30.8   | 3    | 1       | 28   |
| 88  | 149.4, 32.2   | 2    | **0**   | 0    |

The leg count RISES (3 -> 4) while search effort collapses (`expands` 16 -> 0), and the player's own
position oscillates rather than converging. `fineCell=1.5m`: at close range the target is only a
couple of cells away, so the coarse grid plus the string-pull can flip the chosen cell between
recalcs — a plausible starting hypothesis, NOT a diagnosis.

DEFERRED by user instruction 2026-07-21: recorded with the numbers; the planner was deliberately
NOT changed this session.

**KEYWORDS: dedup deduplication debounce speech suppressed silent menu re-entry focus cache
stale owner index text battle command Attack list status chooser MaybeAnnounce S51**

**Dedup as the fix for repeated speech — WRONG, and it caused silence.** Whenever an announcement
repeated, past sessions added a "same as last time, stay quiet" check. Every one of those was on an
EVENT-DRIVEN hook, where each fire is a real event the player needs. The damage:

- `menu_reader.cpp` `OnFocus` compared `(owner, index, text)`. The active-pane gate above it returns
  early WITHOUT refreshing that cache, so the stale entry survived an excursion to another pane and
  swallowed the return — leaving a pane and coming back said **nothing**.
- `ingame_menu_reader.cpp` `OnBattleCommandFocus` compared spoken TEXT alone, with no owner: backing
  out of the Attack list and reopening it was silent.
- `title_reader.cpp` put its per-draw guard on the shared path, so re-focusing the same title row
  was silent.

A repeat is an annoyance; silence is a lost position. The rule is now: **no speech dedup**, except
(1) the user asks in the current conversation, or (2) it guards a per-frame/per-draw function AND
the comment names that function. A repeat from an event hook means a second call path — find it.
Removed in Session 51. **Do not re-add a filter to quieten a repeat.**

**KEYWORDS: combat_system.md struck claims S49 FUN_0028e110 no message id FUN_00536410 outcome 9
not preview result+0x00 0x20 DAT_0209a1f0 not leader P-E cancelled row+0x00 not action name
neutral foes group 0 only aggression actor+0x6A0 pointer deref actor+0xEA4 not hostility
FUN_00313b30 per action FUN_0035c7b0 0x17 dead FUN_00312280 no reward args** (Session 49) —
**eleven claims in `Docs/combat_system.md` were disproved offline and are STRUCK there.** Do not
rebuild on them. The eight that would have shipped a bug:

1. ~~Hook `FUN_0028e110` for the game's combat sentences~~ — **it never receives the message id**,
   only a dwell class, so the realtime-vs-log-only policy has nothing to key on. Use
   **`FUN_00536410`** (RVA `0x416410`): id at `RCX+4 & 0x7FFF`, finished string into `RDX`, one
   frame, and upstream of the toast fork where the ticker is never called.
2. ~~`result+0x04 == 9` is the AI preview, filter it~~ — 9 is the **default seed**. Preview is
   `result+0x00 & 0x20` (`FUN_00308b90`), which **never calls the applier**. No filter needed.
3. ~~Filter status ticks on `attacker == 0`~~ — **filter `actionId == 0xFFFF`**; `FUN_00310db0`
   makes two *real* calls with a null attacker.
4. ~~`DAT_0209a1f0[3]` is the party leader~~ — it is 4 × `sceneObj*` in roster order. Leader is
   `*(u8*)(W + 0x5AA4)` → BtlChr → pool scan (`FUN_00327150`). **Probe P-E cancelled.** Also note
   the shipped `*(u8*)(bc+5) == 0` test matches *every* roster member, not the leader.
5. ~~Ability name is the id at `row+0x00`~~ — that is a **description** id; `Attack` and every
   `Reserve` row share `4000`. The name is **`row+0x34`** → the `word.bin` pool.
6. ~~The engine files Neutrals under Foes, so a Neutral is a legal attack target~~ — **backwards.**
   The emit gate is `(g & ~2) == 0`: foes = group **0 only**, and Neutrals reach neither list.
7. ~~Aggression: `f = *(u32*)(actor+0x6A0)`, radius `+0x6A4`~~ — **`+0x6A0` is a POINTER.** Same
   missing-deref class as the `4`/`5`/`6` bug. Use `*(u32*)(*(u64*)(actor+0x6A0))`, radius at
   `aiData+0x04`. Still only 0.97 structurally, so it does **not** ship.
8. ~~"In battle" = any party actor with `+0xEA4 != 0`~~ — that mask is "who has an action aimed at
   me", **not gated on hostility**, so an out-of-combat Cure trips it. Use
   `*(u32*)(actor+4) & 0x100000` (`FUN_002fead0`).

Plus three lifecycle mislabels: `FUN_00313b30` fires ~2× **per action**, not per battle;
`FUN_0035c7b0` never receives `0x17` from any caller (use **`FUN_0035c8a0`**); and `FUN_00312280`
passes **none** of EXP/LP/gil/loot as arguments (gil → `FUN_00469e80`, loot → `FUN_003180f0`).

**KEYWORDS: game_text.cpp high-bit fallback wrong 75 of 102 codec escape 0x29 eats punctuation
0x2d numeric slot missing level number 0x10-0x1f two-byte glyph stray letter icon family 0x40
ffxii_codec.py** (Session 49) — the decoder's *"after `0x0f <sel>`, consume every following byte
`>= 0x80`"* fallback is **wrong on 75 of the 102 battle messages**, not just the one apostrophe
previously documented. Digits (`0x85`–`0x8e`) and punctuation (`0x99`/`0x9a`/`0xa0`–`0xaf`) are all
`>= 0x80`, so sentence-final `.` and `!` are eaten as parameters. Additionally: `0x2d` is the
**numeric substitution slot** (why the level-up line lost its number), `0x10`–`0x1f` are **2-byte
extended glyphs** whose trailing byte the mod renders as a stray letter, and the `0x40`–`0x6b` icon
family takes exactly **1** parameter byte. The complete table is
`..\FFXII-Decompile\tools\ffxii_codec.py`, regression-tested to **zero structurally-unexplained
bytes**. Port `game_text.cpp` from it; do not re-derive.

**KEYWORDS: exit destination door pairing getmapdestposbyindex nowjumpindex lastjumpindex jump-index
dispatch gateway record arrival symmetry routine table 2 entries 0x5C routine index __MJ_CTRL
mapData+0x54 +0x84 +0x70 +0x8c** — the long hunt for "which door goes where" (Session 46). Everything
below was tried and is **DEAD**; the answer was the map's own field script (`__MJ_CTRL<N>` owns `+0x54`
slot `N+1` — see `GameArchitecture.md`, *Exit destinations = the map's own FIELD SCRIPT*).

- **Looking for a `destination-by-door-index` getter — there is none.** Every `mapData` accessor was
  traced across all 33,105 functions. `getmapdestposbyindex` is `FUN_00264b90` selector 1 and returns an
  arrival **position** from `mapData+0x84`, not a map id. The doc table said otherwise and **sent this
  session chasing a getter that does not exist** — now struck.
- **Expecting the destination to be a FIELD on the exit record.** It is not. `+0x54` records are
  x/y/z/angle then zero bytes (raw dump confirms); `+0x70` field-sign records hold positions with an
  empty destination slot on interior maps (`destIdx=0`, `areaId=0xffff`); `+0x8c` is keyed by a
  field-sign `+0x1d` byte, never a jump index.
- **"The routine table has 2 entries."** A parse bug, not data: **word 0 is an entry COUNT**. Reading it
  as a record, plus a "stop on zero offset" guard, truncated a 24-routine table to 2.
- **A jump-index dispatch in the script.** `nowjumpindex` (native `0x8f`) has **ZERO** call sites in the
  whole script; `lastjumpindex` (`0x90`) returns a **map id** (compared against the teleport-menu ids
  306/630/66…), not a door index. There is no `switch(jumpIndex)`.
- **Reading `0x5C`'s operand as a routine index.** It is a label/target — observed values (221) exceed
  the routine count (24). Any "routine X calls routine Y" conclusion drawn from `5c <n>` is invalid.
- **The "gateway record"** (`{dest, entrance, ptr}` around `+0x5E00`) is a denormalized mirror of the
  script literal; **no engine code reads its fields**, and it carries no source-door position.
- **Cross-map arrival-symmetry as a *blocker*.** The relation is real and was used to cross-check the
  door rule, but it needs every neighbour's data, so it is not a way to label the map you are standing
  in. The in-map `__MJ_CTRL` naming makes it unnecessary.
- **Offline `.mpk` extraction of mapjump literals** finds zero hits — on disk the bytecode is
  Phyre-encoded/relocated; plain bytecode exists only in the **loaded** blob.

**KEYWORDS: message read-point spec refuted FUN_0057c480 menu system message DAT_0209ac30 0x328
obtained item field chest e5f0 FUN_003c02b0 FUN_002baf80 world map screen page+0x138 map id
mini_face_c texture bundle 0.90 0.98 below bar** — `notes/message_text_readpoints_spec.md` §A and
§B(field) are **REFUTED** (Session 45). §B claimed field-chest "obtained X" reuses the `FUN_0057c480`
surface at **0.90 — below the project's 0.98 bar, and it was wrong**: that proc's case 1 does
`*(longlong*)(DAT_0209ac30 + 0x328) = surface`, i.e. it registers into the MENU manager; it is the
menu system-message window ("cannot equip", "sold") and never fired once in a 17-minute play log.
§A put dialogue on `FUN_002baf80` + `FUN_003c02b0`; those are the **world MAP screen** (`page+0x138`
= map id, `out+8` = map name, `DAT_02b457e0+0xe8` = map DB, assets `ArtData/menu/localmap/`). §A's
"decisive" evidence — that the widget owns the `mini_face_c` speaker portrait — was a misread: it is
a **texture-bundle name** passed to `FUN_0024a5a0` alongside `battle_4_p`/`s_font_c`, and the
`002baf80` family never references it. LESSON: the spec self-tagged these 0.98/0.90; a confident tag
is not evidence. The REAL obtained-item path is `FUN_0035e070` + `widget+0xC8`.

**KEYWORDS: Ghidra dropped argument register passthrough FUN_00264ac0 FUN_00264ae0 group index
Pfn_ExitCount no parameters count=0 every map +0x70 field sign dead never measured** — "the
`mapData+0x70` exit array is empty/dead" (Session 44) was **measuring the mod's own bug**, not the
data. `Pfn_ExitCount` was typed `int(__fastcall*)()` and called as `fn()`, but `FUN_00264ac0` takes a
GROUP index: Ghidra decompiles it as `(void)` only because it never *writes* `ecx` — it passes its
own incoming `ecx` straight through to `FUN_00264ae0(group)`, which uses it as a real array index
(`if (param_1 < *(int*)groupTable) return groupTable[param_1+1] + blob;`). Calling with no argument
left register junk in `rcx`, the bounds check failed, and `count` was 0 on **every** map regardless
of the data. LESSON: when Ghidra shows a call-through with no args and the callee indexes `param_1`,
that is a **passthrough signature**, not a no-arg function — check the game's own caller (here
`FUN_003f9720` passes the same group to both getters).

**KEYWORDS: parse_mapdata.py wrong blob base mpk+0x10 cluster child +0x8c zero all 550 files
contradicted by live log destCount=2 offline extraction premise failed** — `parse_mapdata.py`'s
"`+0x70` and `+0x8c` are zero in all 550 `.mpk`" is **not credible**. The live mod log shows
`destBase` non-null with `destCount=2`, i.e. `mapData+0x8c` IS populated at runtime — so the parser
is reading the wrong blob. Session 44's own note concedes the map-control blob "is a cluster child,
not `mpk[+0x10]`", which is exactly what the parser uses (`mld = mpk[u32(mpk,0x10):]`). LESSON: an
offline parse that disagrees with a runtime read is wrong until proven otherwise — the runtime is
ground truth.

**KEYWORDS: FUN_00353490 getmapjumpanglebyindex not party placement mapData+0x54 arrival spawn
demotion Category::Event Session 43 reverted** — Session 43's "`mapData+0x54` is the party
ARRIVAL/SPAWN table, not exits" rested entirely on *"proven: FUN_00353490 places the party
post-jump"*. `FUN_00353490` is abs `0x353490` = RVA `0x233490` = the mod's OWN
`GETMAPJUMPANGLEBYINDEX` constant (`nav_rva.h`); its body returns a jump's **angle**. It places
nothing. `+0x54` is the map-jump point table = the exits, tester-confirmed by walking into them.
LESSON: check a claimed function identity against the RVAs the mod already has before building on it.

**KEYWORDS: dbg_idx 5140 formula broken native slot action_binding_tables selector 0 index
arithmetic btlAtel resolve by behaviour** — `nav_rva.h:247`'s *"sel-0 slot = mapctrl.dbg_idx - 5140"*
does **not** reproduce: `getmapjumpposbyindex` (dbg 5671 -> slot 529) implies 5142 while
`getmapjumpanglebyindex` (dbg 5943 -> slot 803) implies 5140. The `.dbg` symbol list interleaves
variables and source markers (e.g. `mapctrl.src`) with actions, and duplicate symbol names exist, so
index arithmetic is contaminated. Resolve natives **by behaviour** instead (a run of consecutive
functions funnelling through one resolver, address order matching .dbg order, each body confirmed
against its name) — that is how the seven `btlAtel*FromPartySlot` natives landed at 0.99.

**KEYWORDS: GameText Decode 0x0f escape high bit run eats digits punctuation 0x85 0x8e obtained 3
Potions The command can selector param count FUN_002ac5f0** — skipping a `0x0f` escape by consuming
every following byte `>= 0x80` is **wrong** and corrupts live text. Digits are `0x85`-`0x8e` and
punctuation `0x99/0x9a/0xa0`-`0xaf`, all `>= 0x80`, so a zero-parameter escape (`0x21`) followed by a
number ate it. Seen live as `"Sometimes it's necessary to escape.The  command can"`. Param counts are
**selector-dependent**, from the game's own interpreter `FUN_002ac5f0` (RVA `0x18C5F0`): `0x21`=0;
`0x20/0x27/0x2f/0x32/0x34/0x35/0x37/0x3a/0x3c/0x3d/0x3e/0x56`=2 (`FUN_003ffab0`); `0x31`=3
(`FUN_003fff10`). `tools/ebp_msg_decode.py` has the same latent bug — its rule only held on the
offline corpus.

**KEYWORDS: pathfinder scanner actor pool BtlWork gimmick gate missing enumeration** — Enumerating
field objects by walking the 32-slot BtlWork actor pool `DAT_0208e688` (RVA 0x1F6E688) finds ONLY
characters/combatants (NPCs, party). Static field gimmicks — gates, doors, switches, treasure,
crystals — are NOT in that pool, so a gate the tutorial needs is invisible to the scan (found "2
objects" = the 2 NPCs only). FIX under Solved: enumerate the scene-object HANDLE TABLE instead.

**KEYWORDS: pathfinder classify def+5 kind NPC object inverted** — Classifying NPC vs object by the
actor def kind byte `def+0x05` (`(kind==0)?NPC:Object`) mislabels every NPC as "Object". `def+0x05`
is 0=static-prop / 1=animated-character, and field NPCs are kind **1** — inverted and too weak.
FIX: classify by npcdic id band + the scene-object interaction flags.

**KEYWORDS: pathfinder route turn-by-turn silent drain FUN_00314020 hook game thread** — Draining
the game-thread route planner from `FUN_00314020` (0x1F4020, the render step, `mode==0`) leaves `/`
SILENT: that path sits behind a fixed-timestep accumulator AND an else-branch bypass
(`FUN_001800e0()!=0` → `FUN_002f1770` directly) that a scripted tutorial holds open, so the drain
never runs. FIX: drain from `FUN_0022a770` (0x10A770), the no-arg per-field-frame tick.

**KEYWORDS: scene object position 0xB8 FUN_00265020 class nibble guard gate zero** — Reading a scene
object's world position by replicating engine getter `FUN_00265020` INCLUDING its class-nibble guard
`(*(u8)(sceneObj+3)>>5) ∈ {1,3}` returns (0,0,0) for a gate whose class byte isn't 1/3 → the object
is dropped for "no position". FIX: read the raw chain `*(sceneObj+0xB8)+0/4/8`, guard only on the
transform node != 0 (valid for all world-present categories 1–7).

**KEYWORDS: name resolver FUN_0035d380 party roster model index object** (Session 24) — Resolving a
field object's name with `FUN_0035d380(1, def+4)` always returns empty ("Object"): it is the
party/roster resolver fed a model index. FIX: read the name key at `sceneObj+0x102` → npcdic.

**KEYWORDS: pathfinder walkability Bullet world FUN_006a1a70 FUN_006a0310 ctx capture prologue never
fires no region** (Session 33) — Building route walkability on the **Bullet** raycast world
(`FUN_006a1a70`, world at `*(ctx+0x60)`) fails in the **Nalbina prologue**: it builds NO Bullet
region-world. `FUN_006a0310` is the sole world constructor and runs only inside the master physics
tick `FUN_00698c80`'s per-region loop, SKIPPED when region count==0. FOUR ctx-capture hooks (builder
`FUN_006a0310`, step `FUN_0069f070`, raycast `FUN_006a1a70`, char-ground `FUN_006a5c00`) all install
but NEVER capture; `failMask=0x40[world]`. DO NOT retry Bullet for field walkability. FIX under
Solved: the SQEX floor/wall walkmap (`FUN_003208c0` / `FUN_00230b60`), which is what the AI NPCs use.

**KEYWORDS: egocentric directions faceNode reference wrong node+0xA4 stale idle combat target-facing
DAT_02aedf94 view matrix** (Session 37) — Anchoring egocentric directions ("North"=forward) to the
character facing `faceNode` (node+0xA4) is WRONG: `FUN_00358cb0` writes it ONLY while the leader is
moving, so when stationary (exactly when you query directions) it's stale, and in combat the battle
action/steering subsystems (`FUN_00307300` 0x1c7 / `FUN_0037b4d0` / `FUN_0031adb0`) turn it to face the
TARGET → boss called "NE" while audibly to the left, and ~90°-off field directions. ALSO wrong: the
scalar camera-yaw `DAT_02aedf94` (RVA 0x29CDF94) — `FUN_003820c0` builds it from the view/sibling matrix
`DAT_02aede70` with sign-flipped rows, so its delta to the move heading wanders (the diagnostic's
`camLookDeg` never held a constant offset). FIX under Solved: read camera-forward from the MOVEMENT
matrix `DAT_02aedf30` row 2. (`bVar5 & 4`, the skip-move-facing flag, is script-only — NOT a combat lock,
so don't look for a lock flag.)

## Solved Problems

Problems that were resolved. Each entry has `KEYWORDS:` + `SOLUTION:`. Check this to
reuse known-good solutions.

**KEYWORDS: o key describe stale field no menu active help generation bump pause menu teardown
FUN_00280de0 cat 0x12 NotifyFocusChanged CurrentHelpText g_helpGen IsAnyMenuOpen unusable S67**

**`o` (describe) spoke a closed menu's description while walking in the field.** SOLUTION:
`TextCapture::CurrentHelpText()` is valid only while `g_helpTextGen == g_helpGen`; `g_helpGen` is
bumped by `NotifyFocusChanged()` on menu FOCUS but nothing bumped it on menu CLOSE, so the last row's
description stayed "current" into the field. `ingame_menu_reader.cpp` `HookedFieldPaneWnd` (hook on the
pause-menu ROOT command column `FUN_00280de0`) now calls `TextCapture::NotifyFocusChanged()` on cat
`0x12` (root teardown = whole pause menu closes) → `CurrentHelpText()` returns empty in the field.
Cannot break in-menu `o` (help is re-set for the new generation on each focus; 0x12 fires only on full
close). **NOT** gated on `MenuState::IsAnyMenuOpen()` — that reads "open" in the field/battle because
`DAT_0208ebc0` is never nulled (see Tried & Failed, Session 67).

**KEYWORDS: field party menu entry announce speaks-on-keypress SHOW message cat 0x13
FUN_00280de0 0x160DE0 ArmPaneEntry HookedFieldPaneWnd battle-menu parity FUN_00244830 S52**

**Field/party menu spoke its focused row on key-press, before the menu was visible.** SOLUTION:
release the stashed entry focus on the menu's OWN visible-open event, exactly as the battle command
menu waits for its row draw. The field command column `FUN_00280de0` (RVA `0x160DE0`, `ROW_CHAIN[0]`)
sends **`cat 0x13` = SHOW** (creates the info window, plays the open SE `FUN_00249c60(4)`, unhides the
menu resources) — that is the announce trigger. `HookedFocusSet` now `ArmPaneEntry`s (stashes) for
`IsFieldPaneOwner` panes instead of speaking, and `HookedFieldPaneWnd` speaks on `0x13`. One-to-one
mirror of `g_bcmdPending*` / `HookedBcmdDraw`. Only that class defers; all other panes speak
immediately. No fallback. Full message map + the four refuted signals: `GameArchitecture.md` →
"Field pause-menu entry announce". Commit `6fd347a`. Confirmed in play.

**KEYWORDS: NAV-DIAG per-area flood 1250 lines game-thread stall area change EnumerateFieldSignExits
EnumerateMapJumps DiagScanScriptMapjumps DiagnosticDump backtick opt-in S52**

**Every area transition stalled on a ~1,250-line `NAV-DIAG` dump written synchronously on the game
thread.** SOLUTION: it was pure diagnostic (results discarded; the exit feature enumerates on demand
with `logRaw=false`), so it moved off the automatic area-change path into `NavCommands::DiagnosticDump`
— the `` ` `` key that already does rescan + object dump. Now opt-in, in whatever area the player is
standing in. The area-change branch keeps only the `"Entering <area>"` announce + one context line.
Commit `77c2b47`.

**KEYWORDS: menu re-entry silent pane focus not spoken returning to pane dedup removed S51**

**Re-entering a menu pane was silent.** SOLUTION: the `(owner, index, text)` dedup in
`menu_reader.cpp`'s `OnFocus`. Because the pane-isolation gate returns before the cache is written,
the cache still held the row you left, so the identical focus on return was dropped as a duplicate.
Removed the gate; `g_lastOwner`/`g_lastIndex` survive (renamed `g_focusOwner`/`g_focusIndex`) purely
as the "current row" the config value-change hooks read. See the Tried & Failed entry above for the
general rule.

**KEYWORDS: duplicate RVA different names aliased global BTLWORK PARTY_MGR_PTR FIELD_STATE_BLOCK
value grep centralization S51**

**Duplicate offsets hiding under different names.** SOLUTION: grep constant VALUES, not names — a
name-based check misses `0x2D9F190` appearing as `RVA_BTLWORK`, `PARTY_MGR_PTR` and
`FIELD_STATE_BLOCK` in three files. The exact command is in `Docs/PerformanceIssues.md`
("How the duplicates were found"). Shared offsets now live in `src/core/phyre_types.h`.

**KEYWORDS: read-only input controls game speed 1 2 3 mod not modifying no injection no
SendInput no WriteProcessMemory DirectInput const buffer** (Session 44) SOLUTION: The tester's
player speed jumped mid-session and asked whether the mod alters game controls. **It does not — the
mod is strictly READ-ONLY on input and game memory, verified by audit:** (1) the DirectInput hook
`HookedGetDeviceState` (`src/proxy/dinput8_proxy.cpp`) calls the real `GetDeviceState` and passes the
buffer to the tracker as a `const unsigned char*` — it never writes the buffer; `InputTracker::
FeedDInputKeyboard` only edge-detects/reads. (2) Repo-wide there is **no** `SendInput` / `keybd_event`
/ `mouse_event` / `PostMessage(WM_KEY…)`, **no** `WriteProcessMemory`, and **no** memory-write helper —
the mod has no path to write game memory. The only `VirtualProtect` is the one-time vtable patch that
installs the read-only hook. (3) Every game function the mod calls is a pure getter (area name, ground/
segment/exit queries). **The speed change was the tester's own `1`/`2`/`3` keypress** (those are Game
Speed 1×/2×/4× — see Controls.md; the old "Lock On / Target Group" labels were wrong). The mod reserves
none of `1`/`2`/`3`.

**KEYWORDS: left ctrl escape toggle battle menu won't open menus locked stuck ctrl sound cue ping
whoosh Controls.md binding INPUT-DIAG modifiers not the mod** (Session 47) SOLUTION: The tester could
not open the battle menu and the game acted as though **Ctrl were held** on every keypress. **It is the
game's own binding: Left Ctrl = "Escape"** (`Controls.md:43`, captured from the in-game Controls menu
2026-07-07), and it **toggles** — one press emits a sound cue and locks the menus, a second press emits
another cue and unlocks them. **STRIKE the "stuck Ctrl / the game sees `Ctrl+<key>`" framing** — there
is no stuck modifier and no OS/engine fault to split. A stuck-Ctrl diagnostic was written, committed
(`7d51917`) and reverted the same session; do not re-add it. It would also have spammed the log, since
its change-detector includes Shift and **Left Shift is bound to Walk/Run**. **Lesson: grep
`Controls.md` before instrumenting any suspected input bug** — the game's bindings are captured there
so this is a lookup, not an investigation. Same family as the Session 44 entry above: the "bug" was a
game key doing its job.

**KEYWORDS: egocentric directions camera-relative camera-forward DAT_02aedf30 row2 DAT_02aedf50
DAT_02aedf58 atan2(-fwd.x,-fwd.z) ReadCameraForward North=forward calibration** (Session 37) SOLUTION:
FFXII movement is camera-relative (`FUN_004742a0` RVA 0x3542A0 rotates the stick by camera matrix
`DAT_02aedf30`: `worldMove = stickX·row0 − stickY·row2`; NO top-down branch). Anchor egocentric
directions ("North"=forward=where UP takes you) to the LIVE camera-forward from the MOVEMENT matrix
`DAT_02aedf30` row 2 (+0x20): `fwd.x=DAT_02aedf50` (RVA 0x29CDF50), `fwd.z=DAT_02aedf58` (RVA 0x29CDF58);
**up-direction yaw = `atan2(−fwd.x,−fwd.z)`** (UP ⇒ stickY>0 ⇒ worldMove = −row2). Same `atan2(x,z)`
convention as faceNode → drop-in for the existing egocentric transform (`ego = BearingDeg(target) − (180 −
facingDeg)`). Writer `FUN_003820c0` (RVA 0x2620C0), live in field gameplay. Valid idle, after a camera
rotate, AND in combat (unlike faceNode). Shipped as `PlayerState::ReadCameraForward`; all `facingRad`
sources repointed to it; `;`="Forward points <cardinal>". **Sign + handedness CONFIRMED** by the baked `'`
position-delta calibration: W leg `camFwdNeg==moveVec` (168.9°), D leg right=forward−90 ⇒ ego +90=East.

**KEYWORDS: pathfinder field object enumeration handle table DAT_02098e10 gate NPC gimmick**
SOLUTION: Enumerate the scene-object HANDLE TABLE `DAT_02098e10` (RVA 0x1F78E10) — the master
registry the game's own interaction scanner `FUN_0025b820` (0x13B820) walks, holding EVERY live
field object (NPCs + static gimmicks) from map load. 5 containers × 0x288: active `+0x10&1`; entry
array `*(base+c*0x288+0x08)`; count `*(int)entries`; object i `*(entries+0x08+i*8)`. Filter
interactive by `sceneObj+0x1C` flags (`0x400`=talk/NPC, `0x4`=action/gate-door-switch) + npcdic
band. Position `*(sceneObj+0xB8)+0/4/8`. This is the correct source for a proactive object list.

**KEYWORDS: pathfinder SQEX field walkmap ground wall FUN_003208c0 FUN_00230b60 mask 4 walk class
getgroundy prologue** (Session 33) SOLUTION: Field walkability = the game's own **SQEX floor/wall
mesh** (the AI NPCs walk it; loaded per-map, independent of Bullet, so it's live in the prologue).
Ground-at-XZ: **`FUN_003208c0`** (RVA 0x2008C0) `bool(float x, float z, float* outY)` → walkable-floor
exists + height (uses cached ctx `DAT_02ec1370`). Wall/segment: **`FUN_00230b60`** (RVA 0x110B60)
`int(ctx0, out16, from[4], to[4], u16 mask, u32 flags)` → hit index >=0 BLOCKED / <0 CLEAR. ctx0 =
`*DAT_0209a678` (RVA 0x1F7A678), gate `DAT_0209a670` (RVA 0x1F7A670). Both reentrant/read-only, safe
for thousands of calls/route. **mask is a query-CLASS enum (compared `==4` in `FUN_0022cc50`), NOT a
bitmask: pass mask=4, flags=0 (WALK class)** — exactly what the player leader's own wall feelers use
(`FUN_0032cf50`→`FUN_003d97e0(…,4)`→`FUN_00230b60(…,4,0)`). Class 4 blocks real + character-only
walls, skips camera-only planes/floors/ceilings; **0xffff/1 is the CAMERA class and is doubly wrong**
(falsely blocks camera planes → phantom-wall detours, falsely passes character-only walls). Shipped as
`src/navigation/map_query.{h,cpp}` (`MapQuery::GroundAt` / `SegmentClear` / `SegmentHit`).

**KEYWORDS: pathfinder route drain FUN_0022a770 game-thread per-frame tick turn-by-turn** SOLUTION:
Drain the game-thread route planner from `FUN_0022a770` (RVA 0x10A770), the no-arg per-field-frame
tick (walking state `DAT_02064ad3==2`, returns u64). Drain unconditionally at entry — no accumulator
or bypass can hold it silent (unlike FUN_00314020). Match the u64 return.

**KEYWORDS: scene object world position sceneObj 0xB8 transform node universal** SOLUTION: Any scene
object's world XYZ = `node = *(sceneObj+0xB8); x=node+0, y=node+4, z=node+8` (floats). Works for
NPCs and static gimmicks alike (categories 1–7). Guard only on `node != 0`; do NOT apply
FUN_00265020's class-nibble gate.

**KEYWORDS: pathfinder classify npcdic id band interaction flags NPC person object** SOLUTION:
Classify by npcdic id (`sceneObj+0x102 & 0xbfff`): 433–469 = gimmick band (434 Treasure, 466 Gate
Crystal, 469 Save Crystal, 468 Urn, 435–459/467 crystals) → sub-type; else scene-object talk flag
(`+0x1C & 0x400`) → Person; else Object. Locale-independent; the spoken label is always the game's
own text.

**KEYWORDS: non-live scanner category switch stale count objects-0 ChangeCategoryLocked rescan** SOLUTION:
Switching nav categories (`-`/`=`) spoke a STALE count (e.g. "Interactables, 0") when the target
category's actors weren't in the last scan — `ChangeCategoryLocked` (`entity_list.cpp`) counted
`g_entities` WITHOUT a fresh scan, while every other command (`[`/`]`/`\`/`` ` ``) calls
`RescanLocked()`. FIX: call `RescanLocked()` at the top of `ChangeCategoryLocked` before counting, so
the count reflects live actors. This was the last non-live path in the scanner (Session 26).

**KEYWORDS: enemies scanner BtlWork pool DAT_0208e688 def+5 foe polarity Category::Enemy** SOLUTION:
Enemies are NOT in the scene-object handle table's interactive set (no talk/action flag) — list them
from the BtlWork combatant pool `DAT_0208e688` (32×0xf50, count `DAT_0208e6a0`): per slot def=`+0x698`
(null=empty), active `+0x00 & 0x10`, ENEMY = `*(u8)(def+5)==0` (0.85 — verify via the `'` pool dump;
flip `NavRva::ENEMY_DEF_KIND` if party shows as enemy), name codec* = `+0x18`, position via
`sceneObj=+0x10`→node+0xB8. `ScanEnemiesLocked` appends them as `Category::Enemy` (Session 26, pending
in-game polarity confirmation).

**KEYWORDS: in-game menu reading field FUN_002a6190 battle FUN_0055cd40 0x8000 not-index cell-pointer** SOLUTION:
The "universal" 0x8000 focus reader was silent in-game because `FUN_00247510`/0x8000 is a shared
transport, not a shared contract. FIELD menu = window class `FUN_002a6190` (0x186190; ALL submenus);
row text = `0x03`-separated codec buffer at `window+0xF8`, focused row DATA index at `window+0x124`
(set by the game handler → read AFTER `s_origDispatch`). BATTLE command menu = window class
`FUN_0055cd40` (0x43CD40; ALL submenus); 0x8000 `val` is a POINTER to the focused 0x38-byte cell, name
codec* at `cell+0x00`, validity s16 at `cell+0x08` (-1=empty). Handled in `src/ui/ingame_menu_reader.cpp`,
delegated from `menu_reader`'s single hook (Session 26, pending in-game validation).

## Mod Architecture

Current module interaction diagram + logging format. Keep up to date as modules land.

```
                  ┌────────────────────────┐
                  │ FF12 Module Loader     │
                  │ (dinput8.dll proxy,    │
                  │  ffgriever, BSD-2)     │
                  └─────────┬──────────────┘
                            │ loads our DLL from modules/
                            ▼
┌──────────────────────────────────────────────────────────┐
│ FFXII-Screen-Reader.dll                                  │
│                                                          │
│  ┌─────────────┐                                         │
│  │ proxy/      │ → DllMain → deferred init thread        │
│  │ module_entry│                                         │
│  └─────┬───────┘                                         │
│        │                                                 │
│        ▼                                                 │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐     │
│  │ core/       │  │ speech/     │  │ input/       │     │
│  │ logger      │  │ speech      │  │ keyboard_    │     │
│  │ memory      │  │ locale      │  │ hook         │     │
│  │ config      │  │ phrasebook  │  │ hotkeys      │     │
│  │ hooks       │  └──────┬──────┘  └──────┬───────┘     │
│  │ events      │         │                │              │
│  │ phyre_types │         │ LoadLibrary    │              │
│  └─────────────┘         ▼                │              │
│                     Tolk.dll              │              │
│                  (user-supplied)          │              │
│                                                          │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐     │
│  │ ui/         │  │ navigation/ │  │ battle/      │     │
│  │ game_handler│  │ entity_list │  │ combat_log   │     │
│  │ menu_state  │  │ pathfind    │  │ event_capture│     │
│  │ dialogue    │  │ compass     │  │ battle_state │     │
│  │ menus/*     │  └─────────────┘  └──────────────┘     │
│  └─────────────┘                                         │
└──────────────────────────────────────────────────────────┘
```

## Logging Format

`FFXII-Screen-Reader-Latest.log` (next to `FFXII_TZA.exe` in `x64\`):

```
[+0000ms] [INIT     ] Mod entry; deferred init started
[+0123ms] [SPEECH   ] Tolk.dll loaded
[+0124ms] [SPEECH   ] Screen reader detected: NVDA
[+0125ms] [HOOKS    ] MinHook initialized
[+0140ms] [CONFIG   ] mod_config.ini loaded; 12 RVAs validated, 0 self-healed
[+0145ms] [SPEAK    ] FFXII screen reader loaded
```

## Known Issues

### PATHFINDER routes through impassable ELEVATION — FIX SHIPPED S68, awaiting one tester round (was S67 PROGRESS-BLOCKING)

**KEYWORDS: pathfinder elevation cliff pit ledge impassable not moving kMaxStep kEdgeSubMax passable
walkable slope limit char-controller directline route field diagnostic start here kStepDiscont step
discontinuity FUN_0033bc80 route-profile**

**UPDATE (S68): root-caused and fixed pending confirmation — see the Session 68 Tried & Failed entry at
the top of this file.** Root cause is a **step-height DISCONTINUITY** (~0.3 m, `FUN_0033bc80`), NOT a
slope (the field engine has no slope limit — `FUN_0022cc50` gates on walk-type flags only). The A*
`kMaxStep = 1.5` was ~5× too loose. Shipped a fine step-discontinuity edge test (`kStepDiscont = 0.35 m`
provisional) + the `LogRouteProfile` diagnostic that measures the descent's real step height. **Next: one
tester round on "The Stepping" (Rogue Tomato) — read `NAV-ROUTE route-profile … WORST step=` to confirm
the tomato route stops crossing the ledge and city/stairs still route; tune `kStepDiscont` once if
needed, then move this to Solved.** The historical S67 write-up below is kept for the reasoning trail.

See the full Tried & Failed writeup above (Session 67 pathfinder). Short version: `PathSearch::passable`
connects cells on a 1.5 m centre-to-centre height delta, which cannot distinguish a ramp from a
ledge/cliff, so A* routes the player onto descents/climbs they cannot traverse (character jams, does not
move). The dense elevation edge-check now in `passable()` (`kEdgeSubStep`/`kEdgeSubMax`) did NOT fix it —
the un-walkable descent passes a ~50 deg cap.

**The ONE thing to pin (>=0.99) first:** the player's REAL field walkable-slope / step-up / step-down
limits, from the WALKMAP char-controller (NOT the Bullet `FUN_006a5c00`; there is no Bullet world here —
`hasWorld=0`). Candidate movers = the `GroundAt`/`FUN_003208c0` consumers `FUN_00335180`, `FUN_00337470`,
`FUN_0033bc80` (`FUN_0033a720` is enemy AI STEERING, not it). Then set the edge test to those limits
(and decide LEDGE vs SMOOTH-STEEP: a ledge needs finer sub-sampling; a too-steep smooth slope needs a
real slope-angle cap from the poly normal or the char-controller constant).

**Open sub-question:** is the un-walkable descent a stepped LEDGE (dense floor sub-sampling with a
tighter cap catches it) or a SMOOTH slope steeper than the player can walk (needs the actual slope-limit
constant)? To decide, log the ROUTE's own per-cell Y (extend the route-field dump to print Y along the
`*` cells, or dump `rawPoly` Y-deltas) — the current directline only samples the STRAIGHT line, which
misses the route's actual descent.

**Also consider:** if the player is on a promontory with NO walkable descent to the target's level, the
correct answer is `NoPath` (approach the pit from another side), not a fabricated route. A tight-enough
edge test would produce that automatically.

**DIAGNOSTIC TOOLING LEFT IN `path_planner.cpp` (log-only, tag `NAV-ROUTE`), reuse it:**
- `directline seq=N len=.. wmBlock=I(why) firstUp=J: <marks>` — fractional-Y profile of the STRAIGHT
  player->target line. Marks: `.` clear, `^` >1.5 m step, `W` walkmap wall, `u` up-step 0.4-1.5 m,
  `X` no floor. Numbers are the floor Y.
- `==== route field center=(x,z) playerY=.. ====` + a 33x33 ASCII map — player-relative height tiers:
  `P` you, `T` target, `*` the A* raw path, `=` +/-0.5 m, `-` up 0.5-1.5, `,` down 0.5-1.5, `^` up >1.5
  (cliff up), `v` down >1.5 (cliff down/pit), `.` no floor. North-up, west-left. Fires only when the
  straight line is blocked (`firstBlock>=0`). **Evidence (seq 116):** `P` sits on a small `=`/`-` patch
  (Y~29-31); nearly the whole map is `v` (the pit, >1.5 m below); the `*` route descends off the patch
  into the `v` and crosses to `T` — a descent the tester cannot walk.

**DEFERRED THIS SESSION (resume after the pathfinder is fixed):** the menu-feature plan — Part B (items
qty), C (shop), D (gambit), E (status virtual buffer), F (dropped-items pathfinder category). Plan +
findings are in `~/.claude/plans/our-goal-this-session-federated-kay.md` and the S67 memory notes.
Already landed: Part A (`o` stale-in-field fix, DONE + deployed). Written but NOT wired/built:
`src/ui/virtual_buffer.{h,cpp}` (FF1 NavigationBuffer port, in CMake, compiles), `src/ui/status_reader.{h,cpp}`
(NOT in CMake — provisional offsets, kept out until probe-confirmed), and the arrow/Home-End input
plumbing in `input_tracker.cpp` (dormant callback). Probes authored: `probe_menu_windows.js`,
`probe_status_data.js`.

> **UPDATE (Session 71):** the reason `status_reader.{h,cpp}` was "kept out until probe-confirmed"
> was understated — it hooks the **wrong function entirely** (`0x45EDB0` = the save/load pane, see
> the Tried & Failed entry below). Its five PROVISIONAL vitals offsets turned out to be *correct*;
> the hook target, the `0x13` activation category, and the save-record read were the real defects.
> The real controller is `FUN_002c2320` (`0x1A2320`) — see `GameArchitecture.md` § "Status screen".
> `probe_status_data.js` has been rewritten against the new chain and moved to `frida\` (active).

### STRUCK: `FUN_0057edb0` / RVA `0x45EDB0` is the SAVE/LOAD pane, not the status screen (S71)

**KEYWORDS: status screen wrong hook FUN_0057edb0 0x45EDB0 save load slot pane 200 slots FUN_003bb3e0
FUN_0057fe80 FUN_00583040 status_reader CAT_SHOW 0x13 does not exist copied from FUN_00280de0
FUN_002c2320 0x1A2320 real status controller**

**What was tried:** `src/ui/status_reader.{h,cpp}` (Session 67, never compiled) hooks `0x45EDB0`,
believing it to be the status Attributes page, and activates on packet category `0x13` (SHOW) /
deactivates on `0x12`. Wiring it into CMake + `MenuReader::Init()` would have shipped a reader that
activates on the **save screen** and reads nothing on Status.

**Why it is wrong** (conf 0.99, decompile only, no probe needed):

1. `00583040_FUN_00583040.c:37-53` scans a **200-entry save-slot table** (`FUN_003bb3e0`) gated on a
   save-vs-load flag at `+0xC1`, and only then creates `FUN_0057edb0` (`:90`) — as a sibling of the
   save-slot list `FUN_0057fe80` (`:100`).
2. Its seven sub-panels read a 4-byte-per-member packed **save preview** record
   (`0057e6d0_FUN_0057e6d0.c:20,39,46` — member id, `0xFF` = hide, level clamped to 99, two nibble
   gauges, flag bits). A real stat page draws HP as a number, not a 3-segment nibble gauge.
3. `FUN_0057edb0` has **no `0x13` case at all** — it handles `1`, `0xf`, `0x12`, default. The `0x13`
   constant was copied from `FUN_00280de0` (the field pause command column), a different window proc.

**The trap to avoid repeating:** the three header labels `FUN_002f9860(0x875/0x876/0x877)` that
`FUN_0057edb0` resolves were read as "status column headers" in
`..\FFXII-Decompile\notes\status_char_select_labels_2026_07_11.md:47`. They are the **save pane's**
column headers. A label cluster on a screen is not evidence of *which* screen — follow the creation
chain (`FUN_00244f50(size, proc, args)` call sites) before believing a function's identity.

**Replaced by:** `FUN_002c2320` (RVA `0x1A2320`), the shared Status/Equipment container, gated on
`*(int*)(ctrl+0x160) == 0x4b4`. Full chain, offsets, and the game-supplied attribute label ids in
`GameArchitecture.md` § "Status screen". Confirmation probe: `frida\probe_status_data.js`.

### License-board confirm pop-ups do not read their prompt BODY text (open, S67)

**KEYWORDS: license board pop-up prompt body not read choose this license board confirm learn node
buy license Yes No BodyText PopupReader IsChoicePopup IsConfirmWindow FUN_002cdf20 FUN_00241d40
menuCtx+0x2e8**

**Symptom** (tester): the two license-board confirmation pop-ups — (1) the **"select/choose a board"**
prompt raised from the job-select ring, and (2) the **confirm-purchase** prompt when learning a license
node — **speak only the Yes/No buttons, never the prompt BODY** ("Choose this license board?" / the
learn-node question). The body text is on screen but unvocalized.

**Where it lives.** `menu_reader.cpp` `OnFocus` announces a pop-up body once on entry via
`PopupReader::BodyText(owner)` for `IsConfirmWindow` (`FUN_00241d40`) OR `IsChoicePopup`
(`FUN_002cdf20` @ `menuCtx+0x2e8`). Either these board prompts are a THIRD prompt class neither
predicate matches (so the body path never runs), or they match but `BodyText` reads the wrong
offset for this prompt and returns empty. NOT yet diagnosed — needs the prompt's `owner` obj[0] RVA
(hook `FUN_00247510` 0x8000 while the prompt is up, or the existing menu probe) to tell which case.
The buttons read because they come from the code-fixed Yes/No path, which does not need the body.

### "Interactables" is spoken as a NAME for objects the game deliberately leaves unnamed (open, S64)

**Symptom** (tester, Rabanastre North End): `Interactables. North, 10 steps (below)`. There should be
nothing *called* "Interactables" — it is the CATEGORY word being used as a label.

**What the object is.** Two entries on North End, from the `` ` `` object dump:

```
obj [0:15] kind=4 flags=00002134 nameIdx=-1 act=65535 talk=65535 door=0 named=0 pos=(27.99,0.00,68.93)
obj [0:19] kind=4 flags=00002134 nameIdx=-1 act=65535 talk=65535 door=1 named=0 pos=(38.79,0.48,72.01)
```

- `nameIdx = -1` = the custom-string form, whose text `fieldsignmes` writes to `sceneObj+0xf8`.
- `named = 0` = that string resolved **empty**, so `entity_scan.cpp` fell back to
  `CategoryWord(Category::Object)` = "Interactables".
- `act = 0xFFFF`, `talk = 0xFFFF` — **no interaction payload of either kind.**

**The game agrees there is no name.** Interacting with it shows title `???` and body
*"(You're not sure what this sign is for.)"* — a flavour sign that is deliberately anonymous.

Contrast a shop doorway, same `kind=4` and `nameIdx=-1`, whose sign string DOES resolve:
`named=1`, `door=1`, label "Migelo's Sundries". So the discriminator already exists and is recorded on
every entity: **`gameNamed`**.

**Why this is a rule violation, not a cosmetic nit.** `CLAUDE.md`: *never hardcode user-facing speech
text unless there is no game-supplied text*, *no fabricated UI labels*, *never speak filler when there
is nothing to report — be SILENT*, and *remove dead fallbacks; silence is better than wrong speech*.
A category word standing in as a proper name is all four.

**Fix (ready, not yet applied — tester asked for this to be documented first):** in
`entity_scan.cpp`, stop substituting the category word for a missing name. An object with
`gameNamed == false` and no action/talk payload carries no information the player can act on — its own
game text is "you're not sure what this is" — so it should not be listed. The category word stays where
it belongs: announcing the CATEGORY on `=` / `-`, never an individual entry.

Watch when applying: `gameNamed` is also what the sign-twin dedup keys on, and the unnamed-object dump
in `entity_diag.cpp` must keep reporting these so a genuinely useful unnamed object can still be found
in the log.



### Session 40 fixes (SHIPPED, pending runtime confirmation) — supersede the S39 pending items
Three tester regressions, root-caused (Bug 3 from the live log; Bugs 1-2 from two agreeing decompile traces):
- **`p` routed once then "No target" (FIXED).** The `[TARGET] GetLockedTarget` log showed `tickMs` FROZEN
  between target changes → the target nameplate render (`FUN_002bfd20`) is event-driven, not per-frame, so
  any cache-age window ages out. Fix: `GetLockedTarget` now gates on the LIVE `DAT_0209be80` state
  (`gate=PtrAt(P,0x10F78)`, `liveHandle=*(u32)(P+0x9FD8)`) at press time (input-thread, SEH-guarded) and
  re-resolves a fresh pos from the cached `bc`. Removed the age window. Confirm: press `p` repeatedly on a
  held target → keeps routing.
- **Exits still 0 (FIXED).** `EnumerateExits` missed a dereference: use slot 0, `mapData=*(u64*)(container
  Base+0)`, `tag=*(u16)(mapData)>2`, `tableOff=*(u32)(mapData+0x54)`, `exitBase=mapData+tableOff+reloc`.
  Prior code used `containerBase` directly → garbage → 0. Auto per-area `[NAV-DIAG] exit-table slot0` dump
  now fires on area load (no `'` needed). Confirm: dump shows `mapData≠0 tag>2` + sane count.
- **Only enemies navigable (FIXED).** Scanner now also lists **named** objects (gates/doors/field-signs/
  NPCs, categories 1-4) via `ResolveObjectName`, not just TALK/ACTION/gimmick. Confirm: `rescan: N` rises;
  `]` cycles named objects.

### Session 39 fixes (SHIPPED, pending runtime confirmation)
Two Session-38 runtime bugs, root-caused from the live log + already-documented RE:
- **`p` "No target" even with a target selected (FIXED).** The target reader announced the target
  (`[TARGET] "Imperial Swordsman..."`) but the `p`-cache write is gated on `havePos`, and
  `havePos = ReadSceneObjectPos(target sceneObj)` returned false — the battle target's `sceneObj+0xB8`
  transform node is null during attack-menu selection (works for combatants in real-time combat, so
  state-dependent). Fix: `NameForBtlChr` falls back to `actor+0xE0/E4/E8` (documented field-actor pos,
  `GameArchitecture.md:728`) when the scene node fails/reads (0,0,0); freshness 300→1000 ms; baked
  `[TARGET]` log records both sources + cache age. Confirm: `p` in a battle → `src=1/2 havePos=1` and a
  route; if both sources are 0 during selection, escalate to gating on live `DAT_0209be80+0x10F78`.
- **No routable fortress objects → wired the never-populated `Category::Exit`.** Documented follow-up
  (`plan.md` marked exits `[x]` prematurely; nothing populated it). Exits = walk-into map-jump zones,
  invisible to the `FLAG_TALK|FLAG_ACTION|gimmick` scanner filter; read the per-map exit array behind
  `getmapjumpposbyindex` (`MapQuery::EnumerateExits`, offsets ~0.85) behind a strict sanity gate;
  surfaced as fixed-position `Category::Exit`. Confirm via the `'` exit-table dump in a fortress
  corridor (sane `count`+positions → canonicalize offsets; else the dump shows which term is off).

### `p` = route to locked battle target (SHIPPED Session 38, pending runtime confirmation)
`p` (VK_P / DIK_P 0x19) routes to the game's locked/selected battle target via the same
`PathPlanner::Request` pipe as `\`, sourcing the target from `battle_target_reader`'s live cache
(`DAT_0209be80` → `+0x9FD8`, gate `+0x10F78`, refreshed every flagged-target render; `GetLockedTarget`
returns it if <300 ms old). Built + deployed, UNCOMMITTED. Two open confirmations, both answered by the
baked `NAV-ROUTE` log (no guess):
1. **PRE-SHIP CHECK** — press `p` under **Lock-On (`2`) with NO command menu**. Routes ⇒ Lock-On drives
   `DAT_0209be80+0x9FD8` (persistent target, ≥0.98); "No target" ⇒ `p` only works during command
   target-selection. Record the verdict in `GameArchitecture.md`.
2. **Nav-safe in battle** — the drain line proves whether the field stays nav-safe in battle-state mode.
   Expected yes (battle is on-field; `FIELD_ACTIVE2 0x1F69300` is the "field/battle-active" flag). If a
   persistent "*not nav-safe*" appears, relax the single battle-cleared gate bit in `IsFieldNavSafe` — do
   NOT loosen the gate pre-emptively without that log evidence.
Also Session 38: `PlanRoute` target-cell walkability snap (off-mesh goal → nearest walkable cell) and
`entity_list` cursor lock-by-identity (`CursorId`) — both pending the same runtime pass.

### Turn-by-turn routing polish (FIX SHIPPED Sessions 34+37, pending runtime confirmation)
Routing WORKS (SQEX walkmap, `plan=Route`) but had three quality problems reported by the
user. Detail + hypotheses in `sessions_001_current.md` Session 33. **Resolution:** #1 (directions point
away) was the world-cardinal-vs-camera-relative mismatch — FIXED by Session 37's camera-forward egocentric
model (`ReadCameraForward`); #2 (distance cap) ELIMINATED and #3 (through-walls) FIXED by Session 34.

**Session 34 rebuilt the planner on a whole-map walkability OVERLAY read directly from the SQEX
walkmap grid** (`nav_grid.{h,cpp}` + `map_query` grid reads; the walkmap is a uniform ≤32767-cell
grid — `FUN_0022ffe0`/`FUN_00233050`/`FUN_00231890`, see GameArchitecture "Walkmap grid").
- **#2 (40 m cap) ELIMINATED** — no `kMaxRange`; A* searches the whole map at native resolution,
  per-cell floor sampling is now a zero-raycast overlay read.
- **#3 (through walls) FIXED** — `MapQuery::SegmentTraversable` dense validator (1 m samples: floor-
  continuity + `|ΔfloorY|≤kMaxStep` + per-substep `SegmentClear` mask=4) replaces the single long
  ray in the string-pull, so a detour is never straightened through a wall.
- **#1 (points away) — directions kept WORLD-ABSOLUTE per user directive** (no egocentric/facing
  mode; camera only moves the view). Chiefly a symptom of #3; `NAV-ROUTE` now logs yaw°/target/raw+
  smoothed polyline/spoken text to catch any residual first-waypoint geometry bug.
- **Confirmation = baked C++ self-diagnostic** (user's choice, no Frida): the direct grid read is
  0.92 offline; the `'` key logs the grid header + cross-checks `ReadCellFloor` vs the confirmed
  `GroundAt` oracle (`grid xcheck walkAgree %`). Ships behind that; `NavGrid::SetDirectRead(false)`
  falls back to a GroundAt bake if agreement is low. **Documented straight-to-C++ exception** (no new
  RE crash surface — tuning already-confirmed primitives + a struct read confirmed live in-C++).
- **Move to Solved once the tester confirms** `walkAgree ≥~90%` + `\` routes past 40 m + a detour
  around the reported wall. The three items below are the original open reports (kept for context).

1. **Directions point away from the objective (intermittent).** Holding a route direction
   moves the player *further*; re-route reports a *growing* distance ("East 15"→"17"→"20").
   User: not a constant axis flip ("works sometimes"). Prime hypothesis: **world-cardinal
   directions vs. camera-relative movement** — the route speaks WORLD cardinals (north=-Z,
   east=+X) but the stick is camera-relative, so "east"=="right" only at some camera angles
   → likely needs an EGOCENTRIC route mode (off `PlayerState::ReadPlayerYaw`) or a facing cue.
   Also rule out a geometric first-waypoint bug (see #3). Next: log raw + smoothed polyline +
   target + yaw.
2. **Distance cap too small.** `path_planner.cpp kMaxRange=40m` → targets past ~53 steps get
   "Too far to route". User wants routing to ANY map entity regardless of distance. Raise/
   remove `kMaxRange` AND raise A* budgets (`kMaxRays=2000`/`kMaxExpand=500` — a 27×15 route
   already used 1266 rays/157 expands; long routes will blow them → need bigger caps or a
   coarser long-range pass). Verify `entity_list` enumerates distant objects (no distance
   filter, but confirm the handle table / actor pool holds far map objects).
3. **Routes cut through walls/rooms.** A route said "East 15, South 17" (a single straight
   diagonal) to a target unreachable by going east/south (walls between). Walkability is
   UNDER-detecting walls, or the LOS string-pull straightened a valid detour into an
   impassable line (opposite of the old 0xffff over-block). Next: log the mask=4 seg-test
   result along the reported line; add the deferred smoothed-segment validation (denser
   sample + floor-continuity + `|ΔfloorY|≤kMaxStep`); check `GroundAt` isn't returning floor
   across gaps/room boundaries. Related to #1.

### Enter key intermittently drops mid-game (diagnosed — Session 33; upstream, NOT our mod)
The `GetDeviceState` diagnostic captured it: `INPUT-DIAG keyboard GetDeviceState FAILING
hr=0x8007001E` = **DIERR_INPUTLOST** → the keyboard DirectInput device went UNACQUIRED (game
sees no keys). This is the game's own acquisition / a focus loss, **not** our read-only hook
(and the redundant `WH_KEYBOARD_LL` hook is now retired once DInput latches). Optional future
mitigation: a mod-side re-`Acquire()` nudge when we detect the failure — defer unless it
becomes a real blocker (it touches the game's device).

## Session Log

Index of session log files (split into `sessions_*.md` every 50 sessions).

- `sessions_001_current.md` — sessions 1–N (current)

**KEYWORDS: FUN_003112f0 call volume 1275 vs 20 real damage applier not one call per hit tick
actionId 0xFFFF Tier 2 source filter per-frame hook risk FUN_00310db0 FUN_00233f70** (Session 49,
tester-reported) — **`FUN_003112f0` fires FAR more often than there are damage events.** A live
messages run counted **1275+ calls against only ~10-20 actual damage instances**. Do NOT report a
raw count of this function as "damage events" — an earlier note in this session did exactly that
and the conclusion drawn from it was wrong.

This matters because `FUN_003112f0` is the designated **Tier-2 source** for the mod's own attack
lines (`combat_system.md` §9.1.3b) — the game composes no message for a basic attack, so the log
must synthesise one from this call. If the function fires ~100x per real hit then:
1. the filter is load-bearing, not cosmetic — see the **F2** correction (filter `actionId ==
   0xFFFF` for ticks, NOT `attacker == 0`, because `FUN_00310db0` makes two real calls with a null
   attacker); and
2. hooking it may approach the **no-per-frame-hooks** rule and needs a deliberate decision.

Suspected driver: `FUN_00310db0(actor)` does per-actor periodic work and is one of the three tick
callers; its own caller `FUN_00233f70` sits in the battle-update band. **Not yet proven** —
`probe_combat_damage.js` now buckets every call (real hit / tick / zero-delta / invalid / null
attacker) and prints the ratio, which answers it directly. Settle before building Phase 5.

**KEYWORDS: FUN_003112f0 is PER-FRAME FUN_00310db0 FUN_00233f70 callback +0x38 frame loop
FUN_00265a20 FUN_0026ce60 status tick actionId 0xFFFF attacker 0 Tier 2 source per-frame hook rule
combat log damage line** (Session 49, tester-caught) — **`FUN_003112f0` is called PER ACTOR PER
FRAME, not once per damage event.** `combat_system.md` §9.1.3b presents it as "one call = one
fully-resolved outcome", which is true of its *content* but badly wrong about its *rate*.

Chain, confirmed: the frame loop (`FUN_0026ce60` → `FUN_00265a20`) registers **`FUN_00233f70` as a
per-object update callback** (`*(code **)(obj + 0x38) = FUN_00233f70`). That callback runs a
battery of per-actor updates including **`FUN_00310db0`**, which does:

```c
memset(local_1a8, 0, 0x148);
FUN_00385df0(bc, local_1a8);
FUN_003112f0(local_1a8, 0, bc, 0xffff);      // attacker = 0, actionId = 0xFFFF
```

Live evidence: a session with only ~10-20 real hits logged **1275+** applier calls, essentially all
with a **null attacker**, climbing steadily with playtime (~20/sec). Confidence 0.97.

**Consequences:**
1. The tick filter (`actionId == 0xFFFF`, per the **F2** correction — NOT `attacker == 0`, because
   the loop just below makes real calls with a null attacker) is **load-bearing**, not cosmetic.
2. Hooking `FUN_003112f0` means a detour on the game's per-frame path, which engages the
   **no-per-frame-hooks** rule. It is not on §5.7's never-hook list, but it belongs in that
   conversation. Cost per call is small (trampoline + 2 reads + compare) and there is precedent —
   `battle_target_reader.cpp` already hooks the per-render `FUN_002bfd20` with a cheap early-out —
   but this must be a **deliberate, recorded decision**, not an assumption.
3. Any claim that "N applier calls" means "N damage events" is wrong. Do not repeat it.

**KEYWORDS: never ask the user to read the screen blind tester visual confirmation invalid probe
design reaction words sprites result+0x04 backtrace processing code FUN_00384e50 equipment mask
FUN_00389370 gate FUN_003896b0 roll ladder parry block evade** (Session 49) — **a probe step that
required the tester to read an on-screen word was authored, and it is invalid by construction: the
tester is blind.** Probe P3 and the reaction-word instructions in `probe_combat_damage.js` both did
this and are struck.

**The correct method, and the one to reach for by default: backtrace the processing code to the
condition that produced the value** (the approach used on DQ7R). Applied here it resolved the
question completely and offline:

- `FUN_00384e50(bc)` → equipment mask: bit0 = main-hand (`bc+0x50 != 0x1000`), bit1 = off-hand
  (`bc+0x52 != 0x1000`), bit2 = animation-set hash `0x2902032b`.
- `FUN_00389370:73-81` zeroes each defensive rate unless its gate bit is set — `DAT_02aedff0` needs
  off-hand, `DAT_02aedff4` needs main-hand, `DAT_02aedfec` needs the animation set.
- `FUN_003896b0:38-70` rolls them in order and writes the result into **`result+0x04`**.

⇒ **`result+0x04` names the mechanic** (1/2 parry · 3/4 block · 5 evade · 6 no-effect ·
7 nullified · 8 reflected · 10 absorbed · 0xB avoided), identified by which equipment slot gated
the roll. Confidence 0.98. **`FUN_00328480` is dropped from the design** — one hook, not two.

**Rule going forward: if a value has no text behind it, do NOT ask for a visual reading — trace the
code path that set it.**

## License Board / Job system — Tried & Failed (Session 53, 2026-07-22)

- **`cell+0x10` is NOT a node description — it is the CATEGORY word.** Shipped `o` reading it as
  the description; it spoke "Weapon"/"Magick". The cat‑0x19 record holds only NAME (`+0x18`) and
  CATEGORY (`+0x10`); there is no long per-node description anywhere. What the panel actually shows
  is the **granted-entry list** (`FUN_00559e30`). Confirmed by probe log + screenshots.
- **Do not include a granted entry's description whenever it decodes.** The game draws it **only**
  for kind‑1 entries with `(ushort)(id-0x9e) < 0x18` (the technick block). Telekinesis shows one;
  Cure/Blindna and gear entries do not. A looser rule leaks text a sighted player never sees.
- **Blank board tiles are NOT "where the second job's board goes."** Hypothesis raised and
  disproved: `FUN_003242f0` toggles the *viewed* board (`record+0x1c5`) between job1/job2 — the two
  jobs are **separate boards**, and `board+0x558` builds from exactly one of them. Blanks are that
  job's own locked/unreachable nodes (zeroed to `id=0xFFFF` by `FUN_0055bff0`).
- **Reading the confirm prompt's body at button-focus time returns nothing.** `FUN_002cdf20` stores
  no text; it hands the composed string to a `FUN_0057c480` surface, readable **only at that
  surface's case‑1 birth**. Reading `prompt+0xc0 → +0x1B0` later (from the 0x8000 focus) yielded
  empty every time. Fix: capture at birth in `message_reader` and consume via `TakeConfirmPrompt()`.
  Also do **not** just flip `kSpeakSurfaceConfirms` — speaking at birth gets cut off by the Yes/No
  focus that fires milliseconds later; route it through `OnFocus`'s `preambleSpoken` ordering.
- **`GameText::IsMostlyPrintable` silently drops the `"?"` placeholder.** It requires `alpha >= 1`
  (at least one A–Z/a–z). The `F` summary pages put the game's own `"?"` (`FUN_002f9860(0x4C7)`)
  in unlearned slots, so **every** unlearned row decoded to empty and went silent — the entire
  Magicks page before any magick is learned. The RVAs/offsets were correct the whole time; the
  gate was the bug. Detect the placeholder explicitly. (Speak it as a word — a literal `?` is
  commonly dropped by TTS punctuation settings, which re-silences it.)
- **The deferred `FUN_00285a10` status-chooser hook can never serve the License char-select.** That
  hook targets the Status/Equip chooser (`FUN_00285290` @ `menuCtx+0xf8`); licenses use a dedicated
  controller `FUN_00560910` @ `menuCtx+0x158`.
- **At char-select SHOW, `ctrl+0xd0` is stale.** `FUN_00560ee0` fills it later; the global
  `menuCtx+0xde0` is written first (`FUN_00285f20`), so read that for a reliable entry announce.

## Inventory quantity / category — Tried & Failed (Session 70, 2026-07-24)

**KEYWORDS: inventory items quantity row+0x0E row+0x0C equipped-count STRUCK category tab
FUN_005655f0 onEnter onLeave one-item-list silent-on-entry probe dedup-key-collision**

- **`row+0x0C` is NOT the equipped count — STRUCK by tester ground truth.** The offline read
  (`FUN_0057dad0:26-32` fills it from `master+0x20`; `FUN_00563560:54-59` draws `+0x0E − +0x0C`
  alongside `+0x0E`) made "how many are equipped" look obvious. It is wrong: the probe logged
  `Dagger qty=1 equipped=0` while **that Dagger was equipped to Vaan**. Every equipment row observed
  had `+0x0E==1, +0x0C==0`, so no candidate meaning can be discriminated. **Not read, not spoken.**
  Resolving it needs a save owning ≥2 of one equipment item with some equipped and some spare.
  *Lesson: a field label inferred from a draw routine is a HYPOTHESIS. The tester's game state is
  the oracle — one contradicting fact beats a clean-looking decompile reading.*

- **A cursor-move-only reader is SILENT when you ENTER a list.** Hooking only `FUN_00247510` 0x8000
  means nothing speaks on entry, because nothing moved. Invisible on long lists (you arrow
  immediately) but total on a **one-item list** — the tester hit it on an Items screen holding only
  Potion. Fixed by announcing from `FUN_005655f0` (0x4455F0), which fires on screen OPEN as well as
  on category change. Do not assume a focus signal implies an entry signal.

- **Hooking `FUN_005655f0` on LEAVE inverts the speech order.** Its own
  `FUN_002d47c0:15-20` re-fires `FUN_00247510(child, 0x8000, idx)` *during* the call, so on exit the
  item has already been announced and the category lands after it. Hook on **ENTRY** —
  `FUN_00564010:56-70` populates the tab table and `+0x180` before the call, so every field the name
  needs is readable there. (The probe log shows `[row]` before `[cat]` on every event: that was the
  onLeave artifact, not game behaviour.)

- **Two `Speech::Output(…, interrupt=true)` in one frame collapse to one utterance.** Announcing the
  category and then letting the item announce normally would cut the category off mid-word. The item
  is QUEUED behind it (`interrupt=false`) for exactly one announcement. **This is not a dedup** — it
  suppresses nothing and both lines are spoken; it only orders them.

- **Probe artifact, not a game behaviour: the `[cat] ITEMS` line never appeared.** `probe_inventory.js`
  keyed its console dedup on `class:tabIdx:tabCount:empty`, and **ITEMS and LOOT share window class
  `+0x443930`**, both at tab 0 of 1 — so the second collided and was suppressed. `[tabs]` (keyed on
  the table pointer) still printed it. Similarly **KEY ITEMS *was* captured** (`+0x443D20`, line 7 of
  the log) despite appearing absent. *When a probe "misses" something, check the dedup key before
  concluding the game did not fire.*

- **Empty categories never occur** — the `gateId` at tab-table `entry+6` filters them out before they
  are tabbed (every `[cat]` reported n≥1; tab counts vary by screen state). A "speak the category
  alone when the list is empty" branch was designed and then dropped as dead code. Do not add it.

## Ground loot + rewards — Solved / corrected (Session 72, 2026-07-24)

**KEYWORDS: ground loot drop pool DAT_02ec0fa0 second pool not handle table marker DAT_022be7f0
FUN_00272cb0 not item resolver scene-object handle FUN_003588b0 FUN_00263990 7 slots not 4
FUN_0028fb80 rejected 0.85 dropped register args FUN_00312280 defeat line moved msg 0x0D realtime**

- **SOLVED: "items dropped by enemies are not on the pathfinder."** Root cause is not a filter bug —
  ground loot is **a completely separate object pool**. The scanner walks the scene-object handle
  table `DAT_02098e10`; drops live in `DAT_02ec0fa0` (RVA `0x2DA0FA0`, 10 slots, stride `0x60`) with
  positions in a parallel marker table `DAT_022be7f0` (RVA `0x219E7F0`). No amount of classification
  work on the handle table could ever have found them. *Lesson: when a whole class of object is
  missing rather than mislabelled, look for a second backing store before touching the classifier.*

- **STRUCK: `FUN_00272cb0` is an "item name codec".** It is
  `handle → FUN_003588b0 → FUN_00263990`; `FUN_003588b0` decodes its argument as a pooled record
  handle (pool selector / index / generation vs `rec+0x16`) and `FUN_00263990` reads `rec+0x102` /
  `rec+0xf8` with the `0xffffbfff` npcdic mask — the **scene-object** name chain. Ghidra dropped the
  register-passthrough arg, so its input semantics are NOT established offline, even though the call
  is play-confirmed in the battle item sublist. It is reused as a **game call from the game thread
  only** (`core/item_names.cpp`), never reimplemented on a guess. *Lesson: a comment saying
  "CONFIRMED working" confirms the OUTPUT, never your model of the arguments.*

- **STRUCK: the loot payload is "4 slots".** It is **7** — 5 normal + 2 rare. Both `FUN_003180f0`
  and `FUN_00319920` loop seven times. `combat_system.md:1007` was wrong;
  `notes/combat_re_2026_07_20_battle_state.md:228` was right.

- **REJECTED as the EXP/LP source: `FUN_0028fb80(actorId, exp, lp)`**, the on-screen popup. Its args
  *are* the drawn numbers, which made it look like the obvious hook — but Ghidra renders the call
  site with what look like the gil accumulator in the value slots (dropped register args), leaving
  the argument identity at ~0.85, below the bar. Used the before/after diff of `BtlChr+0x18C`/`+0x190`
  instead, which needs no inference at all. *Lesson: when a cheap source depends on an argument
  identity Ghidra could not recover, prefer the source that observes state directly.*

- **The enemy-defeated line was being emitted from the wrong event.** It came from `CheckVitals`,
  i.e. off a damage *calculation* one step before the HP write — which is why it could not be joined
  to the rewards, and why it was announcing a death the game had not committed yet. Moved to
  `FUN_00312280` (`0x1F2280`), the real per-death event.

- **OPEN, watch on first playtest (0.95):** that `FUN_00312280` is reached for *every* enemy death
  rests on a static "sole caller" xref (`FUN_0030e360:204` case 0), not observation. Failure mode is
  a kill that announces nothing at all — check a poison/doom death specifically.

## Clan / Hunt surfaces — SILENT, reported Session 72, NOT diagnosed

**KEYWORDS: hunt mark bill notice board clan primer Which bill would you like to read Mark Rank
Status Thextera Available Done multi-item reward panel Red & Rotten in the Desert gil Potion x2
Teleport Stone titled reward list quest reward silent**

Two surfaces the tester reported as reading **nothing at all**. Both belong to the Clan/Hunt system,
which the mod has never touched. **Reported only — no RE done, no function identified. Do not
implement from a guess; find the surface first.**

### 1. The multi-item reward panel

A bordered panel with a **title line** (the bill/quest name, e.g. `Red & Rotten in the Desert`), a
rule under it, then one row per reward:

```
Red & Rotten in the Desert
  [icon] 300 gil
  [icon] Potion          x  2
  [icon] Teleport Stone  x  1
```

Observations that matter:
- It is **not** the single-item obtained toast the mod already reads (`message_reader.cpp`,
  `FUN_0035e070`, text at `widget+0xC8`) — that one is a one-line toast with no title and no
  quantity column. This is a multi-row list with a heading.
- It is **not** battle messages `0x24`/`0x25`/`0x26` either: those are one obtain per message and
  are already spoken in realtime. Nothing about this panel came through.
- Each row has an **icon, a name, and a separate `x N` quantity column** — the quantity is its own
  field, exactly like the inventory rows (`row+0x0E`, Session 70), not part of the name string.
- Gil has no quantity column; it is formatted into the name ("300 gil").

**First checks next session** (in this order, per CHECK-GameArchitecture-FIRST):
1. Grep `GameArchitecture.md` and `debug.md` for an existing quest/reward/clan read-point before
   anything else.
2. Does the universal focus signal `FUN_00247510` (msg `0x8000`) reach this panel at all? It has no
   cursor, so probably not — which would make it a **draw/open** surface, like the shop
   (`FUN_0056e5d0`) rather than a focus surface.
3. If it is a row list, it is far more likely to share the row-chain / list-widget shape the
   inventory and shop readers already parse than to need anything new. Look for the row array and a
   count before inventing a reader.

### 2. The hunt notice board — "Which bill would you like to read?"

A **cursored** list with a prompt line and **column headers**:

```
Which bill would you like to read?
   Mark        Rank   Status
-> Thextera      I    Available
   Done
```

Observations that matter:
- **This one HAS a cursor** (the pointing-hand glyph moves between rows), so unlike the reward panel
  it should be reachable by a focus signal — and it still says nothing. That gap is the interesting
  part: **check whether `FUN_00247510` 0x8000 fires here before assuming a new hook is needed.**
- A row is **three columns** — Mark name, Rank (roman numeral), Status. A useful readout must join
  all three; speaking only the focused cell would be useless. The headers are separate text from the
  row content.
- `Status` is a state word (`Available` here) drawn in its own colour — treat it as game text to
  read, never as a word to hardcode.
- **`Done` is a row in the same list**, not a separate button, so the reader must not assume every
  row has three columns.
- The prompt line ("Which bill would you like to read?") is game text and should be the entry
  announce, the same way other panes announce on entry.

**Do not assume these two share a controller.** One has a cursor and one does not; that is exactly
the kind of surface-shape assumption that cost Session 71 a wrong hook (`0x45EDB0` turned out to be
the save/load pane because a label cluster looked right). Follow the creation chain for each.

---

## Navigation elevation-blindness — Solved / corrected (Session 73, 2026-07-24)

**Symptom.** Story-blocking. In the Rabanastre Clan Hall the mod announced *"Montblanc. right next
to you"* and routed *"Northeast 1. 1 steps"*, but pressing Confirm always talked to a Clan Member.
Montblanc was **6.92 world units directly overhead**; Clan Member 4 was 1.86 units away at the
player's own height.

### Root cause — `f(x, z) -> y` everywhere

| Layer | Evidence |
|---|---|
| `MapQuery::GroundAt` | a **game** fn taking `(x, z)` only — cannot be asked for the floor nearest *my* height |
| `ScanTopFloorAt` (`map_query.cpp:190`) | `if (!found \|\| y > bestY)` — visited **every** floor poly, kept only the **highest** |
| `NavGrid` (`nav_grid.h:20`) | 2D grid, one `floorY` per cell → an overhead target lands in the player's own cell (`nearDist=0.0m`) |
| `nav_common.cpp` | the reach short-circuit returned a bare `L"right next to you"` **above** the `ElevationSuffix` line |

The mod had already measured the gap (`maxStep=6.93m`, `wmBlock=1(step>max)`) and gated nothing on
it — `path_planner.cpp:122` marks that dump *"diagnostic, log-only"*.

### Fixed

`MapQuery::AllFloorsAt` (keeps what `ScanTopFloorAt` discarded, merge `kLayerMerge = 0.35`);
`ScanTopFloorAt` demoted to a wrapper that tracks its top independently of the layer array, so it is
byte-identical; `NavCommon::ReachPhrase` re-attaches elevation to the reach phrase; the `'` dump now
prints `layers=[…] top=… nearest=… dY=…` per column, where **`top != nearest` is the bug, printed**.

### TRIED & FAILED / STRUCK

- **STRUCK: "the mod's `near=`/reach distance corroborates 3D adjacency."** It does not — every
  distance in `nav_common` is `Distance2D`, X/Z only. `"right next to you"` was never evidence about
  elevation; it is *structurally incapable* of carrying it. Do not read it as a 3D claim again.
- **STRUCK: "the 15:16:32 Clan Hall telop was Montblanc."** Asserted at 0.9 — below the bar, and
  wrong. The tester confirmed the interaction icon read *"clan member"*. Never state a sub-0.98
  identification as fact; it also *confirmed* the diagnosis (the engine never selected Montblanc).
- **REVERSED mid-session: "the route key should go silent when there is no walkable route."**
  Tester requirement is turn-by-turn to **every** destination, nothing silent, and the pathfinder
  must route **across levels**. If `No path` gets *more* common after the layered grid, that is a
  regression, not a truer answer.
- **Frida-first waived for pathfinding** (tester: "much easier to diagnose in C++"). A written
  `probe_walkmap_layers.js` was retired unrun to `frida/archive/`. Diagnostics for nav go in C++ and
  come back through the mod log.
- **Do NOT design a "cycle interaction target" key.** `FUN_0025b820` keeps exactly one winner
  (`DAT_0209a2b8`), reset per frame by `FUN_0025d650`. The engine has no such concept — 0.98.
- **Do not call `MapQuery::GroundAt` from the diagnostic.** It is game-thread-only (`nav_grid.h`);
  the `'` dump runs off the input thread. `top=` from `AllFloorsAt` is the memory-only equivalent.

### Still open

Built and deployed, **not play-confirmed**. The layered grid (`col,row` → `col,row,layer`) is
designed, not built. Unproven at 0.90: that scene-object Y and player Y share a reference frame —
the new `player floor:` line settles it.

### Containment fix VALIDATED in play (Session 73, second round)

`'` dump, Clan Hall, after adding `PointInPolyXZ` + strict `B > 0.001`:

| | before | after |
|---|---|---|
| player column | `layers=16 raw=103 [-1751.74 … 271.02]` | `layers=1 raw=1 [0.00]` |
| Montblanc | `layers=16 raw=112 … nearest=6.74 dY=-0.19` | `layers=1 raw=1 [6.93] dY=+0.01` |
| STACKED columns | 32 | **0** |

Every named NPC now lands on a real floor: Montblanc 6.92→[6.93] (**dY=+0.01**), Krjn 6.00→[6.00],
Clan Members 0.00→[0.00] and 6.00→[6.00], and an object at 0.75→[0.75] (an intermediate value, so it
is not merely snapping to 0/6). **The `(i+1)%3` next-vertex assumption for `DAT_00908de8` is
validated** — a wrong winding would have produced `layers=0` everywhere, not exact matches.

**SETTLED at 0.99: scene-object Y and player Y share a reference frame.** Montblanc is genuinely
6.93 units above the player. The 0.90 caveat carried since the start of the session is closed.

### STRUCK: the layered-grid design (`col,row` -> `col,row,layer`)

**Not justified by the data.** Post-fix histogram over the whole dump: `layers=1` x28, `layers=0` x16,
**`layers>=2` x0**. The walkmap here is a single-valued height field with a big step, NOT overlapping
levels. The garbage that looked like stacking was plane extrapolation, and it is gone. Do not build
a layer dimension without a map that actually shows one.

### The REAL cause of "Montblanc. 1 steps"

`NAV-ROUTE ... tgtCell=(24,36) expands=0 touched=1 nearest=(24,36) nearDist=0.0m`.

**`expands=0` means A* never expanded a node** — the goal cell WAS the start cell. Player
(37.00,54.60) and Montblanc (37.04,55.10) are 0.50 apart horizontally and `kFineCell = 1.5`, so both
land in ONE fine cell. The search short-circuits as "already there", **so no edge is ever tested and
`kStepDiscont` never runs.** The 6.93 m step is invisible because nothing ever looks at an edge.

Fix is therefore NOT a layer dimension but a **goal-surface check**: compare the target's own Y with
the floor at the target's XZ (now readable correctly), and if it differs from the player's surface by
more than a step, the target is not "1 step away" — route to an approach cell instead. `kFineCell`
collapsing a 0.5 m separation is the proximate trigger; the surface check is resolution-independent
and is the durable fix.

### Approach-cell routing — BUILT (Session 73, not yet play-confirmed)

**Symptom.** Routing to Montblanc gave "1 steps" from the ground floor and failed outright from the
upper floor, where Krjn (y=6.00) is reachable but Montblanc (y=6.93) is not.

**Cause.** `path_search.cpp` snapped the goal **only** `if (!NavGrid::WalkableAt(tc, tr, ty))`.
Montblanc's dais IS walkable (floor 6.93), so no snap ran and A* was asked to **stand on the dais** —
a 6.93 step from the ground floor, 0.93 from the upper one, both past `kStepDiscont` (0.35). From the
ground floor the 1.5 m fine cell additionally collapsed player and target into one cell
(`expands=0`), which is where "1 steps" came from.

**Fix — the goal is now a SET, and the test is the engine's own predicate.** A goal cell is any
walkable cell within `kApproachCells` (3) / `kApproachRadius` (4 m) of the target whose floor lies
inside the target's **interaction band**. For Montblanc that admits both the 6.93 dais and the 6.00
walkway; A* never reaches the dais, so it finishes on the walkway with no special-casing. Multi-goal
A*: `isGoal()` membership test, `heur()` = min distance over goals (still admissible), and on
termination `tc/tr` adopt the reached cell so reconstruction, stats and the bridge/near-goal
fallbacks are unchanged.

**Non-regression, by construction:**
- Ground-level NPC → its own cell is in band at distance ~0 → primary == original cell → identical
  route, and the true target position is still appended to the polyline.
- Transition/exit → band deliberately not read (its destination IS the surface you walk onto).
- `p` (battle target) → default inverted band → old single-cell path.
- No band readable / nothing admissible → falls through to the original `SnapToWalkable`.
- `snapped` is forced true when the route ends on a cell other than the target's own, so the
  unreachable last leg onto the dais can never be re-appended.

**Plumbing:** `InteractTarget::ReadBandFor` → `EntityList::GetCurrentTarget(..., outSceneObj)` →
`PathPlanner::Request(..., bandLo, bandHi)` → `PathSearch::Run(..., bandLo, bandHi, ...)`.

**Log lines to check:** `request: interaction band=[lo,hi]`, `goal-set: N cell(s) in band ...`, and
`goal-set: reached alternate approach cell (c,r)`.

### OPEN FOR NEXT SESSION — the approach cell is in-band but out of REACH

**Symptom (tester, end of Session 73):** routing lands you *near* the target but not close enough to
interact, so you still have to wiggle on crow-flies directions.

**Measured:**
```
goal-set: 12 cell(s) in band [44.04,47.88], primary (356,141)->(356,141) d=0.9m
goal-set: reached alternate approach cell (355,142)     ... nearDist=2.1m
```
A 0.9 m cell existed; the search stopped at a 2.1 m one.

**Two defects, one fixed, one OPEN.**

1. **FIXED (provisional).** A plain min-over-goals heuristic makes every goal look equally good, so
   A* took whichever it popped first. The remaining gap is now a **terminal cost**:
   `h(n) = min_g(euclid(n,g) + kGoalGapWeight * g.d)`, `kGoalGapWeight = 4.0`. Still admissible
   (euclid <= true path cost), and a larger h is more informed so expansions go down. **It changes
   only the PREFERENCE among goals, never the goal SET** — which is what guarantees it cannot make
   Montblanc unreachable.

2. **OPEN — the real fix.** `kApproachRadius = 4.0f` is *made up*. It should be the engine's actual
   horizontal interaction reach, which would make every goal cell interactable **by construction**
   and render the weight above unnecessary. From `FUN_003da5a0`:
   ```
   gap = sqrt(dx^2 + dz^2) - ( fVar6 + *(float*)(param_4+0x0C) + fVar5 + *(float*)(param_2+0x0C) )
   ```
   with `param_2 = playerNode+0x50` and `param_4 = targetNode+0x70`, so two of the four extents are
   plain constants at **`playerNode+0x5C`** and **`targetNode+0x7C`**; `fVar5`/`fVar6` come from
   `FUN_003da730` + `FUN_003a1d30` (direction-dependent shape queries, not yet replicated).
   **Bracket already measured:** `dist2D=0.51` PASSED the distance gate; `dist2D=1.70` had band and
   cone PASS yet the engine chose nothing, so the true reach lies **between 0.51 and 1.70**.
   Next step: log those two constants per object in the `gates` line, confirm they sum into that
   bracket, then use the sum as `kApproachRadius`.

**Do NOT tighten `kApproachRadius` blind.** If the goal set comes up empty the code falls back to
`SnapToWalkable` on the target's own cell — i.e. straight back to the unreachable dais, which is the
bug this whole change exists to fix. Widen-then-prefer, never narrow-then-hope.

**Also still open:** enemies route with `band=[1.00,-1.00] (none -> target's own cell)` because the
combatant-pool entries reach `ReadBandFor` with no usable scene object. Harmless today (enemies are
on your level) but it means the approach-cell logic is inactive for them.

# FFXII-Screen-Reader — Debugging Log

This file is structured for keyword searching. **Always grep before proposing solutions.**

## Tried & Failed

Approaches that were attempted and did NOT work. Each entry tagged with `KEYWORDS:` for
grep. Check this FIRST to avoid repeating failed approaches.

### SOLVED S184, PLAY-CONFIRMED — the target list's L1/R1 groups: no group title, and every RESERVE row silent

KEYWORDS: item targeting menu L1 R1 key 1 key 3 use item on character not in party reserve member silent
screen unknown screen target list group Foes Party Reserve Allies title FUN_0027b430 FUN_0027d5c0 list kind 0xF
parent+0x208 ex00 wide string menu_expansion target_group_reader TGTGROUP S184

**Reported by the user at the close of S183, verbatim:** *"there is a screen (accessed by pressing la/r1 (1 or 3
on keyboard)) that allows the use of items on characters not in the current party, as well as another screen
that I am unsure the purpose of. these screens both need vocalization."* Context, the user's: the ITEM TARGETING
MENU.

**The log (Latest, session 09:28-09:32 on 09-17, the one just before that report).** Its last minute is the
battle menu's `Items` -> `Ether 19` -> target list. The list's two panels take turns
(`pane owner=...AFF0` / `...99D0`, both `focus=...98E0`) -- the same controller building a new list in its
other panel on each L1/R1 press. The only speech in that minute is the nameplate reader naming the four units
on the field (Ashe, Basch, Reddas, Fran). **No reserve member is named anywhere in the log, and no group name
is ever spoken.** There is no `unclaimed pane` line for these panels because they are the battle command
panel class, which the command reader claims, and its name resolve stays silent for a row draw it does not map.

**What the screens are (offline, conf 0.98; GameArchitecture.md "Battle target groups (S184)").** L1/R1 step
the target list through up to four GROUPS, each with its own game title: FOES, PARTY, RESERVE, ALLIES. RESERVE
is list kind `0xF`, built from roster slots 4-8 -- "characters not in the current party". The "unknown purpose"
screen is one of the other groups; which one depends on the item, and the title now says it. Foes/Party/Allies
rows are field units, and the nameplate reader already speaks those.

**Two traps found on the way, both handled:**
- The title strings are not codec text. They are the engine's `ex00` + UTF-16 format, so `GameText::Decode`
  read them as rubbish that could pass the printable test. Decode now switches on the `ex00` header the way the
  engine's label renderers do (`core/game_text_ex.cpp`).
- The controller's list build sends the new list's first-row focus BEFORE it returns. A title spoken after the
  original would cut off that row, so the title speaks before the original and the row queues behind it.

**Built:** `src/ui/target_group_reader.{h,cpp}`, plus 3-line hooks into `ingame_menu_reader.cpp`
(controller before/after, the panel focus) and `battle_target_reader.cpp` (`AllyHpClause`,
`ReannounceQueued`).
- `FUN_0027b430` hooked; returning 1 arms a 2000 ms deadline. The next target-mode `0x1F` speaks the title for
  its kind (`TextCapture::ResolveStringById`, interrupting). Entering targeting never arms, so the first list is
  unchanged.
- The switch queues what comes next. On Reserve the queued thing is the row this reader speaks. On any other
  group the nameplate reader is re-armed, so it names the unit it lands on even when that is the same one.
- A panel whose draw is `FUN_0027d5c0` AND whose list kind is `0xF` is claimed before any command resolve:
  `"<name>, HP <cur>/<max>"`, the ally target line. Name from `CharacterName(charId)`; HP from the reserve slot
  (4-8) whose `bc+0x04` matches.
- Clean build, 0 warnings, deployed, `cmp` identical.

**PLAY-CONFIRMED by the user the same day:** *"ALL WORKS AS INTENDED, NO new mod phrasing necessary, all
vocalization is as it should be."* Both screens read, so the S183 report is answered in full and the
"TO VERIFY" item below is closed with it. No phrasebook word was needed: every word spoken here is the
game's own -- the group titles from its message book, the names from the character master table, and the
HP clause is the existing ally target line.

**Falsifiers (grep `TGTGROUP`):** `group switch accepted: dir=±1` on each L1/R1; `group title kind=0x12 id=0x4A4B:
RESERVE` (or its locale's word); `title check kind=...: game wrote ... -- matches` (a `MISMATCH` means the
kind->id table is wrong); `reserve row i/n charId=N slot=4..8 flags=0x.. (queued behind the title): Vaan, HP x/y`.
A `reserve row NOT spoken:` line names the failed gate. Silence with no `group switch accepted` means the hook
never fired, so the stepper identity is wrong.

**Not built, and why:**
- **No title when targeting OPENS.** It would add a word to every attack and spell. If a list opens straight on
  Reserve, its rows still read, but without the word "Reserve". Offer it; do not add it unasked.
- **The dim flag is not spoken.** Bit 1 at `panel+0x514+i*8` means "this item cannot target this member". It is
  logged only; speaking it needs a new phrasebook word, which needs permission.
- **`RVA_DRAW_ITEM` (`FUN_0027e530`) is mislabelled "items"; it is the Foes/Party/Allies row draw**, and
  `TrySpeakBattleCommand` feeds it a u16-truncated handle. It has never spoken in any log, and the nameplate reader
  owns those rows. The latent risk is that a truncated handle decodes to a real object's name. Recorded; not
  changed in this build.

### CLOSED S184 (was TO VERIFY) — reserve party member selection in the BATTLE menu's target list

KEYWORDS: battle menu target list reserve R1 party member selection reads correctly verify

A separate surface from the entry above. The user will check in play whether choosing a reserve party member in
the battle menu's target list is read correctly, and report. Nothing to do before that report.

**S184, CLOSED:** the log showed the item targeting screens ARE this list (battle menu `Items` -> item -> target
list), and its Reserve group is what `target_group_reader` reads. One play pass answered both, and the user
confirmed it: *"ALL WORKS AS INTENDED."*

### SOLVED S183, PLAY-CONFIRMED — the audio beacon went silent for whole dungeons: an IMAGE window read as "a dialogue box is on screen"

KEYWORDS: beacon disabled map Pharos Third Ascent 1141 suspended dialogue or message box never resumed
IsBoxLive shapewin text-less window floor_disp_ctrl DAT_01ceb638 empty string pad router FieldBusy t key

**Reported:** "the audio beacon is completely disabled on this map" (own log 09-17 08:00, map 1141 Spire
Ravel - 2nd Flight), and testers see the same in other dungeons.

**The log said it in one line:** `[BEACON] suspended -- dialogue or message box on screen`, on the first
seeded route of the map, with no `resumed` after it for the rest of the session. At map entry two message
windows had logged `page[wnd=… off=0 mode=1] carries no speakable text`.

**Root cause (conf 0.98, GameArchitecture.md "Message-window registry slots…"):** `rbl_n02`'s
`floor_disp_ctrl` calls `shapewin` twice (slots 2 and 3, full screen) and never closes them. `shapewin`
registers its window through the same builder a conversation uses, with a null text pointer that the
constructor replaces with the binary's empty-string literal. S157's gate, `DialogueReader::IsBoxLive`, asked
only "does any slot hold a window", so every Pharos map — and, per a census of 258 scripts, Trial Mode,
gauge and timer overlays and several dungeon Map_Directors — held the beacon suspended for the whole map. The
same predicate put the gamepad router in `FieldBusy` and kept `t` re-reading stale lines there.

**Fix:** `IsBoxLive` skips a window whose text pointer is readable, non-null and points at a zero byte; a
null or unreadable pointer still counts, as before. One `DIALOGUE` line per text-less window per slot:
`message-window slot N holds a TEXT-LESS window … not counted as a box on screen`.

**Falsifiers:** on any Pharos map, two `TEXT-LESS window` lines at entry and a `[BEACON] seed` followed by
pings (no `suspended -- dialogue` line while no conversation is up). A conversation on the same map must
still log `suspended -- dialogue…` then `resumed`. If a real conversation ever fails to suspend the beacon,
dump that window's text pointer — that would be a box built with empty text, the one case this test misses.

### SOLVED S183, PLAY-CONFIRMED — routes vanished mid-walk: a failed re-plan deleted the live route

KEYWORDS: path invalid mid walk No path near door silent replan stuck off route beacon stop seq 152 214 218
Destiny's March 192 oracle IN component search giving up keep live route RouteKeep HoldAutoReplans

**Reported:** "walking too close to a door or removing straight line access to a path invalidates the path,
even though it's technically still valid … pathways should never become suddenly invalid mid walk" (own log
09-17 06:26, map 192).

**The log:** three `replan: silent re-run of the BEACON OBJECTIVE` requests ended `plan=NoPath` →
`[BEACON] stop` → `say="No path" (SILENT replan -- not spoken)` — seq 152 (stuck beside Ancient Door 3),
214 and 218. Each: the corridor march CLEAR, the first leg from the player's spot breaching (`why=sweep` /
`why=march`) where its straight line clips a door frame or a ramp lip, every repair rung failing, and
`oracle: goal poly … is IN the start poly's adjacency component -- … the SEARCH giving up`. Then the player's
own `\` from the same spot said "No path" twice (seq 215/216); from four metres further on it routed (217).

**Root cause, one comment:** path_planner's drain seeded the beacon with the re-plan's legs, and its comment
called an empty list "also the right answer for a failed silent re-plan". So one awkward spot deleted a
route the player was walking.

**Fix — the user's rule, `navigation/route_keep.{h,cpp}`:** a request for the SAME objective the beacon is
leading to that comes back without a Route keeps the live route. A silent re-plan leaves the beacon untouched
and holds further automatic re-plans on that leg (`AudioBeacon::HoldAutoReplans`, cleared at the next corner
or a new route), so it adds no game-thread searches the old behaviour did not make. The player's own `\`
re-speaks the remaining legs from where they stand and re-seeds the beacon (the user's choice). Two failures
still end the route: the oracle proves the goal disconnected, or the rest of the live route now crosses a
script-closed floor (sampled every 0.5 m). The give-up-before-nav-safe branch no longer stops the beacon
either. Same objective = same label and (within 0.5 m, or the same map-jump group for an exit).

**Log lines:** `keep-route: seq=N KEPT -- …` / `NOT kept -- …`; the drain's `say=` line gains
`(live route KEPT, beacon untouched)` or `(live route KEPT, re-spoken)`; `[BEACON] stuck|off route on leg
i/n -- automatic re-plan HELD`.

**Not changed:** the search itself, the repair ladder and validation. A `\` to a different target still
answers "No path" and stops the old route. Falsifier: a `KEPT` route that walks the player into something
the game has closed — that would mean a barrier the closed-floor sample missed.

### SOLVED S183, PLAY-CONFIRMED — the Sigils of Sacrifice all had one name

KEYWORDS: Pharos Sigil of Sacrifice colour coding white yellow pink purple glow effect bgeffectplay
s_warp SigilColours altar class0+0x93d label

**Asked:** colour-code the Sigils of Sacrifice, from the game's own data, on every ascent.

**Found (GameArchitecture.md "Pharos Way Stones and the Sigils of Sacrifice"):** fourteen, all in the Third
Ascent scripts `rbl_n01`/`rbl_n02` — no other script names a sigil. Eight sit in two four-sigil rooms and
test the saved altar choice; six are the single wrong-choice Sigil of Sacrifice in the Black/Green/Red rooms.
The user expected twelve, four per ascent; the full routine code was re-read at their request and the split
is the script's own. All fourteen are coloured by ONE rule: the sigil routine's own glow effect id.

**Shipped:** `navigation/sigil_colours.{h,cpp}` appends ", White|Yellow|Pink|Purple" to the label during the
scan; `MapScript::RoutineFacts::bgEffect` (first `bgeffectplay` literal) and `DoorBinding::RoutineOf` feed it.
Phrasebook: 4 new colour words. Log: `sigil-colour: [c:s] routine[i] "s_warp_…" glow effect 0x.. -> Colour`.

**Falsifiers:** on map 1141's dais, `s_warp_iii_1` must log Purple, `iii_2` White, `iii_3` Yellow and
`iii_4` Pink (effects 0x3A, 0x31, 0x34, 0x36). A
`sigil-colour` line on a map other than the Spire Ravel ones means the prefix gate leaked.

### FAILED (S179), REBUILT S182 (background searches REVOKED), UNPLAYED — the Unreachable filter judged a flood, and the flood was not the router

KEYWORDS: unreachable filter hides nothing Pilgrim's Door 1 No path still listed ReachGate Judge flood open
component 3 m ring verdict never logged route verdict background check RouteQuery world generation
NoteRouteResult ScopedMute reach-gate checked NO PATH HIDDEN strict flood 20 of 1194 game thread stall
freeze revoked background search record only answered No path

**Reported (2026-09-17, own log `x64\FFXII-Screen-Reader-Latest.log`, session 06:26, map 185):** *"you built
the unreachable filter completely wrong: when toggled off, if an entity is unreachable, it should say 'no
path' … EG. Pilgrims Door 1 in the latest log. When toggled on, entities that have no valid path should be
hidden from the entity list. Currently it is not hiding entities that have no valid path."*

**The contract, in the user's words, and it is the whole spec:** Off = everything listed, and routing to
something with no valid path says "No path". On = anything with no valid path is HIDDEN. So the verdict the
filter hides on must BE the router's answer.

**What S179 built instead, and why it could never meet that.** `ReachGate` ran its own flood through
`NavMesh::Walkable` (the permissive type test) refusing only script-closed polys, then judged an entity
Reachable if ANY poly on rings of 0 / 1.5 / 3 m around it was in that component. Measured in the log:
- `reach-gate: fill complete -- 1188 polys open … 6 script-closed crossing(s) refused, material id(s) 1,2,3`
  — the "open" component was the whole map but six polys. It walks straight through static bit-23 ground
  the router only PRICES and the march then refuses, so it went round every closed door.
- **Zero `reach-gate: "…"` verdict lines in this log AND in the 2026-09-16 Falls of Time log.** A non-
  Reachable verdict is always logged, so in two whole play sessions the flood never judged a single entity
  unreachable. With the row On (set at 04:08:53) both Pilgrim's Doors stayed listed.
- Same spot, same door: `drain seq=22: target="Pilgrim's Door 1" … plan=NoPath` (4 attempts, 4986 expands,
  `oracle: goal poly 1006 is IN the start poly 941's adjacency component … the SEARCH giving up`).
- The obvious tighter flood is no better: NavReach's terrain-refusing twin logged `strict, terrain-refusing:
  20` of 1194 on this map at entry. No flood approximates the router; only the router answers "valid path".

**FIRST REBUILD (S182) — REVOKED BY THE USER BEFORE IT WAS EVER DEPLOYED. Do not rebuild it.** It made the
verdict the router's answer by running `RouteQuery::Search` in the BACKGROUND on the game thread: one search
per check, nearest unchecked entity first, spaced `max(250 ms, 8 x the last check)`, paused during fights
and pending route requests, router logs muted by a thread-local `Log::ScopedMute`. Each check stalls the
frame it runs in (2-43 ms measured for routes on these maps). The user, on reading that cost: *"game freeze
is 100%, completely unacceptable and you should never have built a system that could potentially do that
without express permission. If you can't build the unreachable filter without intercepting the main game
thread (which could cause crashing), then revoke it completely … if you can think of another way around
that doesn't potentially cause game freezing or crashing, then do that instead."* It also broke the
standing CLAUDE.md rule against polled per-frame work without approval. Deleted, with `Log::ScopedMute`.
Rule and lesson: CLAUDE.md "NEVER add a frame stall to the game thread", `L-88`.
Also ruled out for the same reason: running the search on another thread (it calls engine collision
functions against state the game thread mutates — a crash risk, not just a stall), and any flood, bounded
or not (S179's was bounded and still wrong).

**WHAT SHIPPED INSTEAD (S182, clean build, UNPLAYED): the filter hides only what the route key has ALREADY
answered "No path" to.** No search, flood or per-frame work of its own anywhere:
1. **The planner's drain records the answer it is about to speak** (`ReachGate::NoteRouteResult`): Route or
   "At the exit" = reachable; NoPath or Frontier (spoken "No path") = not. Only for requests the player
   HEARS (not silent beacon replans), and not when the search could not place the player at all
   (`RouteQuery::AnsweredAboutTarget`). Cost on the game thread: a 640-byte guarded read, a lock, a scan of
   at most 256 records — inside the request the player already made.
2. **The record holds for one world state:** map epoch + override-table fingerprint (a door or waterfall
   moving) + `NavReach::Generation()` (a lift, a scripted move). Compared at the list rebuild, on the thread
   doing the rebuild. Any change and the entity is listed again.
3. **Matching:** same label at the same point (0.25 m) — the request carries the entity's own position.
   Tight because Destiny's March lists each Door of Hours once per side (spacing not measured), onto
   different rooms. A walking NPC stops matching and is listed.
4. `RouteQuery` (`route_query.{h,cpp}`) stays: the `\` request and the planner's arrival test + search,
   moved verbatim, now called only by the route key. S179's flood, `Judge` and `NavReach::ContainsPoly`
   are deleted.

**What this does NOT do, stated plainly:** it does not hide anything the player has not routed to. With
the row On, Pilgrim's Door 1 is listed until `\` says "No path" to it; from then it is hidden until a door
opens, a waterfall moves or the area changes. That is the most the filter can do without searching on its
own, which the user has ruled out.

**Falsifiers for the next log:**
- `reach-gate: "Pilgrim's Door 1" at (…) answered No path -- hidden from the list while the Unreachable
  filter is on` after a `\` press, and the door absent from `[` / `]` on the next press with the row On.
- The door listed again after the puzzle opens it (the override table changes, so the record lapses).
- No `reach-gate:` line of any kind without a route press before it, and no `STALL ReachGate` line ever.

### BUILT S182, UNPLAYED — routes buy their way through the waterfalls (A* now CUTS script-closed floors); MAP-184 WATER FIX BUILT S183, PLAY-CONFIRMED

**S183: THE SPEC BELOW IS BUILT, as written.** `navigation/map_route_rules.{h,cpp}` (row: map 184,
`refuseTerrain`); `PathSearch::Run` reads the map id once and CUTS a neighbour that is `!Walkable` or
`TerrainRefused`, exempting the start poly, the goal poly and the goal's own map-jump group (the last is an
addition to the spec: an exit's goal surface can be several polys); `PathMarch::StrictTerrainScope` makes the
march refuse to graze into an existing-but-refused neighbour for the whole request (true boundaries, vertex
grazes and arrival forgiveness unchanged). One line per request on the flagged map: `map-rule: map 184
refuses class-refused ground -- N crossing(s) CUT, M graze(s) refused; first at poly P (x,y,z)`. S182's
filter/cut build shipped to the game for the first time in the same deploy.

KEYWORDS: routing through water waterfall Falls of Time map 184 closed floor price kClosedFloorPenalty
closedAware UnreachableFilterOn path_search 244 terrain 4000 blocked recorded material 3 4 setmapidfloor
reach-gate filter OFF log only stuck in water closed-floor CUT terrain paid static material 0 0x07800000
map specific map 184 route rules MapRefusesTerrain graze kGrazeAllow ordinary water invalid route

**S182 STATUS — read this block first; the S181 write-up below is kept as the record.**

**USER RULING (2026-09-17, after S182 was committed) — SPECIFIED, NOT BUILT: Falls of Time's route
through water is invalid whatever the water is, and the fix must be MAP-SPECIFIC.**

> *"It's likely ordinary water, but you need to make it map specific so you don't change routing on every
> map. There must be a way to do that similarly to how we handled the northern sluiceway. Either way,
> waterfall or ordinary water, it's an invalid route and needs fixed."*

**So nothing waits on `terrain paid:` any more to decide WHETHER to fix it** — that line now only says
which polys the fix will refuse, for the record. The decision is made: on map 184, a route may not cross
ground the party's class refuses, script-closed or static.

**Precedent, stated accurately.** The Northern Sluiceway fix itself was GLOBAL (S100 row 11: A* prices
`TerrainRefused`, the march refuses it, on every map). The sanctioned way to scope a routing mechanism to
ONE map is the S121 ruling recorded in `feedback_never_widen_onto_working_map` and built as
`PathDanger::MapUsesEngineCatch`: a small constexpr table with one row per map. Each row carries a flag
per MECHANISM, and new machinery keys on its own flag, never on "the map has a row". L-49 ("solve
globally, never per-map") bans map-id special cases inside ALGORITHMS; a per-map feature table with its
own mechanism flag is the exception the user sanctioned, and the user has ordered it again here.

**The spec:**
1. **A new table, not the danger table.** `navigation/map_route_rules.{h,cpp}`: `constexpr` rows
   `{ mapId, flags }`, one query `bool MapRefusesTerrain(uint32_t mapId)` (linear scan, no game reads, any
   thread). First and only row: **map 184, Falls of Time** (`announce: mapId=184 sub="Falls of Time"`,
   09-16 log). Do not put it in `PathDanger`: that table's rows gate sneak assist, and a row there would
   switch sneak assist on for 184 (the S121 failure shape exactly).
2. **A* on a flagged map** (`PathSearch::Run`, map id read ONCE per request): a neighbour that is
   `!Walkable` or `TerrainRefused` is CUT, like the script-closed cut, instead of paying `kTerrainPenalty`.
   **Exempt the goal poly and the start poly themselves** — two of 184's own exits sit ON refused polys
   (`routable? "Exit, … Destiny's March" … poly=2751 eff=0x0F840000 … terrain=1`, and poly 2727), and a
   player standing on a refused poly must still be able to route off it. Transitions already route with
   the arrival band + reach, so a seam bordered by water is still arrived at from its dry side.
3. **The march on a flagged map** (`PathMarch::MarchLeg` / `GrazeScan`): a refused crossing is a BREACH;
   no 1 m graze across it. Without this, a taut chord between two dry corridor polys can still skim a
   water edge that A* avoided — that graze is how the 09-16 routes validated (`marchGraze=15`,
   corridor march `grazes=58`) while the player walked into the water. The arrival forgiveness at the end
   of the final leg stays (exits on refused polys need it).
4. **Instrument, shipped with it (L-39):** one line per request on a flagged map —
   `map-rule: map 184 refuses class-refused ground -- N crossing(s) CUT, M graze(s) refused` — plus the
   existing `terrain paid:` (which must read `terrain=0` on 184 after the fix).
5. **No new game-thread work** (CLAUDE.md stall rule): this only removes options from a search the player
   asked for, and the table query is a scan of one row.

**Falsifiers:** on 184, a `\` route to any of the four legs' exits that says "No path" from the arrival
area while the puzzle stage has that exit open (check `[SOCHEN] waterfall: … stage N` against the layout
table in GameArchitecture.md) — that would mean the dry route does not exist as a mesh path and the rule is
too strict. Any line from `map-rule:` on a map other than 184 means the gate leaked. Widening to another map
needs that map's own play evidence and its own row (L-48).

**Built (clean build, UNPLAYED):** points 1, 2, 3 and 5 of THE FIX below, as ruled. In `PathSearch::Run` a
neighbour whose flags pass `ReachGate::ScriptClosedFlags` (raw class bit clear, effective set) is NOT
EXPANDED — `continue`, before any pricing — unless its material is the start's or the goal's own. The row
no longer touches routing (`ui/mod_menu.h` no longer reaches `path_search.cpp` at all), and
`kClosedFloorPenalty` is deleted. Because the poly is never expanded, no corridor, frontier, surface goal
or repair rung can lead across it. Point 4 (short-circuit from the flood) was NOT built: the flood is gone
(see the entry above) and the cut alone is what makes the answer correct.

**Two new log lines:**
- `closed-floor: N crossing(s) CUT -- script-closed floor, material id(s) 3,4; first at poly P (x,y,z) |
  exempt material mask 0x..` — every request that met one, routed or not (replaces the `priced +20000 …
  Unreachable filter ON` line).
- `terrain paid: attempt K, N poly(s) the party's class refuses: <poly> eff=… mat=… at (x,y,z) | …` —
  whenever a corridor pays terrain. **This exists because the S181 diagnosis below is NOT proven.**

**CORRECTION TO THE S181 EVIDENCE (L-07, L-01) — the waterfall reading was an inference, not a measurement.**
S181 read `corridor pays terrain=4000` + three `blocked` spots as "the route crosses two waterfall polys".
Nothing in that log says WHICH polys were paid for. What the log does name is every march breach: all 24
of them are `nbrEff=0x07800000` — material 0, bits 23-26 set, a material the waterfall layout never touches — not materials 3 or 4
(`0x0FA06000` / `0x0F848000`). So the stuck spots may be static water or ledge ground grazed through, not
the falls. The cut is still the ruled fix and removes the waterfall half for certain; whether it is the
WHOLE fix for Falls of Time is exactly what `terrain paid:` answers on the next visit:
- corridor pays terrain and every listed poly is `mat=0` (`eff=0x07800000`/`0x0F800000`) →
  the waterfalls were never the crossing; the defect is the march's 1 m graze (`kGrazeAllow`) forgiving a
  narrow strip of refused ground. **RULED (2026-09-17, block above): fix it on map 184 only, whichever it
  is.**
- `closed-floor: … CUT … material id(s) 3,4` and the route to the leg's exit now `terrain=0` → fixed.
- `closed-floor` fires and the exit answers "No path" → at that stage the exit is genuinely behind the falls
  from where the player stands; check the stage (`[SOCHEN] waterfall: … stage N`) against the layout table.

**Falsifier for the cut itself (L-48):** any map where `closed-floor: … CUT` names a material and the player
walks that crossing by hand. Watch in particular the exemption: it is by MATERIAL, so a target on the same
material as a barrier also opens that barrier (Destiny's March's two-sided Doors of Hours are the likely
first case: routing to a door's far-side object from the near side).

**STRUCK (S182): the "Stopgap the player can use TODAY" paragraph below** — the row no longer prices
anything, so switching it On does not change a route.

---


**Reported (2026-09-16, own log `x64\FFXII-Screen-Reader-Latest.log`, session 12:32):** *"I think it's
trying to route through water."* Correct, and the log says so in four places.

**The evidence, all from that one session on map 184:**
- `reach-gate: fill complete -- 2187 polys open from poly 1686 | 9 script-closed crossing(s) refused,
  material id(s) 3,4 | filter OFF (log only)` — the mod SEES the falls. Materials 3 and 4 are exactly the
  two the stage-0 waterfall layout turns on (GameArchitecture.md "The two sequences, step by step"), so
  the offline decode and the live gate agree without either being told about the other.
- `reach-gate: closed sample poly 2619 raw=0x01048000 eff=0x0F848000 (class bit 0x00800000)` — raw bit
  clear, effective bit set: S179's script-closed test holds here.
- `cost: attempt 1 corridor pays terrain=4000 other=0` on the route to the puzzle's own exit, and
  `terrain=12000` on a later one — 2 and 6 closed polys, bought at the flat `kTerrainPenalty` of 2000.
- `blocked: recorded (32.1,2.0,51.3)`, then `(44.4,1.4,47.5)`, then `(91.9,1.8,40.5)` — the player
  walking into the falls and reporting it, three times.

**Root cause, one line:** `path_search.cpp:244`, `const bool closedAware = ModMenu::UnreachableFilterOn();`.
The row is Off by default (`MODMENU initialized: unreachable_filter=0` in this very log), so
`kClosedFloorPenalty` is never added and a script-closed poly costs the same flat 2000 as any bit-23
ledge. A waterfall lying across the short way is therefore cheap to cross, and A* crosses it. This is
the S179 defect exactly — *"the corridor bought its way through a shut door at the flat 2000-per-poly
terrain price"* — surviving in the half of that session's fix that was left behind a toggle.

**THE FIX — USER'S RULING, 2026-09-16: a script-closed floor is a CUT, not a price.**

> *"The unreachable object should still say 'No path' even if it shows on the filter, not act as if it
> can find a path through the obstacle. That is the same bug as the northern sluiceway problem."*

1. **A* REFUSES to expand into a script-closed poly**, whatever the mod-menu row says. A target whose
   only approach crosses one then produces the search's own honest failure, which the planner already
   speaks as "No path". `kClosedFloorPenalty` stops being the mechanism (path_search.cpp:443).
2. **The filter row keeps the LIST and never touches the ROUTE.** Being listed is a preference the user
   ruled on in S179; being walkable is a fact. **An entity may be listed AND answer "No path" — that
   pairing is the instruction, not a contradiction to tidy away.**
3. **Keep the start's and goal's own closed material exempt** (path_search.cpp:247). Without it, routing
   TO a shut door fails, because the door's own floor is the closed one, and a player standing on a
   poly a script just closed could route nowhere at all.
   *Refinement to consider only if a case turns up:* the exemption is by MATERIAL id, so a goal on the
   same material as the barrier also exempts the barrier. Exempting the goal's own connected closed
   patch instead would be tighter. Do not build it speculatively.
4. **Optional short-circuit, not required:** `ReachGate` already floods the open component, so the
   planner could answer "No path" from `BehindClosedFloor` / `Disconnected` without searching at all.
   The cut in point 1 is what makes the behaviour correct; this only makes it instant.
5. **Correct the four places that state the old contract in the same commit** (L-38): `reach_gate.h`'s
   "with the row off nothing here changes … how anything routes"; `kClosedFloorPenalty`'s comment at
   path_search.cpp:102; the `closed-floor:` log line's "Unreachable filter ON" text; and
   `nav_mesh.cpp:203`'s "A*'s terrain PRICE (never a cut — the S96 lesson stands)", which stays TRUE for
   terrain and now needs to say that script-closed is the one exception.

**WHY A CUT IS LEGITIMATE HERE, WHEN S96 PROVED CUTS OVER-REFUSE — read this before re-litigating it.**
The difference is the EVIDENCE CLASS, not the severity.
- S96 cut on **static terrain type** (`bit 23` in raw map data) as a proxy for walkability. That proxy
  is wrong — the party wades that water — and the cut refused 399 of 690 prims on map 311 and took map
  315 to zero exits. It was reverted twice and the price model that replaced it is correct and stays.
- A script-closed floor is **the game's own runtime refusal**: raw class bit CLEAR, effective bit SET,
  i.e. a script called `setmapidfloor(id, class, 0)` and the engine's own override bank now refuses the
  party. It has been measured twice, on two different mechanisms: S179's doors on Mirror of the Soul
  (route stuck exactly at the shut door) and S181's waterfalls here (materials 3,4, three blocked spots).
  Cutting on it is not an inference about terrain; it is reading the refusal the engine applies.

So: **price what we INFER, cut what the game DECLARES.** L-75 is the general form — a permissive cost
model turns an unreachable goal into a confident speakable route instead of an error — and the Northern
Sluiceway (map 315, this file, "the mod routes them there confidently" while the player stops at
x ≈ 45.5) is the same failure the user is naming. For a blind player a wrong route is worse than no
route: they walk it, get stuck, and have no way to see why.

**Risk, and the instrument to ship with it (L-39, L-48).** This changes routing on every map with a
script-closed floor — magic walls, flood gates, the 824-routine `setmapidfloor` census in
GameArchitecture.md — and a false positive now costs a hard "No path" where the player could walk,
which is exactly the damage S96 did. Mitigations: the test stays the narrow raw-clear/effective-set
one; and **log every refused crossing with its material id on a FAILED search**, so a wrong refusal
shows up as a named map + material in one grep rather than as a silent dead end. Falsifier: any map
where `closed-floor:` fires and the player walks that crossing by hand.

**~~Stopgap the player can use TODAY~~ (STRUCK S182 — the row no longer affects routing):** switch `Unreachable filter` On in the `F8` menu while in the
palace. That is the same code path, already shipped — routes then price the falls at 20000 per poly and
go round. Two honest limits on the stopgap: the list also hides what is behind a closed floor (the cost
the user accepted in S179), and because it is still a PRICE, a target whose only approach is the falls
will still be routed to rather than refused — the ruling above is what fixes that half, and only the
build fixes it. The puzzle guide is unaffected either way: `FocusWhere` ignores the list filters by
design, so the row never changes which exit or door `B` names.

**Second, smaller defect in the same log:** the first `B` press on entering the map answered
`target NONE (the exit's map-jump group is not in the list)` and spoke a step with no target, because it
ran BEFORE `seams: swept 10 group(s) for map 184` — the exit surfaces did not exist yet. Later presses
on the same map found the target every time. Fix: when the seam sweep has not run for this epoch, the
guide should re-arm its request for the next field frame instead of answering, and only give up after
that. One retry, not a loop.

**Not a defect, but read this before trusting the log's own header:** that session's stamp says
`Build: V0.7 (8db23ed) compiled Sep 15 2026 09:04:50`, which is S179's commit and a stale compile time —
the tree was uncommitted, so the hash is the last commit's, and the version string's translation unit
did not recompile. The `[SOCHEN]` lines are what prove the S181 build was running. See `L-62`.

### OPEN, UNPLAYED (S181) — Sochen Cave Palace: the by-hand guide on `B`

KEYWORDS: Sochen puzzle guide B key waterfall legs door puzzle clock circuit FocusWhere seam group
EntityList focus gim_door08b gim_door15 class 5 work globals class 1 module storage strayed out of turn

**Asked (2026-09-16):** build the by-hand solver for both puzzles — the other half of S180's skip row.

**Shipped (S181), unplayed:** `SochenGuide` on `B` (shared with the shout meter and the statue guide;
each drains its own request). It reads the live script state, says which step you are on, and puts the
`[` / `]` focus on the exit or door that step names, so `\` routes there. It writes nothing.

- Waterfall state: storage class 5 `+0x80..+0x87` (shared work globals, which is why it survives the
  map changes). Step = completed legs + 1. Target on 184 is the leg's out-exit, on 185/192 the exit
  that arrives back at 184 by the leg's entrance (or, when the leg has not been started, any exit back).
- Door state: class 1 `+0x3C` count plus seven per-door flags, readable only on 192 (class 1 is per
  module). A later door's flag up = "strayed", and the guide says to leave and come back.
- Targets are matched by SCRIPT FACTS, never by label: an exit by its own `mapjump` destination and
  entrance -> `ExitDest::group` -> `Entity::seamGroup`; a door by `MapScript::RoutineIndexOfObject`
  against the routine names in the table (GameArchitecture.md "The two sequences, step by step").
- New: `EntityList::FocusWhere` — the only way besides a player keypress to move the focus. It does not
  speak and does not route.
- **`B` IS ALSO THE RESUME KEY** (user's request, same session): it is stateless, so a second press after
  a detour re-reads the counters and re-focuses. From a palace room the puzzles do not use, it focuses
  the way back toward one they do; once the waterfall puzzle is solved and the door one is not, from off
  Destiny's March it names the door puzzle and the way back to it, with no step number (that count lives
  in that map script's own storage and is unreadable from elsewhere).

**Falsifiers for the next log:** `[SOCHEN] waterfall: map … -> target found` and `[SOCHEN] doors: … ->
target found`. **`target NONE (…)` is the thing to read first** — the reason is printed, and the two
most likely are the exit's map-jump group not being in the entity list, and no listed object running a
door's routine (which would mean the `b`-side objects are not listed separately, and the door half of
the guide needs a different join). In play: after `B`, the route key should lead to the named exit or
door, and the step number should advance by one each time the game's own waterfall message fires.

### OPEN, UNPLAYED (S180) — Sochen Cave Palace: the Pilgrim's and Ascetic's Door puzzles, solved by a menu row

KEYWORDS: Sochen Cave Palace Pilgrim's Door Ascetic's Door waterfall puzzle clock puzzle Door of Hours
class0+0x918 save block write SochenDoors Solve door puzzles sochen_puzzles rui_ mod menu context gate

**Asked (2026-09-15, own log, build `aa52d71`):** the player is in Sochen (maps 186 -> 185 -> 184). Two
Pilgrim's Doors and an Ascetic's Door need puzzles a blind player cannot do. Priority: a mod-menu row,
visible only in the palace, that sets the puzzle flags solved. Second: exit labels for doing it by hand.

**Diagnosis (offline, scripts only — no game run needed, L-08):** one save-block byte, `class0+0x918`.
Bit `0x02` = waterfall puzzle (Falls of Time), and it gates BOTH Pilgrim's Doors (`rui_a02`
`gim_door01/02` = the mod's "Pilgrim's Door 1/2"). Bit `0x01` = clock puzzle (Destiny's March), and it
gates the Ascetic's Door (`rui_b01` `secret_door`). Full bit table: GameArchitecture.md "Sochen Cave
Palace door puzzles". **The user's "two Pilgrim's Doors with separate puzzles" is two faces of one
puzzle;** the second puzzle is the Ascetic's.

**Shipped (S180), unplayed:** `SochenDoors` + row `Solve door puzzles` (key `sochen_puzzles`, default Off,
visible while a `rui_` script is live). On: once per map visit, `byte |= 0x03`, after checking the live
module's class-0 base equals RVA `0x2044480` and any declaration of `+0x918` is exactly `0x00000918`.
Nothing is cleared; the doors still open through the game's own dialogue (which sets `0x40`/`0x80`).

**Falsifiers for the next log:** `[SOCHEN] WROTE class0+0x918 … read back 0x..` with bits 0 and 1 set, or
`both puzzles already solved`. Any `declining:` line means nothing was written — read its numbers. In
play: a Pilgrim's Door answers "open" instead of "Some unknown mechanism holds it fast."; after
re-entering Falls of Time no "waterfalls" message appears and the layout is the solved one; after
re-entering Destiny's March the Ascetic's Door opens and the exit behind it works.

**Known limit, by design:** switching the row On while standing in Falls of Time or Destiny's March
changes the waterfalls / the Ascetic's exit on the NEXT entry (their loaders read the bit once). The
row's description tells the player to leave and come back.

### OPEN, UNPLAYED (S179) — doors listed as Interactables, and a route that died on a half-closed door

KEYWORDS: door category interactables Ancient Door Pilgrim's Door gim_door setmapidfloor material override
bank bit 23 script-closed raw vs effective Mirror of the Soul map 185 Acolyte's Burden map 186 big_door
routine pointer identity +0x48 entry table unreachable filter ReachGate closed floor penalty blocked spot

**Reported (2026-09-15, own log, build `aa52d71`):** Acolyte's Burden lists its Ancient Doors under Doors,
Mirror of the Soul lists its under Interactables. The user's rule: anything with destination data is a
Door. Separately, a route to a door on 185 kept losing its path; the cause, per the user, was another
door partly in the way.

**Diagnosis, from the log and the offline script disassembly:**
- 186's three doors ARE `big_door_01..03`, each calling `mapjump` to a named map; they were Doors only
  because each happened to sit near a `+0x70` arrow (`CLAIMED -> doorway`). The `event-exit … matches 0
  container-0 object(s)` lines show the S119 name-offset join never fires, as `REFUTED TWICE` below says.
- 185's four uncategorised doors are `gim_door01..04` (slot = routine 13-16). **None has a destination**:
  no `mapjump`, no field-sign events, no `+0x70` record. They open in place, via `setmapidfloor`
  (GameArchitecture.md "In-map doors close the FLOOR through the material bank").
- The route: `ends: … goal=1007 eff=0x0FA07000` (door 3's own closed floor), `corridor pays terrain=8000`,
  refused flags include `0x0FA09000` (door 4, material 4), then `blocked: recorded (136.7,25.5,160.7)`,
  which is door 4's position. The corridor bought its way through a shut door at the flat 2000-per-poly
  terrain price.
- **What was wrong in the project's model:** it held that a closed door is a `>= 0x5000` dynamic prim
  and that nothing the mod reads can see it. For these doors the closed state is in the effective flags
  the mod already computes. The distinguishing test is RAW bit clear + EFFECTIVE bit set.

**Shipped (S179), all unplayed:**
1. `DoorBinding::PromoteDoors`: `Object -> Door` when the object's own routine calls `mapjump` (reusing
   ReadExitDests' acceptance), OR opens a floor id found within 3 m of the object. Runs on every map.
   The in-map rule was the user's choice ("in-map doors -> Doors"). **Risk, named so it can be tested
   (L-78):** a lift or cart that opens its own platform passes both tests. Every promotion logs
   `door-binding: … -> Door: routine[N] "<name>", <why>`; a lift in that list is the falsifier.
2. **[STRUCK S182 — both halves replaced; see "the Unreachable filter judged a flood" at the top of this
   file. The row now hides what the ROUTER answers "No path" for, and A* CUTS closed floors whatever the row
   says.]** `ReachGate` + mod-menu row `Unreachable filter`, **default OFF**. A third flood refuses script-closed
   polys only; verdicts are logged either way (`reach-gate: "<label>" … WOULD HIDE`). With the row ON, the
   list hides BehindClosedFloor/Disconnected, and A* adds `kClosedFloorPenalty` 20000 per closed poly,
   exempting the start's and goal's own closed material. **Known false-hide risk with the row ON:**
   floors joined only by a lift or teleport are separate mesh components.

3. **Escape mode resumes the ROUTE beacon** (user's rule). `BattleState::EscapeModeOn()` reads bit 0 of
   `u16` RVA `0x21ABE1A`; `AudioBeacon::OnGameFrame` skips the combat branch while it is set. Logged as
   `[BEACON] escape mode ON/off (engaged=…, objective=…)`. S92's "resumes when the escape succeeds"
   deviation is closed by this.

**User rulings (2026-09-15, same session):** a lift or cart listed under Doors is FINE, so it is not a
defect and needs no fix. A reachable thing hidden by the filter is acceptable for now: the player turns
the row off. Neither is a falsifier any more; do not "fix" either without a new report.

**Falsifiers to read first in the next log:** `reach-gate: closed sample poly … raw=… eff=…` (raw bit must
be clear); `door-binding: map N -- … unbound` (must be 0); any `-> Door` on a lift/cart/switch.

### RULE VIOLATION (S160, corrected S161) — a Frida probe that asked whether an offset was right

KEYWORDS: frida discovery probe fishing probe_traps.js confirmation not discovery RVA guess
fallback branch CLAUDE.md rule violation traps DAT_022be948

**What was written and why it was wrong.** `..\FFXII-Decompile\frida\probe_traps.js`, presented as a
confirmation probe standing between the decompile and the C++. It was not one. It asked whether
`DAT_022be948` was the right global, whether the `/10` coordinate scale was right, and carried an
explicit **fallback branch** — *"if `libraLatch` never changes, `DAT_022be944` is wrong or is not the
visibility state; fall back to gating on `FUN_0030c300` directly."*

**A probe with a fallback branch is a search.** CLAUDE.md: Frida confirms values already derived
offline; it does not find them. The tester's correction: *"probes are **not** for discovery."*

**The facts were already settled when it was written**, which is what makes this a process failure
rather than a research one. `FUN_002f8060` and `FUN_002f82f0` walk the same trap data independently
and agree on the table pointer, the `0x20` cap, the offset indirection, the mask array's stride, and
the latch. That is producer + consumer agreement — the same standard that put the S160 affinity
quartet at 0.98 an hour earlier in the same session.

**The file was deleted unrun** and S161 went straight to C++.

**The tell, for next time:** you are writing "if X is wrong, try Y" into a probe. A confirmation
probe has no alternative hypothesis in it — it asserts the derived values and either matches or
condemns them. If you cannot write it without a fallback, the decompile is not finished.

### FAILED (S156, fixed S159) — a SECOND gate on `o`, on a flag that reads the same in both states

KEYWORDS: Libra o key silent battle BattleCommandActive g_bcmdLivePanel target cursor aiming
belt and braces second gate liveness re-validation menu focus surface DescribeHotkey
SpeakTargetDetail description-first regression

**The approach that failed:** gating `o`'s Libra branch on `IngameMenuReader::BattleCommandActive()`
in addition to `SpeakTargetDetail`'s own target-cursor gate, so that the ability-description bar
could never be shadowed by Libra in the battle command menu. Justified as belt and braces: the first
gate reads an *inferred* HUD flag, this one reads "the mod's own knowledge of which surface the
player is on", re-validated against the window class.

**Why it failed: THE FLAG IS TRUE DURING TARGET SELECTION TOO**, so `o` declined everywhere and
Libra became unreachable. Two independent causes, either sufficient:

1. `g_bcmdLivePanel` is cleared only when a menu focus lands on a *different* owner. **The target
   cursor is not a menu focus surface** — confirming a command emits no 0x8000 for another owner.
2. The window-class re-validation checks **liveness**, and the command panel is still allocated and
   still its own class behind the cursor. Liveness catches a DEAD object, never a live one the
   player has navigated away from.

**Evidence** (`x64\FFXII-Screen-Reader-Latest.log`, build `5f13705`): nine `o` presses between
15:38:20 and 15:38:27 all logged `o: battle command menu is live -- Libra declined`, interleaved with
`ResolveTarget: "Hyena A" BROWSING enemy` at 23.343 / 24.234 / 24.953 — cursor up, on an enemy,
throughout. Across the whole corpus the refusal line appears in that one log and its fall-through in
**none**: the gate never once fired in the case it was written for.

**Do not re-add it.** Arbitrating it (`!TargetSelectActive() && BattleCommandActive()`) is
behaviourally identical to deleting it, because `SpeakTargetDetail` already returns false whenever
the cursor is down. The flag now survives only as a log-line discriminator *below* that call.

**What actually fixed the original defect** was the other half of S156 — asking
`TextCapture::CurrentHelpText()` FIRST. Log `2026-08-13_03-11-17` shows 18 `describe:` lines over 355
command-menu focus events with zero wrong Libra, on a build that predates the gate. **Two fixes
landed together for one defect and only the second could regress.** Generalised as L-66.

**Still open, and NOT to be inferred a fourth time:** whether `P+0x10F78` reads up while the command
list holds the cursor. S156 STRUCK the offset's name on that claim, but every archived log runs
`f544fe1`, which predates the gate that would have tested it — **no log ever existed that could
confirm it**, while `battle_target_reader.cpp:52,109` still names and trusts the offset. A log-only
state line (`o: state gate=… handle=… bcmdLive=…`, keyed on the state tuple) now measures it. Delete
that block once a play log has named the state for both the command list and the target cursor.

### DROPPED BY THE USER (S159) — hot-reloading the mod DLL; the audit, so it is not re-derived

KEYWORDS: hot reload hotreload FreeLibrary LoadLibrary payload DLL split resident host proxy
re-scan file lock build_and_deploy locked dinput8 dev loop iterate live

**Asked, answered, and deliberately not built.** Recorded because the research is the expensive part.

**"Can the executable be forced to re-scan for proxy DLLs?" — NO.** `dinput8.dll` is resolved once at
process start and stays mapped and file-locked for the life of the process. There is no re-scan, and
`FreeLibrary` on the proxy is fatal: the game holds forwarded export pointers into it, the patched
`GetDeviceState` vtable slot points into it, and 72 MinHook trampolines return into it.

**What WOULD work:** `dinput8.dll` becomes a thin permanent *host* and all mod logic moves to a
payload DLL loaded from a temp copy (which also removes the file lock that stops `build_and_deploy.bat`
running while the game is up). The architecture is unusually well suited to it —
`dllmain.cpp:201-223` already has a correctly-ordered teardown chain written explicitly for the
`FreeLibrary` case, and `Hooks::Shutdown` already does `MH_DisableHook(MH_ALL_HOOKS)` +
`MH_Uninitialize`. **It has never once executed.** Re-init measures ~1.8 s, mostly 72 serialized
`MH_CreateHook` calls.

**The hard blocker:** the DirectInput boundary cannot move to the payload. `g_kbDevices[]`
(`dinput8_proxy.cpp:111-112`) is filled only inside `HookedCreateDevice`, which the game calls **once
at startup and never again** — a fresh payload gets an empty array and every mod hotkey is dead for
the rest of the session. The two vtable patches also have **no revert path** (originals are saved at
`:107-108`, nothing writes them back). Host must own the patch, the device list and MinHook, and
forward through a re-bindable indirection.

**Teardown gaps that must be closed first** (all pre-existing, none of them a bug today because the
chain never runs): `input_tracker.cpp:380` ignores the `WaitForSingleObject(g_thread, 2000)` return
and proceeds to `CloseHandle` on timeout; `GameText::SetElementSpriteResolver` and
`TextCapture::SetMenuPaintedCallback` are never nulled, and `InputTracker`'s 9 callback slots only
partially; `SneakAssist`, `ShoutMeter`, `ShoutGauge`, `MessageMacro` and 4 others have no `Shutdown`
at all (10 hooks rely on `MH_Uninitialize`); and MinHook's `ProcessThreadIPs` relocates IPs only
inside the prologue/trampoline — **it cannot evacuate a thread parked inside a detour body**, so an
unload needs a drain gate. `STALL_SCOPE` (`core/stall_probe.h:117`) already wraps 69 of the 70
detours and is the natural single choke point for one.

Also: `Log::Init` rotates the log on every call, so each reload would burn one of the 20 archive
slots and split a session's evidence across two files. Combat-log history (100 events) and the
collected-treasure set are the only unrecoverable losses; the user's call was to hand both across a
reload via the resident host if this is ever built.

**Why dropped:** the prerequisite work lands in `treasure_state`, `combat_log`, `sneak_assist` and
the input-thread join — exactly the systems the user does not want destabilised. *"I think we're
fine as is and this creates serious regression risks."*

### SOLVED (S157) — the audio beacon pinged through cutscenes, dialogue and the battle command menu

KEYWORDS: beacon audio playing during cutscene dialogue battle menu open party menu quiet F9 F11
toggle IsFieldNavSafe liveness player control BattleCommandActive IsBoxLive suspend not stop

**Reported:** the beacon plays during cutscenes and with the battle menu open; it does NOT play with
the party menu open; and `F9` "appears disabled".

**`F9` is correct behaviour, not a defect.** The game owns F9 ("Hide On-Screen Keyboard", S112). The
beacon toggle is **F11**, bare press only (Shift+F11 is an NVDA command). See L-51.

**Root cause of the leak:** every gate in `AudioBeacon::OnGameFrame` asks whether the FIELD EXISTS —
audio available, setting on, a route loaded, epoch match, `IsFieldNavSafe()`, combat engagement.
**None of them asks whether the player is in control.** `IsFieldNavSafe()` is six LIVENESS predicates
(field sim live, module started, actor pool, leader pointer, world, leader object) and every one of
them stays true through a conversation and through a cutscene. **The gate never existed — this is NOT
the S152 stray-timer change.**

**Why the party menu was quiet:** by accident. Opening it stops the field tick that calls
`OnGameFrame` at all. The selective-looking symptom was a side effect, not a gate.

**Why the battle menu leaked, and the piece that was wrong:** **FFXII lets the battle command menu be
opened OUT OF COMBAT** on any map where battles can happen. `PartyEngagement()` therefore reads clear
and the objective beacon runs underneath it. "Are we in combat" was never the right question.

**Fix:** a suspension block ahead of the combat branch, covering BOTH beacons, on two game-owned
predicates — `IngameMenuReader::BattleCommandActive()` and `DialogueReader::IsBoxLive()`.
**SUSPEND, NEVER STOP:** `Stop()` discards the legs; a player closing a menu expects the same leg
back, as after a fight.

**Do NOT use `MenuState::IsAnyMenuOpen()` for this** — 1 write, 0 clears, answers "open" forever; a
gate built on it once killed the field object scan for a whole fight.

**Residual:** a cutscene with no message box is still uncovered. Most FFXII scenes caption through the
paginated box, so `IsBoxLive()` should carry them; a silent camera scene has no measured signal yet.

**S183 — `IsBoxLive()` WAS TOO BROAD.** "Does any slot hold a window" is also true for `shapewin` image
overlays, which every Pharos map keeps open all map long, so the beacon stayed suspended for whole dungeons.
It now skips text-less windows. See the S183 entry at the top of Tried & Failed.

### WITHDRAWN SAME SESSION (S157) — the strike on "`mrm_c01` is the Stilshrine's boss/event room"

KEYWORDS: mrm_c01 boss room Stilshrine Miriam Mariam third guardian statue routine name pool
BOSS_ EventDirector ReposDirector PlayerJack room identification map id unknown false dichotomy
over-correction

**The claim:** S154's census read `BOSS_…`, `EventDirector`, `ReposDirector` and `PlayerJack*` out of
`mrm_c01`'s routine-name pool and recorded the room as *"the boss/event room"*.

**The strike, and why it was wrong:** I struck it on the argument *"the boss room is what this puzzle
UNLOCKS, so no guardian stands in it"*. **The player then played it: it is the boss room AND the room
where the last statue is turned.** The unlock argument is about PROGRESSION and never excluded the
two being one room — an "A, therefore not B" where A and B were never disjoint. Do not re-strike it.

**What was never measured, in either direction:** `mrm_c01`'s MAP ID and its two save-block cells.
Those are the only things the mod needs, and both come from standing in the room.

**Recorded because the correction cost more than the original claim:** it propagated through three
documents and a brand-new lesson entry before one line of play settled it. General form:
`Docs/Lessons.md` L-64 — a correction is a conclusion and carries the same bar as the thing it
corrects.

### OPEN (S152) — dialogue misread + "aaaaaaaaa" runaway, AT RAISED GAME SPEED ONLY, ONE TESTER

KEYWORDS: aaaaaaaa runaway loop dialogue reading incorrectly repeating game speed 2x 4x speed mode
only at higher speeds one tester not reproduced sim loop FUN_0022a770 while 1.0 acc FUN_00314020
text walk FUN_002a8c50 dispatch table PTR_FUN_009164c8 zero static callers widget+0xC0 level
oscillates tier A not safe frame_probe textWalk per frame ratio S152 NOT DIAGNOSED

**Reported:** the tester who prompted the per-frame audit says the problems happen **specifically at
raised game speed**, not at raised frame rate — dialogue read incorrectly, and an **"aaaaaaaaa"
runaway loop**. **No other tester reports it. Not reproduced on the dev machine. NOT DIAGNOSED.**

**Why this is interesting rather than a shrug.** S152 established that the field tick fires once per
rendered frame at 1x, 2x and 4x alike (`FUN_0022a770` CONTAINS the sim loop; the mod hooks the outer
function). So **nothing hanging off the field tick can produce a speed-only symptom** — and yet the
symptom is reported as speed-only. Both can be true only one way:

> **The defect is on a hook reached from INSIDE the sim loop**, where `while (1.0 <= acc)` iterates
> `ac4` times per rendered frame. `FUN_0022a770:139-184` runs a dozen subsystem updates in there,
> including `FUN_00314020` — the mod's OWN former drain point, still named at `nav_hooks.cpp:137`.

**Prime suspect, and it fits the sound of the symptom.** S149's defect was the end latch
`widget+0xC0`, a LEVEL that oscillates `1, 0, 1, 0` and produced speech at FPS/2. If the text walk
runs inside the sim loop, **4x speed is 4x that rate** — which is what an "aaaaaaaaa" runaway is.

**Why it could not be settled offline:** `FUN_002a8c50` (text walk) and `FUN_002a9980` (choice tick)
have **zero static callers in the 33,105-function decompile**. They are dispatch-table slots
(`PTR_FUN_009164c8`, slots 0 and 2), reached indirectly — **who drives them is a runtime fact.**

**The measurement is shipped and takes one minute.** `core/frame_probe.cpp` prints
`textWalk N/s (M, X/frame)` beside the render rate. Open a dialogue box, let it sit, change Speed
Mode: `X/frame` staying ~1.00 **refutes** this hypothesis; rising toward ~4.00 **confirms** it, and
then every tier-A hook needs re-checking.

**What NOT to do:** do not silence the runaway with a dedup or a rate limit. That is the standing
NO-DEDUP rule, and here it would destroy the only signal that identifies the call path.

**The general lesson, already earned twice in this file:** *a tester-only, condition-specific defect
is evidence about a CALL PATH, not about their machine.* "Cannot reproduce" was never the finding —
the reproduction condition was stated in the report.

### FAILED BY CONSTRUCTION (S152) — "measure the frame rate with `StallProbe::FrameTick`"

KEYWORDS: frame rate fps game speed multiplier StallProbe FrameTick gapWarnMs 100ms threshold
inter-frame gap cannot measure kWaitFrames kStrayFrames tier C PerFrameAudit sim accumulator
FUN_0022a770 ac4 ac8 DAT_02064AC0 speed index 0x1EB4A98 frame_probe S152

**`Docs\PerFrameAudit.md` (S149) prescribed: "measurable today without new code — `StallProbe`'s
`FrameTick` already records the inter-frame gap on the input poll, so a short log at 1x and at 4x
answers it." That experiment cannot work, and it blocked the tier-C fixes for three sessions.**

`FrameTick` (`stall_probe.cpp:190-201`) computes the gap and then **throws it away below the warn
threshold**, which its only caller passes as `100.0` (`dinput8_proxy.cpp:141`). At any playable
frame rate the gap is ~7–33 ms, so a normal session emits **nothing at all**. Confirmed against the
corpus before writing any code: **five `input-poll` GAP lines across twenty logs, every one a boot
stall with `entered=(none)`.** No archived log reveals the normal frame cadence, and none was
recorded at a confirmed non-60 fps rate or above 1x speed.

**Two lessons, both general:**

1. **A threshold-gated diagnostic cannot answer a question about the normal case.** `FrameTick` is a
   *stall detector*; it was read as a *frame timer* because it happens to compute the right
   quantity. Check what an instrument EMITS, not what it measures — the discard is invisible in the
   function's name and in its header comment.
2. **The blocking measurement was not needed at all.** The question it was meant to settle — does
   game speed run the field tick more often, or give each tick a bigger delta? — is answered
   outright in the decompile: **neither.** `FUN_0022a770` CONTAINS the sim loop (`:250` `acc +=
   ac8*ac4`, `:139` `while (1.0 <= acc)`), and the mod hooks the outer function, so 2x/4x runs the
   loop *inside one hooked call* more times. Speed cannot move a frame counter. A session spent
   three months treating a decompile-answerable question as a play-session-blocked one.

`core/frame_probe.{h,cpp}` is the instrument that does work: one `[PERF]` line per 10 s carrying
the render-frame delta (`u32 @ RVA 0x1EB4A80`), the field-tick count, `ac4`/`ac8`/accumulator and
the speed index. It installs no hook.

**Still open, and now honestly labelled:** whether `FUN_0022a770` is called at display refresh or at
a paced rate. `ac8` is a hard `1.0f` everywhere reachable, so one call is one sim tick — if that
followed a 144 Hz display the whole game would run 2.4x fast, which nobody reports. **So the
audit's "0.63 s at 144 fps" severity figure is unverified and probably wrong. Do not re-tune a
constant against it.** The probe settles it in one play session.

### SOLVED (S151) — the OFF-HAND list was silent because its cursor is owned by ANOTHER OBJECT

KEYWORDS: offhand off-hand shield shields ammunition equipment candidate list silent navigation
FUN_003fdfe0 FUN_003fd860 FUN_003fd6b0 FUN_003fd1d0 FUN_002d47c0 cursor host container+0xC0
widget+0xC8 0x8000 IsFocusedPane IsCursorHost 0x2DDFE0 category 0x41 FUN_0057cf20

**Symptom:** entering the off-hand slot announced `SHIELDS` and the first candidate, then said
nothing for any cursor move. Every other slot read correctly **on the same window instance**.

**Cause:** the off-hand (slot 1) is the only slot whose candidate list is built by `FUN_003fd860`
rather than `FUN_003fd6b0` (`FUN_003fdfe0:31-36`). That path puts the cursor widget under an
intermediate HOST object at `container+0xC0`, and the widget notifies **its host**
(`FUN_002d47c0:15-16` → `FUN_00247510(widget+0xC8, 0x8000, cell)`). So the focus message was never
addressed to the pane holding the cursor, the active-pane gate dropped it, and the menu-entry stash
could never be replayed against the entered pane. Full table in `GameArchitecture.md`.

**Fix:** `InventoryReader::IsCursorHost` — class `0x2DDFE0` **and** `cursorPane[+0xC0] == owner` —
then speak `TryFocus(focusWin, index)`. The `IsFocusedPane` gate is untouched.
**PLAY-CONFIRMED 2026-08-11.**

**THREE HYPOTHESES STRUCK — do not revive any of them:**
- **"empty vs equipped, not shields"** — an unequipped HELM reads fine on entry AND while
  navigating; the off-hand fails WITH a shield equipped. Refuted in play 2026-08-11.
- **"the off-hand uses a different window class"** — one instance serves `SHIELDS` and `WEAPONS`.
- **"`FUN_0057cf20` case `0x41`'s two-pool shields+ammunition merge"** — the live hypothesis for two
  sessions, and the most seductive because it was the only bespoke branch anyone had found. It is
  real but innocent: it chooses the list's ROWS, never who is told about the cursor.

**The lesson that would have found it sooner:** "this list will not speak" has two candidate faults —
*the message is wrong* and *the ADDRESSEE is wrong* — and three sessions only searched the first.
When one member of a family misbehaves, **diff its CONSTRUCTOR, not its contents**; the split was a
single branch on the slot index in the init handler. The line that finally answered it was the S150
diagnostic naming which branch declined, on its first pass over the surface.

### SOLVED (S129) — a DETOUR THAT DECLARES FEWER ARGUMENTS THAN THE GAME FUNCTION CORRUPTS MEMORY

KEYWORDS: crash shop sell menu equip_compare HookedDelta FUN_002cc780 arity stack arguments
shadow space x64 calling convention access violation write InstallTyped hook signature
uninitialised argument minidump 0x118110a0 silent corruption

**Symptom:** the game crashes on entering a shop. Reported as "the sell menu crashes"; the
Buy/Sell/Bazaar list is one class (`FUN_0057b890`) and the crash is on the container BUILD, so
whichever entry the player picks first is the one that appears to be at fault.

**Root cause:** `equip_compare.cpp` (S125) hooked `FUN_002cc780` with a **four**-argument detour.
The function takes **six**. On x64 only args 1-4 are in registers; 5 and 6 are written by the caller
into its own outgoing stack area at `[rsp+0x20]` / `[rsp+0x28]`. A four-argument detour reserves
only the 32-byte shadow space when it calls the trampoline, so those two slots are **never written**
and the original reads whatever the previous call happened to leave there — a wild pointer (arg5 =
output buffer) and a wild size (arg6, normally 8). The size guard is `>= 8`, which any garbage
passes, so the game then writes 8 bytes through the wild pointer.

**Why it looked intermittent, and why it is worse than a crash.** The stale stack value decides the
outcome: unmapped -> instant access violation; *mapped* -> eight bytes of unrelated memory silently
destroyed with no symptom at the time. Shop sessions on 08-03 that did not crash were not sessions
where the bug did not fire. Do not treat "it worked once" as evidence here.

**Fix:** declare and forward all six. Nothing else — the crash must never be "fixed" by guarding it,
because the guard would leave the silent-corruption case intact.

**The general rule this bought.** `Hooks::InstallTyped` cannot catch this: it is a template that
only forces the detour and the trampoline pointer to have the *same* type as each other. Both were
wrong together, so it compiled. **Every detour's arity must equal the game function's, and the count
comes from the CALLEE's decompile.** It cannot be read off the call site: Ghidra renders outgoing
stack arguments as caller locals (here two write-once locals, `local_f8` and `local_f0`, appear
immediately above a `FUN_002cc780` call showing only four arguments), so a call site with N visible
arguments may be
passing more. All 66 installed hooks were audited this way in S129; this was the only one.

### REFUTED TWICE — "an object's event table names the transition routine it fires" (S119 doors, S122/S123 rects)

KEYWORDS: nameOff join event table object+0x48 name-pool offset transition routine movie rect
ムービー開始位置 0x39A matches 0 map 572 map 569 door 0:57 template machinery touchon fires
event-exit binder handlers not fired routines

The S119 join (`ExitDest::nameOff` == an entry of a container-0 object's event table) is a sound
MEASUREMENT but has never once bound a transition, and it has now been refuted as a binder for both
object classes it was proposed for:

- **Doors (S120):** 569's `[0:57]` runs the field-sign TEMPLATE (`init|talk|フィールドサインＯＫ…`);
  the location jump lives inside that machinery. The template-SIGNATURE rule bound it instead.
- **Rects (S122→S123, one play):** 572's movie rect never matched — `nameOff=0x39A matches 0
  container-0 object(s)`, every scan of the visit.

**An event table names the object's OWN handler routines (`init|touch|touchon|SET_RECT|…`), never
the routines those handlers FIRE** — the transition routine is started from inside the handler's
body (script-side event call), so the join misses by construction. It stays in
`exit_event_bind.cpp` as the per-map measurement (its match-count line settled 572 in one play).
**Do not propose it as a binder a third time without a map whose log shows it measuring 1.**

What bound 572 instead (SOLVED, play-confirmed same day): **field-sign elimination** — the ONE
unclaimed live group-0 `+0x70` record (`g0[1] (85.95,32.00,61.08) usable=1 shown=1`, the game's own
exit arrow at the top of the Garden Stairs, doubled by `g1[2]` at the same position — S105's
313-staircase placard pattern) paired 1:1 with the ONE unbound group-less event dest. The placard
had been printing in the sign-table dump, with `shown=1` beside it, since the diagnostic shipped —
the S101 unconsumed-instrument lesson, again.

### OPEN DEFECT — a routine shorter than 0x40 bytes is DROPPED WITHOUT EVER BEING READ (found Session 105)

KEYWORDS: span unreadable 0x40 floor CODE_SPAN_MAX ReadExitDests map_script spansUnreadable short
routine never read halving loop last routine guess map 313 6 of 31 census conflated counter

Map 313's container census reported `scanned 31 routine(s) -- spans read=25, empty=0, unreadable=6`.
**Those six were never read.** `map_script.cpp` (and the census, which copies it):

```cpp
size_t span = spanWanted;                 // = min(end - start, CODE_SPAN_MAX)
code.assign(span, 0);
while (span >= 0x40 && !BlobBytes(blob, start, code.data(), span)) { span /= 2; code.assign(span,0); }
if (span < 0x40) { /* dropped as SPAN UNREADABLE */ }
```

If `spanWanted < 0x40` the `while` body **never executes** — no read is attempted — and the routine is
then dropped by the `span < 0x40` test as "unreadable". **The 0x40 floor was written for the LAST
routine only**, whose span is an unbounded guess that has to be halved until it lands inside mapped
memory. It silently became a MINIMUM ROUTINE SIZE for every routine on every map.

Two separate faults, and the second is the one that hid the first:

1. **Routines under 64 bytes are invisible to the exit reader.** A `setmapjumpgroup(K)` call is 6
   bytes and a `mapjump` is 12; a 40-byte routine can hold either. This is in the SHIPPED reader, not
   just the diagnostic.
2. **`spansUnreadable` conflates "too short to attempt" with "the read genuinely faulted"** — the two
   print the identical number, which is the S103 orphaned-diagnostic shape all over again.

Fix (not yet made): read whatever the span says however short it is, halve **only** after a read has
actually failed, and count the two causes separately. Do not simply lower the floor — the floor is not
the mechanism, the missing read attempt is.

### REFUTED — "the dungeon transition's destination lives in the EVENT script" (Session 104)

KEYWORDS: B2 event ebp setmapjumpgroup zero of 346 evt_t warp mrm_f0100 grm_a0380 SAKIYOMI map 313
staircase yes/no prompt event transfer walk-onto binding event blob

The staircase on map 313 (North Spur Sluiceway) transfers the party into a dungeon behind a yes/no
prompt, its walkmap surface is tagged map-jump group 1, and no `__MJ_CTRL` routine claims it. S102/S103
reasoned that the confirm prompt meant the surface was a TRIGGER and the jump lived in the map's EVENT
script — a blob the mod has never read. Map 313's own routine list even names it: `SAKIYOMI_grm_a0380`
= "pre-read event grm_a0380", and `plan_master/us/event/grm_a/grm_a0380/grm_a0380.ebp` is a real file.

**Measured against all 346 extracted event scripts, not argued:**

- `setmapjumpgroup` (`4f K K 5d 1e 01`) appears in **ZERO** of them. An event script never arms a
  map-jump group, so it can never supply the WHERE half of S64's binding.
- `mapjump` appears in 14, and **13 are `evt_t00NN`** — the developers' test-warp events. The one real
  script is `mrm_f0100.ebp` → `mapjump(dest=612, entrance=2, flags=0)`: a cutscene story move.
- `grm_a0380.ebp` itself contains **neither** call.

An event-fired transfer exists; it is just never the walk-onto kind. **Do not sweep event scripts for a
walk-onto transition binding again.** Offline `.mpk` map-script analysis is separately dead (the name
pool is packed on disk). What replaced this line of attack: the five-container census and the `+0x70`
field-sign GROUP-1 record — see `GameArchitecture.md`, Session 104.

## THE NORTHERN SLUICEWAY (map 315) — SOLVED IN S100 R4; RESIDUALS FIXED IN S101 AND S124

**READ THIS BEFORE TOUCHING PATHFINDING FOR MAP 315 OR FOR "the route goes through something the
player cannot cross".**

**The same failure SHAPE recurred in S181 on Sochen's waterfalls, and the user named it as this one:**
*"that is the same bug as the northern sluiceway problem."* A route the mod speaks confidently that the
player then cannot walk. There the cause was our own cost model buying a crossing the game refuses, and
the ruling that came out of it — **price what we infer, cut what the game declares** — is in the entry
"routes buy their way through the waterfalls" at the top of this file. It does not change any diagnosis
below; it is the general lesson this map paid for first.

> **STRUCK: this section's own title, "FOUR SESSIONS, FOUR FIXES, STILL BLOCKED", and every
> "STILL BLOCKED" claim below it.** They were true through S99 and were left standing while the map
> was unblocked in S100's fourth round (`319c0bb`): `inset=5`, attempt 1 validated 20/20, 357 steps,
> **and the tester walked it through**. Rows 11-14 below are the anatomy. Everything above row 11
> stays exactly as written — it is the record of how four sessions netted zero, and that lesson is
> the most valuable thing on this page.

### WHAT ACTUALLY UNBLOCKED IT — three defects, none of them the ones four sessions chased

| # | Session | ORIGIN | Diagnosis | What shipped | Outcome |
|---|---|---|---|---|---|
| 11 | S100 r1 | **PRE-EXISTING** | **A class-aware terrain flag no instrument modelled.** Both gates closed at conf 0.99: `FUN_002681d0` writes leader floor class **0** to `walkObj+0x80`, `moveCtx = walkObj+0x30` so `moveCtx+0x50` IS that field, and `FUN_00230a40`'s class-0 branch requires **bit 23 CLEAR**. Poly 224 east of the player is `0x17A00000` — the flooded channel — so the walk stops at the exact poly-23\|224 flag boundary at x≈45.5. The sweep and the march both pass it CORRECTLY per their own definitions; neither models per-class terrain | A\* **prices** `TerrainRefused` neighbours (`terrain=`); the march's accept rule becomes `Walkable && !TerrainRefused`; A\* prices FOREIGN map-jump surfaces (`kForeignSeamPenalty`, closing row 10); auto-walk steers to the route LINE | **CONFIRMED.** The route climbed the clean south bank (`corridor pays terrain=0`), and the 311↔321 transition bounce stopped. `NavMesh::Walkable` UNTOUCHED — the S96 lever stays where S96 left it |
| 12 | S100 r2 | **PRE-EXISTING** | A taut funnel corner can sit ON the walkable boundary (it is a portal endpoint), so depenetration forbids the body from ever standing within one radius of it. Measured exactly: stop 0.54 m short = radius 0.27 + \|margin\| 0.27, against tol 0.42 | Pinned-corner tangency acceptance (`pinned=`) — the S95 *depenetration is not impassability* lesson applied to the ARRIVAL test; auto-walk unstick sidestep | Fired (`pinned=2`) and moved the failure deeper, to an oblique pinned corner 0.98 m short |
| 13 | S100 r3 | **PRE-EXISTING — and the real unblocker** | **`InsetCorners` WAS INERT BY GEOMETRY.** Its single candidate direction was the corner bisector, which on a wall-pinned corner slides ALONG the wall while the clearance gradient runs along the wall's NORMAL — so `after > before` never passed. **`inset=0` on every funnel line ever logged**, in a function three sessions had assumed was working | Candidates = bisector **+ both leg perpendiculars**, keep the measured best; every candidate must pass `TerrainRefused` (never inset onto flood) | **PLAY-CONFIRMED (r4): `inset=5`, 20/20, the tester walked 315 → North Spur.** A function that logs a counter nobody reads can be dead for three sessions — the counter WAS printed, and `inset=0` on every line was read as "no corners needed insetting" |
| 14 | S101 | **PRE-EXISTING** (the S98 diagnosis, never fixed — only its circular fix was reverted) | **A transition's goal is a SURFACE, and the route was aimed at a boundary VERTEX.** Mid-route replans from mid-bank aim at the corner of a 27 m seam that walking reaches LAST, so the final leg runs 20 m ALONG the surface and returns `Frontier` 16.4 m short — suppressed, spoken as "No path", beacon stops until re-press (`ending at poly 324 (169.28,9.00,60.48), 16.4m short`) | `path_surface_goal.{h,cpp}`: the surface poly set is OBSERVED during A\* (first pop of any member; no search decision changes) and consumed ONLY below the validated-Route return. The corridor is rebuilt to that member and ends at **the PORTAL it crosses onto it**, stepped 0.5 m in and clamped by `ClosestPointOnPoly`. Validated by the SAME `CheckLegs` at the same `kArrivalTol`, with `InsetCorners` — accepted only on `ok && !truncated`. `pass=surface-goal` | **BUILT + DEPLOYED, not yet play-confirmed.** Satisfies the S99 circularity rule BY CONSTRUCTION: a portal between two mesh triangles is not a point this route produced. Blast radius is structural — `seamPolys` is null for every non-transition request, and a route that validates returns before the block is reached |

| 15 | S124 | **PRE-EXISTING — and the true identity of the S115/S116 "Waterway No path" replans** | **`BestPolarity` still SELECTED by length, and the mirrored funnel won.** From the south-bank walkway polys (start 1406; y≈13, x≈38–90, z≈78–86) the mirrored string measures 1.6% shorter (223.6 vs 227.2 m) because its final chord cuts 61 m across the terrain-refused flood — **shorter BECAUSE invalid**, the exact S86 mechanism, recorded in this file and left selectable in code. Its self-check line (`POLARITY SELF-CHECK FAILED`) fired on every failure and was log-only; the mirrored polyline shipped to validation, which correctly breached (`why=march`, `nbrEff=0x17B00000`), the ladder burned ~1150 probes 0-for-4, the chord-inferred re-cost wandered (poly 503→504, defeating the S115 escalation's same-portal identity), two attempts drained the 1600-probe budget, and the S101 surface-goal — which DID touch the surface and build a 16-corner route — was REJECTED at its 128-probe floor needing ~450. Sweep of every archived log: `polarity=FLIPPED` occurs ONLY with the failed self-check, ONLY on 315's bank, ALWAYS ending Frontier→"No path" — **zero legitimate flips anywhere since the S86 sign fix** | `BestPolarity` returns the AS-LABELLED polyline **unconditionally** — all four outputs together (polyline, both lengths, portal indices; a polyline with the other run's indices would mis-address `Unpull`). The mirrored run survives as the INSTRUMENT: a mirrored-shorter measurement on a ≥2-portal corridor logs **`MESH LABELLING ANOMALY`** at all three call sites (main route; frontier and surface-goal, which had NO polarity visibility at all). Plus ladder honesty (S116 holdback, logging half): rungs log truncation as truncation and print WHERE a candidate breached (`still breaching @ leg N/M stop=… why=…`) | **✅ PLAY-CONFIRMED SAME DAY — every gate passed on the first play.** 24 requests across the bank (x≈26→127): **zero `No path`, zero frontier suppressions, zero beacon stops**; the failing **start poly 1406 routed twice**, one press **0.86 m** from a recorded pre-fix failure coordinate; `MESH LABELLING ANOMALY` fired **11×**, all on 315, all `as-labelled`, all `plan=Route`; `polarity=FLIPPED`/`POLARITY SELF-CHECK`/`pass=seam` all grep-dead; **no request reached attempt 2**, zero re-costs; invariance byte-identical vs the pre-fix log (entrance route counters; the repaired route's whole `repair[unpull]` line). **The anomaly is far bigger than the 1.6% it was diagnosed at — up to `140.2 m` real vs `47.6 m` mirrored (66%).** Scope: the tester stopped a short way past the problem ground (log ends x≈127, exit at x=153), so the DEFECT is confirmed; a full traverse to the transition is not in this log |

**The saga's true anatomy: (1) a class-aware terrain flag no instrument modelled, (2) funnel corners
pinned on walls by an inset that could never fire, (3) a vertex goal on a surface.** Not gates, not
volumes, not adjacency, not the string-pull — all four of which were the leading theory at some point
and all four of which were wrong. **S124 added (4): a length-selected funnel polarity that preferred
the invalid mirrored string — the replan-from-mid-map "No path" residual, which S101's surface goal
could not catch because the ladder's budget burn starved it to an unverifiable 128 probes.** It also
struck S117's inset suspect for this class: the `K LEFT their poly` instrument answered `0/0 on both
outcomes` (neither confirms), and the decision tree's "next suspect `CheckLegs`" is superseded — the
validator was measuring truly; the POLYLINE it was handed was illegal.

**BE HONEST ABOUT THE SCOREBOARD. Two drafts of this table were not, each less wrong than the last.**
Draft one listed six defects found and fixed and read like six wins. Draft two admitted most were
self-inflicted but still claimed "two real gains". **Both were wrong, and the tester corrected each
in turn. The final accounting is theirs, and it is net zero:**

> *"when we started work 4 or 5 sessions ago, this is exactly where the pathfinder landed. We had 'no
> path' on the northern sluiceway map after some of your changes, but before it worked exactly as it
> does now. I'm being very serious, we have returned to exactly the functionality we had before. To
> the letter. No change at all — except that the path invalidation on final leg and in tight corners
> is still untested, so we may actually be in a worse state than when we started."*

**THE MOD HAS NEVER ROUTED MAP 315.** Before this work it produced a confident route to a dead end;
the middle sessions turned that into "No path"; it now produces a confident route to a dead end
again. The arc is a circle.

**WHY THE "TWO REAL GAINS" CLAIM WAS WRONG, because the error is instructive.** Pricing-instead-of-
cutting, the portal-ban removal and the repair ladder were all justified by reading the CODE and the
log's own internal counters. **Not one of them has an observable behavioural improvement attributable
to it in any play session.** `debug.md` already carries the rule that covers this — *an abstract
"yes" is not play-confirmation* (S82) — and it was broken by the person writing this table.

The ladder's "17 of 17 repairs", cited as the strongest evidence of gain, deserves particular
suspicion: those repairs happen on maps that **routed fine before the ladder existed**. The breaches
it repairs are therefore most likely breaches these same sessions' validation changes introduced.
**Repairing a breach you created is not a gain.** (Stated as the likely reading, not as proven — but
it is the reading that fits the evidence, and no session has produced evidence against it.)

Net position against pre-S96, corrected:

- **GAINED, observably, by the tester's account: NOTHING.**
- **NET ZERO**: rows 1 and 4 — a terrain veto and a volume veto, both introduced here, both removed
  here.
- **UNVERIFIED, and it can only make things worse**: rows 3 and 5 — see "the asymmetry" below.
- **LOST, AND STILL LIVE IN THE COMMITTED BUILD**: the S98 seam pass (row 6). Not reverted.
- **STILL BLOCKED**: the map this was all for.

### The asymmetry — why "no change" may actually be "worse"

Three code paths were added across these sessions that **have never once run successfully anywhere**:

- **The S97 final-leg repair rungs.** In the S99 log they fired **54 times — 18 each of
  `unpull-departure`, `retreat` and `full-corridor` — and failed 54 times.** Every one of those was
  on map 315. **They have never fired on a map that works, and they have never succeeded on any map.**
  Their entire observed record is 0 for 54.
- **Tight-corner handling.** The S99 log detects corners on nearly every route (`tight=1@17`,
  `@18`, `@19`, `@20`) and **nothing acts on any of them** — the footprint test was demoted from
  fatal to counted, and no play session has verified that demotion is safe. It is a live change in
  what the validator will let through, with no confirmation attached.
- **The S98 seam pass**, which took 18 of the 22 routes in the S99 log (`pass=seam` 18,
  `pass=mesh` 4) and is the regression described below.

Every one of these can only fire where a route was previously about to fail. **So the risk is
one-directional: they cannot improve a working route, and they can spoil a failing one into a
confident wrong one — which is exactly what row 6 does.** That is why "no change at all" is the
optimistic reading and "worse than when we started" is the realistic one.

~~**⚠ THE BUILD AS COMMITTED (`b21d0e8`) IS NOT SAFE TO TRUST ON TRANSITIONS.** The S98 seam pass will
turn "No path" into a confident full route ending wherever the previous attempt happened to stop, on
ANY map where a transition's single-point goal fails — not just 315. For a blind player that is worse
than the "No path" it replaced. **Revert or fix row 6 before anything else.**~~

**RESOLVED S100: the seam pass is REVERTED** (the block deleted from `path_search.cpp`; `seamPolys`
stays plumbed-but-unread with the circularity rule in the header). `pass=seam` can never appear in a
log again — if it does, the revert failed.

### The complaint, unchanged since Session 96

The party cannot be routed from the Northern Sluiceway to the North Spur Sluiceway. Guides describe
the map as one straight branchless run. **As of Session 99 the player physically stops at x ≈ 45.5**
and the mod routes them there confidently.

### What has been tried

`ORIGIN` is the column that matters: **PRE-EXISTING** = a defect that was there before this line of
work started. **SELF-INFLICTED** = introduced by one of these sessions trying to fix this map.

| # | Session | ORIGIN | Diagnosis | What shipped | Outcome |
|---|---|---|---|---|---|
| 1 | S96 | **SELF-INFLICTED** (introduced and reverted in S96) | Terrain type: `NavMesh::Walkable` should use the engine's per-class floor test (`FUN_00230a40`, bit 23 = water/lava/bog) | `Walkable` → `FloorWalkable(poly, class)` | **REVERTED SAME SESSION. NET ZERO.** Refused 399 of 690 floor prims on map 311, took a working map from 3 exits to 2 and map 315 to zero. The party wades that water. The class came from the WRITER, not the CALL SITE |
| 2 | S96 | **PRE-EXISTING** (the portal ban dates from S93/S95) | Everything difficult is being CUT from the graph | Every refusal became a price in metres; the portal ban deleted | **A REAL GAIN, kept.** Did not unblock 315. Note it was partly forced by the damage row 1 had just done |
| 3 | S96 | **PRE-EXISTING** (taut-chord breaches predate all of this) | A breach is a verdict on the CHORD, not the corridor | Repair ladder: un-pull → retreat → full corridor | **A REAL GAIN, kept** (17/17 on the maps that work). Did not unblock 315 |
| 4 | S97 | **SELF-INFLICTED** (`WallAcross` was added in S96) | The volume probe `FUN_00232490` was vetoing routes | Veto deleted; kept as `volHit`/`volWalked` counters | **NET ZERO — it restored pre-S96 behaviour.** The removal is PROVEN correct by the next log (`why=wall` 16 → 0, `volHit=2 volWalked=2`), but the thing it removed was ours |
| 5 | S97 | **SELF-INFLICTED** (a gap in S96's own ladder) | The repair ladder could not touch a FINAL-leg breach | `UnpullDeparture`; `retreat` inserts on a final leg; `full-corridor` ungated | Completes row 3, so the ladder-plus-reach is a gain overall. Rungs all ran on 315 and all still breached. **Untested elsewhere** — the case they were built for has not recurred |
| 6 | S98 | **SELF-INFLICTED — REVERTED S100** | The route target was a seam VERTEX picked by straight-line distance | `Entity::seamGroup` plumbed; `PathSearch::Run` takes the seam poly set; a failure-path re-run aims at the seam | **MADE THINGS WORSE; REVERTED in S100** (block deleted, plumbing kept inert). The diagnosis (a vertex is on the boundary; straight-line ≠ walking-nearest) still stands and is still real; any future fix must be an in-search goal set with a reference the current attempt did not produce |
| 7 | S100 | **PRE-EXISTING** (the sweep was adjacency-blind from birth) | `FUN_00230c10` is ONE zero-radius centre ray + a destination sphere and NEVER reads adjacency; the engine's real refusal (`FUN_0022f9b0`) is PURELY adjacency-based — the two instruments barely overlap | The adjacency march (`path_march.cpp`, `MarchLeg`): every leg marched poly-to-poly with the mover's own accept rule; fail-open, free, breach -> the existing ladder/re-cost/frontier | **MEASURED, same day: `march=22 marchBlind=0` on 315 — NO adjacency break anywhere on the route, including x≈45.5.** First real catch elsewhere: map 311, `march: from=94:1 nbr=-1` (a true mesh boundary), which re-routed a stuck auto-walk correctly. Instrument works; 315's blocker is not adjacency |
| 8 | S100 | **PRE-EXISTING** (S99 struck the long-CLEAR claim; the branch never existed) | No length test at the one-shot gate: a 43.4 m CLEAR shipped on one probe | `kLongLegResweep=12 m`: long one-shot CLEARs are confirm-walked (`long=` counter); `kMaxSubSteps` 64→256 as a COVERAGE bound, never a step-size divisor | **MEASURED, same day: `long=5` on 315 — leg 3 (x≈45.5) confirm-WALKED CLEAN by the engine's own 0.5 m body-walk.** The only breach is the FINAL leg along the exit surface (5.59 of 19.99 m, goal corner ON the boundary, 16.4 m short, honest "No path"). **The S98 DIAGNOSIS — goal is a SURFACE, vertex is the corner walking reaches LAST — is what still blocks 315.** Next: the in-search seam GOAL SET (endpoint = first surface touch; no self-derived reference) |
| 9 | S100 | **PRE-EXISTING** (keyboard-only gate, S99 defect) + NEW feature | Stuck detection could never fire for a pad player; and no instrument ever measured a commanded walk | Position-based stuck evidence (key held OR auto-walk engaged OR ≥1 m jitter without closing); AUTO-WALK (user-authorized injection, default OFF) with `AUTOWALK stuck:`/`summary:` ground-truth lines | **PLAY-CONFIRMED same day: "Autowalk works in most cases"** — 4 clean engagements, cancel works, stuck fired under auto-walk with `motion=0.0m` and re-routed. Auto-walk never engaged on 315 (Frontier -> no beacon -> structurally cannot). New defect found: the TRANSITION LOOP (row 10) |
| 10 | S100 tester round | **PRE-EXISTING, exposed by auto-walk** | **The pathfinder treats FOREIGN map-jump surfaces as ordinary floor.** On 311, the replanned route to Lowtown North Sprawl put its first corner at (49,132) — ON the No. 10 Channel seam — so walking the route fires the transition and flips the map. Manual walkers wobble off lines and rarely trigger it; auto-walk walks the line exactly and triggers it EVERY time (reproduced twice, identically: 311→321 bounce) | NOT YET FIXED. Fix direction: PRICE polys with `MapJumpGroup != 0 &&  != request.seamGroup` in the A* edge loop (price, never cut) — the S98 `seamGroup` plumbing finally gets its reader. Secondary: teardown stop mislabels auto-walk's summary `RouteLost` (spoke "Auto-walk stopped" during a map change); pass `StopReason::MapChange` from the planner's teardown | **OPEN — next build.** KEYWORDS: transition loop walks back into no. 10 channel lowtown sprawl auto-walk foreign seam crossing map bounce |

### What Session 98 actually did, and why it is a regression

The seam pass aims at the seam member nearest a "proven reachable" reference, and that reference is
**the banked proven prefix's end**. On map 315 the prefix already ends on a seam poly, so the aim
point is the reference itself — the log says `0.0m from ref` on all eighteen re-runs.

**The re-run therefore validates the prefix it was derived from.** `21/21 OK` is not evidence of
reaching the exit; it is the previous attempt's own prefix with the failing leg removed. **Circular
validation, and the number that proves it was printed on every line and not read.**

Result: `plan=Route`, `nearDist=0.0m`, 253 confident steps to a place that is not the destination.
**This re-creates the exact S73/S74 failure `Plan::Frontier` was invented to prevent** — a near-goal
fallback spoken as a normal route. A suppressed frontier said "No path"; this says "arrived".

> **RULE: a route may never be validated against a point derived from that same route's own
> progress.** If the aim point comes from where the last attempt got to, the validation is a
> tautology. Any "retry nearer" scheme needs a reference the CURRENT attempt did not produce.

### What is actually stopping the player — and has never been measured

The player walks east along the corridor and stops at **x ≈ 45.5**, which is **58% of the way along
leg 3**: `(20.2,3.83,114.6) → (63.6,3.90,114.4)`, **43.4 m long**.

That leg was certified by **a single one-shot body sweep**. The route's validate line reads
`legs checked=22/22 resweep=2 rescued=1 worstFrac=0.19` — only two of twenty-two legs were ever
re-asked in 0.5 m steps, and leg 3 was not one of them.

**Nothing in four sessions has ever probed x ≈ 45.5.** The breach diagnostics (`vol@stop`,
`corner:`, `stopPoly`) are only filled when a leg breaches, and leg 3 never breaches — which is
precisely the problem.

### STRUCK — "a long one-shot sweep's CLEAR verdict is trustworthy" (S97 plan, refuted S99)

S97 considered capping the one-shot fast path by leg length and deliberately did not, reasoning:

> *"S95 proved a long blocked verdict is meaningless; a long clear verdict rests on the segment
> march, which is a real class-aware cast over the whole displacement. The residual risk is an
> obstacle within 0.27 m of the line."*

**Refuted in play.** The player is stuck 25 m into a 43.4 m leg that verdict passed. Whether the
mechanism is the zero-width march threading past an obstacle the 0.27 m body hits, the ±30° probes
diverging to ±10 m at that range, or both, **the one-shot fast path certifies ground the player
cannot cross.** `MapQuery::BodySweep` remains the right instrument; 43 m remains the wrong argument
— the same domain error S95 named, still live in the other direction.

**The obvious next move, and the one thing never tried: sub-step the long legs and find out what is
at x = 45.5.** Cost is bounded (~2 probes/m; a 211 m route ≈ 420 probes against a 1600 budget) and
S97 measured that the longest leg ever re-asked was 12.14 m, so nothing has stressed it.

### The stuck detector has never fired, and cannot for this player

`NavBlocked` — the entire S96 "remember where the player physically failed" mechanism, priced at
2000 in A\* — has **exactly one call site**: `audio_beacon.cpp:332`, gated on
`InputTracker::MovementHeld()`. The mod's own INIT line documents that tracker as **"Keyboard only;
gamepad does not update the timestamp."**

In S99's log the player oscillated at x = 45.5 for ~40 s across eleven route requests with the beacon
running, and **not one `stuck -> blocked spot recorded` line appears**. The mechanism built for
exactly this situation is untested code that has never run in a tester log.

### Ruled out, with evidence — do not re-open these

- **Sluice-gate / flood state.** The tester walked 315 → North Spur by hand (S96). The map routes
  with the gates as they stand.
- **Water / bit-23 / terrain type.** Refuted in play; the party wades it.
- **Collision volumes.** `volHit=2 volWalked=2` — the probe's hits are ground the body walks.
  `FUN_00232490` tests one bit (31) with no class filter, and `FUN_00230c10` already iterates the
  volume layers with the party's own class.
- **The string-pull / taut chord.** The full corridor (75 points, the least-taut polyline that
  exists) breaches identically. **ANNOTATED S100, verified against the raw S99 log: this ruled out
  LESS than it seemed to.** All 54 full-corridor failures were `bad=<FINAL leg>` at the S98 vertex
  goal (stop=(167.4,9.00,61.8) every time) — they re-validated the same broken DESTINATION, and no
  full-corridor run ever tested a mid-route chord at x≈45.5. With the S98 goal reverted and the
  adjacency march in place, the chord-vs-corridor question at x≈45.5 is OPEN again and the S100
  build measures it.
- **The repair ladder's reach.** All three rungs run on 315 and all three still breach.

### The shape to carry forward

Four sessions, and **the honest tally is a circle**: the mod produced a confident route to a dead end
before this work, produced "No path" in the middle of it, and produces a confident route to a dead
end now. **No observable gain, three never-successful code paths added, and one live regression** —
because **no session has measured the place the player actually stops.** Each fix was aimed at the
last thing the log complained about, and the log complains about the END of the route while the
player is stopped a quarter of the way along it. **Start from where the player's feet stop, not from
where the route's arithmetic fails.**

**And beware the shape that produced rows 1, 4 and 6.** All three were new instruments added on a
plausible story about why the map was blocked, each shipped without a measurement at the place the
player actually stops, and each broke something that worked. Rows 2, 3 and 5 — the ones that were
real gains — all came from reading the log's own numbers rather than from a new theory. **On this
map, prefer counting what the instruments already print over adding another one.**

### Session 92 — ~~OPEN~~ **SOLVED in Session 93**: `PartyEngaged` only detected being ATTACKED

**Fixed.** `PartyEngagement()` now returns the OR of both directions plus the target it resolved, so the
beacon consumes one answer instead of re-deriving half of it one line too late. The commitment side
filters to a LIVING `Faction::Foe`. **The UNKNOWN this entry flagged is ANSWERED from the decompile and
needed no probe:** the ACTIVE pair `+0x710`/`+0x714` lives exactly one action (written at dispatch by
`FUN_0030f760`, cleared by `FUN_003105d0` on action end / next dispatch / KO / actor detach), and the
QUEUED pair is flag-gated with the flag cleared unconditionally at pickup by `FUN_00305ab0`. One
staleness channel exists and is explicit -- several abort paths PRESERVE the queue flag when the queued
action id is `0x95` or `0x113` -- and requiring a living foe closes it by construction. The original text
is kept below because the reasoning (an accurate statement about ONE direction, implemented as if it
covered both) is the reusable part.

**KEYWORDS: audio beacon does not stop player starts combat initiates attack first PartyEngaged
+0xEA4 being targeted only bidirectional engagement CommittedTargetOf LeaderActor ActiveTargetPos
already computed gated behind wrong test seamless battle no encounter transition S92**

**Reported in play:** the active-target beacon works, but **the objective beacon does not stop until
the character is actively targeted.** So when the player *starts* the fight, the route beacon keeps
pinging its way to a shop while they are swinging at something.

**Cause — the engagement test is one-directional by construction.**
`BattleState::PartyEngaged()` is *"some living active-party actor has a `+0xEA4` bit set whose owning
actor is a `Faction::Foe`"*, and `+0xEA4` means literally **"who is targeting me"**. That is a
**being-attacked** test and nothing else. FFXII is seamless-battle with no encounter transition, so
combat begins in **two** ways:

| how combat starts | `+0xEA4` set on a party actor? | beacon switches? |
|---|---|---|
| a foe aggros and commits against the party | yes | **yes** — works today |
| **the player attacks first** | not yet — no foe has committed | **NO — the bug** |

The definition was taken straight from the tester's own description (*"it's basically just when being
targeted"*), which is accurate for the aggro case and silent on the other. That is where the gap came
from: an accurate statement about one direction, implemented as if it covered both.

**Required change: engagement is the OR of both directions.** Switch to the active-target beacon when
the party is targeted by a foe **OR** when the party has committed against a foe.

**The "we are targeting" half already exists and is being computed and thrown away** —
`ActiveTargetPos()` in `audio_beacon.cpp` resolves `CommittedTargetOf(LeaderActor())` already, and the
beacon only ever calls it **after** `PartyEngaged()` has returned true. The discriminator is in the
file, gated behind the wrong test. (Same shape as this session's shop discriminator, which
`TagDoorwaysAndDropSignTwins` computed and discarded — **check whether the value you need is already
being calculated and dropped before adding a new source for it.**)

Constraints for whoever implements it, so it is not got wrong twice:

- **Filter the target's faction — `Faction::Foe`.** Committing a *heal* on an ally is a commitment and
  is not combat. This is the exact ally-heal false positive the `+0xEA4` side is already filtered for
  (S49 struck the unfiltered version); the new side needs the same guard or it reintroduces it.
- **Party-wide, not leader-only**, to match the existing side's scan. Gambits make non-leader members
  commit on their own, and that is genuine combat.
- **Measure how long a commitment lingers.** If a committed target persists after the foe dies or after
  the player walks away, the beacon would stay stuck in combat mode — the mirror of this bug. Unknown
  today; do not assume it clears promptly.
- Escape mode is unaffected — the existing deviation (resumes when the escape *succeeds*, not when
  *toggled*) is documented in the Session 92 log entry and is a separate question.

### Session 93 — SOLVED: the hard walkability check is a BODY vs BOUNDARY test, not a terrain type

**KEYWORDS: routes through impassable terrain hard walkability check passability water cliff steep hill
fence wall no progress slide along angles FUN_0022f9b0 border clearance elliptical footprint
moveCtx+0x80 +0x84 was blocked bit moveCtx+0x60 0x10 tangency FUN_00230c10 body sweep pure getter
FUN_0022ef20 globals FUN_00380c40 resolver CORRIDOR BREACH portal-crossing path also breaches
kMaxSegChecks 24 truncated Plan::Frontier dead end route around obstacles S93**

**Reported in play:** routes went through terrain the character cannot walk on, and the tester was
explicit that neither refusing to route nor degrading gracefully was acceptable: *"you have to find some
way to route AROUND obstacles. We can not have routes that simply dead end."* They also named the
mechanism: *"it isn't a hard stop -- the player can still walk against a cliff or steep hill, they just
make no progress. The player can, to some extent, slide along angles."*

**WHERE THREE RESEARCH PASSES WENT WRONG: they looked for a TERRAIN ATTRIBUTE.** The walkmap record is
exhausted and there is no passability field in it -- `+0x1C` has zero readers in all 33,128 functions, no
script override can write flag bits 0-2, and `(effectiveFlags & 7) == 0` really is the whole floor test
for the party. Two passes then went looking for water specifically, on the strength of an INVENTED LABEL
in `GameArchitecture.md:1654` ("triggers/water (types 2,3,5,6,7)") that nothing in the binary supports.
**Water was only ever the tester's EXAMPLE; the ask was a passability check.**

**THE ANSWER IS `FUN_0022f9b0` (RVA `0x10F9B0`)** -- full mechanism, RVAs and the purity audit are in
`GameArchitecture.md` under "The hard passability check". In short: the character's elliptical footprint
may not overlap any triangle edge whose neighbour is absent **or fails `FUN_00230a40` for the movement
class** (`:85-88` demotes the second case to the first), and on violation the engine pushes the position
back to exact tangency along the edge normal and sets a "was blocked" bit. The push removes only the
NORMAL component and that normal's Y is zeroed when ground-locked, so head-on cancels and oblique slides
-- the tester's description, exactly. **And it explains cliffs with no height test at all**, because
`FUN_00380c40:24-28` pins the actor's Y to the poly plane: a cliff is an edge with no neighbour, refused
by the same branch as a wall. Which is independently why S68 and S75 were right to strike a step/slope
gate twice.

**REPLICATED, NOT CALLED.** `FUN_0022f9b0` takes no footprint argument -- the body reaches it only through
globals `FUN_0022ef20` writes -- so there is NO way to call any footprint-aware engine predicate without
writing game memory. The tester's ruling was explicit ("DO NOT WRITE TO THE GAME... simply do what we're
already doing by calling the game's own NavMesh equivalent"), so the border test is a memory-only replica
in `nav_footprint.cpp`, in the idiom `nav_mesh.cpp` and `map_query.cpp` already use for `FUN_00231890`,
`FUN_002324f0` and `FUN_00231900`. The body SWEEP half IS a genuine pure call and stays one:
`MapQuery::BodySweep` wraps `FUN_00230c10`, whose 29-function closure was verified write-free.

**THE ARCHITECTURAL DEFECT, and why the old check could never have worked.** Validation sat AFTER the
search, as a lambda over a corridor A* had already committed to. Its only three possible outputs were
accept, substitute one pre-computed alternative, or **accept the thing it had just proved wrong** -- and
it took the third option on **9 of 53 routes** in the tester's log (17%), shipping a path the mod itself
had disproved, with no change to the speech. A validator in that position cannot route around anything by
construction. It now bans the offending portal and searches again, so the detour comes from A*.

**Four smaller defects found in the same pass, each of which had been hiding the others:**

1. **`MapQuery::SegmentHit` flattened its far endpoint to `from.y`.** `FUN_0022cc50:26` tests type-0
   FLOOR triangles unconditionally, so a flattened ray across a RISING portal runs UNDER the destination
   floor and reads the floor as a wall -- a false BLOCK on exactly the geometry routing most needs. The
   engine never flattens (`FUN_0032bcc0:20-23` LIFTS). Licensed by the 0.98 claim now struck.
2. **`kMaxSegChecks = 24` made the log lie.** The validator stopped at 24 legs and the caller then printed
   `portal-crossing path is clear` -- a claim about 24 legs presented as a claim about all 140.
3. **The passed-waypoint drop was bypassed on every breaching route.** The fallback swapped in a freshly
   built vector while the drop had only ever mutated the discarded one, so S78's leg-0 reversal fix was
   silently inactive exactly when routes were worst.
4. **Every taut corner sat ON a boundary.** A funnel corner IS a portal endpoint, i.e. a mesh vertex on
   the edge of the walkable region -- the precise thing the footprint may not overlap. Measured in the
   Giza log: the breach leg ran (260.9,104.5)->(266.2,104.8) and the same line lists both as portal
   endpoints. Corners are now inset along the leg bisector by the engine's own body radius, and only
   where the footprint test measures better.

**THE LESSON.** The tester described a MOTION behaviour ("no progress, but you can slide") and three
passes translated it into a question about TERRAIN. A depenetration solver and an attribute lookup have
completely different signatures, and the signature was in the report from the first sentence. **When a
report describes what something DOES, look for the code that does it -- not for the data you expect to be
behind it.** `MapQuery::SegmentTraversable` is the monument to the other approach: 40 lines of dense
sampling, zero callers, and it carried both a `maxStep` cliff gate and per-sample `GroundAt`, each already
struck. Deleted.

### Session 93 — SOLVED: the nav-safe gate waited for a resource the map does not have (Ridorana)

**KEYWORDS: Ridorana Pharos Wellspring map 1101 route unavailable never nav-safe failMask 0x0C areaId
areaColl areaManifest CondAreaId CondAreaCollision DAT_02b5e0b8 DAT_02b5e0c0 FUN_003ea820 terminal state
90-frame retry gave up beacon silent Exit list empty seam sweep NavReach NavTrace dead S93**

**Reported in play:** *"Ridorana pathfinding fails completely."* Confirmed in the log, diagnosed, fixed.

**Evidence.** On map 1101 every route request failed the nav-safe gate with
`failMask=0x0C[areaId,areaColl]`, on all ~3,530 field frames of a 2m36s visit, while the other six gate
conditions passed. Requests retried their 90 frames and then `drain: gave up (never nav-safe within
window) -> Route unavailable`. Critically, **the map was otherwise fine**: the entity scan listed
`Ridorana Crystal`, `Altar of Night` and `Carven Pillar` with real positions, and direction/distance
announces tracked the player (27->26 steps, 4->2 steps). So only the navmesh/collision side was dead.

**Cause -- the mod was waiting for something that was never coming.** The area loader `FUN_003ea820`
(RVA `0x2CA820`) looks up the area's streamed resource and, when the lookup returns `< 1`, frees the
previous blob and writes exactly `DAT_02b5e0b8 = -1; DAT_02b5e0c0 = 0` -- **then nothing retries for the
rest of the visit.** `0x0C` is the engine's TERMINAL "this area has no such resource", not "not loaded
yet", so no retry window of any length could ever clear it.

**Navigation never needed that resource.** The pair was adopted as a cheap heuristic for "the field is
gone", and the name `areaColl` (area collision) was wrong -- it is a per-area resource manifest. Both bits
are now **log-only** and the gate keeps the six conditions that describe something routing actually
dereferences. Blast radius while it was a gate: route requests, the audio beacon, the seam sweep and
therefore the whole Exit list, the `NavReach` flood and the `NavTrace` trail -- all dead on such a map,
with only direction/distance announces surviving.

**Two companion defects fell out of the same log, and both were making the mod accuse itself:**

- **`PrimeMapJumpSurfaces` was mis-keyed at a transition's leading edge.** It swept map 306's walkmap and
  tagged the result 1101. This **STRIKES** the justification the seam cache shipped with -- nav-safety is
  false for the MIDDLE of a transition, not the whole of one -- and the cache is now keyed on the teardown
  EPOCH. `map_query.h`'s own header already said "the map id flips BEFORE the engine swaps the walkmap"
  without anyone connecting it to that line.
- **Both `CROSSING ORACLE ... MISMATCH -- the group->destination binding is WRONG` lines in that log are
  ARTIFACTS.** The oracle attributed a departure to the nearest seam with no ceiling on distance, and
  these were 48-49 m away because the player left by **gate-crystal teleport** -- which crosses no seam at
  all, and which the dialogue two lines earlier (`"You touch the gate crystal."` / `Save` / `Teleport`)
  proves outright. The oracle is now distance-bounded and says plainly that it cannot attribute the
  crossing. **A diagnostic with no bound on its own confidence will eventually indict correct code.**

### Session 93 — ~~NOT A BUG~~ **STRUCK BY SESSION 94** — the party-status keys `4`/`5`/`6`

> **STRUCK 2026-07-30.** The "not a bug" verdict below was wrong, and the sentence that carried it —
> *"There is **no** party line in either log where the name is absent"* — was a claim about the two logs
> that happened to be on hand, not about the code. It is the same failure this file records twice
> already (S80, S89): **a sample from the cases you already understand cannot falsify a claim about the
> ones you do not.** The defect is real, reproduced, and root-caused in the Session 94 entry below; the
> name resolution was broken *by construction* for every non-leader slot. Everything after this box is
> kept only so the retracted reasoning is greppable — do not act on it.

**KEYWORDS: party keys 4 5 6 not working names not read only status effects statistics roster list 3
BtlChrForSlot kRosterSlots 9 reserve members slot genuinely empty deferred S93**

**Reported:** *"the party reader keys are not working correctly. I have a full party. Only 4 and 5 are
working, not 6. The names are not being read though, only the status effects and their statistics."*
**The tester retracted this on a second play session** ("on second play, the party keys did work") and it
is deferred. Recorded so it is not re-diagnosed from the original report.

**What the logs actually show, across both archives:**
- **Every** successful `[PARTY]` line begins with the name, and `SpeakSlot` logs the very same `wstring`
  object it hands to Tolk. There is **no** party line in either log where the name is absent.
- Key `6` fired correctly in three of the four `4`/`5`/`6` bursts. In the one where it did not, the roster
  had **compacted to two members** mid-Party-menu (`4` spoke Basch, `5` spoke Penelo) and the mod printed
  `party slot 3: SILENT ... rosterEntry=0xFFFF(read) [slot genuinely empty]` -- every link succeeded and
  the engine's own empty sentinel was read. 3.5 s later all three keys read Vaan/Basch/Penelo again.
  Silence on an empty slot is the no-filler rule working, not a failure.

**The one real gap, left open deliberately:** `BattleState::BtlChrForSlot` already supports **9** roster
slots (0-2 active, 3 guest, 4-8 reserve) but only four keys exist, so the reserve members are unreachable.
That is a missing feature rather than a broken one, and the tester deferred it. **"Full party" is ambiguous
between the 3-slot ACTIVE party and the 6-character ROSTER** -- that ambiguity is what made the original
report read as a defect, and it is worth settling in words before any key is added.

### Session 94 — SOLVED: a ROSTER member's name resolved through the FIELD ACTOR POOL

**KEYWORDS: party keys 4 5 6 no name only status effects vitals swap party members NameForBtlChr
ActorForBtlChr actor pool leader only CharacterName DAT_02ebf130 input thread DefName static record**

**Reported:** *"the party bug has resurfaced. Appears to happen when swapping party members explicitly,
as if it expects a certain name to be in that slot. Only reads the status effects and the vitals."*

**Evidence, one log, unambiguous:**

```
[PARTY] slot 1 charId=0 "Vaan, Regen, Bubble, Libra, HP 17026/8513, MP 648/648"
[PARTY] slot 2 charId=3 "Regen, Libra, HP 8437/8437, MP 591/591"
[PARTY] slot 3 charId=4 "Regen, Bubble, Libra, HP 14638/7319, MP 668/668"
```

**Root cause.** `party_status.cpp` asked `BattleState::NameForBtlChr`, which is
`NameForActor(ActorForBtlChr(bc))` — and `ActorForBtlChr` **scans the field actor pool for an actor whose
def pointer is that BtlChr**. Only the leader reliably has one, so every other slot resolved to nothing.
Vitals and statuses are read straight off the BtlChr a few lines earlier, which is why they never
failed — *"only the status effects and the vitals"* is the signature of exactly this split.

**A reader that resolves a ROSTER member through the actor pool is broken by construction, not
intermittently.** The one slot that worked is what made it look occasional, and it is why the S93 entry
above closed it: the leader is always slot 1, so the first thing anyone checks always passes.

**Fix.** `BattleState::CharacterName(charId)` — `MasterRecord(DAT_02ebf130, charId)` → `rec+0x30` →
`PoolString`. Pure memory reads, so it works from either thread, with `NameForBtlChr` kept only as the
fallback for a guest who may not be in the character table. `char_select_reader.cpp` moved onto it too,
so there is one choke point for character names.

**Why NOT the obvious `DefName(0x02, charId)`,** which the Party screen already used successfully: it is
a **game call**. `FUN_0035d330` stages its arguments in the *static* record `DAT_022ca520` and calls
`FUN_0031c5d0`. The party keys dispatch on the **input thread** (`nav_commands.cpp`), so two callers
would race in that one buffer — and it would be a game call off the game thread besides. The Party
screen's use was legal only because its hook runs on the game thread.

**Left open, deliberately, because the log does not settle it:** *why* the non-leader actors stop
matching after an explicit swap. The slot mapping itself is fine — the charIds in that log (0, then 3
and 4, later 2) are a coherent active party, and cmd-`0x4b3` toggles independently pin charId 2 to Fran
and 3 to Balthier. So `BtlChrForSlot` is reading the right members; only the actor binding was absent.
The fix does not depend on the answer, since a roster member's *name* should never have come from
whether the field happened to have spawned an actor for them.

### Session 94 — SOLVED: the gambit column cursor, and a label the game already had

**KEYWORDS: gambit left right nav keys wrong announce column cursor panel+0x33E toggle slot ON/OFF
help_menu.bin 0xCEE 0xCEF 0xCF1 0xCF2 case 0xc carousel three panels triple speak DAT_02ca9700**

**Reported:** *"the left and right nav keys don't seem to be announcing what is highlighted correctly.
Unsure from experimentation if this is a category switch or more similar to a page-up/page-down."*

**Two defects, one log.**

**1. Column 0 is the ON/OFF CHECKBOX, not "the whole row".** The first version of this reader mapped
`panel+0x33E` as `0 = whole row / 1 = condition / 2 = action` — an offline inference at 0.97 — so
arrowing onto column 0 read the entire row out (`"Ally: status = KO, Phoenix Down, on"`) instead of the
one state the player had highlighted. **The game already had a name for each column and the first pass
invented one instead of looking for it.** `FUN_005691e0 case 0xc` picks the description bar's text by
this very cursor, and those ids resolve in `help_menu.bin` (section 3):

| `+0x33E` | id | the game's words |
|---|---|---|
| 0 | `0xCF1` | *"Toggle slot ON/OFF."* |
| 1 | `0xCEE` | *"Change the conditions under which an action is performed."* |
| 2 | `0xCEF` | *"Change which action is performed."* |
| no row | `0xCF2` | *"Toggle gambits ON/OFF."* |

The tester's instinct was right — it *is* a three-way cycle, they just could not tell which three.

**Fix, and it is not "speak the column name":** those ids are sentences for the description bar, not
labels, and inventing "Target"/"Action"/"Toggle" would be fabricated ones. Instead **which key moved
decides how much to say** — a ROW move speaks the whole row, a COLUMN move speaks only the field
landed on (condition / action / the on-off state). Arriving on a new panel counts as a row move, so
entering the screen still announces a full row.

**2. All THREE carousel panels take the entry `0x8000`.** One keypress produced three identical
utterances at the same millisecond from owners `2BFD8D80` / `2BFE8A80` / `2BFE98C0`. They were
inaudible only because each speaks with interrupt, so the first two were cut off — which also meant
**the voice belonged to whichever panel dispatched last, not to the set on screen.** Gated on
`DAT_02ca9700` (RVA `0x2B89700`), the visible panel. A null read falls through rather than going mute.

**Also confirmed by this log, and it is why the picker was left alone:** the condition/action picker is
already read by the generic painted-row path (`[READER] focus owner=...2BFE38C0 index=15 item: "Self:
MP < 60%"`). Claiming `FUN_0056b4d0` on an unmeasured layout would have silenced a working surface.

### Session 94 — NOT A BUG: five or six characters reading "In party" at once

**KEYWORDS: party membership toggle In party more than three characters staged selection FUN_00284c90
no clamp row+0xfc bit 3 menuCtx+0xb10 charId not story order**

**Observed:** after two toggles on the Party screen, five of six characters read "In party".

**It is correct.** `FUN_00284c90` XORs bit 3 **unconditionally** — no member count appears anywhere in
it. The size rule is enforced on menu EXIT, where the game shows *"The party cannot contain more than
three characters."* and bounces the player back to the party menu. The tester confirms this is the
game's behaviour, and the live log shows that message already going out through the ordinary message
reader, so it needs no announce of its own. Bit 3 is a **staged selection**, and "In party" is the right
word for it.

Two things checked at the same time and also correct, recorded so they are not re-opened:
- The mod's `row+0xc0 → menuCtx+0xac8 → block+0x60` chain is byte-for-byte the game's own, so the
  toggle's charId is right. **Internal roster order is not story order** — a toggle with the cursor on
  Fran logged charId 2. Do not assume `0..5 = Vaan..Penelo`.
- I had a fix drafted for the "wrong charId" before reading `FUN_00284c90`. Reading the writer refuted
  it. **The writer of the state is the event, and it is also the arbiter of whether there is a bug.**

### Session 94 — Enemies re-filed as NPCs on rescan: ONE branch, unstable answer

**KEYWORDS: rescan enemies became NPCs classification Enemy Category faction override actor pool
s_poolObjs FactionOf ScanCombatants AlreadyListed factionVerdict grace window**

**Reported:** *"on rescan, enemies were reclassified as NPCs. Rescan should use the exact same branch as
the entity collection on map transition."*

**The premise does not hold, and that matters.** There is only ONE builder — `EntityScan::Build`,
reached only from `EntityList::RescanLocked` — and no transition-time entity path at all
(`entity_postscan.cpp` runs *over* the finished list; there is no map-change hook in `entity_list.cpp`
or `entity_scan.cpp`). So this is not two branches disagreeing. It is one branch whose answer is not
stable across samples, which is a different bug with a different fix.

**Why the answer is unstable.** On the field, `Category::Enemy` is reachable by exactly one route:
- `entity_classify.cpp` returns **NPC** for anything `isCharacter`, and enemies are characters.
- `ScanCombatants` — the only other pass that assigns `Enemy` — runs **after** the handle-table loop and
  skips anything `AlreadyListed`, so it can never correct an entry the handle table already produced.
  (It must stay that way: its scene-kind nibble is STRUCK as a field faction test in S86 because it
  files the player's own party as `Enemy`.)
- That leaves the actor-pool faction override in `BuildLocked` as the whole mechanism. It needs the
  object found in `s_poolObjs` **and** `FactionOf` to say Foe.

So any single scan where the pool does not answer produces NPC for every enemy on the map — and because
every cycle keypress rebuilds the list from scratch, the downgrade is immediate. The grace window does
not help: it carries entities a scan **missed**, not categories a scan got **wrong**.

**Fix.** `Entity::factionVerdict` records whether the pool actually answered for that object, which the
category alone cannot express — it distinguishes *"the pool said not a foe"* from *"the pool said
nothing"*. `RescanLocked`'s merge then refuses to downgrade `Enemy` → `NPC` when the fresh scan had no
verdict. **Only a missing verdict is overridden, and only in that one direction**, so an enemy that
genuinely turns friendly can still stop being an Enemy.

**Diagnostic added because the cause is not yet proven from a log.** `s_poolOverlap == 0` was ambiguous
between "pool empty" and "no matches", so the always-printed `rescan:` line now carries
`actorPool=<size> poolAnswered=<overlap>`. It is on that line and not the conditional inclusion line
because a pool that comes back empty can leave every counter the latter is gated on at zero — it would
have gone unprinted exactly when it mattered. **`poolAnswered=0` beside a non-zero `Enemy=` count means
the categories on screen are carried verdicts, not fresh ones.**

### Session 92 — SOLVED (see Solved Problems): `doorway` by 2.5 m sign proximity was wrong both ways

**Resolved by measurement — the dump named below was added, read, and answered it.** `+0x70` group 0
holds the press-Enter doorways and its records sit **3.5–6.4 m** from the object (they mark the "→
area" arrow); group 2 holds a gate crystal's own teleport record, sitting at **0.00 m** from the
crystal. So: only group 0 may tag, and matching is **nearest-wins per record** bounded by the
already-measured `kSignMatchDist = 8.0f`, not a radius. Full group table in `GameArchitecture.md`.
The original text is kept below because the *reasoning* — a false positive and a false negative
together is never a threshold problem — is the reusable part.



**KEYWORDS: doorway proximity kSignObjectDist 2.5m South Gate Lowtown not tagged gate crystal false
positive +0x70 field sign group arrival marker LogSignTableOnce Door category portal map data S92**

`Entity::doorway` — the sole input to the Door/Shop split — is set in exactly ONE place
(`entity_postscan.cpp`): a **2.5 m ground-plane proximity test** between a scene object and any
`+0x70` field-sign record. Measured on the tester's Rabanastre map, from the `'` dump:

```
obj [0:15] kind=4 flags=00002134 nameIdx=-1 door=0 cat=8 "South Gate"          (124.00,-10.00,127.00)
obj [0:16] kind=4 flags=00002134 nameIdx=-1 door=0 cat=8 "Lowtown"             (137.82,-10.00,144.00)
obj [0:17] kind=4 flags=00032134 nameIdx=435 door=1 cat=4 "Rabanastre Crystal" (115.00,-10.00,151.00)
```

**It fails both ways at once.** The two actual portals — "South Gate" and "Lowtown", both field signs
with `nameIdx=-1` so their names come from `sceneObj+0xf8` — got `door=0` and stayed in Interactables.
The gate crystal, which is not a doorway, got `door=1`. So the East End observation the test was built
on (*"every doorway matched a record inside ~2 m; not one of its twins matched at all"*) **does not
generalise**, exactly as that comment's own caveat warned.

**Do NOT just widen the threshold.** A false positive and a false negative on one map is not a
distance problem — and `EnumerateFieldSignRaw` walks **every** `+0x70` group, including the arrival
markers `map_exits.h` records as group 3 on East End. An arrival marker sits where the party spawns,
which on this map is beside the gate crystal — the likeliest source of the false positive. Per-map
threshold tuning is also banned outright (solve globally).

**Nothing has ever printed that table**, so a distance problem, a wrong-group problem and a
record-that-does-not-exist are currently indistinguishable. Added `LogSignTableOnce` — dumps every
record (`group`, `index`, pos, `areaId`, `destIdx`, `usable`, `shown`) plus each candidate object's
nearest record and distance, once per map id, before the empty-table early-out. **Read that dump
before proposing any fix here.** The likely shapes: filter to the doorway group, or find the direct
object → `setfieldsignlocationjumpinfo` link and stop using geometry as a stand-in for a data
relationship. Tester's definition to satisfy: *"doors are portals that have map data, shops are doors
that also have signs with the same label within a close distance from them."*

### Session 92 — PLANNED, NOT BUILT: entity spatialization (the other seven sounds)

Recorded so the intent survives; **none of this exists in code.** The audio beacon (Session 92) plays
two of the nine sounds in `D:\Games\Dev\Custom\FFXII\FF 12 SFX\`. The other seven are for a **spatial
positioning system**: every scanned entity emits its type's sound from its own world position, and the
sound *moves with the entity*. That is a much larger feature than the beacon — many simultaneous
sources instead of one, live per-source tracking, and a real distance model.

**Asset inventory** (verified 2026-07-29 — ~~**all nine are mono, 44,100 Hz, 16-bit PCM**~~, so one
loader path covers every one; the originals are DAW-library exports that are 60–90% metadata and must
be stripped as `assets\*.wav` already are):

> **CORRECTED 2026-08-04 (Session 130).** The table below is stale in three ways, all measured from
> the WAV headers with the stdlib `wave` module rather than from file size:
> 1. There are now **twelve** files, not nine. `Gate_crystal.wav` (1.834 s), `save_crystal.wav`
>    (1.480 s) and `item.wav` (0.284 s) arrived 2026-08-03 and close the three gaps this section
>    calls pending — **every one of the ten real categories now has a sound.**
> 2. **`Gate_crystal.wav` and `save_crystal.wav` are STEREO.** "All nine are mono" is false. The
>    loader survives it — `audio_clips.cpp` passes `want.channels = 1` to `SDL_ConvertAudioSamples`
>    — but the two crystals are down-mixed, which is a fact about the assets, not a property of the
>    loader to rely on silently.
> 3. Durations are **highly non-uniform: 0.251 s to 1.834 s, a 7.3x spread.** `npc.wav` is 1.307 s.
>    A loop period must be derived from each clip's own length; a single shared period would either
>    truncate the long ones or leave the short ones silent. Full table in the Session 130 plan.

| File | Bytes | Category |
|---|---|---|
| `objective.wav` | 188,498 | **shipped** — route beacon |
| `Active_target.wav` | 176,138 | **shipped** — combat target beacon |
| `Entrance.wav` | 25,500 | `Category::Exit` |
| `door.wav` | 162,794 | `Category::Door` |
| `shop.wav` | 209,358 | `Category::Shop` |
| `Treasure.wav` | 39,418 | `Category::Treasure` |
| `npc.wav` | 279,824 | `Category::NPC` |
| `enemy.wav` | 293,120 | `Category::Enemy` |
| `interactable.wav` | 44,232 | `Category::Object` |

**The Door/Shop split (Session 92) was what closed the mapping gap.** Before it, `door`, `Entrance`,
`shop` and `interactable` were all finer-grained than the categories that existed, and `shop.wav` had
nowhere to go at all. **Still unmapped: `SaveCrystal`, `GateCrystal`, `Items`** — the tester is
sourcing sounds for those. That is a pending asset, not a design problem.

**Constraints already established that will bind the implementation:**

- `AudioEngine` today is a **single stream playing one sound at a time** (retrigger, not overlap). A
  many-source version needs either several streams or a real mixer, plus a nearest-N cull — do not
  assume it scales by being called more often.
- Entity identity is the **scene-object pointer**, not the label (`entity_scan.h`). A voice must key
  on that: `EntityList::OnFieldFrame` rebuilds the list constantly and labels get numbered suffixes.
- The **polled-monitor exemption** in `audio_beacon.h` covers one beacon. A per-entity version is a
  much bigger per-frame cost and needs its own approval and its own measurement.
- The pan / behind-attenuation / lowpass math in `audio_engine.cpp` **is the reusable part** — extend
  it, do not stand up a second spatialization path (CLAUDE.md CENTRALIZE).

**KEYWORDS: spatialization spatial audio entity sounds positional 3D audio FF 12 SFX objective.wav
Active_target.wav Entrance.wav door.wav shop.wav Treasure.wav npc.wav enemy.wav interactable.wav
SaveCrystal GateCrystal Items missing sound category mapping AudioEngine voices nearest-N cull
audio beacon SDL3 mono 44100 16-bit RCDATA beacon_assets.rc**

### Session 91 — STRUCK: pagination driven by an observed KEYPRESS; and the `FUN_003cb650` dialogue lead

**STATUS: IN TESTING (2026-07-29).** Multi-page dialogue pagination is **CONFIRMED IN PLAY** by the
tester on the new mechanism. The rest of the restructure has NOT been exercised yet and is what a
regression would show up in first:

- **`choice_reader` was rewired**, not left alone — same detectors and same speech, but it now learns
  the page from `NotePage(base, byteOffset)` fed by `dialogue_reader`, and `OptionCodec` seeks to the
  byte offset instead of counting `0x03` breaks. **Exercise a mid-dialogue Yes/No (the hunt
  petitioner) and the Hunt notice board.**
- **Tutorial / telop banners** used to be spoken by the content setter `FUN_002e16b0` directly; that
  hook is deleted and they now arrive through the page cursor like everything else. Same widget, so
  they should read normally — unconfirmed.
- If dialogue ever goes silent, grep the log for `DIALOGUE text widget outside the message-window
  registry` — that is the live-widget gate (`DAT_0215f200`) rejecting a page it should have spoken,
  and it is a one-line fix.

**KEYWORDS: dialogue pagination controller gamepad silent page 2 Space Enter SetConfirmCallback
WM_CONFIRM DIK_SPACE DIK_RETURN keyboard only XInput FUN_002a8c50 0x188C50 widget+0x8A page cursor
text dispatch table PTR_FUN_009164c8 0x7F64C8 slot 0 slot 2 FUN_002a9980 type 0 type 1 null
DAT_0215f200 0x203F200 message window registry FUN_003cb650 0x2AB650 struck telop FUN_002e16b0
NextPage g_pageIdx dialogue_reader**

**Struck: advancing the spoken dialogue page when the mod sees Space or Enter go down.** Shipped in
Session 52, and it made multi-page dialogue **keyboard-only**: `InputTracker` reads the DirectInput
*keyboard* buffer (`dinput8_proxy.cpp` only records `GUID_SysKeyboard` devices) while the game reads
pads through **XInput**, which the mod does not touch. A controller player heard page 1 and silence
after it. The mod was never observing "the box advanced" — only "a key that usually advances it".

**Also struck: the page INDEX model.** Counting one page per Confirm is wrong even on a keyboard —
the *first* press on a page skips the typewriter reveal without turning it, so the count drifts.

**The replacement is a CURSOR, not an event to catch.** `widget+0x8A` is the byte offset of the page
on screen, and **`FUN_002a8c50` (RVA `0x188C50`) is its writer** (`:199-200` advances it past a
`0x03` break; `:77` starts the walk from it). Whatever moved it — pad, keyboard, mouse — moved it.

**What settled which function to hook** was the text-draw dispatch table `PTR_FUN_009164c8`
(RVA `0x7F64C8`), already dumped to `FFXII-Decompile\output\text_dispatch_table.txt`:
`FUN_002a8c50` is **slot 0 of BOTH** widget types, while `FUN_002a9980` — the tick `choice_reader`
hooks — is **slot 2, which is NULL on type 1 (plain)**. That is the whole shape of the bug: the
existing per-frame dialogue hook structurally could not see a choice-less dialogue box.

**Struck: `FUN_003cb650` (RVA `0x2AB650`) case 1 / case 0x20 as "the best unverified lead".** Carried
as the way forward in `GameArchitecture.md` (twice) and `sessions_001_050.md` for six sessions. It is
a *different* window singleton (`DAT_02b47760`) whose text is a pre-compiled glyph resource we cannot
decode, so a correct advance event there would still carry no readable page. **The answer was on an
offset `choice_reader.cpp` was already reading in play** (`OFF_W_OFFSET = 0x8A`, used live since
Session 87) — the lead was chased instead of the code being re-read.

**Lesson.** When a feature "needs an event", check whether the game already keeps the *state* that
event would announce. A cursor you can read beats an event you have to catch: it needs no
subscription, cannot fire twice, and cannot desync from what is on screen.

### Session 84 — STRUCK: include-by-KIND+model (`present`). ONE route, THREE symptoms

**KEYWORDS: present route include by kind model HasModel shadow NPC bare NPC n phantom stacked bodies
party slots enemies in NPC category DropUnplacedCharacters kFloatingDrop kNpcReachTol Session 79
widening nameless interactable icon**

**Struck: `present = (kind == 1 || kind == 5) && HasModel(obj)` in `entity_scan.cpp`.** Added in
Session 79 to surface characters the scan rejected. It caused three separate reported defects:

1. **Shadow NPCs.** Unnamed `kind=5` characters standing 0.6–1.3 m from a named NPC entered the list
   and spoke as bare `NPC n` — 40+ across four maps. Tester: *"Masui is 'Nomad2' in my game so she has
   a classification. She just also now has an extra NPC shadow that never gets named, even after
   talking to her."*
2. **Three stacked bodies on EVERY map.** Unnamed `kind=1` objects sharing one authored coordinate:
   Lowtown `[0:24-26]` (92.00,−0.19,51.77) · Eastgate `[0:75-77]` (200.00,−10.00,81.00) · Nomad
   Village `[0:32-34]` (37.89,**6.06**,71.87) · Garamsythe `[0:15-17]` (32.52,0.00,121.96).
   `inclusion: kind1=3` on all four.
3. **Enemies classified as NPCs.** A field enemy is a character with a loaded model, so `present`
   admitted it from the handle table, where `ClassifyByNameKey`'s `isCharacter` test files it as
   `Category::NPC`. `BuildLocked` runs before `ScanCombatants`, which skips anything `AlreadyListed`
   — so the dedicated enemy classifier (faction from the actor pool) never saw it. Before `present`,
   an enemy failed every route and fell through to the pool, which is why it used to work.

**Also struck — Session 79's founding premise.** It claimed an unnamed woman behind the Nomad Elder's
tent was "reachable by NO inclusion path" and built the widening for her. She was listed the whole
time under her own npcdic name (`Nomad N`); what the widening added was her **shadow**. Everything
built on that premise across Sessions 79–83 rests on a fact that was never checked.

**Also struck — `DropUnplacedCharacters` + `kFloatingDrop` (2.0) + `kNpcReachTol` (1.5), Session 83.**
It dropped an unnamed NPC floating >2 m above its floor or outside the reachable set. It appeared to
fix Nomad Village and nothing else, and the reason is #2 above: that map's three stacked bodies are
the only ones 6 m in the air. The identical three on the other maps stand on the floor, inside the
reachable set, and the same code looked at them and kept them. **The code was never map-specific; its
EFFECT was — which is what a threshold read off one map's dump will always be.**

**Replaced by one test:** `if (!named && !interactive) continue;` — the game names it, or the engine
offers an interaction on it. Basis, from the tester: *"there is no such thing as an interactive but
nameless object. Every object in the game has a little icon that says something like 'action: nomad'
… no objects are interactable that don't have one of these."* Verified against all 130 dumped objects
across four maps: every real NPC is `nameIdx≥0, flags=0x…0004, avail=1`; every phantom is
`nameIdx=-1, flags=0x…0000, avail=0`.

**Known accepted cost, MEASURED not argued:** `+0x1C` is mode state and a disabled object reads zero
flags, so a story-gated town gate that carries no name is not listed until the script arms it (the
Session 54 case). `s_dropPayload` counts every drop that carried a real `+0xCC`/`+0xDC` payload id —
non-zero means the cost was actually paid and the gate needs a route back keyed on that field.

**LESSON (fourth session running to pay for it): when a filter keeps needing new evidence to justify
itself, check whether the thing it filters should be in the list at all.** Sessions 81, 82 and 83 each
guessed what these objects *are* — party members, roster bodies, story-gated bodies — and each guess
was refuted. The question was never what they are.

### Session 84 — STRUCK: "on East End `+0x70` covers a doorway no `__MJ_CTRL` routine owns"

**KEYWORDS: East End missing exit +0x70 field sign completeness source __MJ_CTRL incomplete gate**

`map_exits.h` justified treating the `+0x70` field-sign array as an exit *completeness* source with a
single piece of evidence: an East End doorway supposedly invisible to the map-jump reader. **There is
no missing exit in East End.** The thing being hunted there was a GATE, and it was not in that area at
all. Left standing, that sentence invites a second exit source on a map that does not need one —
which is how the `+0x54` ∪ `+0x70` union got built and refuted in Session 55.

`__MJ_CTRL` may still be incomplete (the tester reports missing exits in the Waterway), but that must
be measured. `ScanExits` now logs a full surface inventory including any walkmap map-jump group **no
controller claims** — which is exactly what an exit the script reader cannot see would look like.

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
matrix `DAT_02aedf30` row 2. (bit 2 of the flag byte, the skip-move-facing flag, is script-only — NOT a combat lock,
so don't look for a lock flag.)

## Solved Problems

### Dialogue choices silent on highlight - SOLVED (Session 175, 2026-08-30), PLAY-CONFIRMED

**PLAY-CONFIRMED 2026-08-30 by the user: "the dialogue choice works".** Which prompts were
exercised is not recorded, so this closes the defect but does NOT separately attest the notice
board, the gate-crystal destination list or the Draklor lift - see the S175 session entry.

KEYWORDS: dialogue choice options not spoken on highlight Archades Commit this tale to memory
child list window window+0xC0 bit 22 0x400000 widget+0xB0 widget+0x54 window+0x124 FUN_002b2ce0
FUN_002a5590 FUN_002a9980 FUN_002a6190 OnFocus two detectors g_dispatchCovers stale page NotePage
choice SILENT no 0x0E block on this page notice board gate crystal teleport destinations SOLVED

**Reported:** an Archades conversation where a 2-option prompt read its opening option and then
nothing. Player, asked what they heard: *"commit this tale to memory was spoken as the initially
focused option, but neither option was spoken on highlight"* - including moving back onto option 0.

**THE CHAIN OF EVIDENCE, in the order it actually settled things** (L-79 - do this before any
"the mod cannot see this surface" chase):

1. `FFXII-Screen-Reader-Latest.log` had the answer printed: `choice SILENT (no 0x0E block on this
   page ...) wnd=2D8E9E40 a=0 b=156`, where `b` is the byte offset scanned - and `156` is the offset
   of the page spoken **2.5 s earlier**. The reader was searching the previous page.
2. `2D8E9F10 - 2D8E9E40 = 0xD0`, so window and text widget were the ordinary pair; nothing exotic.
3. Corpus sweep: exactly two detectors ever emit. `dialogue-choice[...]` (the per-frame tick, keyed
   on `widget+0x58`) and `choice[...]` (the `0x8000` dispatch). The gate crystal shows BOTH on one
   window: Save/Teleport via the tick, the 26 destinations via the dispatch. Nothing in the corpus
   shows either detector covering both.
4. The decompile said why. See `GameArchitecture.md`: `FUN_002a5590` builds a **child list window**
   at `window+0xC0` for some prompts and then sets **bit 22 of `window+0x180`** - which is
   `widget+0xB0` - and `FUN_002a9980` tests that bit first thing in mode 2 and returns. So
   `widget+0x58` never moves for a child-list prompt, and the dispatch never fires for an inline
   one. **Each detector was blind to exactly the half the other saw**, and the `g_dispatchCovers`
   stand-down flag had been hiding it.
5. Why the dispatch detector could not cover its own half either: it scanned a message snapshot
   pushed in by `DialogueReader::NotePage`, and that call sat **below** `EmitPage`'s printability
   bail. A page carrying nothing but an option block decodes to no text at all
   (`GameText::ControlLength(0x0E)` returns -1), so the bail fired, `NotePage` never ran, and the
   snapshot stayed on the previous page for as long as the prompt was up.

**The fix is a deletion, not a repair.** Both flavours resolve the highlight through the same helper
`FUN_002b2ce0` into the same address (`widget+0x54` **is** `window+0x124`), so the per-frame tick
reads that one field and there is exactly one detector. Gone with the second one: `ChoiceReader::
OnFocus`, the cached message and its page offset, `g_dispatchCovers`, and `AbsoluteIndex` (our own
re-implementation of `FUN_002b2ce0`, which existed only because the dispatch hook ran BEFORE the
game's handler and read `window+0x124` stale). `choice_reader.cpp` 597 -> 470 lines.

**A mode gate had to be added with it.** `+0x54` means different things per mode; in mode 0/5 it is
the park reason, and `3` (a page break) would have read as "option 3" on every page turn. Only
modes 2 and 4 are selections.

**Regression evidence, offline.** The `0x0E` block walk moved to `src/ui/choice_block.h` (pure - no
Windows, no GameText) so it can be built and asserted without the game. 24 checks over the shapes
the corpus records: block on page 0 behind a question (notice board, teleport list); block on a
later page behind a question (Nilbasse); **block on a page with no text of its own** (this defect);
two blocks in one message, each page selecting its own and never the other; capacity byte larger
than the real list; and each bounds refusal naming itself. All pass.

**Instruments shipped with the fix, both deliberately deletable once read:**
- `STALL_SCOPE("ChoiceReader::HookedChoiceTick")` moved to the TOP of the hook. It used to sit below
  the change-check, so `[PERF]` counted **emissions, not calls** - which is why no log in the corpus
  could say how often the tick runs while a child-list prompt is up, the one question the redesign
  turned on. It now states the call rate outright.
- `dialogue-choice[slot/count] child=<0|1>` names which flavour laid the list out, so one grep shows
  both reaching the same speaker.
- `[DIALOGUE] page[...] carries no speakable text -- noted for the choice reader, not spoken`, once
  per page turn, is the direct print of step 5 above.

**OPEN, found in passing and deliberately NOT fixed here (L-31 - no log shows it firing).**
`GameText::DecodePages` drops empty pages, so when the page at the cursor is blank `EmitPage` speaks
`pages.front()` - a page the player is not looking at. Unobserved: in this log the options page was
the LAST page, so the list came back empty and nothing was spoken. A message with a bare option page
followed by more text would read the wrong screen aloud. The fix would be a public passthrough to
the existing internal `DecodeToPages` (which keeps the empties) and taking element 0.

Problems that were resolved. Each entry has `KEYWORDS:` + `SOLUTION:`. Check this to
reuse known-good solutions.

**KEYWORDS: audio beacon pan mirrored flipped reversed left right stereo BearingToPan
RelativeBearingDeg atan2 dx dz CompassFaceDeg negation sin cos front back northwest panned right
two encodings computed twice S92**

**The audio beacon panned the MIRROR of what the route spoke.** Route to the south gate said
"Northwest"; the ping sat hard right. Both the beacon and the spoken leg took `facingRad` from the
same `ReadCameraForwardStable` call, which was wrongly assumed to make disagreement impossible —
**sharing an input is not sharing the answer.** `BearingToPan` re-derived the bearing as
`atan2(dx, dz) - facingRad` while `nav_common.cpp` uses `atan2(dx, -dz) - CompassFaceDeg(facingRad)`
where `CompassFaceDeg == 180 - yaw`. Those are not two spellings of one expression: the second is the
**exact negation** of the first. Negation preserves `cos` and flips `sin`, so **front/back stayed
correct and only left/right inverted** — the failure mode that survives a code read, because every
term is present and individually plausible. SOLUTION: the real defect was that
`Norm360(BearingDeg - CompassFaceDeg)` existed **six times as an unnamed inline expression**, which
is what made a seventh wrong copy the easy path. Promoted to one shipped function
`NavCommon::RelativeBearingDeg(from, to, facingRad)` (degrees, 0 = forward, 90 = right, 180 = behind,
270 = left); `CardinalBearingRelative`, `EgoBearing`, `RelativeOctant` and `BearingToPan` all call it.
`AudioBeacon::Seed` now logs the first leg's octant beside its pan so the next sign error is a grep,
not a play session — octants 1-3 pan positive, 5-7 negative, 0 and 4 near zero. **A value two
subsystems must agree on needs a NAME, not a convention.**

**KEYWORDS: South Gate Lowtown gate not classified Door doorway 2.5m radius refuted kSignObjectDist
kSignMatchDist 8m group 0 doorway group 1 walk-onto group 2 gate crystal teleport group 3 arrival
nearest wins per record 3.6-6.4m arrow offset two constants one fact S92**

**Gates outside Rabanastre's interiors were never classified as Doors.** "South Gate" and "Lowtown"
stayed in Interactables while Lowtown's own map classified doors correctly. `Entity::doorway` had one
writer: a **2.5 m radius test against ANY `+0x70` record**. Measured on map 306:

```
g0[0] (119.95,-10,127.00) destIdx=20 -> "South Gate"          (124.00,-10,127.00) = 4.05m  MISSED
g0[1] (138.24,-10,140.53) destIdx=21 -> "Lowtown"             (137.82,-10,144.00) = 3.50m  MISSED
g2[0] (115.00,-10,151.00)            -> "Rabanastre Crystal"  (115.00,-10,151.00) = 0.00m  tagged
g1[0] (112.12,-10,198.00) areaId=14  -> (walk-onto exit surface)                  = 47m
```

SOLUTION, two independent faults: **(1) only group 0 may tag.** Group 1 is the walk-onto map-jump
surface (already `Category::Exit`), group 2 is a gate crystal's own teleport record, group 3 arrival
markers — consulting all of them is what tagged the crystal, killed by DATA rather than by category
precedence. **(2) nearest-wins per record, not everything-in-radius.** A group-0 record marks the "→
area" ARROW, so it is *never* co-located with the door — 3.5/4.05 m here, 3.6–6.4 m already recorded
on East End, against ~24–25 m to the next candidate. Each record claims its closest eligible
non-NPC object; `kSignMatchDist = 8.0f` becomes a sanity bound, not a discriminator. Loop inverted to
records-outer: per record there is exactly one door, per object the question is ill-posed. Unused
group-0 slots read exactly `(0,0,0)` — filter on position, **not on `shown`**, which is a live render
gate and would make classification depend on camera direction.

**Two lessons.** First: *a false positive and a false negative on the same map is never a threshold
problem* — widening 2.5 m would have papered over a wrong-group bug. Second, and worse: the refuting
constant was **ten lines above the broken one in the same header**. `kSignMatchDist = 8.0f` already
carried the note *"a sign marks the '→ area' arrow and the slot is the volume you step into, so they
are never coincident: measured 3.6-6.4 m apart on every East End district door"*. Two constants for
one geometric fact, and the tighter, wrong one was load-bearing. Lowtown's map passed only because its
records happen to fall inside 2.5 m — **the threshold was never right, it was lucky**, which is why
this looked map-specific. `kSignObjectDist` is struck in place.

**KEYWORDS: gate crystal Save Crystal wrong category npcdic id 435 Rabanastre Crystal 435-465 area
gate crystals ClassifyByNameKey band GameArchitecture already correct read before you dig rescan
tally missing Door Shop columns S92**

**The gate crystal was announced under Save Crystal, and the Gate Crystal filter read 0.**
`ClassifyByNameKey` mapped npcdic ids `435-459` to `SaveCrystal` on the vague comment "area/life
crystals". The tester's crystal has id **435**, which the game's own npcdic table names **"Rabanastre
Crystal"** — a per-area teleport crystal. SOLUTION: read the bands straight off the game's name table
(`FFXII-Decompile\notes\npcdic_names.csv`): **435-459 are the 25 named area crystals** (Rabanastre …
Ridorana), **460-465** unused `(Crystal 26)`-`(Crystal 31)` placeholders, **466** the generic "Gate
Crystal" — so `435-466 → GateCrystal`, and only **467 Life Crystal / 469 Save Crystal → SaveCrystal**.
Conf 1.00, no probe: the game supplies the names.

**The lesson is not the band, it is where the answer already was.** `GameArchitecture.md` stated
*"435–465 = area gate crystals"* **correctly** under "Object name (master data)" — and, 130 lines
later, restated it mushily as *"435–459/467 crystals"*. The code implemented the mush. Both entries
were in the canonical registry, so grepping it was not enough; the vaguer of two statements of one
fact is a bug waiting to be built on. **When you restate a fact you already recorded, restate it
exactly or point at the original.** The mushy line is now struck in place with the full table.

Contributing cause: the `rescan:` tally in `entity_scan.cpp` was never given **Door=/Shop=** columns
when those categories were added, so it read `Save=1 Gate=0` and the two buckets an object could have
been promoted into were invisible. The tally now counts every category and sums to `out.size()` — **a
breakdown that does not add up hides the bug it exists to expose.**

**KEYWORDS: gate crystal classified as Door category promotion doorway hasNameSign
TagDoorwaysAndDropSignTwins npcdic 466 ClassifyByNameKey heuristic overwrote positive
identification Object refine S92**

**The first gate crystal the tester reached was filed under Doors.** `ClassifyByNameKey` identifies
it correctly from the game's own npcdic id (466 → `Category::GateCrystal`), and the new Door/Shop
promotion then overwrote it — that loop re-categorised **any** entity with `doorway == true` that
was not an NPC. A gate crystal teleports, so the map script binds it a
`setfieldsignlocationjumpinfo` record and `doorway` is genuinely TRUE. SOLUTION: the flag was never
the bug, the promotion's reach was — **Door/Shop now refine `Category::Object` ONLY**, the bucket the
classifier uses when it recognised nothing. Every other category is a positive identification off
the game's own name id or the character class, and this pass decides category from PROXIMITY to a
field-sign record. **A proximity heuristic must never overwrite a name the game supplied.** Clearing
`doorway` instead would have been the wrong repair: the sign-twin dedup keys on that flag rather than
on category and would have begun leaking duplicate shop signs. Near-miss worth noting — the old guard
was `!= NPC`, the right instinct (a person near a shop sign is not a door) applied to one category
instead of stated as the general rule.

**KEYWORDS: empty category SHIELDS spoke previous category row stale paint Leather Cap helm
IsEmptyCategory row array null +0xE0 FUN_0057cf20 scroll count clamped 1 generic painted cell
TryFocus claim silent STRUCK empty categories do not occur S89**

**An EMPTY category announced the PREVIOUS category's row.** SHIELDS (no shield owned) said
`"SHIELDS"` then `"Leather Cap"` — a helm, not highlighted, not a shield. The log identified the
speaker: populated categories emit `[INV] item:` (`InventoryReader::TryFocus`), but the empty one
emitted `[READER] item:` — the **generic painted-cell path**. `TryFocus` had declined the empty list,
so `menu_reader.cpp:365` fell through to the generic reader, which reads the cell's last-drawn text.
SOLUTION: the game *builds* empty categories rather than filtering them — `FUN_0057cf20:253-257`
frees the row buffer and returns NULL at zero rows (`+0xE0` = null), then `FUN_005655f0:49-52`
**clamps the scroll widget's row count from 0 to 1**, so the widget reports a phantom row at index 0.
A null `+0xE0` with the scroll widget and tab table still present is the ONLY empty signal. New
`IsEmptyCategory()` detects it; `TryFocus` returns **true** (claims the focus) and speaks **nothing**,
so the generic path never sees the cell. Populated categories are untouched — `ReadList` succeeds and
the first row still announces on the switch. **Do NOT "fix" this by making the generic path smarter**;
it has no way to know the cell is stale. **STRUCK by this:** "empty categories do not occur"
(`GameArchitecture.md`, S70) — that rested on "every `[cat]` had n≥1", a property of the sample.

**KEYWORDS: shop category tab silent not spoken WEAPONS AMMUNITION LOOT interrupted cut off mid-word
two speakers race Speech::Output interrupt queue g_queueNextItem ConsumeCategoryAnnounce
FUN_005655f0 FUN_0056e410 FUN_0056ded0 FUN_0056e5d0 inaudible S88**

**Shop tab switches said nothing about the category — but the category was already being spoken.**
The mod log showed it resolved, logged AND uttered (`[INV] category: "WEAPONS"` + `[SPEAK-OUT]
WEAPONS`), immediately followed in the SAME millisecond by `[SHOP] item:` + its `[SPEAK-OUT]`, with
`Speech::Output calls=2`. The row line ran `interrupt=true` ~0.2 ms later and cancelled the category
before a syllable was heard. SOLUTION: the shop's tabs route through the same shared refresh the
party item lists use (`FUN_0056ded0:82 -> FUN_0056e410:50 -> FUN_005655f0`, then `FUN_0056ded0:83 ->
FUN_0056e5d0`), and `InventoryReader` had already solved this with `g_queueNextItem` — but the flag
was file-static so `shop_reader` could not see it. `OnCategoryRefresh` now also records the owning
surface and exposes `InventoryReader::ConsumeCategoryAnnounce(owner)`; `ShopReader::OnShopHighlight`
consults it and speaks its row QUEUED behind the category, clearing `g_lastItemId` so the new tab's
row always speaks. Owner-scoped, so one surface can never consume another's announcement. The party
path (`TryFocus`, `g_queueNextItem`) was deliberately NOT edited. **Diagnostic marker:** a
`[SHOP] item (queued):` line is the handshake firing; a bare `item:` straight after a `category:`
means it did not. **Do NOT "fix" this by reading the tabs again inside `shop_reader.cpp`** — that
duplicates the chain and re-creates the same race one layer down.

**LESSON: a feature can be fully implemented, logging correctly, and still be inaudible.** Read the
log for the *utterance* before concluding a feature was never built; when two readers speak on one
surface, find out who interrupts whom before adding a third.

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
segment/exit queries). **The speed change was the tester's own keypress** (~~`1`/`2`/`3` are Game Speed 1×/2×/4×~~ -- STRUCK
S183: keyboard `1` = pad L1, which cycles game speed; `3` = R1; `2` no observed effect; see Controls.md). The
mod reserves none of `1`/`2`/`3`.
**AUDIT UPDATE, Session 100:** the read-only rule now carries ONE recorded, user-authorized
exception — Auto-walk (`AutoWalk::OnDevicePoll`, dinput8_proxy.cpp) may OR the W/A/S/D bits into
the keyboard buffer while engaged, default OFF, with the tracker still fed the PRE-injection state.
Everything else in this audit still holds: no SendInput, no memory writes, nothing swallowed. See
CLAUDE.md's amended rule for the full boundary list.

**AUDIT UPDATE, Session 162 — a SECOND exception, and it is the opposite direction.** The gamepad
intercept (`PadRouter::OnPoll`, reached only from the `XInputGetState` IAT hook in
`src\input\pad_hook.cpp`) may **CONSUME** pad input: clear a button bit or zero a stick axis in the
`XINPUT_STATE` the game is about to read. It may **never set one** — that is the category line
against Auto-walk, and it is what keeps "the mod cannot press a button for you" true.

So **"nothing swallowed" above is no longer unqualified**: on the KEYBOARD it still holds absolutely
(the mod passes that buffer as `const`), and on the PAD the mod now swallows exactly what it claims.
Do not quote the keyboard sentence as if it covered the pad.

Bounds: one function writes an `XINPUT_STATE` and nothing else does; the router is fed the
PRE-consumption state; with the `Controller` mod-menu row off or no pad present `OnPoll` returns on
its first line (byte-identical, not merely skipped); a fault latches the intercept OFF for the
session; game-foreground gated; and what may be consumed is decided on the GAME thread and published
stamped, expiring ~250 ms after the field tick stops. Full list in CLAUDE.md.

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
transport, not a shared contract. PARTY menu = window class `FUN_002a6190` (0x186190; ALL submenus);
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

zeroes a 0x148-byte stack record, fills it via `FUN_00385df0(bc, rec)`, and then calls the applier
`FUN_003112f0(rec, 0, bc, 0xFFFF)` — **attacker 0 and action id `0xFFFF`**, i.e. a synthetic
no-attacker call, once per actor per frame.

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
- **STRUCK (S149): `FUN_00323600` does NOT answer "can this node be bought."** It has no adjacency
  test of any kind, so its `0`/`9` means "you own the LP", not "you may spend it" — and the board
  only lets you buy a node touching one you already own. Shipped as availability, it announced "can
  learn" on nodes the game then rejected with the invalid-action sound. The purchasability bit is
  `cell+0x18 & 0x1000`, set by `FUN_0055e090` and tested by the Confirm branch `FUN_0055cd40` case
  `0xc` / `0x8001`. See the S149 Solved Problems entry.

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

### 1. The multi-item reward panel — ✅ SOLVED (Session 178), PLAY-CONFIRMED 2026-09-15

> ✅ **PLAY-CONFIRMED on the first payout:** `[REWARD] reward panel: title="Antlion Infestation"
> rows=3 | [id=0xFFFF value=4300 "4300 gil"] [id=0x1173 value=1 "Bubble Belt"] [id=0x20E2 value=1
> "Sickle-Blade"]`, then a single `SPEAK-OUT`. It is followed by a harmless log-only `[READER]
> unclaimed pane: obj0 RVA=0x2D4330`, because the menu census cannot see readers outside
> `menu_reader`.
>
> **Still unplayed, with the user's expectations noted:**
> - **A key-item payout** should read. Key items route to the obtain toast, which already speaks key
>   items.
> - **The menu opener `FUN_0057a4e0`** is likely the Clan Centurio hunt-board turn-in. It uses the
>   same reader.
> - **`queststartwindow`** is likely hunt acceptance. It has not been traced and is low priority.
>
> Check any silence report on these against a `[REWARD]` / `[MSGTEXT] item:` line first.

> ⛔ **S178: THE S147 "STRIKE" BELOW IS ITSELF STRUCK. S72 WAS RIGHT.** The panel is **not**
> `FUN_0035e070`. It is **`FUN_003f4330`**, opened by the script native **`questresultwindow`**
> (`0x37C`) via `FUN_00344c60` -> `FUN_00290130` -> result window `FUN_003f4e70` -> sequencer
> `FUN_003f4840`. The panel holds the title at `+0xC0`, the row count at `+0xC8`, and rows at
> `+0xCC`/`+0xD0` (id `0xFFFF` = gil). A hunt's KEY items (`0x8xxx`) go to a second window that
> hands them to the `FUN_0035e070` toast, which is the only part S147's function ever touched.
> Reader: `src\ui\reward_panel_reader.cpp`. Layout and chain: `GameArchitecture.md` §Session 178.
>
> **How S147 went wrong:** it read the body of the function the mod already hooked and found a row
> loop, a quantity and a gil kind — three of the report's features — and struck S72 without checking
> the feature S72 listed FIRST, the title line. The composed text in our own logs (`"You obtain a
> Wind Globe!"`) has no title. **What found it:** grepping the `.dbg` native names for the surface's
> own words (`gil`, `win`, `clan`) turned up `questresultwindow` in one command (`L-85`).
>
> **Settles in play with:** `[REWARD] reward panel: title="…" rows=N | [id=… value=… "…"]` followed by
> the spoken line. `no row resolved, staying silent` means `DefName(1, id<<16)` failed on that id
> class. No `[REWARD]` line at all on a hunt payout means the hook never fired.


> ⚠ **STRUCK, Session 147:** ~~"It is **not** the single-item obtained toast the mod already reads
> (`message_reader.cpp`, `FUN_0035e070`, text at `widget+0xC8`) — that one is a one-line toast with
> no title and no quantity column. This is a multi-row list with a heading."~~
>
> **It IS that function.** `FUN_0035e070` has a row loop, a separate quantity field and a gil kind —
> everything this entry said it lacked. The claim was made from the mod's own one-line *usage* of the
> function rather than from its body, and it then stood for 75 sessions as a reason not to look there.
> Descriptor layout, row kinds and the `0F 31` composition are in `GameArchitecture.md` §Session 147.
>
> **WHAT IS STILL OPEN is why it is silent, and that has NOT been guessed at.** Across all twenty
> archived dev logs the reader's init line appears twenty times and its
> `diag: item popup proc FUN_0035e070 fired` line appears **zero** times — so there is no evidence the
> surface was ever visited in a dev session, and the S143 lesson applies: *before instrumenting a
> refusal, confirm the surface was even visited.* S147 shipped the descriptor logging (mode, row
> count, layout flags, and every row as `[item N xQ]` / `[gil N]`) plus an explicit line when the
> composed buffer decodes to nothing. One hunt reward now separates "never fired", "fired with an
> empty descriptor" and "fired but the text would not decode".
>
> The original observations below are kept because the SHAPE they describe is confirmed correct —
> only the "different surface" conclusion was wrong.

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

## Funnel polarity — a WORKAROUND that hid a sign error for four sessions (Session 86, 2026-07-28)

**KEYWORDS: funnel polarity FLIPPED as-labelled string-pull portal left right TriArea2 dtTriArea2D
winding routing through walls impassable terrain unwalkable corners portals Recast Mononen**

**DO NOT REINTRODUCE `FunnelBestPolarity`'s "run it both ways and keep the shorter path".** It looked
like a robustness win and it was a bug amplifier.

> **FINISHED IN S124.** The S86 sign fix landed, but the length SELECTION survived in
> `PathFunnel::BestPolarity` (reframed as a "self-check") — and on map 315's south bank the mirrored
> run won again, by 1.6%, with a 61 m chord across the flood: four sessions of replan "No path"
> (S100/S115/S116/S123-era logs), the marker printing every time. `BestPolarity` now returns the
> as-labelled polyline unconditionally; the mirrored run is an instrument only, and a mirrored-shorter
> measurement logs `MESH LABELLING ANOMALY`. See the Sluiceway table, row 15.

What it did: ran the funnel with the portals as labelled and again with every portal mirrored, then
kept whichever produced the shorter path. The comment justified it honestly — the author had derived
the sign convention twice, traced both branches against the reference twice, and the log still
disagreed — so the sign "stopped being an argument to win".

Why it was wrong, three ways:

1. **It was not a measurement.** The log from the Giza Plains session: 75 routes chose FLIPPED, 14
   chose as-labelled, and *every one of those 14 had ≤1 portal* — where mirroring cannot change the
   length and the strict `<` comparison decides it. So 75 of 75 real routes flipped. A constant
   wearing a measurement's clothes.
2. **It hid the actual fault**, which was one character wide: `TriArea2` is the exact negation of the
   reference `dtTriArea2D` the funnel's four comparisons were transcribed from. Negated helper +
   verbatim comparisons = the funnel's whole notion of left and right inverted.
3. **It made a per-portal error unrecoverable and then PREFERRED it.** A global mirror cannot fix one
   mislabelled portal, and shortest-wins actively selects the corrupted run — a funnel that accepts a
   bound on the wrong side cuts *through* the wall, so the invalid path is the shorter one. The route
   through impassable terrain was chosen *because* it was invalid.

**Why nothing caught it for four sessions:** both shipped invariants (`lenKept > midLen`,
`lenKept > 1.8 × straight`) are LENGTH tests, and an out-of-corridor path is shorter, not longer.
They sat at zero hits through the entire session that produced the bug report. The replacement is
`MapQuery::SegmentClear` walked over consecutive corners — ask whether the party can actually walk the
leg, rather than whether the number looks plausible.

**General lesson, the fourth time this project has hit it:** when a fix takes the form "try both and
keep whichever looks better", the thing being avoided is a fact you have not established. Establish
it. Here the fact was free — the mesh winding pins portal left/right exactly (`GameArchitecture.md`,
"VERTEX WINDING").

### The sign was only HALF of it — a portal is an OPENING, not an edge

The sign fix was confirmed correct in play (`FLIPPED` 0, `as-labelled` 26) **and routes still crossed
walls.** `CORRIDOR BREACH` fired 20 times on its first outing, always `leg 1/1` — the funnel had
produced the plain straight line, and that line genuinely does cross every portal in order. **The
string-pull was right; the corridor was wrong.**

`EdgePassable` probed **one short straddle at the edge midpoint**. On this mesh a triangle is often an
entire corridor and shared edges run 8-16 m, so A\* certified that a crossing *exists* and said nothing
about *where*. Measured on the failing route: the taut path crossed portal 4 **4.8 m** from the only
point that had been tested, and portal 5 **2.7 m** away. The obstacle was in the untested part.

**Do not "optimise" `EdgeClearSpan` back down to a single probe.** The midpoint probe is exactly the
version that shipped this bug. Specifically:

- Sample the edge (`kEdgeSamples = 7`) and clip the portal to the clear sub-span; the funnel then
  cannot thread a blocked part, because that part is no longer inside any portal.
- Clip to the **longest run** of clear samples, never to "all clear samples" — a pillar mid-edge
  leaves two gaps, and a portal spanning both re-creates the bug through the pillar.
- `EdgePassable` passes on **any** clear sample. "Is the midpoint clear" also *falsely rejected*
  doorways whose middle is blocked, which is the same error in the other direction.

### The breach detector measured the FLOOR — false positive on 100% of routes

**STRUCK: the reading that "20 breaches" evidenced obstacles in the untested parts of portals.** The
count was real; the interpretation was not. Routing is confirmed working in play after the sign +
clipping fixes, and `CORRIDOR BREACH` was firing on *every* route regardless.

`MapQuery::SegmentHit` flattens both endpoints to `from.y`, so passing it raw path points casts the
ray **along the ground**, where it clips the terrain the path stands on. The giveaway was in the log:

```
CORRIDOR BREACH on leg 1/3 -- (49.1,84.1) -> (50.0,84.0) is not walkable
```

— a **0.9 m** leg starting at the player's own feet, on a route that was then walked to the end.

**Any new caller of `SegmentClear` must add the `0.9f` body pad**, exactly as `NavMesh::StraddleAt`
and `entity_commands.cpp:85` do. Without it the test answers "is there floor here", not "can the
party walk here".

**Lesson: a diagnostic is code and needs its own falsifier.** This one was believed on sight because
it confirmed the reported symptom, then used to justify a theory. The disproof was free — a 0.9 m leg
under the player's feet cannot be unwalkable. When a new instrument fires on everything, suspect the
instrument first.

**And note what this means in general: the navmesh alone cannot answer "is this line walkable".** The
walkmap triangles are coarse floor polys; the real walls are collision volumes the mesh knows nothing
about. Only the walk-class segment test sees them. Any future routing change that reasons purely from
triangle adjacency will reproduce this class of bug.

## Party membership: ask roster list 3, never a kind byte (Session 86, 2026-07-28)

**KEYWORDS: party member enemy NPC classification faction scene kind nibble BtlChr roster list 3
Penelo Urstrix Hyena handle table actor pool overlap Category::Enemy**

**The scene-kind nibble (`sceneObj+0x0E & 0x0F`) does NOT separate party from enemy on the field.**
The Giza Plains dump reads `kind=1` for Penelo *and* for all three enemies present. `entity_scan.cpp`
called it "the game's own faction test" — **STRUCK**; that comment now carries the correction.
`phyre_types.h:113` had already flagged the constant as unverified.

Two things that DO work, both already in `battle_state.h`:

- **Party membership = presence in roster list 3** (`BtlWork+0x5A7E`, nine u16 BtlChr indices, via
  `BattleState::BtlChrForSlot`). It is the game's own party list; membership is a lookup, not an
  inference, and it cannot mistake an enemy for a party member.
- **Faction = `BattleState::FactionOf(actor)`**, which reads the BtlChr kind byte *before* it ever
  falls back to the scene nibble.

Related trap, and the reason the fix took the shape it did: **the handle-table walk wins every tie.**
`BuildLocked` runs before `ScanCombatants`, and `ScanCombatants` skips anything `AlreadyListed` — so
any widening of the handle-table admission rule silently takes the faction classifier's inputs away.
Session 84 struck one such route (`present`, KIND+model) and left the older `named` route open, which
is why that fix held on four enemy-free maps and failed on the first map with enemies. Check the
ordering against **any** future widening.

## "<Name> joins the party!" banner — SILENT, reported 2026-07-28, NOT diagnosed

**KEYWORDS: joins the party party join banner eyecatch full screen black centered wings winged
ornament gold name Penelo joins the party new party member recruit silent not vocalized**

Reported by the tester with a screenshot. **Observation only — no RE done, no function identified,
no read-point. Do not implement from a guess.**

A **full-screen banner**: the screen is entirely black and a single line sits centred, flanked by
two gold winged ornaments:

```
        [wing]  Penelo joins the party!  [wing]
```

Observations that matter:

- **The character name is drawn in a different colour** (gold) from the rest of the sentence
  (white). So this is one string with an inline colour/parameter escape, or a template with the name
  substituted — **not** a name the mod should compose itself. Read the composed string; do not
  rebuild the sentence from a character id and a hardcoded "joins the party!".
- **No cursor, no panel border, no rows.** It is a transition/eyecatch surface, so the universal
  focus signal `FUN_00247510` almost certainly never fires here — expect a **draw / content-set**
  read-point (the shape of `FUN_0035e070` or `FUN_002e16b0`), not a focus one.
- It is **not** the telop the mod already reads: the telop is `HEADER<0x02>BODY` inside an on-screen
  overlay box, and `message_reader.cpp`'s `OnTelop` would have spoken this if it were.
- It is **not** the "obtained \<item\>" toast (`FUN_0035e070`, text at `widget+0xC8`) — that draws in
  the corner over live gameplay, not on a blacked-out screen.
- It appears at a **story beat**, i.e. it is script-driven. The `ctrl` symbol dump
  (`FFXII-Decompile\notes\dbg_symbols_ctrl.csv`) has `addpartymember` (idx 1557) and `refreshparty`
  (idx 1698); the banner is plausibly raised near an `addpartymember` call. **That is a LEAD at ~0.5,
  not a conclusion** — resolve the native by behaviour, never by index arithmetic
  (`script_native_table.txt`'s own self-check says its id join is NOT COHERENT).

**First checks next session** (in this order, per CHECK-GameArchitecture-FIRST):
1. Grep `GameArchitecture.md` and this file for an existing party-join / eyecatch / banner
   read-point before anything else.
2. Reproduce with `probe_notice_board.js` attached — it already logs **every** `FUN_002f9860` string
   id with decoded text. If the banner's sentence appears there, the string id and its resolver are
   handed to us for free and the only remaining question is which widget draws it.
3. Only then chase the widget: find the writer of the composed string, and treat **that** function
   as the event (see the standing rule — games always fire an event; find the WRITER of the state).

**Do not assume this shares a surface with the Clan/Hunt panels above.** Three silent surfaces
reported close together is not evidence they are one bug; Session 71 lost a hook to exactly that
kind of shape assumption.

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

**Two defects, one fixed, one OPEN.** — *both entries below are superseded; see "Pathfinder 3D
rebuild — Strikes + Solved (Session 74)" at the end of this file.*

1. ~~**FIXED (provisional).**~~ **STRUCK (Session 74) — it does not fix it.** The claim was that a
   terminal cost `h(n) = min_g(euclid(n,g) + kGoalGapWeight * g.d)` with `kGoalGapWeight = 4.0` makes
   a nearer-to-target goal win. Measured afterwards in the tester's log: `primary … d=0.7m` existed
   and the search still finished on a **4.0 m** goal (`gap to target 4.0m`, `nearDist=3.4m`,
   `expands=5`). **Why it cannot work:** the terminal cost is in the HEURISTIC, but termination is
   `isGoal()`, which fires on **any** goal the moment it is popped — a far goal popped early ends the
   search whatever the heuristic charged it. Raising the weight cannot help. Do not add a second
   weight either; cap the goal SET instead (see 2).

2. ~~**OPEN — the real fix.**~~ **SOLVED (Session 74).** `kApproachRadius = 4.0f` is indeed made up,
   and the note below that the two computed extents are "direction-dependent shape queries, not yet
   replicated" is **out of date — they are replicable**. `FUN_003da730` is an ellipse radius along a
   direction and `FUN_003a1d30` is `sqrtf(fabs(x))`, so all four extents are plain memory reads; the
   shapes are 4-float records `{A, B, yaw, extra}` at `playerNode+0x50` / `targetNode+0x70`. Better,
   the result **self-checks**: `DAT_0209a2b0` (already read as `Chosen::score`) holds
   `2*dist2D - reach`, so `reach = 2*dist2D - score` is the engine's own answer. Full derivation,
   the confidence bar, and the "do not narrow the goal set until the log shows them agreeing" caveat
   are in the Session 74 block at the end of this file. The measured bracket below still stands as
   the acceptance test.

   The relation, kept because the bracket is still the acceptance test: the gap is the horizontal
   distance `sqrt(dx^2 + dz^2)` **minus the sum of four extents** — two computed, and two read as
   floats from `+0x0C` of the two extent blocks. Those blocks are `playerNode+0x50` and
   `targetNode+0x70`, so two of the four are plain constants at **`playerNode+0x5C`** and
   **`targetNode+0x7C`**.
   **Bracket already measured:** `dist2D=0.51` PASSED the distance gate; `dist2D=1.70` had band and
   cone PASS yet the engine chose nothing, so the true reach lies **between 0.51 and 1.70**.

**Do NOT tighten `kApproachRadius` blind.** If the goal set comes up empty the code falls back to
`SnapToWalkable` on the target's own cell — i.e. straight back to the unreachable dais, which is the
bug this whole change exists to fix. Widen-then-prefer, never narrow-then-hope.

**Also still open:** enemies route with `band=[1.00,-1.00] (none -> target's own cell)` because the
combatant-pool entries reach `ReadBandFor` with no usable scene object. Harmless today (enemies are
on your level) but it means the approach-cell logic is inactive for them.

---

## Pathfinder 3D rebuild — Strikes + Solved (Session 74, 2026-07-27)

**KEYWORDS: kGoalGapWeight struck isGoal terminal cost heuristic interaction reach solved
FUN_003da5a0 FUN_003da730 ellipse radius 2*dist2D-score DAT_0209a2b0 self-check kApproachRadius
Upper Apartments Highhall stacked exits single-height grid span graph GATE A GATE B ReadBandFor
0x107 position offset bug NAV-PROBE**

### STRUCK — "`kGoalGapWeight = 4.0` fixes the approach cell landing out of reach" (Session 73)

Session 73's addendum 3 recorded this as **FIXED (provisional)**. It is not fixed. From the tester's
own log, `FFXII-Screen-Reader-Latest.log` (Nalbina, routing to the Save Crystal):

```
request: interaction band=[-2.49,2.50]
goal-set: 8 cell(s) in band [-2.49,2.50], primary (40,28)->(40,28) d=0.7m
goal-set: reached alternate approach cell (39,26), gap to target 4.0m
stats plan=Route pass=strict tgtCell=(39,26) expands=5 touched=21 nearest=(39,26) nearDist=3.4m
```

A 0.7 m goal existed; the search finished on a 4.0 m one, with `expands=5`.

**Why the weight cannot work where it was put.** `h(n) = min_g(euclid(n,g) + w*g.d)` puts the terminal
cost in the **heuristic**, but termination is `isGoal()` (`path_search.cpp:295-299`), which returns
true for **any** member of the goal set the moment it is popped. A far goal reached early ends the
search regardless of the terminal cost the heuristic assigned it. Raising `w` cannot fix this; it only
changes the order goals are *approached* in, never the fact that the first one popped wins.

**Do not "fix" this by raising `kGoalGapWeight`, and do not add a second weight.** The correct fix is
the one `path_search.cpp:96-99` already names: cap the goal set at the engine's real interaction
reach, so every goal is interactable **by construction** and first-popped is the right answer (it is
then also the nearest by walking, which is what removes the 3-4 step overshoot the tester reports).

### SOLVED — the engine's interaction reach, and it checks its own arithmetic

Supersedes the OPEN item "`kApproachRadius = 4.0f` is made up" (this file, Session 73 block). The two
extents that were written off as "direction-dependent shape queries we do not replicate" are
replicable, and the whole reach is a memory-only read.

`FUN_003da730` is an **ellipse radius along a direction**; `FUN_003a1d30` is `sqrtf(fabs(x))`:

```
w = cos(-yaw)*dz - sin(-yaw)*dx ;  u = sin(-yaw)*dz + cos(-yaw)*dx
r^2 = (u*u + w*w) / ( u*u/(A*A) + w*w/(B*B) )        // 0 when the two points coincide
```

`FUN_0025bad0:83` calls
`FUN_003da5a0(out, playerXform+0x50, playerXform, targetXform+0x70, targetPosAdj)` and rejects the
candidate unless the return is `< 0`. That return is `dist2D - reach`, with

```
reach = ellipse(playerShape -> target) + playerXform[+0x5C]
      + ellipse(targetShape -> player) + targetXform[+0x7C]
```

Each shape is a 4-float record `{semiAxisA, semiAxisB, yaw, extraRadius}` — player at `xform+0x50`,
target at `xform+0x70`. **These are different node offsets; do not collapse them.**

**THE SELF-CHECK.** `FUN_0025bad0:139` stores `DAT_0209a2b0 = (dist2D - reach) + dist2D`, where the
second term is `param_1[3]` — the distance `FUN_003da5a0:36` writes into its out-vector. The mod
already reads that global as `InteractTarget::Chosen::score`. Therefore, for whichever candidate the
engine chose:

```
reach = 2*dist2D - score          <-- the engine's OWN answer, no replication involved
```

`InteractTarget::ReadReachFor` returns the replica and this measured value together, and `NAV-PROBE`
prints them side by side marked **CONFIRMED** or **MISMATCH**. Offline confidence in the replica is
**0.97**; the runtime identity is what lifts it over the project's 0.98 bar. **Until a log shows them
agreeing, do NOT narrow the routing goal set with the replica** — the widen-then-prefer rule below
still stands, and an empty goal set still falls back to the target's own cell.

A confirmed value must also land inside the previously measured bracket: `dist2D = 0.51` PASSED the
distance gate and `1.70` did not get chosen, so `0.51 < reach < 1.70`.

### BUG in shipped code — `ReadBandFor` ignores the target's position offset

`FUN_0025bad0:72-76`: when the byte at `targetXform+0x107` has bit 0 set, the engine adds
`targetXform[0x10..0x12]` (byte offsets `+0x40/+0x44/+0x48`) to the target's position **before both**
the distance gate and the vertical band test. `InteractTarget::ReadBandFor` reads `XFORM_POS_Y`
directly and never adds it, so its band centre — and therefore the goal band `PathSearch` routes to —
is wrong for any target carrying the flag. `InteractTarget::ReadGatePos` now applies it and
`NAV-PROBE` reports the flag; `ReadBandFor` itself is corrected in Phase 3.

### ROOT CAUSE — Upper Apartments routes to the wrong exit (tester report, Session 74)

Not a regression in the Session 73 elevation work. It is the limit of a single-height grid.
`FFXII-Screen-Reader-Latest.log:7781-7782`, map 280:

```
__MJ_CTRL000 group=1 -> "Nalbina Fortress: Lower Apartments" (279) at (53.5,-1.9,27.0) | box x[52.1..55.0] z[24.3..29.8]
__MJ_CTRL001 group=2 -> "Nalbina Fortress: The Highhall"    (282) at (50.4,+7.8,29.6) | box x[49.0..51.7] z[29.5..29.8]
```

**~4 m apart horizontally, ~9.7 m apart vertically.** Three independent 2D blindnesses collapse them:

1. `NavGrid::WalkableAt` stores **one height per 1.5 m cell**, a single `GroundAt` sample
   (`nav_grid.cpp:39-51`).
2. `SnapToWalkable` ring-searches up to `kSnapMax = 6` fine cells (~9 m) for a walkable cell
   (`path_search.cpp:129-152`), so the Highhall goal can snap onto the Lower Apartments floor.
3. The "At the exit" short-circuit is XZ-only with Y **deliberately** ignored
   (`path_planner.cpp:304-305`), so it fires for a seam the player is standing 7.8 m beneath.

Also note `exit_scan.cpp:148-149` overwrites the exit's Y with `GroundAt(x,z)`, which cannot tell the
Highhall seam from the floor under it. Phase 3 takes the Y from the seam's own vertices instead.

**Revision notice.** Adding a Y tolerance to the exit-arrival test deliberately revises the earlier
rule in this file, *"never test 'am I at this exit' in 3D — X/Z only"*. That rule was correct when an
exit's Y came from the map-control blob and was not necessarily a floor. Since Session 64 an exit's
position is a **walkmap floor polygon**, so its Y is a floor. The rule is superseded for walkmap-
derived exits only; it still stands for anything blob-derived.

### The two gates Phase 1 must not be started without — BOTH RETIRED (Session 75)

> **RETIRED, and both were the wrong question.** The engine does not resolve levels through stacked
> lists in a column at all — it resolves them through **per-edge polygon adjacency** (`poly+0x16/
> +0x18/+0x1A`), so "does any column stack" never had a bearing on the rebuild. And Gate B's
> disagreements were an artefact of the oracle, not the reader: `MAP_GROUND_AT` is not a floor query
> (`FUN_0026e3c0` takes the topmost floor and THEN climbs up to 30 units and casts back down with
> mask 0xFFFF, returning whatever it hits). `AllFloorsAt` is an exact replica of `FUN_00231900` and
> was right the whole time. See the Session 75 block at the end of this file.
>
> Kept below because the *reasoning* is still worth reading: a null result that would have been read
> as a fact about the game was really a fact about our sampling.


- **GATE A — does `MapQuery::AllFloorsAt` ever report >= 2 layers?** Across every log to date it never
  has (`layers>=2` x0, zero `STACKED` lines) — which is precisely why Session 73 struck the layered
  grid, and that strike was correct **on the evidence available**. The evidence was incomplete: the
  diagnostic only sampled the player's column and each listed object's column, and Upper Apartments'
  two seams sit in **different** columns 3.1 m apart, so the stacking in that room was never sampled.
  `NAV-PROBE`'s SPAN-PROBE sweeps an 11x11 block instead. **If every column still reports one span,
  the span graph is isomorphic to today's flat grid — stop and re-derive rather than build it.**
- **GATE B — is span height trustworthy?** The `grid xcheck` diagnostic reports **100% walkability**
  agreement between the memory-only walkmap read and the `GroundAt` oracle (416/416, 404/404,
  520/520) but height agreement as low as **41/45** — 24% of walkable Nalbina cells differ by more
  than 0.5 m while reporting a single layer. Walkability is settled at >= 0.98; **height is not**, and
  span Y is what the entire rebuild keys on. SPAN-PROBE reports which span index `GroundAt` lands on
  per column; the tie-break tally counts **only multi-span columns**, because on a flat map every
  single-span column would score as "top" and manufacture an answer.

### Still standing, carried into the rebuild

- **Widen-then-prefer, never narrow-then-hope.** STILL LIVE, in a new form: the mesh goal test samples
  the poly's centroid AND all three corners against the band and the reach, because a navmesh triangle
  can be metres across and a centroid-only test would reject a poly whose near corner is comfortably
  inside reach. (`SnapToWalkable` and the 4 m in-band tier are both gone with the grid — Session 75.)
- **No slope gate, and now NO STEP GATE EITHER.** Session 68's finding stands and goes further: the
  engine imposes no walkable-slope limit AND no step limit between adjacent polys — walkability is a
  flag test (`FUN_00230a40`). `kStepDiscont = 0.35` was ours, and it made any staircase with a taller
  riser unroutable. Deleted with the grid (Session 75). The tester
  asked for steep slopes to be rejected; the decision taken was to **carry `cosSlope` on every span
  and log it on every edge, with the gate inactive**, so the threshold can be set from evidence
  instead of reintroducing a struck behaviour.
- **Never route to an arrival/spawn point.** Exits stay anchored to the walkmap map-jump surface.
- **Do not propose a sixth blob-derived transition model.** Seam geometry is `(polyFlags >> 3) & 0x1F`.

---

## The walkmap is a NAVMESH — Solved (Session 75, 2026-07-27)

**KEYWORDS: navmesh poly adjacency 0x16 0x18 0x1A FUN_002327d0 nav_grid deleted kMaxStep kStepDiscont
SnapToWalkable near-goal bridge removed WALK_POLY_MJ_MASK 0xF struck movement class 4 FUN_00230a40
FUN_00232020 override table GroundAt not a floor query climb-and-drop volumes dynamic obstacles
closed gate EdgePassable kAtExitDy stairs Waterways**

### THE RULE

> **Every walkmap floor triangle carries the index of its neighbour across each of its three edges at
> `+0x16`, `+0x18`, `+0x1A` (`< 0` = none). That is the routing graph. It is the same graph the
> character mover walks, and it is elevation-correct by construction — a balcony and the floor beneath
> it are two disconnected components sharing a grid cell.**

Verified by reading `FUN_002327d0` directly: it keeps a CURRENT POLY INDEX across frames and, when
`FUN_002324f0` reports the position left the triangle across edge *e*, reads
`polyArr + poly*0x20 + 0x16 + e*2` and steps onto that neighbour, gated by `FUN_00230a40`.
Corroborated at `FUN_0022f9b0:86`. Confidence 0.99.

**The mesh was never hidden — the mod has read it for height since Session 33. What was unknown is
that the triangles are LINKED. We were using a navmesh as a heightfield.**

### STRUCK — the entire grid pathfinder

`nav_grid.h/.cpp` is deleted, and with it:

| removed | why it was wrong |
|---|---|
| one height per 1.5 m cell | cannot hold a balcony over a walkway; stacked destinations collapsed |
| `kMaxStep = 1.5`, `kStepDiscont = 0.35` | **the engine has NO step limit between adjacent polys.** Any staircase with a taller riser was unroutable — the likely whole story of "the Waterways fail completely" |
| `SnapToWalkable` (9 m ring) | how a goal 7.8 m overhead got answered with the floor beneath it |
| bridge + near-goal recovery passes | both existed to paper over invented connectivity; near-goal spoke a FAILED search as a confident route |
| per-cell `GroundAt` sampling | see below — it is not a floor query |

**Do not reintroduce a step or slope gate.** The engine's only geometric blockers are walls/volumes
and the flag test. Both gates were ours, both refused edges the game walks.

### STRUCK — `MAP_GROUND_AT` is "the floor height at (x,z)"

`FUN_003208c0` → `FUN_0026e3c0` is TWO-STAGE. Stage 1 takes the topmost type-0 floor plane. **Stage 2
then climbs from it in 1-unit steps (up to 30) to the first point outside any collision volume and
casts a segment back down with mask `0xFFFF` / flags `0` — and if that hits anything, the returned Y
is the HIT's Y.** Confidence 0.97.

So it can return a wall, a ceiling or a rooftop. This is the source of the Session 74 "Gate B"
height disagreements (1.2–1.5 m systematic, one 21.5 m outlier) that were wrongly read as evidence
against our own reader. **`AllFloorsAt` is an exact replica of `FUN_00231900` and was correct.**

Corollary: **`GroundAt` is fine for a rough "is there floor here", and must not be used as the height
a route or a destination is anchored to.**

### STRUCK — `WALK_POLY_MJ_MASK = 0x1F`

The map-jump group field is **four** bits. Verified in two functions:
`FUN_00232020` and `FUN_00230a40` both compute `(flags >> 3) & 0xf`.

A seam poly with bit 7 set computed as `group + 16`, matched no `setmapjumpgroup(K)`, and was
**silently dropped**. Upper Apartments' Highhall seam therefore read as 2 polys spanning a 0.3 m depth
with a 1.9 m rise — geometrically impossible for a walkable threshold, because it was a fragment.
**This is the "exit in completely the wrong direction" half of the bug**, independent of the routing
half. Session 64's "the exact width of the field cannot matter" is struck with it.

Also fixed: `ReadMapJumpSurfaces` recorded only `WALK_POLY_VERT0` — one corner of three per triangle.

### RESOLVED — walkability, exactly

`FUN_00230c10(ctx, from, to, out, class, radius)`; both actor movers pass class **4** normally
(`0xffff` only as an unstick mode). Class 4 matches none of `FUN_00230a40`'s 0/1/2/3/5 branches and
falls through to walkable, so:

> **For the party, floor walkability is exactly `(effectiveFlags & 7) == 0`.**

`effectiveFlags` = `FUN_00232020`: two banks of `{u32 mask, u32 value}` at `DAT_0209a3e0`
(RVA `0x1F7A3E0`, 0x50 entries), indexed `(flags>>13)&0x1F` and `((flags>>3)&0xF)+0x40`.
**Floor FINDING uses RAW flags** (`FUN_00231900` bypasses the table deliberately); **MOVEMENT uses
effective flags.** They are different questions — do not collapse them.

### The one raycast that survives — and why you cannot delete it

Floor adjacency knows nothing about blockers. The prim index space has THREE ranges:

| range | meaning |
|---|---|
| `[0x0000,0x4000)` | floor polygon — the graph |
| `[0x4000,0x5000)` | static volume — walls, pillars |
| `[0x5000, …)` | **dynamic obstacle** — doors, moving platforms |

So **the floor under a closed gate is still adjacent to the floor before it**, and a pure poly-graph
A* routes straight through it. `NavMesh::EdgePassable` casts one short walk-class segment straddling
the shared edge (not centroid-to-centroid: a long diagonal between two large triangles passes close
to unrelated geometry, which is exactly how the old grid's clearance rays islanded doorway cells).

The struck `">= 0x5000 => empty/sentinel"` comment in `nav_rva.h` was wrong; that range is live.

### Also this session

- **`kAtExitDy = 3.0`** — the "At the exit" test now gates on height too. This deliberately revises
  *"never test 'am I at this exit' in 3D"*: that was correct when an exit's Y came from the map-control
  blob, and stopped being correct in Session 64 when an exit became a walkmap FLOOR POLYGON. Without
  it, Upper Apartments announced "At the exit" for a seam 7.8 m overhead.
- **Exits aim at the seam's nearest vertex**, with the seam's own vertex Y — not the centroid with a
  `GroundAt` height. **AMENDED Session 98: that is the right PROXIMITY answer and the wrong ROUTE
  answer.** It stands for `/`, the `[`/`]` listing distance and the "At the exit" check. It does NOT
  stand as a route endpoint — a vertex is on the walkable boundary by construction, and
  straight-line-nearest is not walking-nearest. Routing falls back to the seam's POLY SET on the
  failure path; see the Session 98 entry below.
- `NavReach` floods the mesh (memory reads, no raycasts) instead of the grid.

### OPEN — the only thing unmeasured

Whether the mesh is CONNECTED from the player to a given seam on a given map. Everything says it
should be, but that is an assertion about map data, and asserting things about exit data is how six
sessions of the exit saga went wrong. `NAV-PROBE` answers it: the `FLOOD:` line and one
`seam group=N ... inPlayerComponent=` line per exit, with the poly ids when the answer is no.

---

## Destinations disagreed: the search never ran — Solved (Session 76, 2026-07-27)

**KEYWORDS: expands=1 touched=0 IsGoal too generous corner sampling centroid arrival mismatch
destinations disagree crow-flies turn-by-turn Waterways save crystal two interactable classes
class 1 class 3 FUN_0025be50 score units reach identity struck ObjectClass ClosestPointOnPoly**

### Symptom

`/` crow-flies "Save Crystal. North, 6 steps" (correct) vs `\` turn-by-turn "Save Crystal. South 1.
1 steps". Direction wrong, distance wrong, and the answer changed as the player shuffled in place.

### Cause — A* terminated on its first pop

```
mesh: start=41 goal=33 end=41 polys=1 expands=1 touched=0 rays=0
pass=mesh-reach startPoly=41 goalPoly=33 endPoly=41
```

`start == end`, `expands=1`, on **20 of 30 routes**. Two Session-75 errors compounding:

1. `IsGoal` accepted any poly with ANY of {centroid, 3 corners} in the interaction band and within
   `reachRadius`. Navmesh triangles are whole corridors, so the player's own triangle nearly always
   has a corner within ~1.6 m of a target a few steps away — the goal test passed before the search
   moved.
2. On a non-goal finish the polyline ended at `Centroid(reached)` — **not** the corner that passed
   the test. So the destination was the centroid of the triangle the player was standing in.

**LESSON: `expands=1 touched=0` in a route dump means the search never ran.** It is the single
cheapest tell for this whole class of bug and it was sitting in the log unremarked. Watch for it.

**LESSON: if a predicate tests point A, the code must ARRIVE at point A.** Testing a triangle's
corners and then walking to its centroid is not a rounding error, it is a different destination.

### Fix

`IsGoal` is `p == goal`. Reconstruction **always appends `to`**, so both direction keys name the same
`FVec3` from the same source and cannot disagree. Overshoot is impossible — a path ending at the
target cannot end past it, which was the original Session 74 complaint.

The "cannot stand on the target" case (Montblanc's dais) is recorded DURING the main search — the
first qualifying poly A* pops — and used only when the goal poly is never reached, arriving at
`NavMesh::ClosestPointOnPoly`, the exact point tested. No second search, no change to the common case.

### STRUCK — `reach = 2*dist2D - score` is universal

It is **class-3 only**. `DAT_0209a2b0` mixes units between the two scorers; `FUN_0025be50` stores
`FUN_003a1960(player, node)`, a plain distance. Applied to a class-1 winner the identity produces a
confident, meaningless number — and the probe printed it next to the word CONFIRMED, which is exactly
how a wrong value gets promoted to fact. `haveMeasured` is now gated on class 3.

**LESSON: a self-checking identity is only self-checking within the code path that established it.**

### STRUCK — one interaction field layout

There are **two** interactable classes (`sceneObj+0x03 >> 5`), and they share almost no offsets —
band, cone, cone aim point and score units all differ. `ReadBandFor` had been reading character-band
fields on gimmick objects and returning plausible garbage. See `GameArchitecture.md` §"TWO
interactable classes" for the table. Every interaction read now branches on `ObjectClass`.

### Refuted subagent claim, recorded so it is not re-derived

A subagent reported class-1's interaction point at `node[+0x10/+0x14/+0x18]` (absolute world pos).
`FUN_002646c0`, the engine's own getter, returns elements `[0..2]` of that float block = bytes `0x00/0x04/0x08` — **the plain
position** — for class 1. `+0x10/+0x18` is where `FUN_0025be50` aims the facing cone.
`FUN_0026bb00`'s class-1 setter does write `+0x10/+0x14/+0x18`, so a field exists there, but the
getter does not read it and the two are unreconciled. **Class-1 interaction point = the plain
position (0.98). The `+0x10` field is UNIDENTIFIED — do not build on it.**

### Carried forward from DQ7R (read for the idiom this session)

- **The interaction point is authored game data, not geometry the mod computes.**
  `RefreshEntityPosition` overwrites `entry.pos` with the interaction volume's cached world position,
  and everything downstream consumes that one field. That is the "one destination, both keys" design.
- **Not yet ported, worth having:** `if (e.hasBoxComponent) { e.reachable = true; return true; }` —
  an entity with an authored interaction point is reachable **by definition**; do not ask the walkmap.
- **Does not port:** DQ7R's box is *where the player stands*. FFXII has no such point — its
  equivalent is a computed region (`dist2D(P,anchor) < reach && band`), which is strictly more
  information. Do not stretch the analogy.

---

## Route reversals + four deleted NPCs — Solved (Session 77, 2026-07-27)

**KEYWORDS: reversal doubles back funnel string pull portal midpoints not elevation sign-twin
dropped Nomad name equality no proximity kStackedDist NumberDuplicateLabels contradiction orphaned
diagnostic character inclusion mode state flags KIND_DEAD counted REVERSAL invariant**

### STRUCK — "a polyline through portal midpoints is a route"

Session 75 built the route as `[from] + midpoint of every shared edge + [to]` with `outPoly = rawPoly`
— no string-pull at all. On a navmesh whose triangles are often whole corridors, successive edge
midpoints sit at opposite ends of their portals and the line saws between them:

```
"Dire Rat 1. South 2, North 7, West 5, North 5, West 14, then 3 more. 36 steps"
"Dire Rat 1. West 2, East 12, North 5, East 11, North 16, then 4 more. 50 steps"
```

**The A* was never wrong** — `pass=mesh`, `nearDist=0.0m`, 11-poly chain in 12 expansions. Only the
line drawn through it was. Replaced with the **funnel algorithm**, which returns the shortest path
inside the corridor and therefore *cannot* double back.

**LESSON: `PathDirections` cannot fix a bad polyline.** Its RDP pass PRESERVES shape by design, so a
zigzag survives simplification — the zigzag *is* the shape. Smoothing belongs in the search, not the
describer. If routes ever zigzag again, look at the polyline, not the wording.

**Elevation was the natural guess and it was wrong.** Ruled out three ways: the searches were clean,
the routes were flat (y=0 both ends), and the *same* start/goal produced the bug with and without a
spurious first leg purely from the player shuffling inside one triangle. Height cannot do that.

**Guard: the REVERSAL invariant.** `PathDirections::Describe` now logs any two consecutive legs
turning >= 135°. Do not remove it — this class of bug was invisible in the log for a whole session.

### STRUCK — the sign-twin filter's name-only twin test

`TagDoorwaysAndDropSignTwins` deleted **four NPCs** on Nomad Village, one story-critical:

```
sign-twin dropped "Nomad": [0:40] (45.97,0.00,67.37) repeats doorway [0:38] (53.80,0.00,38.80) 29.6m away
   ... three more at 20.5m, 27.6m, 17.1m
```

Two compounding defects:
1. **Doorway tagging by proximity alone** — any object within 2.5 m of a `+0x70` field-sign record
   became a "doorway", including an NPC standing near a shop sign.
2. **The twin test was name equality with NO proximity check.** The distance printed in the log line
   was computed *only for the message*. So one mis-tagged "Nomad" erased every other "Nomad" on the
   map, at any distance.

**The codebase already contained the correct rule, in the other duplicate handler.**
`NumberDuplicateLabels`: *"two at different positions are two real objects that share the game's own
name (which is normal — 109 npcdic ids all read 'Rabanastran')"*. That pass NUMBERS them; this one
DELETED them, and runs first. Same map: `dup-label "Nomad Youth" x4`, `dup-label "Cockatrice" x6`.

**Fixed scope (tester's rule):**
- Doorway tagging **never applies to a person** — a character can no longer become the anchor.
- The doorway-twin removal is **interactables only**: same name, the other carries the location jump
  (`Entity::doorway`), this one does not. That is the shop-sign case it was written for.
- **NPCs dedupe only when literally stacked** (`kStackedDist = 0.05 m`). `j < i` so the first of a
  stacked pair survives.
- **Every drop logs unconditionally.** It used to log only on a new population high-water mark, so a
  twin removed on any later rescan was silent.

**LESSON: two subsystems held opposite policies on the same fact and the destructive one ran first.**
When one pass numbers duplicates and another deletes them, they cannot both be right.

### LESSON — an orphaned diagnostic makes its bug invisible, not absent

`EntityList::LogDiagnostic` / `EntityDiag::DumpLocked` (the raw handle-table walk, which shows objects
the scan REJECTED) lost its only caller when the `'` dump was stripped in Session 74. For three
sessions the log could only show what PASSED the filters — which is exactly why four deleted NPCs
went unnoticed. Re-keyed onto the `'` probe. **Before stripping a diagnostic, check what question it
was the only answer to.**

### OPEN, instrumented — characters can only enter the list via mode-state flags

`entity_scan.cpp`'s inclusion gate had both `gimmick` and `named` written `&& !isCharacter`, so a
person's ONLY route in was non-zero `+0x1C` flags — mode state that this file already records as
reading **zero on a disabled object** (see the S54 entry above: *"Include by KIND; use the flags only
for 'what can I do with it right now'"*). Applied to gimmicks in S54, never to people.

A named character is now included, but **this shipped on inference, not evidence**, so it counts
itself: `inclusion: N character(s) admitted by NAME ONLY (no interaction flags)`. If N is always 0,
the change is a no-op and should be reported as one, not left looking like a fix.

`ScanCombatants`' `kind == KIND_DEAD` skip is **counted, not flipped** — `phyre_types.h` labels that
constant "NAME IS WRONG" and kind 5 = NPC on the field, but the combat track owns it and the pool is
documented to hold no gimmicks. A non-zero tally on a field map falsifies that premise and the skip
must then go.

---

## The route reversal is a PASSED WAYPOINT — Solved (Session 78, 2026-07-27)

**KEYWORDS: reversal leg0 leg1 80% passed waypoint drop leading corner static target immune mobile
target Dire Rat Rogue Tomato log forensics 1059 routes 109 reversals grid pass=strict 86 not new
dual-polarity funnel measured not derived funnel longer than midpoints invariant corridor quality
portal crossing cost camera ref swing**

### The archive settles the history

Forensics across all 20 archived logs: **1,059 spoken routes, 109 with an immediate reversal.**

- **GRID era: 86 / 998.** ALL from `pass=strict` — completed searches, up to 601 expands and 55,628
  rays. **Zero** from recovery: the 24 `pass=near-goal` routes contain none, and no `bridge:` line
  exists anywhere in any log.
- **NAVMESH era: 23 / 61.**

**The tester's recollection was correct — this is not a rewrite regression.** But they are two bugs
with one name, and the difference is magnitude: grid reversals wasted up to **20 steps**
(`"Rogue Tomato. South 24, Northwest 20, North 22. 66 steps"`); navmesh reversals never more than 6.
"North 7, South 25" is a grid-era memory; the navmesh has never produced one that large.

### ROOT CAUSE — the first waypoint is behind the player

**87 of 109 (80%) are leg 0 -> leg 1**, in both eras. The target distribution names it:

```
Dire Rat 1     14/22  (64%)     Save Crystal      0/36  (0%)
Rogue Tomato   38/113 (34%)     Stair to Lowtown  0/55  (0%)
Montblanc      25/183 (14%)     Rabanastre exits  0/41  (0%)
```

**Static targets are immune. Moving targets dominate — but the target's motion is not the cause.** A
moving target makes the player re-press *while walking*. The route's first corner is fixed; the player
drifts across it; from one step past it, leg 0 points BACKWARDS to that corner and leg 1 turns around.
The archive caught the identical path (`firstLeg=(46.5,160.0)`, same `expands=38`) spoken four
different ways in six seconds as the player rocked over one waypoint.

**FIX: drop leading waypoints the player has already passed.** Project the player onto the
corner->next-corner segment; positive parameter = beyond it = history, not a waypoint. Iterate. This is
independent of the funnel, the corridor, and which pathfinder is underneath — which is why it also
addresses the grid-era symptom.

**LESSON: a reversal at leg 0 is not a geometry bug, it is a STALE waypoint.** Check *where* in the
route a defect sits before theorising about how the route was built — the position is the diagnosis.

### The funnel was not string-pulling (Session 77's fix, incomplete)

Every route: `portals=6 corners=7`, `7->8`, `8->9` — one corner per portal, three for three. A working
funnel collapses a corridor to a handful of corners.

I derived the left/right convention (FFXII has north at **-Z**, which flips handedness against every
reference implementation of this algorithm), traced both branches against Mononen's reference twice,
and the code looked correct. The log disagreed.

**So the polarity is now MEASURED, not derived:** the funnel runs both ways and keeps the shorter
path. The correct polarity is the shortest path through the corridor by definition; the inverted one
is the zigzag. Impossible to get wrong, and it logs which won so the branch can be deleted **on
evidence** rather than on a third round of my sign reasoning.

**LESSON: when a hand-derived sign convention and the log disagree, stop deriving.** Two sessions were
spent on this; the resolution cost six lines.

### The invariant that would have caught it in one line

The funnel's output can never be longer than the portal-midpoint path through the same corridor — it
is the *shortest* path in that corridor by definition. Now logged, along with a corridor-quality check
(taut length vs straight-line distance). Together they separate **"the funnel is wrong"** from
**"A\* chose a wandering corridor"** — the exact distinction three sessions of reading code could not
make. **The funnel can never shorten past its corridor.**

### A* edge cost — STRUCK: centroid-to-centroid distance

It was `Dist3(centroidA, centroidB)`. On a mesh where a triangle is often a whole corridor, centroid
hops are a poor proxy for walking distance: two huge adjacent triangles score far apart even when the
player barely clips the shared edge. Now costs the actual crossing,
`centroid -> portal midpoint -> centroid`. Heuristic stays Euclidean, so still admissible.

### Correction — the REVERSAL detector's attribution

`path_directions.cpp` credited the bug to Session 77. The archive shows 86 grid-era reversals three
days earlier, from `pass=strict`, and considerably worse. Amended. The detector postdates every
archived log, so none of that history was ever caught by it.

### Not the bug, but half of what is seen — camera-relative rotation

`ref` swung **-24.2 -> -72.3 deg** across six presses while the leg distances stayed fixed at 11, 11,
6, 8. Identical geometry, different words, because directions are camera-relative. Session 56 already
rejected the camera lock, the travel-anchored frame and the spoken notice — **do not re-propose them.**
But it masks real geometry defects, so when testing a reversal, hold the camera still between presses.

---

## SOLVED — an NPC missing from EVERY map, not just Nomad Village (Session 79, 2026-07-27)

**KEYWORDS: missing NPC behind tent elder slot 55 cat 66 hex class 3 character kind 5 isCharacter
clause include by KIND loaded model presence blast radius unmeasured per-kind tally shadow
registration exact stacked entity_labels container slot unstable nameIdx -1 collision position anchor
claim self-inclusion creep 1 3 5 6 7 F6 never bound dead code VK_F6 merchant no marker shop name
odd npcdic slot Frida-first**

### The one clause

`entity_scan.cpp`'s `gimmick = (kind == KIND_ACTION_GIMMICK) && !isCharacter`. The `&& !isCharacter`
rejected a placed, enabled, model-loaded CHARACTER whose interaction flags were zero — **on every map
in the game.** A tester needed one of them (a woman behind the Nomad Elder's tent, story-critical,
confirmed present by sighted assistance) and she was reachable by no inclusion path at all.

**`cat` prints in HEX in the dump.** `cat=66` is 0x66 -> low-5 = 6 -> class 3 (character). Reading it
as decimal 66 gives class 2 and a completely wrong object model. Cost part of a session.

**FIX: include by KIND + a LOADED MODEL** (`+0x14 & READY_MODEL_BIT`) — the honest test for "there is
a body standing there". Kinds 1 AND 5. Kept purely additive: the old `gimmick` term is unchanged, so a
model-less trigger volume cannot start vanishing as a side effect.

### STRUCK — "unflagged characters are left to the combatant scan or the talk-flag path"

The comment that justified the exclusion. Both escape hatches are fictional: the combatant scan reads
the **BtlWork pool**, which holds no field NPCs, and the talk-flag path needs a flag the engine clears
(`FUN_0025ad10`) on anything the script has not armed.

### The blast radius was UNMEASURABLE — instrument, do not guess

The object dump is Session 77 code; **all 20 archived logs predate it**. Only one map was ever dumped.
The `inclusion:` tally now splits admissions by kind so the next log states the cost per map. **If a
change cannot be sized offline, ship the counter with it** rather than an assurance.

### STRUCK — `mapId . container . slot` as an entity's identity

The slot is assigned at map load in script order and is NOT stable across loads. Map 243 held five
Nomads numbered **1, 3, 5, 6, 7**: a re-slotted object looked new, and `NumberFor`'s "lowest free"
scan counted **the record it was renumbering** as taken, so a number could only creep upward.

Now `{mapId, baseLabel, nameIdx}` + a **position anchor when that triple repeats**. The anchor is not
optional: every anonymous object is `{map, "NPC", -1}`, and the widening above admits exactly those —
keying on `nameIdx` alone would have broken the people it just added. **Unique triple = match on
identity alone**, so a wandering NPC with its own id stays stable wherever it walks.

**Known limit, not a bug:** objects sharing a label, a `nameIdx` AND a roaming path (the six
`nameIdx=238` Cockatrices) have no distinguishing identity in the game's own data. Their numbers may
move between visits. Inventing an identity for them would be a fabrication.

### F6 was never bound to a key — the feature was 100% built and 0% reachable

Handler, clipboard read, persistence and the apply-before-numbering pass all shipped in Session 65,
and BOTH `Controls.md` and `README.md` documented the key. **`DIK_F6` was never defined and no
`DInputEdge` ever registered it**, so `case VK_F6:` in `nav_commands.cpp` was dead code from the day it
was written. Independent proof: the label store held **127 records and zero player labels**.

**LESSON: documenting a key is not binding it.** A feature whose docs, handler and persistence all
exist can still be unreachable; the binding is a separate artifact and nothing tests it.

### The merchant — CLEAR NEGATIVE, do not re-attempt

No per-NPC merchant marker exists:
- `sceneObj+0xCC` / `+0xDC` are slots 2 and 10 of an 18-entry `u16[]` at `+0xC8` indexed by
  interaction mode — per-map **event indices**, not shop ids. Every Nomad Village NPC reads `0xFFFF`,
  `5`, `8` or `9`.
- **npcdic has no merchant band.** Candidates are scattered (23, 24, 118, 285, 505, 676, 839, 1134 …)
  and 429/505/507 straddle the existing gimmick band. The game names this merchant `Nomad` anyway.
- The NPC->shop binding exists only in compiled map script behind `openfullscreenmenu(10, shopId)`.

**A learned NPC->shop binding was deliberately NOT built.** It would work and it is the Session 62
mistake by definition.

The shop's own NAME is readable: `shopId = *(u8*)(DAT_02ca9790+0xC0)` -> master table `DAT_02ebf158`
-> npcdic id via the **ODD** slot. **No C++ written** — FRIDA-FIRST governs a new behavioral feature;
`probe_shop_name.js` confirms it first.

**STRUCK before it shipped: "`FUN_0057c010` is the shop-open event."** It is a **dialog callback**
(`FUN_0057c010`, registered by `FUN_0057a4e0` into `FUN_003f47e0`). The probe reads from
the confirmed `FUN_0056e5d0` instead.

**RESOLVED (S126) — "the shop-open hook point is still unestablished" no longer blocks anything.**
It was never established, and it did not need to be: the shop is one of the three families served by
`FUN_005655f0` (`0x4455F0`), the unified list refresh the game runs **on screen OPEN as well as on
every category change**. `InventoryReader` already hooks it. `ShopReader::OnListRefreshed` now hangs
off that hook, after the original has rebuilt the rows, and that is what makes a shop announce the
row it opened on. **Look for an event the game already fires into a hook you already own before
hunting a new one** — the party-menu item lists were given this exact treatment years of sessions ago
for the identical symptom ("entering a one-item list moves no cursor").

### STRUCK PERMANENTLY — the learned NPC->shop binding (S79, tester directive)

**Verbatim:** *"labelling any NPC after a visit is unacceptable unless the player chooses to custom
label it. if you can't get the lookup from database resolution like we do for named NPCs or NPCs with
custom identifiers, then we don't do it."*

The design that is now banned: catch the shop-open event, attribute it to whichever NPC the player had
just interacted with, persist the binding, and thereafter speak the merchant as
`"Nomad 3, Antiqued Armors"`. It would have worked, the identity key from this session would have made
it stable, and the game genuinely has **no** per-NPC merchant marker.

**It is still banned, and "there is no other way to get it" is a reason to ship NOTHING.** A learned
label is the mod asserting a fact the game never told it: right until it is silently wrong (a shop
opened by a cutscene, a bazaar counter, a mis-attributed window), with a blind player unable to catch
it. It also only helps AFTER the player has solved the problem it claims to solve.

**A label has exactly two legitimate sources:** database resolution (npcdic via `nameIdx`, the map
script's custom string at `sceneObj+0xF8`, a field-sign record, a map-jump destination), or the
player's own F6 text. Nothing the mod observed.

**NOT struck by this:** speaking a shop's own name when the shop opens. That names the SCREEN the
player is in from the game's own text — the same category as reading a menu title — and involves no
NPC and no inference.

**The only door still open** for "which NPC is the merchant" is **static script resolution**: the
NPC's talk event index (`sceneObj+0xDC`; Nomad Village NPCs read `0xFFFF`, `5`, `8`, `9`) resolved
through the map's own loaded EBP2 script to the shop-open native and its `shopId` literal. That is
game data present before the player touches anything. **Unproven** — it needs the loaded script to be
walkable at runtime, `talkId` to map to a routine, and the shop-open native resolved BY BEHAVIOUR
(index arithmetic is struck). Do not start it without a decision that the cost is worth it.

#### CORRECTION (same session) — "only in compiled map script" is NOT a dead end

The entry above filed the NPC->shop binding under "unreachable" because it lives in the map's compiled
script. **The tester rejected that reasoning and was right: it is not different from how exit
destinations already resolve.**

`map_script.cpp` **already walks the loaded field-script blob at runtime** and does precisely this
operation, generically, on every map:

- routine table at `hdr+0x18` (`{+0x00 nameOff, +0x08 codeOff}`, stride 0x30), name pool at `hdr+0x4C`
- a routine's code span is `[codeOff, next-highest codeOff)`
- scan that span for `4f <lit:u16> 5d <nativeLo> <nativeHi>` — `0x4F` push-u16, `0x5D` CALLACTPOPA
- that is how `mapjump` (native `0x8D`) gives the destination and `setmapjumpgroup` (`0x011E`) gives
  the group binding

**A shop native is the same scan with a different native id.** Reading a literal out of the map's own
compiled script is DATABASE RESOLUTION — the data is present before the player touches anything, which
is exactly what separates it from the banned learned binding.

**What is actually still unknown** (all answerable offline, no play session):

1. **The shop-open native's id.** S63's procedure applies unchanged: `dbgIndex = nativeId + 5140`
   against the archived .dbg symbol table (20,417 symbols) — the same lookup that named
   `setmapjumpgroup`. A name lookup, not new research. **STRIKE the note that this native "cannot be
   resolved by index arithmetic"** — S63 resolves natives by SYMBOL/BEHAVIOUR, and index arithmetic was
   never the proposed method.
2. **The NPC -> routine link.** Exits use the toolchain's auto-generated routine NAME
   (`__MJ_CTRL<NNN>`). NPCs carry `sceneObj+0xDC` (talk) / `+0xCC` (action) event indices. Does the
   toolchain auto-name talk handlers the same way, or does the index address the routine table
   directly? **Settled by dumping the routine-name pool of a shop-bearing map** — the reader already
   reads that pool.
3. **Whether `shopId` is a bytecode literal** or computed at runtime. Only a literal is readable.

**Two cheaper routes to check first, both also database resolution:**

- **The interact icon.** If the engine selects a different icon for a shop NPC, that is a single
  memory read on the scene object and moots the whole script walk. Cheapest possible answer.
- **The speaker name.** If a talk routine references a message id, the dialogue's speaker resolves
  statically — that would not say "merchant", but it replaces "Nomad 3" with a real name.

---

## NEXT SESSION — START HERE (written end of Session 79, 2026-07-27)

**KEYWORDS: next session cold start interact icon identify NPC interactable label shop native
routine name pool talk index 0xDC script resolution verification checklist S79 deployed unverified**

### Priority 1 — THE INTERACT ICON — **SUPERSEDED by Session 80, see "The 18 interaction MODES" below**

> The icon hunt was overtaken before it started. Walking the two interaction predicates upward found
> something better and cheaper: `sceneObj+0x1C` is an **18-bit interaction-MODE mask** and
> `sceneObj+0xC8` is a parallel **18-entry event-index array**, with the mode index equal to the bit
> index. The table below is still correct about what the mod reads today; the "limit" column is now
> understood as *two of eighteen bits*. **The icon was never located and is no longer the lead** —
> nothing here is struck as wrong, it is simply no longer the cheapest route.

The engine draws an icon when the player can interact. **If the icon TYPE is a readable field, it is a
classifier the mod does not currently use** — and unlike anything script-based it is one memory read
per object, on every map, for free. This is the highest-value/lowest-cost item open.

What the mod reads today, and why the icon might beat it:

| field | what it gives | limit |
|---|---|---|
| `sceneObj+0x1C` FLAG_TALK `0x400` / FLAG_ACTION `0x004` | talk vs action | **MODE state** — reads 0 on anything the script has not armed (this is the S79 bug) |
| `sceneObj+0x0E & 0xF` KIND | 1 person / 5 gimmick | coarse; does not separate a shop from a door |
| `sceneObj+0x03 & 0x1F` scene class | 3 = character, 1 = volume | coarse |

**Do this, in order:**

1. **Find where the icon type is chosen.** Start from the two interaction predicates already
   identified — `FUN_0025bad0` (class 3, characters) and `FUN_0025be50` (class 1, gimmicks) — and walk
   their callers to whatever selects the prompt sprite. `FUN_0025b820` is the per-frame walk that
   decides what the player is near; the icon choice is at or below it.
2. **Enumerate the distinct icon ids.** If the set is richer than {talk, action} — a distinct shop /
   save / door / examine icon — that is a real classifier and it should drive `Category` directly.
3. **Check whether the icon id is stored on the object or computed per frame.** Stored = the mod can
   read it in the scan. Computed = it may still be derivable from its inputs.
4. **Only if the icon is a dead end**, fall through to Priority 2.

**Confidence bar applies: ≥0.98, offline in the decompile first, and no C++ until a Frida probe
confirms it against the live process.**

### Priority 2 — the shop binding by SCRIPT RESOLUTION (only if the icon fails)

See the CORRECTION above. `map_script.cpp` **already** walks the loaded field script generically:
routine table `hdr+0x18` (`{+0x00 nameOff, +0x08 codeOff}`, stride 0x30), name pool `hdr+0x4C`, code
span `[codeOff, next-highest codeOff)`, pattern `4f <lit:u16> 5d <nativeLo> <nativeHi>`.

Three unknowns, all answerable offline:

1. **The shop-open native's id** — S63's procedure unchanged: `dbgIndex = nativeId + 5140` against the
   archived .dbg symbol table (20,417 symbols), the same lookup that named `setmapjumpgroup = 0x011E`.
2. **How an NPC reaches its routine.** Exits use the auto-generated name `__MJ_CTRL<NNN>`. NPCs carry
   `sceneObj+0xDC` (talk) / `+0xCC` (action) event indices (Nomad Village reads `0xFFFF`, `5`, `8`,
   `9`; `0xFFFF` means INHERIT from the map's object record, so some need a second hop). **Settled by
   dumping a shop-bearing map's routine-name pool** — the reader already reads that pool.
3. **Whether `shopId` is a bytecode literal** rather than computed. Only a literal is readable.

**Priority 3 — the speaker name.** If a talk routine references a message id, the dialogue speaker
resolves statically. It will not say "merchant", but it replaces "Nomad 3" with a real name for
anonymous NPCs generally — a broader win than the merchant case.

**BANNED, do not revisit:** any binding learned by watching the player (see the strike above and the
tester's directive). Database resolution or nothing.

### Session 79 shipped but NOT play-confirmed — verify these first

Built, deployed, committed; no play session yet. Read the next log for:

1. **`inclusion: by-KIND+model kind1=N kind5=N (of which N are CHARACTERS)`** — the blast radius that
   could not be measured offline (all 20 archived logs predate the object dump). **A large `kind1` on a
   city map means that half is sweeping crowd NPCs and the kind-1 clause comes out.**
2. **The missing NPC appears** — Nomad Village should list an unnamed NPC ~1.3 m from the Nomad Elder,
   routable with `\`. Slots 51 and 56 too. **Slot 39 must NOT** (exact-position shadow of slot 38).
3. **`shadow dropped:`** lines — should fire on slot 39 and nothing else.
4. **Numbering survives a round trip** — note which Nomad is "Nomad 2", leave, return: same person,
   numbers contiguous. The store version bumped 1 -> 2, so the old 1/3/5/6/7 records are discarded and
   **numbers will differ from the last session on purpose, once.**
5. **F6 works at all** — it never has. Copy text, select an entity, press F6, hear "Labelled <text>".
6. **`KIND_DEAD` in the tally** — non-zero on a field map falsifies the actor-pool skip and it must go.
7. **S78's passed-waypoint fix** — the tester confirms loopbacks fired on SHORT routes inside the camp,
   so the camp is a valid test bed (an earlier note wrongly said a long route was required). Look for
   `dropped N leading waypoint(s)`.

### Also open, unrelated to the above

- `probe_shop_name.js` (authored, never run) confirms shopId -> master table -> npcdic. Reading the
  shop's own name when a shop OPENS is **not** covered by the learned-label ban (it names the screen,
  not an NPC) — but the shop-OPEN event itself is still unestablished (`FUN_0057c010` is a dialog
  callback, struck).
- `map_query.h` is 157 lines, over the 150 header ceiling.
- ~1,000 lines of exit/entity diagnostics have zero call sites (deliberate — see PerformanceIssues.md).

---

## The 18 interaction MODES — Session 80 (2026-07-27)

**KEYWORDS: interaction mode mask 18 bits sceneObj 0x1C bit index equals mode index 0xC8 event index
array FUN_002652d0 FUN_0026b4a0 arm FUN_0025d5e0 disarm FUN_00266bd0 default mask by scene category
category 7 talk native table offline Ghidra dump_script_native_table delta 5140 two anchors
setmapjumpgroup mpk no EBP2 mapctrl_ebp_disasm mis-based address order refuted classification**

### SOLVED — what `sceneObj+0x1C` actually is

An **18-bit interaction-MODE mask**, not "a flags word with a talk bit and an action bit". The mode
index IS the bit index; `sceneObj+0xC8` is a parallel `u16[18]` of per-mode event indices. Full detail,
offsets and the eight-mode confirmation table are in `GameArchitecture.md` — do not re-derive them here.

The two constants the mod ships are now *derived*: mode 2 = ACTION = bit `0x004` = `+0xCC`;
mode 10 = TALK = bit `0x400` = `+0xDC`.

### The correction that matters for classification

`FUN_00266bd0` sets the **default mask from the scene CATEGORY alone** (cat 5/6 -> `0x00030004`,
cat 7 -> `0x00034c85`, and so on). So the mask's resting value carries no information the mod does not
already have from `sceneCat`. **A classifier must be built on the `+0xC8` array — static per-object map
data — never on the mask.** Corroborates Session 79 from the other side: the Nomad Elder's
`flags=0x00030004` *is* the cat-5/6 default, and the woman behind the tent's `0x00030000` is that same
default with bit 2 cleared by script. She was a standard character with one mode switched off.

### TRIED & FAILED — three offline routes to the native ids

The remaining unknown is **native id -> handler function**, needed to name the 16 unnamed modes and to
find the shop-open native. It cannot come from the decompile export, which holds function bodies and
**no `.data` bytes**. These were tried first; do not retry them:

1. ~~**Interpolating ids from handler ADDRESSES** ... **Handlers are not laid out in native-id order.**~~
   **STRUCK the same session** — that refutation was itself computed from the bad stride-8 assumption.
   Run 1's data shows handler addresses *mostly* ascend with slot order (895/1179 adjacent pairs), so
   ordering is **unresolved**, not refuted. Moot either way: the stride is measured now, not inferred.
2. **The extracted `.mpk` map controllers.** All 20 files under
   `extracted\ps2data\plan_master\map_ctrl\` were scanned for the `EBP2` magic. **Zero hits** — they are
   map data, not bytecode. There is no map script to disassemble offline from that directory.
3. **`output\mapctrl_ebp_disasm.txt`.** Mis-based: routine 0 opens on `PREQ`/`LABEL`/`LABEL` and the
   sweep is decoding data as instructions. Unusable as a native-call listing without fixing the code
   base first.

### What DOES resolve it offline — and it is the exit chain's own method

A **Ghidra script**, exactly like `dump_mapjump_native.java`, which is where the `0x1EEE8B0` table slot
came from in the first place. `ghidra\dump_script_native_table.java` (authored this session, USER-RUN,
`-process -noanalysis`): validates the `mapjump` anchor before emitting anything, searches for the exe's
**own native NAME table** so natives can be named with **no delta assumed**, joins the archived `.dbg`
list as a second opinion and **reports every disagreement**, then reverse-looks-up which native calls
the arm/disarm/event primitives (the constant at the call site names the MODE) and forward-looks-up
`openfullscreenmenu` / `setshopname`. **No play session needed.**

### STRUCK — "the delta has a second anchor now"

Claimed and withdrawn the same session. `setmapjumpgroup`'s id `0x011E` came from bytecode and was then
*named* via the 5140 delta; re-deriving 5140 from that name is the same fact counted twice. **`mapjump`
is still the only true anchor**, because its handler was identified by behaviour (it calls the
transition-loader chain `FUN_00314440`), independent of any delta.

### TRIED & FAILED — run 1 of `dump_script_native_table.java` refuted its own assumption

It validated the anchor, then assumed a **dense qword array** with `mapjump` at index `0x8D`. Its own
output kills that, and the evidence is worth keeping:

- **1197 `.text` pointers over 4096 slots, 776 gaps of exactly 3** — a pointer every 4th qword. A dense
  qword array has no such period.
- **Names nonsense against handlers known by behaviour**: `FUN_00355830` (disarms interaction mode 2)
  -> `@SWCOD_000162`, a compiler switch label; `FUN_00346020` -> `sin`, though its body is a wait-poll
  identical in shape to `waitv`.
- **`FUN_00355540` (arm), `FUN_003558f0` (disarm), `FUN_00351f40` were not in the window at all**, yet
  all three are certainly natives — four-arg signature and **zero references anywhere in `.text`**.
- **No exe-side native NAME table** exists at either probed layout, so naming still depends on the `.dbg`
  join, which makes the stride the whole problem.

**`output\script_native_table.txt` is kept as evidence. Cite NOTHING from it but the anchor.** The
replacement, `ghidra\dump_native_slots.java`, assumes no layout: it locates the slot of ~26
behaviourally-identified natives, takes the **GCD of the slot deltas** as the stride, and **self-checks**
by requiring `FUN_00355540`/`FUN_003558f0` to land on a matched enable/disable name pair — reporting
**NOT COHERENT** and withholding the ids if they do not.

### Secondary, and NOT required for the chain

`frida\probe_interact_modes.js` dumps the mask and the whole `+0xC8[18]` array for every scene object,
once per map. It exists to break a tie if the native names leave a mode ambiguous — it is not the route.

### RUN 2 — `dump_native_slots.java`: right verdict, wrong stride. The table is 32-byte RECORDS.

Reported `stride=8 self-check=NOT COHERENT`. The **NOT COHERENT was correct and saved a wrong mapping
from being adopted** — but the stride is 8 only because the GCD was taken over samples that hit
*different fields of the same record*. Three samples land exactly `0x20` apart: ARM `FUN_00355540` /
DISARM `FUN_003558f0` (`0x1eedc60`/`0x1eedc80`); fire `FUN_0034e5c0` / fire-all `FUN_0034f380`
(`0x1eeec60`/`0x1eeec80`); and `FUN_003537b0` registered **four times** at `0x1eeed30/50/70/90`.

**The quad settles it** — one handler serving four adjacent natives is the
`keyscan`/`keyscanr`/`keyscant`/`keyscantr` family shape from the `.dbg` list. And **ARM/DISARM one
record apart is the `reqenable`/`reqdisable` adjacency the self-check wanted: it PASSED at stride 32.**

**LESSON (new): a GCD of address deltas measures stride only when every sample is the SAME field.**
Mixed-field samples collapse the GCD to the pointer size and silently produce a plausible wrong answer.

### TRIED & FAILED — reading the record's 2nd field as a native

Three functions the id-join named as natives are **not natives**:
`FUN_00346020` (`sin`) and `FUN_003453d0` (`waitv`) are the identical wait-poll shape; `FUN_00342f50`
(`settalkiconstatus`) **takes no arguments** and only polls + yields. A `set…` native with no argument
is impossible. They are the record's **continuation/poll field** (the Athena VM's blocking-native
mechanism). `FUN_00356990` (`lastjumpindex`) likewise *pops an arg and writes*, which no getter does.

**Neither `script_native_table.txt` nor `native_slots.txt`'s PROBE IDS may be cited for any id or name.**
Established so far: 32-byte records, ≥3 function-pointer fields per record, arm/disarm adjacency.
`ghidra\dump_native_raw.java` dumps three known windows raw, 32-byte aligned, every qword resolved,
asserting nothing.

### SOLVED (run 3) — the native table, and why two scripts got it wrong

`dump_native_raw.java` read the bytes; the layout was immediate. Full detail in `GameArchitecture.md`
("SOLVED — the Athena script-native table"). The short version:

```
BASE = abs 0x1EED720 (RVA 0x1ECD720), stride 32, name = dbg[k + 5140]
simple[k] = BASE+32k      init[k] = BASE+32k-24      poll[k] = BASE+32k-16
```

**Root cause of both earlier failures: the `-24`/`-16`.** A native's init and poll live in the physical
row *below* its simple slot, so one 32-byte row holds `simple[k]` beside `init[k+1]` and `poll[k+1]`.
Every reader that treated a row as one native blended two natives together — which is why
`FUN_00346020` came out `sin` (it is a POLL) and `FUN_00342f50` came out `settalkiconstatus` (also a
POLL, and it takes no arguments). **The give-away was there all along and was misread as noise: a
handler whose argument count contradicts its name is in the wrong FIELD, not at the wrong id.**

Confirmed by 13 handlers identified from their code before any name lookup, including
`mapjump = 141 = 0x8D` reproducing Session 63's bytecode-derived id, and one sync poll (`FUN_003537b0`)
shared by four adjacent `voice*` natives. Delta 5140 now validated across ids 42..703.

**Answers unlocked:** `reqenable` = native **42 (0x2A)**, `reqdisable` = **43 (0x2B)** arm/disarm
interaction modes; `sysreq`/`sysreqall` = **170/171** fire a mode's event; `fieldsign` = **374**;
`talktreasure` = **703**. Mode names so far: **12 = map-jump/transition** (from S63's `reqenable(12)`),
**13 = name label** (`fieldsign` arms it and sets the display name; matches `FUN_00268d10`'s `& 0x2000`).

**Shop natives are EXTRAPOLATED, not confirmed:** `openfullscreenmenu` -> 1138/1165, `setshopname` ->
1193, all past the validated band. Below the 0.98 bar until their handlers are seen to be menu openers.

### SOLVED + SHIPPED — the mod was speaking the GENERIC name for every NPC the player has met

`FUN_00263990` picks the npcdic slot as **`id*2 + (FUN_0032a930(id) != 0)`**. The mod always read the
EVEN slot, so it said "Nomad" where the game says "Arjie".

**STRIKES Session 54's "the odd slot is byte-identical in the US build; even-only is correct; there is
no second name to mine."** That was a **48-id sample** — ids 0-11 and the 433-469 gimmick band, i.e.
crowd filler and crystals/urns/treasure, the two ranges that cannot hold a personal name. Across **all
1141 ids, 247 differ**, and the odd slot is the character's real name: 221 Nomad -> **Arjie**, 228 Nomad
-> **Lesina**, 239 Nomad Elder -> **Elder Brunoa**, 159 Viera -> **Ktjn**, 220 Cockatrice -> **Agytha**.

**LESSON: a sample drawn from the ranges you already understand cannot falsify a claim about the ranges
you do not.** The sampled bands were chosen because they were the ones already being validated for
crystals and treasure — which is exactly why they were the wrong evidence for this question.

**Shipped** (`entity_classify.cpp`): `TalkNameKnown(id)` replicates `FUN_0032a930` —
`byte[&DAT_02164280 + 0x15B4 + (id>>3)] & (1 << (id&7))`, rejecting `id >= 0x800`. `DAT_02164280` is a
**static array** (`FUN_002ef640` returns its address), RVA `0x2044280`; `0x15B4` folds `FUN_002ef2b0`'s
`+0x200` and `FUN_0032a930`'s `+0x13B4`. `NpcdicName(id, known)` takes the selector, and falls back to
the even slot if the odd one is past the table. The bit is live state (`settalknpcname` /
`releasetalknpcname` write it) so it **flips mid-session** — read per scan, never cached.

`inclusion:` now ends with `N speaking their PERSONAL npcdic name`, so the next log sizes it in play.

**Known consequence, documented not papered over** (`entity_labels.h`): `baseLabel` is part of the label
store's key, so a revealed name orphans a player label and moves the object out of its "Nomad 1..5"
numbering group. The numbering change is CORRECT, and the store holds zero player labels today.

## SOLVED — the label store leaked numbers without bound (Session 81)

**KEYWORDS: Cockatrice 37 numbering leak entity_labels NumberFor anchor never refreshed roaming
unbounded growth 39 records six animals stateless numbering container slot within scan version 3
rewrite on mismatch repeating discarded line party roster bodies scene category 5 NPC 1..6 phantom
odd npcdic slot always Dania Lesina Masyua Nanau Jinn grace window numbered list not spoken list**

Four defects, reported together, three with one root cause: **the mod kept state about objects instead
of looking them up.** The tester named it: *"you're tracking the interaction component for all NPCs
instead of just resolving the NPC location to its interaction component label in the database."*

### 1. The number leak — `NumberFor` allocated a record per roaming step

`EntityLabels::Match` returned null when `{mapId, baseLabel, nameIdx}` was ambiguous *and* no stored
anchor was within `kAnchorDist = 1.5 m`. The anchor is frozen at creation and deliberately never
refreshed, so **a roaming object permanently outran its own record**; `NumberFor` then minted a new
one, and the free-number search counted every leaked record as taken, so the number could only climb.
No cap, no eviction, no cleanup existed anywhere in the file.

**Proof on disk:** the live v2 store held **39 records labelled "Cockatrice", numbered 1..39, for six
real animals** (only slots 49/63/64/65/66/67 ever appeared), plus 6 `NPC` and 4 `Nomad`, and **zero
player labels** — the store's only product was numbers, and the numbers were wrong.

**Fix: numbers left the store entirely.** `NumberDuplicateLabels` now ranks a duplicate group within
the current scan, keyed on `{container, slot}` — the object's place in the game's own handle table,
which is the pair the engine itself uses to name an interaction target, and which is stable for exactly
as long as the map is loaded. Session 79's strike on `container.slot` was about PERSISTENCE and does not
forbid this: unstable across loads is why it may not be stored, stable while loaded is why it is right
for a number recomputed every scan. **A store cannot leak numbers it does not hold.**

### 2. The numbered list was never the list the player heard

`ApplyPlayerLabels` + `NumberDuplicateLabels` ran at the end of `BuildLocked`, but `RescanLocked` merges
the 2-second grace-window survivors **after** `Build` returns. The persistent store hid this (a number,
once assigned, was permanent). Stateless numbering would not have: a group member streaming out for a
frame leaves the survivors to compact to 1..N-1 while the carried entity still holds its old suffix, so
two entries answer to one number for up to the whole grace window. **Both passes moved into
`RescanLocked`, after the merge**, and both were made idempotent (`baseLabel` frozen once; `label` reset
from it before re-suffixing).

### 3. "store is version N -- discarded" repeated forever

`Load()` returned early on a version mismatch **without rewriting the file**, and `Reload()` is unguarded
and runs on every area change. Once numbers stopped triggering saves, nothing would ever have overwritten
it. **`Load()` now calls `Save()` on the mismatch path**, which silences the repeat *and* deletes the
leaked records. The success line is also gated on a non-empty store.

### 4. Phantom "NPC" entries — party/roster bodies

Three scene-category-5 objects (kind 1, no name, all at one position 6 m above the floor) were admitted
by the `present` route and announced as "NPC 1..3", making **six** indistinguishable `NPC n` entries out
of three real anonymous townsfolk — which is why the Session 79 NPC was reported as *still* unfindable
although she was in the list all along. `DropShadowRegistrations` could not reach them: it requires the
ANCHOR of a stacked pair to be `gameNamed`, and all three are unnamed. Now dropped when
`sceneCat == 5 && !named`; see `GameArchitecture.md` for why that is narrowed and instrumented.

### 5. "Nomad 1..5" — the numbers should never have existed

Each of those NPCs has a distinct personal name in its own npcdic record. The mod now reads the odd slot
regardless of whether the game has made the introduction, so the group dissolves into Dania, Lesina,
Masyua, Nanau and Jinn and needs no numbering at all. Only genuinely identical objects (npcdic 238, whose
two slots both read "Cockatrice") still get numbers.

**LESSON: when a disambiguator keeps breaking, check whether the thing it disambiguates should exist.**
Four sessions were spent making a number stable across rescans, streaming, reloads and sessions. Five of
the six numbered groups on that map had distinct real names available in data the mod was already
reading — one slot over.

## SOLVED — a filter that logged success and changed nothing (Session 83)

**KEYWORDS: grace window re-admits filtered entities lastSeenMs never ages out shadow drop undone
WasFilteredThisScan NoteFiltered phantom NPC spawn point floating above floor FindPolyAt no rejection
threshold NavReach unplaced characters majority guard stood down**

### The bug under the bug

`EntityList::Internal::RescanLocked` carries an entity over when its scene object is missing from the
freshly built list — the 2-second grace window that stops the handle table streaming somebody out
mid-approach. **It cannot distinguish "the engine stopped reporting it" from "a scan pass deleted it",**
and the consequence is permanent rather than transient:

1. A filtered object is a **live engine object**, so `RefreshPositionsLocked` keeps reading its
   transform and keeps stamping `lastSeenMs`.
2. So `now - lastSeenMs` never exceeds `kEntityGraceMs`, so it never ages out.
3. So once it is carried in **it is in for the life of the map** — while `Build` rebuilds from scratch
   every scan, re-finds it, re-deletes it, and re-logs the drop.

Net effect: `shadow dropped:` firing on every single rescan while the object stayed in the list the
player heard. This had been quietly undoing `DropShadowRegistrations`, and would have undone the new
placement pass identically.

**Fix:** `EntityScan::NoteFiltered(sceneObj)` from every drop pass, `WasFilteredThisScan()` consulted by
the merge. **LESSON: a "still there?" test that compares against a list some other pass is allowed to
delete from is not asking the question it looks like it is asking.**

### TRIED & FAILED — `NavReach` alone cannot find a floating object

`NavMesh::FindPolyAt` resolves by **XZ containment with Y as a tie-break and no rejection threshold**,
so an object 6 m above the ground still resolves to the triangle beneath it and `NavReach::Reachable`
calls it reachable. The height test has to be a separate question (`PolyHeightAt`, `kFloatingDrop`).

The same fact explains "routed to an obstacle": `PathSearch` resolves its goal with the same call, so a
body on top of an obstacle snaps vertically onto the floor under it and the player is walked to its base.

### STRUCK — the Session 82 story gate (`+0x0E & 0x10`)

Shipped with a counter that would falsify it; the counter read **zero** on the map that motivated it —
every candidate has `en=1`. Removed. Third theory in four sessions killed by its own instrumentation
inside one play session.

---

## SOLVED — the seam cache was a full map behind (Session 85, 2026-07-28)

**KEYWORDS: exits swapped mislabelled missing exit 199 steps unpathable waterway Garamsythe 311 315
Central Spur Stairs Northern Sluiceway No. 10 Channel seam cache stale HasWorld liveness identity
PrimeMapJumpSurfaces CachedMapJumpSurfaces InvalidateMapJumpSurfaces IsFieldNavSafe teardown epoch
crossing oracle FALSE MISMATCH PublishClaims round trip A B A NavTrace transient mapId 0 double
TRANSITION FIRED**

### Symptom

Waterway exits correct on the first load of a map, scrambled after any transition: the exit the player
spawns beside mislabelled, one exit gone, one announced ~200 steps away and unreachable. Reported for
Garamsythe but not map-specific — it applies to **every** transition after the first.

### THE LESSON — `HasWorld()` is a LIVENESS signal, not an IDENTITY signal

```cpp
if (s_map != mapId) { s_map = mapId; s_haveMap = false; s_surf.clear(); }
if (!s_haveMap && HasWorld()) { ReadMapJumpSurfaces(s_surf); s_haveMap = true; }   // WRONG
```

`HasWorld()` proves **a** walkmap is resident. It does not prove it is **this map's**. The map id
flips BEFORE the engine swaps the walkmap, so this swept the PREVIOUS map's polygons on the first
frame of every map and then latched them (`s_haveMap = true`) for the whole visit. The comment above
it anticipated the *empty* case and never the *stale* case — the cache was, permanently, one map
behind.

**Generalised rule, now in `GameArchitecture.md`: any per-map cache invalidates on the TEARDOWN epoch
and fills only behind `PlayerState::IsFieldNavSafe()`. Never on `HasWorld()`.** `IsFieldNavSafe()` is
false for the whole of a transition (`CondAreaId` rejects `0xFFFFFFFF`, `CondLeaderPtr` is zeroed at
teardown start), which is exactly the property needed. This is not a new mechanism — `NavMesh` /
`NavReach` already had both halves and were measured **correct on every load** in the same log
(139 / 1997 / 139 polys across 311 → 315 → 311). The seam cache was the one cache in the stack with
neither.

**And: never a private second copy in a consumer.** `exit_scan.cpp` had its own `CachedSurfaces` with
the identical latch, justified as "saves the rescan a vector copy" — it saved nothing (the shared
cache returns a copy by design) and it meant fixing the shared one alone would have changed nothing.

### How it was found — Session 84's diagnostics, read back, no RE at all

The surface inventory printed the seams per map and they were **byte-identical across two different
maps** (log 1937–1939 vs 2178–2180). That single comparison localised it. The controller dump on the
same frames showed the destination half fresh and correct, so it could only be the positions. No
Ghidra, no Frida, no new model.

### TRIED & FAILED — the tempting third option

**A walkmap fingerprint** (hash the ctx pointer, the four array bases, the grid dims/origin, re-sweep
when it changes). Rejected: it is a NEW unproven identity model invented to solve a problem two
existing, measured-correct mechanisms already solve. It would also have needed its own invalidation
story for `NavMesh`. Do not reach for it.

### FALSE LEAD, worth recording — the crossing oracle accused a correct binding

`CROSSING ORACLE: left map 315 via seam g2 | mod claimed 313 | ACTUALLY ARRIVED 311 <== MISMATCH --
the group->destination binding is WRONG` (log 2281). **The binding was right.** The oracle reads the
same poisoned surfaces to decide WHICH seam group was crossed, so it mis-attributed the crossing and
then blamed the (correct) group→destination map. Session 84 had separately flagged that 311 is the
one map whose controller→group mapping is not the identity (CTRL000→2, 001→3, 002→1) and that it was
the map reported swapping — a coincidence, correctly hedged there as "a lead to measure, not a
conclusion", and the span dump ruled span bleed out on the same lines.

**A diagnostic that shares a data source with the thing it measures can only be trusted about the
other half.** `NavTrace` no longer latches the seams.

### Two more defects fixed in passing, both real

- **`PublishClaims` was latched, not idempotent**, and an **A→B→A round trip hit both bugs** — which
  is precisely what walking between two waterway maps is. (1) `if (mapId == g_claimMapA) return;`
  fired on the FIRST call, so a map whose field script was not readable yet published zero rows and
  could never republish; the oracle then said "the mod claimed NOTHING for that group" for the rest
  of the visit. (2) Re-entering the map still held in `g_claimMapB` skipped BOTH the prune and the
  `B = A` update, so old rows survived and new ones were appended beside them — and
  `ClaimedDestForGroup` returns the FIRST match, i.e. the stale one, while the store grew every trip.
- **`NavTrace::OnFieldFrame` treated a transient `mapId == 0` as a real map.** Every crossing fired
  TWICE (`TRANSITION FIRED: mapId 701 -> 0`, then `0 -> 311`) and the first CLEARED THE TRAIL, so the
  second had no crumbs and the oracle silently returned — the real crossing was never oracled at all.
  Now returns early on `mapId <= 0`.

### KNOWN-SIMILAR, deliberately not touched

`EntityScan::CachedSigns` (`exit_scan.cpp`) has the same latch shape against the `+0x70` field-sign
table. It feeds **door naming only**, not transitions, and there is no evidence it misfires. Recorded
here so it is greppable if door names ever come back wrong after a transition.

## SOLVED — a CHARGE verb on an EXECUTION line (Session 90, 2026-07-29)

### Symptom

Enemy abilities reached the combat log, and the game's own announce was correct
(`"Urstrix A readies Slap."`), but the mod's own line read `"Urstrix A readies Slap on Vaan. 14"` —
an ability that had already connected, narrated as if still winding up. Party side too:
`"Vaan readies Steal on Urstrix A"`.

### Cause

`CombatFormat::DamageLine` fires off the damage applier `FUN_003112f0`, i.e. after the hit. Its verb
switch had been copied from `FUN_00469af0`, the **charge-phase** announce emitter — single caller,
`FUN_00304850` at action start, ids chosen purely by `row+0x1E`. Categories 2/7/9 → `0x0E` "readies",
so every technick and enemy ability got charge wording at execution time. `combat_system.md` §9.1.3a
explicitly instructed the mirroring; that sentence is now struck in place.

### Fix

Two separate vocabularies. Charge stays the game's, read verbatim. Execution is ours:
`attacks` (cat 0 and anything unidentified) / `casts` (cat 1) / `uses` (cats 2, 3, 5, 6, 7, 9, 10).
`Phrase::Id::Readies` is kept but is no longer an execution verb.

### The general lesson

**A verb encodes WHEN, not just WHAT.** Two functions can switch on the same byte and still need
different words, because they sit at different points in the action's life. Before reusing a mapping,
ask what phase the function that owns it runs in.

## Tried & Failed — proposing a new hook for a feature the existing mechanism already covers

Having measured that `FUN_00304850` suppresses the announce on repeat (an actor repeating one ability
on one target announces once), I proposed hooking the action-start writer `FUN_0030f760` so the mod
could announce every cast itself — with a Frida probe first, since that function sits at 0.94.

**The user rejected it, and was right.** The ask was "speak it as well as logging it". That is
`speakNow` in `CombatLog::Append`, the same one-line mechanism loot, defeat+EXP and low-HP already
use. No new hook, no reconstruction, no probe, no 0.98 promotion needed.

The measurement was real; the work item was invented. **A true finding about the engine is not
automatically a thing to build.** Check whether the requested behaviour is already reachable through
a mechanism the codebase has before escalating to a new hook — especially when the escalation drags
in a probe, a confidence promotion and a phase gate. Recorded because the reasoning looked rigorous
the whole way down and was still pointed at the wrong question.

## SOLVED — F4 could silently kill menu reading (Session 90, 2026-07-29)

`F4` was bound to `TextCapture::ToggleInterception()`, a developer A/B that disabled the painter
callback swap. Disabling it **stops list-row text being captured**, so one accidental press left menus
silent with no announcement and no way for a blind player to know what had happened or undo it.
Removed entirely (`InterceptionEnabled()` had zero callers; the flag had one read site). `F4` is now
the combat-verbosity toggle. **Do not put a capability-destroying diagnostic on a bare function key.**

## OPEN — the mod menu's arrow keys also reach the game (Session 90, 2026-07-29)

**Confirmed in play:** with the mod menu (`F8`) open, pressing Up/Down/Left/Right navigates the menu
**and** moves the camera/character, because the game receives the same keypress. Accepted for now by
the tester; `F4` is the workaround (it toggles Combat verbosity with no arrow keys involved).

### This is NOT a broken intercept — there is no intercept to fix

Read this before "fixing" it. The mod is **strictly read-only on the KEYBOARD** (`CLAUDE.md`; since
S100 with the one recorded Auto-walk exception, which INJECTS movement bits and still never SWALLOWS
a key — injection is not swallowing): the DirectInput buffer arrives at the tracker as `const`,
nothing is ever swallowed or rewritten on the read path. The mod observes keys on that path; it has
never consumed one.

> **The word KEYBOARD is doing work in that sentence as of S162, and it did not have to before.**
> The gamepad intercept CAN swallow — that is the whole point of it — so "swallowing is forbidden"
> is now true of the keyboard only. This paragraph is about the arrow keys, which are keyboard, so
> everything below still stands; do not generalise it to the pad, and do not "fix" the pad by
> pointing at it.

`MenuNavCallback`'s `bool` return means only "a mod-side
consumer handled this", which suppresses the mod's *own* fallback dispatch — it has never had any
effect on what the game sees, and was never intended to. The status virtual buffer has behaved this
way since Session 71.

So the options are all real costs, not oversights:

1. **Leave it.** Open the menu while standing still, or use `F4`. Zero risk. Current state.
2. **Rebind the menu to keys the game does not use.** Cheapest real fix. The game binds F1/F2/F3 and
   nothing else in the F-row, so `F9`-`F12` (or a second tap of `F8` to step) could drive the menu
   with no passthrough at all. **`F7` is reserved for autodetail — do not take it.** Costs the
   familiar arrow-key idiom, and the status buffer would still use arrows, so two surfaces would
   navigate differently.
3. **Actually swallow the keys.** Requires hooking `IDirectInputDevice8::GetDeviceState` to *mutate*
   the buffer before the game reads it. This is a **category change** — the mod would be altering
   what the game receives — and per `CLAUDE.md` it needs **explicit user permission and a design
   discussion first**. It also puts a mod-owned write on the game's input hot path, and a stuck flag
   would mean keys silently dying with no way for a blind player to diagnose it.

**Do not implement option 3 without asking.** If this gets picked up, option 2 is the one to price
first. Related: `Docs/Controls.md` "Mod menu (F8)", `feedback_check_controls_md_before_input_diag`.

## SOLVED — three defects behind "routes through walls" and "the beacon stops early" (Session 95, 2026-07-30)

Two tester reports, one log (`FFXII-Screen-Reader-Latest.log`, session of 2026-07-30 19:41–19:51).
Three separate causes, and each one was **already visible in the log the mod itself writes** — the
numbers had been printed for a session and nobody had counted them.

### The counting that found all three

```
plan=Frontier legs   18        worstFrac >= 0.90 BREACH   25   <- corner-footprint failures
plan=Route    legs   17        worstFrac <  0.90 BREACH   32   <- real body-sweep failures
validate BREACH      57
validate OK          17
frontier: goal unreachable (validation never passed)  17
frontier: goal unreachable (unreachable)               1
```

**More than half the routes in the session shipped as `Frontier`, and 17 of those 18 shipped
BECAUSE VALIDATION FAILED.** That single tally is the whole diagnosis: the escape hatch was being
taken on most routes, and the escape hatch emitted an unvalidated polyline.

`probes` vs `checked` separates the two breach kinds with no ambiguity, because `CheckLegs` spends
1 probe per sweep and 1 per interior corner and returns on the first failure: `probes == 2*checked - 1`
means the **sweep** failed, `probes == 2*checked` means the **corner** did. Every `worstFrac >= 0.90`
breach in the log is the even case. That is a derivation from the code, not an inference from the
numbers — the numbers only confirm it.

### 1. The frontier route was never validated, and its geometry was incoherent

`path_search.cpp`, the `Plan::Frontier` block. When no attempt validated, the fallback:

- funnelled **`best.portals` — the corridor to the GOAL poly — toward `fpt`, a point on `best.bestNear`,
  a DIFFERENT poly.** A portal sequence and an endpoint that do not belong to each other. The funnel
  cannot repair that; it threads all the portals and then jumps.
- in the genuinely-unreachable case `best.portals` is **empty** (the code `break`s before reconstructing),
  so the "route" was a bare straight line from the player to a point several polys away —
  `corners=2` in the log, e.g. `19:48:52 ... frontier ... ending at poly 73, 6.5m short ... corners=2`.
- **ran no validation at all.** So `Plan::Frontier` — the enum value introduced precisely so a
  shortfall could never be spoken as a plain route — announced how far short it stopped *while walking
  the player through the wall it had failed to route around*.

The clearest instance, `19:49:06`: four attempts, four BREACHes, then
`frontier: goal unreachable (validation never passed); ending at poly 325, 16.4m short ... corners=20`
and `say="North 210, Northwest 7, West 64, South 7. 288 steps. Blocked, 22 steps"` — 20 corners lifted
straight from attempt 4's corridor, which the validator had just failed at leg 10 of 19.

**Fixed** in the new `path_corridor.{h,cpp}`: the corridor is rebuilt from A*'s parent links **for the
frontier poly**, funnelled, **validated**, and cut back to the part that passed; the shortfall is then
measured from where the route really ends. Two candidates are considered — the furthest-reaching
**proven prefix banked during the attempts**, and a fresh corridor to the nearest poly A* reached — and
the nearer one wins. Neither can contain a leg the body sweep did not pass.

### 2. A tight corner is not impassable terrain

See `GameArchitecture.md`, "THE COROLLARY FOR ROUTE VALIDATION". `NavFootprint::Clears` replicates
`FUN_0022f9b0`, whose response to a violation is to **push the body to tangency** — it is a
depenetration rule. `path_validate.cpp` treated a failure of it as a route breach, which cost 25 of
57 breaches and, worse, **banned the portal on a leg that had swept clear**, so every retry detoured
around a good opening. Fixed: the sweep decides `ok`; tight corners are counted (`tight=N@i` on the
`validate:` line) and validation continues past them.

### 3. `p` was silently redirecting the audio beacon

Tester: *"when the targeting state ended, the beacon only routed me to the leg of the route I was on,
it did not continue on to the next leg."*

`PathPlanner` had ONE "last request" memory. `RequestReplan()` — the beacon's off-route recovery —
re-ran it. But `\` **and** `p` both call `Request()`, and `p` routes to the locked battle target. So:

```
19:50:33.890  'p' pressed: target acquired at (66.23,6.85,108.37)      <- an enemy
19:50:34.078  [BEACON] party clear -> resuming objective
19:50:36.406  [BEACON] leg reached -> advancing to leg 3 of 10          <- objective route intact
19:50:44.875  replan: silent re-run of last target=(66.23,6.85,108.37)  <- the ENEMY
19:50:44.921  drain seq=29: target="Steeling A" ... plan=Route legs=2
19:50:48.578  [BEACON] arrived at destination -> final cue, stop
```

The same thing at `19:49:48` → `19:49:50` with `"Dire Rat C"`. The resume itself works perfectly —
`advancing to leg 3 of 10` proves the route survived combat untouched. What killed it was the FIRST
off-route re-plan afterwards, which is near-certain to fire: the stray test is skipped while engaged,
so the moment the party is clear the player is standing wherever the fight took them.

**`p` already declared it has no business with the beacon** — it passes `seedBeacon=false`, and the
header even explains why. That flag now also gates whether a request becomes the beacon's objective.
`RequestReplan()` restores from a separate objective snapshot; nothing but `\` can write it.

**The shape of this one:** a flag that correctly said "this request is not the beacon's" was consulted
on the outbound path and ignored on the recovery path. One fact, two code paths, and only one of them
knew it.

### Still open, MEASURED, deliberately not changed

**A `Frontier` beacon fires the arrival cue at a point that is not the destination.**
`19:48:52` seeds the beacon with the frontier point of a route 9 steps short; `19:48:54` logs
`arrived at destination -> final cue, stop`. The player then pressed `\` again from near that point and
got a full `plan=Route` (`19:48:56`, `seq=3`) — so the objective was reachable from there all along.
Changing this means either a new cue/word (needs permission) or continuing the beacon past a frontier
endpoint, and both should be judged against the NEW frontier behaviour rather than the old one.
Session 95 fixes 1 and 2 should cut how often it happens (25 of 57 breaches disappear), but they also
make a frontier route SHORTER when it does occur.

**`kMinFraction = 0.90` is a ratio, so it penalises short legs.** A leg is judged on
achieved/requested, and the depenetration pull-back at the far end is a fixed ~one body radius
(0.27 m). On a 2.7 m leg that is exactly the 10% of slack; on a 1 m leg it is more. A distance-based
test (`(1 - fraction) * legLength <= bodyRadius + slack`) would be dimensionally correct. **Not
changed — no measurement yet ties a specific false breach to leg length**, and the 0.85 cluster in the
log could equally be real. Left as a stated hypothesis, not a shipped tuning change.

## Tried & Failed — refusing a TERRAIN TYPE instead of measuring PASSABILITY (Session 96, 2026-07-30)

**Do not re-add a terrain-type gate to `NavMesh::Walkable`.** This was tried, shipped, and refuted in
play inside one session.

The change: `Walkable` went from `(effectiveFlags & 7) == 0` to
`MapQuery::FloorWalkable(poly, PartyMovementClass())` — the engine's own `FUN_00230a40`, whose bit-23
branch was read as "the marker the designer puts on water, lava, bog and out of bounds".

The decompile is not in doubt. The claim built on it is:

- **The party walks on bit-23 polys.** Garamsythe's water is ankle-deep, the game has no swimming, and
  the tester walks it every time they cross a channel. On map 311 the check refused **399 of 690 floor
  prims** and made an exit that had routed for the whole game unreachable.
- **The evidence for it had already been spent.** It was justified as the cause of "three sessions of
  routes through impassable terrain" — which Session 95 had already diagnosed and fixed (the
  unvalidated `Plan::Frontier`). A second explanation stacked on a solved problem.
- **The class it asks about is probably wrong.** `GameArchitecture.md` records at conf 0.97 that the
  movers pass class **4**, traced through the callers that pass it (`FUN_0032bcc0` / `FUN_0032ca70` ->
  `FUN_00230c10` arg5 -> `moveCtx+0x50`). The override reasoned instead from what `FUN_002681d0`
  *writes* (0, to `holder+0x153`) and never showed that field reaches the callee. Class 4 hits no
  per-class branch — consistent with the party walking bit-23 ground.

**The general lesson: prefer the call site over the writer.** Only one of them says what the callee is
actually handed.

**And the question the router needs is not what terrain is made of.** It is "can the character get
there", and the instrument for that is the engine's own body walk at character scale — 0.5 m steps
with depenetration carried forward, which is the question the game asks itself sixty times a second.
`NavMesh::TerrainRefused` still asks the per-class flag question and **decides nothing**; `NavTrace`
checks it every metre against the poly the player is standing on and logs `STANDING-ON-REFUSED` when
either predicate disagrees with the player's own feet.

### Tried & Failed — demoting `NavFootprint::Clears` in `BodyFitsAt`

The knock-on. With water newly a hard border, every sewer walkway had one on both sides, `Clears`
started refusing crossings everywhere, and map 315 produced 618 reachable polys and zero routes. The
response — make `Clears` a counter instead of a refusal — removed the only thing keeping A* out of
gaps the body cannot pass, and cost a third exit on map 311. The tester diagnosed it from behaviour:
*"pure A* got us to all 3 exits, so whatever you added is either flagging walkable terrain or routing
through an obstacle it didn't before."*

**The level was wrong, not the test.** A `false` DELETED the edge; it now makes it expensive
(`kTightPenalty`). See below.

### SOLVED — nothing severs the graph; everything difficult is expensive

Every routing regression this session came from one move: taking a real measurement and using it to
DELETE an edge. Delete enough and a reachable goal becomes unreachable — and once it is unreachable
there is nothing left to validate, repair, or honestly report.

Every refusal in `path_search.cpp` is now a price in metres (terrain 2000, tight 500, measured block
2000, breach 500 accumulating). The only remaining cut is "no neighbour", where there is nothing on
the other side to price. The heuristic stays Euclidean and admissible because penalties only add.

**The portal ban went with it.** The log has A* reaching the goal on attempt 1, banning the breaching
portal, and reporting "goal unreachable" on attempt 2. A ban is a permanent, binary answer to a local
question; a price is neither.

### SOLVED — a breach is a verdict on the CHORD, not on the corridor

The corridor A* returns is walkable by construction: every portal was measured with the body's own
footprint and sweep before the edge was expanded. The taut chord the funnel draws across it is an
optimisation and can leave the walkable strip. Proved both directions in one request on map 311: the
chord's leg 3 stopped the body at 6.23 m of 9.00 m on four consecutive attempts, while the frontier's
less-taut polyline walked the same ground with `cutByValidation=0`.

Repair is now a ladder, **entirely on the failure path**: taut chord -> un-pull the failing leg and
the corner it aimed at -> retreat to `badStopAt` (the engine's own resolved position, reachable
whatever is in the way) -> full corridor with no string-pull. A route that validates on the chord runs
none of it and cannot be changed by any of it.

### SOLVED — the ladder's REACH was the bug: a final-leg breach had no rung at all (Session 97)

**The ladder repaired 17 of 17 breaches it was offered. All 7 "No path" results on the maps that
otherwise route were breaches all three rungs declined by their own guards** — `firstBad == total`
every time. `Unpull`'s corner replacement is gated on `interior`; `retreat` on
`bad + 1 < poly.size()`; `full-corridor` on `bad == 1`. Then the re-cost is correctly refused for a
final-approach breach, and it falls to the frontier, which is suppressed. "No path" 5.2 m short of a
10 m route.

**It is the same bad corner either side of a leg, and the log proves it inside one map.** Map 321,
target `(47.0,-0.00,150.75)`, corner `(47.0,-0.00,156.0)` in every route: from `(44.60,157.01)` the
breach lands on leg 1, `Unpull` replaces that corner, route spoken; from `(43.17,159.42)` it lands on
leg 2 with the same corner as the DEPARTURE point, no rung, "No path". Pass and fail on identical
geometry, decided by which side of the corner the player stands. That alternation is what "the
pathfinder abruptly cuts out as I walk" looks like from the outside.

Why that corner is unwalkable, from the log alone: `reached=0.27m` of a 5.25 m leg with the engine
resolving the body **+0.3 m in z, away from the target** — depenetration, nothing else. The corner is a
raw portal endpoint (`EdgeClearSpan` hands back the full edge when all 7 samples pass, and `SampleT`
never samples an endpoint), and `InsetCorners` is inert here because its improvement test is
`NavFootprint::Clears`, a *mesh-boundary* test, on maps where the walls are volumes: it printed
`margin=4.73m` and `margin=1e9` at points the body cannot move off. `inset=0` on all 16.

Fixed by **reach, not by changing any rung**: new `PathFunnel::UnpullDeparture` replaces the corner the
leg departs FROM with that portal's measured span midpoint; `retreat` serves a final leg by *inserting*
`badStopAt` before the destination rather than replacing it (the destination never moves — S76);
`full-corridor` loses its `bad == 1` gate, since the corridor is walkable by construction on every leg
and it is the last rung anyway.

**Strictly additive, deliberately.** A draft ordered the two un-pull rungs by `badReached`, which is a
sound measurement — a body that never left its corner cannot be helped by waypoints further down the
leg — and it would have re-ordered two routes that already repair (`reached=0.17m`, map 311). Dropped:
after Session 96, a change that can alter a working route buys its way in with evidence or it stays
out. Every currently-succeeding repair takes the identical path; the only new behaviour is on breaches
that had no rung at all.

### SOLVED — a transition's destination is a SURFACE, not a point (Session 98)

Map 315's "No path" was never a pathfinding failure. **21 of the route's 22 legs validated**; the
body ended ON the goal polygon (`stopPoly=324` == `goalPoly=324`); and all three repair rungs failed
*including the full corridor*, 75 points, the least-taut polyline that exists. **When the corridor
itself cannot reach a point, the point is the problem.**

The exit's route target is written in exactly one place, `exit_scan.cpp:234` →
`MapQuery::NearestPointOnSurface` — a brute-force scan for the **nearest tagged triangle VERTEX in
XZ**. Two independent defects in that one line:

1. **A vertex is not a place to stand.** Every triangle vertex lies ON the walkable boundary by
   construction — which is exactly what the breach diagnostic reported: `margin=-0.27m`, i.e.
   distance to a hard border of 0.00 m. `kArrivalTol = 3.0` has been absorbing this on every map
   since S75.
2. **Straight-line nearest is not WALKING nearest.** On map 315's 27 m, 16-poly seam the
   crow-flies-nearest vertex was the corner the walkable approach reaches LAST.

The consequence: **the route drove 20 m ALONG the exit surface.** Leg 22 ran from ~`(173, 61.7)` to
`(153, 62)` and the surface is `x[153..180] z[52..62]`, so its START was already inside it. The
discarded frontier ended at `(169.28, 60.48)` — also on the surface. *The route arrived; the
arithmetic said it had not.*

Systemic, not map-specific: map 321 has seven surface groups of 5-7 polys spanning 10-17 m, several
with corner-to-centre distances of 9-11 m. A 4-poly seam hides it inside the 3 m tolerance.

**S75 was right about the near edge and wrong about "near".** It moved exits off the centroid onto
the nearest vertex because Southern Plaza's 28-poly seam "overstates the walk and the route drives
through the transition instead of to it". Correct diagnosis, ruler-based implementation — and it
reintroduced that exact failure from the opposite direction the moment a seam was approached from
its far side. Reverting to the centroid re-breaks Southern Plaza; that is not the fix.

**Fix**: only the search knows which part of a seam is reachable. `Entity::seamGroup` carries the
map-jump group to the planner, which resolves it to `surf.polys` (stored since S64 under a comment
that already said "a route to this exit is a search whose goal set is exactly these", read until now
by one diagnostic). `PathSearch::Run` consults it **only** where it would otherwise fall through to
the suppressed frontier, picks the member whose closest point to a PROVEN-REACHABLE position is
nearest, and re-runs the ordinary search there. Depth-1 recursion; the re-run passes no seam set.

**Strictly additive, checkably.** The block sits below the `return Plan::Route` a validated route
takes, so a working route never reaches it. Every touched signature is additive with a default, so
`p` and the `'` probe are untouched. `CachedMapJumpSurfaces` returning false = NOT SWEPT leaves the
set empty and the behaviour identical.

### Two guards that would have been wrong — keep them as written

- **Never guard the seam re-run on `pick != goal`.** On map 315 the failed search's goal poly IS a
  seam member (the huge triangle the body ended up standing on), so `pick == goal` — while its
  centroid is 14 m from the vertex `to` names. **A poly is not a position.** The guard compares
  POINTS.
- **Never aim at the member's centroid.** A seam triangle here can be 16 m long, so its centre can
  sit on the far side of whatever stopped the route. Aim at `ClosestPointOnPoly(member, ref)`, where
  `ref` is a position the search proved it reached.

### `e.pos` is a PROXIMITY measure, not a route target — do not "fix" it globally

It is recomputed every scan from the live player position, so `/`, the `[`/`]` listing distance and
the `kAtExitDist` "At the exit" check all read correctly as the player approaches from any side.
Making it interior, or picking it by walking distance, would change the last leg of every transition
route in the game including the ones that work. One value, two questions; only the route's question
needed a different answer.

### Do not reverse-derive a seam group from a poly's EFFECTIVE flags

The seam sweep reads RAW flags (`map_seams.cpp:45-55`). Bits 3-6 are simultaneously the map-jump
group AND the index into `FUN_00232020`'s group override bank (`C = ((flags >> 3) & 0xF) + 0x40`), so
an override can rewrite the very bits the group would be read from. A group taken from `eff` is not
the group the sweep bucketed by. Plumb it.

### SOLVED — the Northern Sluiceway: a coarser duplicate of a test the sweep already does (Session 97)

Map 315 could not be routed at all. **All 16 breaches on it were `why=wall`, and `MapQuery::BodySweep`
objected to not one leg.** The only thing refusing every route was `PathValidate::WallAcross`: one
`MapQuery::PointInVolume` sample at the midpoint of a leg, taken before the sweep ran, and decisive.

Both halves of the premise it was built on (Session 96: "walls are volume primitives in CSR layers 1-2
... so a wall standing inside a floor triangle passed every check the router had") are **STRUCK**:

- **`FUN_00230c10` iterates layers 0, 1 AND 2** (mask 7, callback `FUN_0022de60`, same `0x4000`-tagged
  `0x90`-stride array). The sweep has always collided with volumes. Conf 0.97.
- **`FUN_00232490` tests exactly one bit (31) and has no class filter**, where the engine's own movement
  collision reads `merged_flags & 7` against the mover's query class — and **class 4 is solid only when
  `queryClass != 4`, which is precisely what the party's movers pass.** It also hard-excludes the
  `>= 0x5000` range, i.e. doors and moving platforms. Full field map in `GameArchitecture.md`.

A point test also cannot answer a question about a line, and the log caught it contradicting itself
inside one request (`seq=41`): leg 2 of attempt 1, `(15.7,114.1)->(20.2,114.6)`, midpoint `(17.95,114.35)`
-> WALL; leg 2 of attempt 2, `(16.5,115.0)->(63.6,114.4)`, passing within 0.6 m of that same point,
midpoint `(40.05,114.7)` -> clear and swept at fraction 0.97. Which verdict a leg got depended on where
its midpoint landed. *When a new instrument fires on everything, suspect the instrument first.*

**Safe to delete where routing works, and that was measured before it was done**: `walls=0` on all 35
validation runs on maps 311 and 321, `walls=1` on all 16 on map 315. The veto could not change any
outcome on the maps that route. It is now `WallSuspect`, ground-pinned, counted as
`volHit`/`volWalked` on the `validate:` line — the pair that either retires the volume theory or
revives it with coordinates.

**Two traps it left behind, both now closed.** The wall branch `return`ed *before* `Diagnose`, so every
`why=wall` line printed `corner: poly=-1 clear=0 margin=0.00m vol@stop=0 vol@+0.3m=0` — defaults, not
measurements: the same shape as the `tight=0@0` trap below, in a second branch of the same file. And a
wall verdict set `badReached = 0`, which disabled the `retreat` rung, so it was **unrepairable by
construction**.

### OPEN — the Northern Sluiceway is NOT a control-room / gate-state problem (Session 96)

Tester, confirmed in play: **the Northern Sluiceway routes through to the North Spur Sluiceway.**

That kills the obvious hypothesis for map 315 — that the sluice puzzle's flood state makes the map
genuinely unroutable at some gate positions (plausible, because `FUN_00232020`'s material bank can
flip a whole material's walkability at runtime without touching geometry). The map routes with the
gates as they stand. Map 315's zero-routes-out-of-five was logged at 10:20–10:40, i.e. the bit-23
build: **those failures were ours.** Do not re-open the gate theory without new evidence.

### Do not read `tight=0@0` on a breaching route as "the corner was clear"

`PathValidate::CheckLegs` returns on a breach **before** reaching its own interior-corner check, so on
any route that breached, `tightCorners` means NOT TESTED. This was misread once and nearly bought a
global geometry change (insetting every portal span by a body radius) on the strength of it.
`PathValidate::Diagnose` now asks the question properly on a breach — footprint clearance at the
breaching corner plus volume probes at the stop and 0.3 m beyond.

### Session 106 — SOLVED: a start-edge breach broke the whole failure ladder (12 false No-paths)

`path_search.cpp` re-cost attribution picked the portal nearest the breaching leg's MIDPOINT. A
first leg crossing a room-sized start triangle (568's z=121 lane: len 8.13m, reached 7.51m, stop at
the stair pinch 7.5m away) put that midpoint nearest the SEED's own protected edge, so the attempt
loop broke instantly: no retry, no banked prefix (firstBad=1), BuildFrontier aimed past the pinch
and failed -> pass=no-frontier -> spoken "No path" while other starts a few metres away got full
routes. Fixed (`01b7746`): when midpoint attribution lands on the seed's edge, re-attribute by the
sweep's own STOP point; concede to the frontier only when that portal is ALSO the seed's edge.
Grep for `re-attributed by the sweep stop` to see it fire.

Same session, related ship: `path_danger.{h,cpp}` (`1a6b9dc`) — soft penalty discs around scripted
danger actors, per-map data, ARMED PER TARGET only (568 door_gunbit). See sessions_101_150.md
S106 for the full write-up and the deferred 0x0f-glyph / controls-overlay groundwork.

**KEYWORDS: midpoint attribution start poly edge strand seed no-frontier false No path z=121 lane
568 danger zones path_danger npcdic 694 Imperial scene-gap capture distance**

## SOLVED — the re-cost loop could not outbid the corridor it had already disproved (Session 115, 2026-08-01)

**Symptom:** `\` on the east side of map 568 answers "No path" (suppressed frontier) while a route
demonstrably exists from the same coordinate seconds earlier. The log shows `attempts=4 banned=2`
and four attempts that are *identical*.

**What the log actually said** (seq 32, start poly 207, target Door 2) — and it is NOT what Session
114 recorded:

```
attempt 1  BREACH bad=3 len=22.60m reached=0.07m stop=(16.2,-7.72,120.4) why=sweep
           corners: (14.4,-8.00,119.7) (15.4,-8.00,120.5) (16.1,-7.75,120.4) (38.6,-0.00,117.9)
           repair[unpull] 4->8 pts / [unpull-departure] 4->4 / [full-corridor] 4->10  -> all still breaching
           replan: portal (poly 449, edge 1) re-costed to 500
attempt 2  IDENTICAL corridor, IDENTICAL breach   -> 1000
attempt 3  different corridor (3 corners), breach at nearly the same place -> portal 453:1 -> 500
attempt 4  BACK to attempt 1's corridor, IDENTICAL breach -> 1500
           cost: corridor pays terrain=0 other=1000
           frontier: 22.6m short. attempts=4 banned=2
```

**TWO CORRECTIONS TO THE S114 WRITE-UP, both material:**

1. **The "the retries are priced out by `terrain=4000`" story does not hold here.** This failure logs
   `corridor paid terrain=0 other=1000` — **there is no terrain price in it at all.** The retries are
   identical because **+500 on one portal is not enough to change A\*'s answer**, full stop. The
   correct general statement is the weaker one: a flat additive re-cost cannot move the search off a
   corridor whose alternative is dearer than four steps of it.
2. **The whole corridor is unwalkable, not just the taut chord.** `repair[full-corridor]` re-inserts
   *every portal midpoint* and still breaches, and the `retreat` rung never even runs because it
   requires `badReached > NavFootprint::BodyRadius()` and the body got **0.07 m**. So un-pulling was
   never going to help, and "find a DIFFERENT corridor" — which is exactly what the re-cost loop is
   for — is the right lever.

**Root cause:** `kBreachPenalty` was added flat (500 → 1000 → 1500 → 2000 across `kMaxAttempts=4`),
so a portal could be re-costed four times and still be the cheapest way A\* knew.

**Fix (all failure-path-only, `path_search.cpp` only):**

- `BannedEdge` carries the `badStopAt` that earned its last price. When the same `(poly, edge)`
  breaches again and the body stopped within `kIdenticalStopTol` (0.5 m) of last time, the price
  **multiplies** by `kBreachEscalation` (3) — 500 → 1500 → 4500 — instead of crawling. A retry whose
  breach MOVED keeps the additive step, because that case was never the problem. **The distinction is
  a MEASUREMENT, not a leg index** (the S111 correction, applied again).
- A **terrain guard**, because escalation is precisely what could buy back S108's regression: the
  moment a retry's corridor starts paying terrain where its predecessor paid none, the loop stops.
  S106 priced the only corridor and the search bought its way onto class-refused ground; a faster
  escalation makes that failure cheaper to reach, so it is now a stop condition rather than a hope.
- A **log-only reachability oracle** (`NavMesh::FloodFrom`, reused from `nav_probe`) and a **corridor
  dump** on total failure — see the next entry.

**Containment (the tester's hard requirement was "do not break pathing on other maps"):** every line
sits after `rep.ok` failed AND all three `PathRepair` rungs failed, on the path to `break`; the
escalation differs from the old behaviour only on the SECOND breach of the SAME portal at the SAME
stop, which needs attempt ≥ 2 of a request that already failed; the terrain guard can only stop the
loop EARLIER and never refuses a route that validated. `git diff --stat` shows ONE file, and
`path_funnel` / `path_validate` / `path_corridor` / `path_repair` / `path_march` / `nav_mesh` /
`nav_footprint` / `map_query` / `path_surface_goal` / `path_planner` were verified unchanged **by
diff, not by assertion**. `kMaxAttempts` / `kMaxTotalExpand` / `kProbeBudget` untouched.

## Tried & Failed — arguing about "No path" from a log that cannot tell the two cases apart (through Session 114)

**"No path" has meant two OPPOSITE things for this project's whole history, and the log printed them
identically:**

- the goal is **genuinely unreachable** — no chain of adjacent walkable polys exists, and refusing to
  route is the honest answer;
- **the search gave up** — a chain exists and something in the costing, the string-pull or the
  validation could not turn it into a walkable polyline.

Every session that theorised about a "barrier" (S111's `refused eff-flags: 0x07A01000 x305`, which
turned out to measure search BREADTH), about over-refusal, or about pricing, was arguing without this
distinction. **Session 115 ships the oracle that settles it in one line**, on every map, on the
failure path only: `NavMesh::FloodFrom` from the start poly — pure adjacency plus the party's own
walkability test, no rays, no volume tests, no costs — and whether the goal poly is in the result.

Shipped alongside it: **the CORRIDOR, dumped on failure.** `corners(xyz)` prints the string-pulled
polyline (what the body was asked to walk) but never what A\* actually found. When
`repair[full-corridor]` fails, that distinction is the entire question — if the corridor's own
openings do not walk either, the chord was never the problem.

**Rule to carry forward: before proposing a cause for a "No path", read the `oracle:` line. If it
says the goal IS in the component, no theory about the mesh being severed can be right.**

## Tried & Failed — blaming WATER for the "cannot replan from a tight spot" defect (Session 116)

**The correlation was perfect and it was still the wrong cause.** In the Garamsythe Waterway log,
every request whose corridor paid `terrain` failed (7 of 7) and every route actually spoken had a dry
corridor (15 of 15). The proposed fix was a dry-first A\* pass with the priced pass as fallback.

**The tester refuted it in one sentence: map 568 has the identical symptom and there is no water in
the palace.** 568's failing corridor pays `terrain=0`.

**What the correlation was hiding:** `repair[full-corridor]` — the rung that rebuilds the polyline
from every portal midpoint — fails on BOTH maps (`4->10 points` on 568, `22->166 points` in the
Waterway). That rung pulls nothing taut, so if it fails the CORRIDOR is unwalkable and whatever sits
in the way is incidental. Water was simply the most common thing to find there.

**Rule: when two maps show the same symptom and only one has your suspect, the suspect is a
passenger.** Same shape as S93 ("a report about BEHAVIOUR is not a report about DATA") and S111
("a refusal COUNT scales with search breadth").

## SOLVED — A* certifies crossings, never the travel between them (Session 116, 2026-08-01)

**Symptom (tester's words):** "the path is technically valid, but if the player wanders off of it too
far for any reason or gets into a tight corner or against an obstacle, the game has trouble
recalculating back to valid terrain." Seen on map 568 (Royal Palace Cellars, approaching Door 2 from
off the corridor) and in the Garamsythe Waterway (North Spur Sluiceway).

**Root cause, and it was written in this project's own header the whole time.** `nav_mesh.h` on
`EdgePassable`:

> 2. the body can be swept across the shared edge **at SOME parameter along it**.

So A\* accepts an edge when the body fits *somewhere* along it. `EdgeClearSpan` hands the funnel that
clear sub-span and `SpanMid` takes its midpoint — but the body has to get from the clear part of one
edge to the clear part of the next, and **nothing in the search ever asks whether it can.** A pillar
between two portals, a ramp lip, a flooded channel or a party-only volume all sit in exactly that gap.

**Every downstream stage then fails for the right reason and the wrong target:**

- validation breaches (correct);
- every repair rung breaches (correct — the ladder reshapes a path *within* a corridor and the
  corridor is the problem), at ~380 probes a rung, ~1,000 of the 1,600 `kProbeBudget` per attempt;
- the re-cost prices "the portal nearest the failing chord's midpoint", **a guess**, so A\* returns
  another corridor carrying the same untested hop and S115's escalation re-prices the guess harder;
- the budget dies, `PathSurfaceGoal::Route` gets its 128-probe floor for a 145-corner polyline and is
  `REJECTED (budget ran out -- NOT verified)`, and the frontier is suppressed as `No path`.

**Why it correlates with the player being in an awkward spot:** from a ledge or a corner the cheapest
corridors thread pinches that a clean start routes around. The same target from open floor validates
first try.

**Fix:** `PathCorridor::MarchCorridor` — walk the corridor's OWN openings, opening to opening, with
the adjacency march, which **spends no probes** (`path_march.h`: "probes price SWEEPS, and this makes
none"). It reuses `PathFunnel::FullCorridor` so it and the last repair rung can never describe
different polylines. Run after a breach and before the ladder:

- **CLEAR** → a shape problem inside walkable ground: the ladder's own case, run it unchanged;
- **BREACH** → the corridor is not walkable: skip the ladder entirely and re-cost the crossing the
  march NAMED (`[MEASURED by the corridor march]` in the log) rather than the chord-midpoint guess.

Fails open: an undecidable hop is counted (`noVerdict=`) and skipped, never promoted to a breach.
Both protections the inferred path always had — never price the final approach, never price the
seed's own edge — are applied to the measured crossing too.

**Falsifier, shipped with it:** the `corridor march: CLEAR|BREACH` line. If these failures come back
CLEAR the diagnosis is wrong, the ladder runs exactly as today and the cost was one free march.

**Containment:** runs only after `rep.ok` failed; CLEAR is byte-identical behaviour plus one march;
BREACH skips a ladder whose every case today ends as `No path` anyway. Net CPU on the failure path
falls. Three files touched; `path_funnel`, `path_validate`, `path_repair`, `path_march`, `nav_mesh`,
`nav_footprint`, `map_query`, `path_surface_goal`, `path_planner` verified unchanged by diff.

## PARTLY RESOLVED S124 — the repair ladder reports a TRUNCATION as a breach (Session 116)

`PathRepair::tryPoly` folded `r2.truncated` into `good = false` and logged "still breaching". Late in
a budget-starved request that produced lines like `repair[unpull-departure]: leg 26, 1 -- 27->27
points, probes=7 -> still breaching` — **7 probes for a 27-leg candidate is a truncation, not a
breach.** The rung consumed budget without answering and the log said the opposite of what happened.
It also never logged *where* a rung's candidate broke, so it was a black box that said "still
breaching" four times per attempt.

**The LOGGING half shipped in S124** (the S116 "re-measure before sizing" condition was met by the
2026-08-02 log — the ladder pressure did NOT disappear, because the 315 failures marched CLEAR and
took the ladder branch): rungs now print three outcomes (`OK` / `still breaching @ leg N/M stop=…
why=…` / `ran out of probes (NOT verified)`), with `good` and the budget flow untouched.

**The SIZING half is CANCELLED — the gate returned ZERO (S124 play, 2026-08-02).** The pre-designed
`kSurfaceGoalMinProbes = 512` was gated on the confirming log still showing `surface-goal: …
REJECTED (budget ran out`. That log shows **zero** such lines and **zero `surface-goal:` lines at
all** — the surface-goal path was never invoked, because with the funnel fixed the ordinary mesh
route validated on attempt 1 every time and no request ever reached attempt 2.

> **THE 128-PROBE STARVATION WAS A SYMPTOM, NOT A DEFECT.** It only ever happened because the ladder
> spent ~1150 probes failing to repair an ILLEGAL polyline; remove the illegal polyline and the
> budget is never under pressure. **Do not ship a bigger floor without a NEW log containing the
> `REJECTED (budget ran out` line** — sizing it now would be tuning a number that nothing reaches.
> This is exactly what S116's "re-measure before you size it" holdback was protecting against, and
> it is the second time that discipline has cancelled a change rather than merely delaying it.

## OPEN — a bare unlabelled door is classified as a SHOP (map 569, Session 116)

**Symptom (tester):** map 569 (Royal Palace: Lower Halls) lists a plain door under **Shop**. "There
is definitely no sign that says what this door is for and it shouldn't be falling into the shop
category, it's just a bare unlabelled door." The rescan line reads `Door=0 Shop=1` — so the map
lists **zero** doors and one invented shop.

**Root cause, found in the log and confirmed in the source** — `src\navigation\entity_postscan.cpp`:

```
[NAV-DIAG] twin dropped (sign repeats a doorway) "Door": [0:57] (83.92,0.00,38.48)
                                            <- keeping [0:56] (41.65,0.00,118.00) 90.06m away
```

Two faults compounding:

1. **THE TWIN FILTER HAS NO DISTANCE TEST ON INTERACTABLES.** The NPC branch guards with
   `kStackedDist` on both horizontal distance and Y. The interactable branch matches on `label` +
   `doorway` alone, so it paired two objects **90.06 m apart**. A shop SIGN stands BESIDE its
   doorway — that proximity is the filter's entire premise and it is the one thing unchecked.
2. **`hasNameSign` was derived from a GENERIC FALLBACK LABEL.** Both objects were labelled `"Door"`,
   the mod's own fallback naming, not game-supplied sign text. Two objects sharing a generic label
   is no evidence of a shopfront.

The drop then sets `out[twin].hasNameSign = true`, and the categorisation loop turns that flag into
`Category::Shop` (`e.category = e.hasNameSign ? Shop : Door`).

**It also DELETED a real door** — that is why 569 lists `Door=0`. This filter has form: it deleted a
story-critical NPC once (S77) and 4 NPCs in another session, which is why it logs unconditionally.

**Fix when this resumes:** a bounded proximity rule on the interactable branch (a sign is metres from
its doorway, not 90), plus a requirement that the shared name be DISTINCTIVE rather than a generic
fallback. Both halves are needed: proximity alone still fuses two adjacent generic "Door" objects,
and distinctiveness alone still fuses two identically-named shopfronts on the same map.

## SOLVED-BY-FALSIFIER — "the corridor is not walkable" was the WRONG diagnosis (Session 116)

S116 shipped `PathCorridor::MarchCorridor` on this reasoning: `repair[full-corridor]` fails, that rung
walks the corridor's own openings, therefore the corridor is unwalkable. **The falsifier shipped with
it says otherwise on its first play: `corridor march: CLEAR` ×81, `BREACH` ×0.** The BREACH branch
never executed, so the change is behaviourally inert so far and nothing observed can be credited to it.

**Both readings were measurements; the inference between them was the error.**

**What the falsifier bought — a sharper suspect.** On the same request, the same polyline:

```
corridor march: CLEAR over 139 hop(s) (grazes=73 noVerdict=0)
repair[full-corridor]: leg 20, 138 -- 21->140 points, probes=310 -> still breaching
```

Adjacency-CLEAR, body-BREACH. Exactly two differences between what they walked:

1. **`PathRepair::tryPoly` runs `PathFunnel::InsetCorners(cand)`; `MarchCorridor` does not.** The
   inset steps EVERY interior corner by `BodyRadius + kClearanceMargin` along its angle bisector —
   right for a 20-corner route through open rooms, and on a **140-point** polyline of portal
   midpoints in a narrow channel every point is a "corner" with a near-straight bisector, so stepping
   all of them can push points off the corridor they were sampled from. **PRIME SUSPECT.**
2. `CheckLegs` also runs the body sweep; the march makes none.

**This predicts the pattern already in the log:** the ladder repaired 9 routes this session
(`unpull` ×4, `unpull-departure` ×4, `full-corridor` ×1), all short, and failed on every long dense
one. `probes=310` for 139 legs is ~2.2/leg — a real validation, not a truncation.

**Cheapest next test, log-only and free:** after `InsetCorners` on a repair candidate, count how many
points left the poly they were sampled from (`NavMesh::FindPolyAt` before vs after). Large on the
failing routes and zero on the repairing ones confirms it; the fix is then to bound or skip the inset
on dense polylines.

**Rule: two instruments that walk "the same" polyline and disagree are not measuring the same thing.**
S97 from the other side — there a coarse probe overruled the engine's sweep; here a fine one agreed
with nothing.

## Tried & Failed — latching the map id in OnFieldFrame to fix the census label (Session 115, still broken)

S115 recorded the sneak census line as naming the wrong map (`OnMapTeardown` read
`MapNames::CurrentMapId()` after the engine had advanced it) and "fixed" it by latching the id on the
field tick. **It did not work.** S116's log still reads `map 567 census` when leaving 313 and
`map 569 census` when leaving 568 — because **the field tick has already run for the NEW map by the
time `OnMapTeardown` fires**, so the latch is advanced too.

Fix needs an id that cannot advance first: the teardown hook's own map argument, or a latch that
updates only when the id CHANGES and reports the PREVIOUS value.

**Rule: a fix for an ordering bug must be verified against the ordering, not against the read.**

## Map 569's catch, the twin filter, the inset instrument — Solved / corrected (Session 117, 2026-08-01)

**KEYWORDS: FUN_0025c830 FUN_003dbb60 FUN_003dbcf0 routine index bound event fire capture routine
name 捕獲 action_binding_tables validated script_native_table incoherent seteventwakerect STRUCK
twin filter kTwinNearDist 20m kTwinNameMaxObjects 2 hasNameSign InsetCorners leftHome census latch**

### SOLVED — sneak assist on map 569: hook the ENGINE's event fire, not a script native

Symptom: guard suppression works on 568 and does nothing on 569; the log carries `clamp ACTIVE on
map 569` and **zero** `touch SUPPRESSED` / `touch REPORTED` lines.

Root cause chain, each step measured:

1. `rrp_a03` (map 569) calls **zero** `0x26D` and **zero** `0x525` — the natives that funnel into
   `FUN_002677f0`. Nothing on 569 ever asks the touch test, so a per-object override of it has
   nothing to override. (S115 measured this and the entry claimed 569 covered anyway.)
2. The catch is `FUN_0025c830`, the engine's per-object trigger-volume update. On the frame its
   inside-mask goes from empty to occupied it calls `FUN_003dbb60(object, 4, routineIdx, 0)` and, if
   that returns 1, takes the field into a scripted scene.
3. `routineIdx` is resolvable: `FUN_003dbcf0` rejects the record when
   `**(u32**)(object+0x48) <= routineIdx`, bounding it against the object's script container's routine
   count — the same table `MapScript` reads.

Fix: `sneak_assist.cpp` hooks `FUN_003dbb60` and, **on danger-table maps only**, declines a fire whose
routine name contains `捕獲` (Shift-JIS `95 DF 8A 6C`), returning the engine's own `2` = "no event slot
free". Keyed on the ROUTINE because the volumes are rect actors with no npcdic name — per-object
identity was never available for them. Fails open on every unknown, and logs every fire with its raw
name bytes.

### TRIED & FAILED — resolving a native through `script_native_table.txt`

`seteventwakerect (0x3DF -> FUN_0034d470) x70` was carried in `GameArchitecture.md`, the S115 memory
and the session log as 569's mechanism and "the first place to look". **Wrong on both halves.** That
dump indexes a different table (base `0x1eee448`, stride 8) and joins `.dbg` names through a measured
delta; its own output ends with `VERDICT: NOT COHERENT -- do not use any id above`. The validated
CALLACT table is `action_binding_tables.txt` (selector 0, `0x1EED700`, stride `0x20`), which
reproduces `0x08D`/`0x290`/`0x525`/`0x26D` independently — and maps `0x3DF` to `FUN_0034dca0`.

`0x3DF`, `0x26E`, `0x409` and `0x40A` (all x70/x77 on 569) are **flag setters on the volume**
(`object+0xC` bit 5, `object+0x8` bit 5, `object+0xB` bits 1/0), read by `FUN_0025c830`. **Not one of
them tests anything**, so no amount of hooking them would ever have caught the player.

**Rule: a name from an unvalidated join is a guess wearing a label.** Check the dump's own self-check
before quoting a handler out of it.

### SOLVED — a bare door listed as a Shop (map 569)

`TagDoorwaysAndDropSignTwins`' interactable branch matched on `label` + `doorway` and **nothing else**,
so it paired two generic `"Door"` objects **90.06 m apart**, deleted one, and set `hasNameSign` on the
survivor — which the categoriser turns straight into `Category::Shop`. Map 569 listed a bare unlabelled
door as a shop and listed no doors at all.

Both new tests gate the DROP, not just the promotion:
* `kTwinNearDist` = **20 m** — East End's shop pairs measure 6-15 m apart, so the bound must admit 15
  and reject 90.
* `kTwinNameMaxObjects` = **2** — a shopfront's name belongs to exactly the two objects that make it
  up; `"Door"` belongs to every door on the map. Measured from the map, not from a word list.
* Refusals are now logged (`twin KEPT ...`), one line per label per map.

**CORRECTION to the S116 write-up:** it also recorded "`hasNameSign` was derived from a GENERIC
FALLBACK LABEL — the mod's OWN fallback naming". **That half is wrong.** `ApplyFallbackLabels` runs
*after* this filter, and `gameNamed` is set from whether the game supplied text at all, so at filter
time `"Door"` is the map's own `fieldsignmes` string. There was one fault, not two.

### The inset instrument (log-only) — the S116 test, now shipped

`PathFunnel::InsetCorners` fills an optional `InsetStats{corners, moved, leftHome}` and `repair[...]`
prints it. `leftHome` counts moved corners whose accepted point landed in a **different mesh poly**
from the corner's own — a point pushed out of the corridor A* certified. Free: both `FindPolyAt`
results were already computed to decide the move. **Prediction: 0 on rungs that report `OK`, growing
on dense candidates that report `still breaching`.** If it is small on both, the suspect is wrong and
the next one is `CheckLegs`' own body sweep.

### The census map label, third attempt

S115's field-tick latch did not work (the tick has already run for the new map when teardown fires).
The latch is now **write-once per map and consumed by the print**: the tick fills it only when empty,
teardown prints and clears it. No ordering assumption survives in it.

## The fired event index is OBJECT-LOCAL — S117's resolver read the wrong table (Session 118, 2026-08-01)

**KEYWORDS: capture survived 569 object-local event index object+0x48 name-pool offset FUN_00263e40
FiredRoutineName verdict cache removed NoteFireOnce object key dedup deleted evidence**

### TRIED & FAILED — resolving a fire's routine through the blob routine table by index

S117's `RoutineNameAt(routineIdx)` assumed `FUN_003dbcf0`'s bound (`**(u32**)(object+0x48) <= idx`)
was against the blob's routine count. It is against the object's OWN event table: `[count:u32]`
[8-byte records] at `object+0x48`, entries = NAME-POOL offsets (`FUN_00263e40(blob,x) = blob + x +
*(u32*)(blob+0x4C)`), blob per object via `FUN_00263ff0(obj[0x15])`. Result on the play test: every
fired name resolved to an early global routine (`setup`, the resident director), the capture rect's
routine never matched `捕獲`, the fail-open path passed it through, and the player was caught.

The tell in the log, visible BEFORE any decompile work: objects fired **consecutive small indices**
(1,2,3 / 2,3,4 / 3,4,5) and "names" no trigger volume could start. **When resolved names are
semantically impossible, the index space is wrong — stop and re-derive the indirection.**

### SOLVED — `MapScript::FiredRoutineName(object, eventIdx)`

Walks object event table -> name-pool offset -> object's own container blob. SEH-guarded, fail-open,
container id read from `obj+0x15` (not assumed 0). Replaces `RoutineNameAt` (no other caller).

Two secondary defects fixed with it:
- **The per-index verdict cache was wrong within a single map** (same index, different object =>
  different routine) — removed, not repaired. Fires are event-driven; per-fire resolution is cheap.
- **`NoteFireOnce` keyed on (kind, index) without the object** — object-local indices collide by
  design, so different objects' fires were deduplicated into one line. That is exactly how the
  capture's own fire went unlogged during the failed play. **A dedup key that omits part of the
  identity deletes evidence.** Now (object, kind, index), 96 slots.

## Suppress at the WRITER; tiered log budgets; the event-table door join (Session 119, 2026-08-01)

**KEYWORDS: trigger update skip class 0x18==1 never fires FUN_003dbb60 FUN_003df760 notification
registers writer not reader log budget tiers spam exhausted 96 slots census event-door nameOff join**

### TRIED & FAILED — declining the capture at the event-fire hook (S117/S118 design)

`FUN_0025c830` fires `FUN_003dbb60` ONLY for objects with class `+0x18 != 1`. Script-created rects
are `+0x18 == 1`: the ENTER branch returns before the call, the kind-3/6 branches guard it out, and
the update's outputs for them are the inside-mask, `object+0xC` bits, and the `FUN_003df760`
notification registers the script polls. **A capture rect of that class never passes through the fire
hook, so no decline there can reach it — the suppression point must be the WRITER
(`FUN_0025c830`), which every read path shares.** S119 skips the update outright for guard objects
and for objects whose event table names a `捕獲` routine (both measured identities, fail-open).

### TRIED & FAILED — one shared budget for the fire log

The S118 play spent all 96 slots on load-time `init`/`main` fires in 18 s; the line that mattered
could never print. The play before, an under-keyed dedup deleted it. **A shared budget is a dedup key
with the same failure mode: whatever fills it first decides what evidence survives.** Now tiered by
what a line can prove: capture-named fires always log; trigger-volume fires get the per-object dedup
and the big budget; script/other spam gets 16.

### SOLVED — a door under Interactables (event-bound doors have no +0x70 record)

`doorway` comes from the field-sign table alone; a door whose transition is event-bound (569's
`[0:57]`, routines `door1`/`door3`) has no record and fell to Object. The join that exists in game
data: `ExitDest::nameOff` (the transition routine's name-pool offset) == an entry in the object's own
event table (`object+0x48`). Integer compare in one pool, container-checked; category Door only, no
destination spoken, `doorway` untouched. **First measured object<->routine binding this project has
had — the 569-exits backlog should start from it.**

### Still unobserved

The 569 capture mechanism has never appeared in a log — two plays, two logging failures. The per-
object trigger census (one line per object: class, flags, event names, position, nearest guard) now
guarantees the next play names it even if the S119 skip does not already stop it.

## The catcher was a guard-riding WAKE rect; doors are template siblings (Session 120, 2026-08-01)

**KEYWORDS: catch rect wake flag fC 0x20 同期 兵士全停止 talk exclusion 8m guard radius template
signature door anchored inference census delivered**

### SOLVED (pending play) — 569 capture: the S119 census named the catcher on its first outing

The catcher: rect obj at (58.0,98.0) riding the patrolling Imperial at (57.97,97.98), event names
init|touch|touchon|touchoff|SET_RECT|同期, fC=0x28. Guards' vision volumes are SEPARATE rect objects
riding the guards — which is why IsGuardObject (not the npcdic actor) and the 捕獲 name rule
(template-generic names) both missed. Exactly SEVEN 569 objects carry fC bit 5 (the WAKE flag,
0x3DF's bit, required by FUN_0025c830's ENTER scene handoff): three 同期 on patrollers, four
兵士全停止 between stationary pairs. Rule shipped: wake flag (live) AND within 8m of a snapshotted
guard (live) AND no `talk` event => skip at trigger update + decline at fire hook. The talk
exclusion protects 568's servant (3.4m from a guard, rect carries talk).

**Lesson: S119's `+0x18==1` theory was wrong (all census objects class=0x00) — the catch DID pass
through the fire hook; there was just no rule for it. The census, not the model, named the catcher.**

### TRIED & FAILED — binding a door object to its transition via ExitDest nameOff (S119)

A door object's events are the FIELD-SIGN TEMPLATE (init|talk|フィールドサインＯＫ/ＮＯＴ/ＯＮ/ＯＦＦ)
in its own container — not the map's door1/door3 routines, and not container 0. The join is kept for
maps where it applies, but 569's doors bind through the template. SOLVED instead by the ANCHORED
template-signature rule: same container + same sorted event nameOffs as a +0x70-proven doorway =>
Door. Fail closed without an anchor; logged as an inference.

## SOLVED — a whole city that never announced, and exits refused for how they LOOK (Session 126)

### SOLVED — Bhujerba's section changes were silent because the announce gate kept an S93-retired bit

`entity_list.cpp`'s `kFieldContextBits` was `0x07`, which includes fail-mask bit 2 (`CondAreaId`).
**S93 had already proven that bits 2/3 are not a readiness signal** — `0x0C` is the engine's TERMINAL
"this area has no such resource" state from `FUN_003ea820`, which never retries — and removed them
from `IsFieldNavSafe()`. The announce gate was missed, so on any map in that terminal state the
"Entering &lt;area&gt;" block was skipped silently, before its own log line.

Measured across 21 logs, no exceptions: `failMask=0x00` announced **11/11**;
`failMask=0x0C` announced **0/3** (Bhujerba 805/806). Now `0x03`.

**TRIED & FAILED — blaming the `s_lastArea` dedup.** The report was "entering another section of the
CITY does not announce", which makes same-region suppression the obvious cause. It is wrong:
Rabanastre announces section-to-section perfectly, six consecutive maps in one log. **When a report
names a place, ask the log whether other places of the same kind behave the same way — before
reasoning about mechanism.**

### SOLVED — Lhusu Mines had no exit onward: `mapjump` flags is PRESENTATION, not kind

`map_script.cpp` admitted a `__MJ_CTRL` controller only when `jumpFlags == 0`. Map 357's two doors
deeper into the mine carry `flags=0x2` (bit 1 = the alternate fade) and were refused, leaving
`surface g2` and `surface g3` — 30 polys each — logged as `NO CONTROLLER CLAIMS THIS GROUP`.
`map_script_internal.h` had documented flags as a presentation bitfield since S64.

Now: the world-map teleport MENU value (`0x0A`) is refused for both classes; beyond that a controller
that armed a group is admitted on any value. Cannot widen onto a working map, structurally — a map
that is correct today has zero unclaimed surfaces, and `exit_scan` still requires a swept surface
carrying the group tag.

**THE REAL DEFECT WAS THE MISSING LOG LINE.** The builder's `span=... | setmapjumpgroup ... | mapjump
at` line lives INSIDE the accepted branch, so a refused controller produced **no output whatsoever** —
indistinguishable from a routine that does not exist. The census could see `__MJ_CTRL001/002`, the
builder could not, and nothing said why. Now both silent-drop paths emit `__MJ_CTRL%03d REJECTED`
beside `SPAN EMPTY` / `SPAN UNREADABLE`. **A reader that drops something must say so; "absent from the
log" must never be able to mean "rejected".**

Still unattributed, for the next play: unclaimed surfaces on maps **318, 319, 321, 322, 568**.
**Map 569 is ruled out** — no `__MJ_CTRL` routines at all, all jumps `flags=0x0`; its `nogroup=3`
backlog item has a different cause.

### SOLVED — THREE guards whose COMMENTS asserted something false about pointer identity

All three shipped with a comment claiming the guard self-clears. **None of the claims had ever been
tested, and `menu_reader.cpp:102` has recorded since S51 that the engine RECYCLES these addresses.**

* **`shop_reader.cpp`** — *"the guard resets when the container (surface) changes, so re-entering the
  shop always re-announces."* It compared `g_lastContainer`, which was reset only at DLL unload, so a
  pooled container handed back at the same address kept the key.
* **`dialogue_reader.cpp`** — *"the key is dropped the moment the message ends."* The `+0xC0`
  end-of-message latch is read PRE-call, so it is visible only on the call AFTER the message ended —
  and when a shop tears the box down that call never comes. Recycled slot + resident message data +
  `off == 0` meant the clerk's greeting collided by construction on re-entry and went silent.
* **`choice_reader.cpp`** — *"re-entering the prompt re-announces, because the widget is rebuilt and
  the remembered cursor no longer matches."* Two function-local statics that **nothing ever cleared,
  not even `Shutdown()`**. A prompt re-opened at a recycled address with its cursor back at the
  starting index matches the stale key exactly.

Fixed by giving each a real game event to re-arm on: `FUN_005655f0` (the list refresh) for the shop
and, via `DialogueReader::ForgetLivePages`, for dialogue and choices; plus the end-of-message latch
for choices; plus making the widget pointer part of the dialogue page key. **A comment is not a
measurement. If a guard's safety argument rests on an address changing, prove it changes.**

### SOLVED — the paint replay retried exactly once, and lost announcements permanently (S126)

`MenuReader::OnFocus` assigns `g_focusOwner = owner` BEFORE its `text.empty()` test, so on the paint
replay `ownerChanged` is already false — and the re-stash was gated on `ownerChanged`, while
`OnMenuPainted` clears the pending slot on its way in. **One paint is not always the right paint:**
the paint that fires the callback need not be the one that fills THAT owner's item map. When it was
not, the row was never announced and the player had to move the cursor. Affects every menu.

Observed signature, from the same log as the rest of S126 — two `(text not ready — awaiting paint)`
for one owner/index, the second WITHOUT `(new surface)`, then nothing.

Now retries up to `kMaxPaintRetries` (8). **The budget deliberately does NOT reuse
`g_pendingOwner`/`g_pendingIndex`** — those are cleared before the replay runs, so they cannot also
answer "have I already retried this one"; `g_retryOwner`/`g_retryCount` survive the clear. Released
when text arrives. Exhausting it logs `TEXT NEVER PAINTED ... this surface is MUTE` **once per
surface**, because a surface that never paints is a defect elsewhere and going quiet is precisely how
it stayed invisible.

**A retry budget of one is not a retry.** Any "stash it and replay on the next event" path needs to
say how many times it will try, and to log the give-up.


## OPEN — the Clan Primer wrap-around settle is SILENT instead of stale (Session 127)

**Status: partially fixed, shipped, and NOT finished. Tester-confirmed 2026-08-03.**

Scrolling a Clan Primer list until it WRAPS (top to bottom, or bottom to top) used to announce stale
text: the focus message arrives before the painter has refilled the item map, so the captured cells
still held the rows that were on screen a moment earlier. The row spoken was real text belonging to a
different row.

The one-paint settle in `MenuReader::OnFocus` fixed the wrong text — **and replaced it with silence.**
The wrapped-to row now announces nothing at all. That is strictly better (a blind player is not told
a lie about where the cursor is) but it is NOT the intended behaviour: the row that finally settles
on screen should be spoken.

```cpp
// menu_reader.cpp, OnFocus, gated on PrimerReader::OwnsSurface
if (!fromPaint && PrimerReader::OwnsSurface(owner)) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_pendingOwner = owner;
    g_pendingIndex = index;
    return;                       // <-- deferred; OnMenuPainted is expected to replay it
}
```

### FIX THE DIAGNOSTIC FIRST — this path drops a focus with NO LOG LINE

That early `return` happens **before any logging**, so the log cannot currently tell apart:

1. the focus was deferred and `OnMenuPainted` never replayed it (the stash sits there forever);
2. `OnMenuPainted` replayed it but `OnFocus` returned at the pane gate (`IsFocusedPane` false while
   the list is mid-scroll);
3. the replay ran, found the item map still unfilled, and the retry budget swallowed it;
4. the game never sent a second focus at all.

**Those four need different fixes and the log distinguishes none of them.** Add a
`"focus deferred (settle)"` line at the deferral and a matching one on the replay before touching
anything else. This is the THIRD time this session a silent-drop path cost real time --
`map_script.cpp`'s refused `__MJ_CTRL` controllers and `MenuReader`'s one-shot paint retry were both
invisible for exactly the same reason. **A path that drops something must say so.**

### Leading hypothesis, to be confirmed not assumed

`OnMenuPainted` only replays when the painted owner matches the stashed one
(`if (owner == pend && idx >= 0)`), and it CLEARS the stash on its way in. If the wrap triggers a
paint whose owner key differs -- or triggers no further paint for that owner -- the stash is never
consumed and nothing ever speaks. The retry budget (`g_retryOwner` / `kMaxPaintRetries`) does not
help here: it only re-stashes when the text came back EMPTY, and in the settle case the deferral
returns before any text is read at all.

### Do not "fix" it by reverting the deferral

Removing the settle restores the stale-text read, which is worse. The two candidate directions are
(a) make the deferral survive until a paint for that owner actually arrives (a bounded re-stash, the
way the empty-text path already does), or (b) find the game's own "the list has settled" signal and
replay off that instead of off the painter.

## "The combat log logs nothing" — NOT a regression; the log could not tell (Session 128, 2026-08-03)

**Reported:** combat log completely broken, logging nothing, suspected broken by a recent session.

### Tried & Failed — the theories the evidence killed

* **"S125 broke it with the element suffix."** REFUTED by `git diff ab99988 HEAD -- src/battle/`.
  `combat_log.cpp` is byte-identical to the last-known-working build. The one functional change is
  the added `elementMask` argument to `DamageLine`; `ElementSuffix` returns `""` for the 447
  non-elemental action rows and otherwise appends at most `" Fire"`. It cannot empty a line.
  **S126+S127 (`7f90dff`) touched no file under `src/battle/` at all.**
* **"A hook failed or was displaced by the 14 new S126/S127 hooks."** REFUTED. All three combat RVAs
  (`0x416410` Tier-1, `0x1F12F0` applier, `0x1F2280` reward) install in the current log at the same
  addresses as the working build, and `uniq -d` across all 66 installed RVAs shows no duplicate.
* **"The ring is being cleared."** REFUTED. `CombatEvents::Init` has exactly one call site
  (`dllmain.cpp:116`) and each log holds exactly one `CombatLog initialized` line.
* **"`CombatEvents::HookedApply calls=1340` proves a battle produced no entries."** REFUTED — and
  this is the trap. That is the **status-tick path** (`actionId == 0xFFFF`), which runs per BtlChr
  per frame **on the field**, not only in battle. It is not evidence of combat.
* **The input path was cleared too:** `,` / `.` / Home / End are registered and dispatched
  correctly, and `g_extraDown[25]` still bounds the highest index used (24), so S127's keys 8/9 did
  not overrun it.

### What was actually true

**No battle occurred in ANY of the fourteen 2026-08-03 logs.** Every rescan reports `Enemy=0`,
`[TARGET]` contains only its install line (the working 08-01 log has 743 readouts), and no battle UI
is drawn. The producers were never given anything to log.

**An absent log entry means nobody wrote one, not that the feature failed** — and here it also could
not mean the opposite, which was the real problem.

### Solved — the defect that was fixable

A battle that logged nothing and a battle that never happened wrote the SAME file, because every
early exit in the combat path returned silently: the applier's unarmed `+0x1c` gate, an empty
formatted line, the Tier-1 sprintf's missing buffer and its decoded-to-nothing case, and
`CombatLog::Append`'s own empty-text guard. All five now report, and a **positive control**
(`realhit xN`) was added so "no drops, no entries" cannot mean both "the gate rejected everything"
and "nothing reached the gate".

Counted on the hot path, **reported only at powers of two** — `HookedApply` runs ~20×/sec per actor,
so O(N) logging would flood the file the console-output budget protects. `1` is a power of two, so
the first of each kind always prints.

**Reading the next battle log:** `realhit xN` with no `DMG |` lines ⇒ the formatter or the name
chain (check `atkNamed`/`tgtNamed` on the `drop[empty-line]` line). No `realhit` at all ⇒ the
applier gate, see `drop[apply-not-valid]`. No lines whatsoever ⇒ the applier is not being reached.

**Rule reinforced (fourth silent-drop path in two sessions):** an early `return` with no log is a
defect in itself — it destroys the evidence that names the next bug. And **do not revert working
code to chase a report the log cannot see**: a revert justified by ABSENCE needs a log in which the
change COULD have fired.

### Session 130 — SOLVED: the combat log was silent because MinHook ran out of trampoline slots

**Symptom:** a tester reported the combat log "not logging anything". It works on the developer's
machine. Session 128 proved the producers byte-identical to the last-known-working build and could
find no cause — correctly, because there is no defect in our combat code.

**Cause:** `include/MinHook/buffer.c` had `MEMORY_BLOCK_SIZE 0x1000`; `buffer.h` has
`MEMORY_SLOT_SIZE 64`; the block header consumes the first slot. That is **exactly 63 trampolines
per block**. The mod installs 66 hooks. On the tester's machine MinHook could not place a second
block within ±1 GB of the target and the last three installs failed:

```
[HOOKS] MH_CreateHook failed at RVA 0x416410 (abs 0x536410): MEMORY_ALLOC
[HOOKS] MH_CreateHook failed at RVA 0x1F12F0 (abs 0x3112f0): MEMORY_ALLOC
[HOOKS] MH_CreateHook failed at RVA 0x1F2280 (abs 0x312280): MEMORY_ALLOC
```

Those three ARE the combat hooks, because `CombatEvents::Init()` is the last subsystem initialised
in `dllmain.cpp`. Everything else installed and worked, so the session looked healthy.

Why it is machine-dependent: the exe's image base is `0x120000`, very low, so `FindPrevFreeRegion`
has almost nothing below it and the ±1 GB window is largely the exe's own address space. Whether a
second block fits depends on that process's virtual-address map — other injected DLLs, overlays,
ASLR. Nothing else in the combat path differs between machines.

**Fix:** `MEMORY_BLOCK_SIZE` -> `0x10000`, marked `[LOCAL]` in the vendored file. Free: on x64 the
blocks are placed by `FindPrevFreeRegion`/`FindNextFreeRegion` stepping by
`si.dwAllocationGranularity` (64 KB), so Windows already reserved 64 KB per block and MinHook was
asking for one page of it. 1023 slots per block instead of 63, zero extra address space.
`FreeBuffer`'s `(p / MEMORY_BLOCK_SIZE) * MEMORY_BLOCK_SIZE` still resolves, because a 64 KB-aligned
block is aligned to the larger size too.

**Two instruments added, because the failure was invisible:**
* `Hooks::LogInstallCensus()` — one line at the end of deferred init: attempted / installed / failed,
  the first failing RVA, and a note that `MEMORY_ALLOC` means the trampoline pool, not RAM. Before
  this there was no total anywhere and no caller but `CombatEvents::Init` checked a return value.
* A **build stamp** in the log banner. No log this mod ever wrote carried a version; identifying the
  tester's build meant counting hook-install lines.

**KEYWORDS: MinHook MEMORY_ALLOC MH_CreateHook failed trampoline pool 63 slots MEMORY_BLOCK_SIZE
MEMORY_SLOT_SIZE hook census build stamp combat log silent tester machine works here not there**

---

### Session 130 — STRIKES `Enemy=N` as a battle predicate

`Enemy=` in the log comes from `src/navigation/entity_scan.cpp` — the **field-object rescan** count.
It is not, and never was, evidence about whether a battle occurred.

Session 128 concluded "no battle ever happened" in fourteen logs on the strength of `Enemy=0` on
every rescan. The tester's 2026-07-28 log has `Enemy=0` on all **361** rescans **and** a complete
Dire Rat fight with kills, EXP/LP and speech; his 08-03 07:31 log has 248 `[COMBAT]` lines. The
inference was wrong, and it made a real report look unfalsifiable.

To ask whether a battle is running, use `BattleState::PartyEngagement()`.

**KEYWORDS: Enemy=0 battle predicate entity_scan field object rescan false inference S128**

---

### Session 130 — a defect reported on another machine cannot be diagnosed from your own logs

Session 128 read the developer's logs throughout and reasoned "over all 66 installed RVAs" — a
number only the developer's machine produces. Every check it made was correct and every one proved
our code innocent, which it is. The answer was in a file nobody had opened.

Before theorising about a tester-only defect: **open the tester's log and grep it for `fail`,
`FAIL`, `error` and `MH_CreateHook` first.** In this case the cause was printed at startup, 2.6
seconds in, in plain text, and had been sitting there for two sessions.

**KEYWORDS: tester log works on my machine dev log 66 hooks unfalsifiable read the log first**

---

### Session 130 — SOLVED: accented characters were DELETED by the codec, not flattened

**Symptom, as reported:** "a bare n instead of an n-tilde".

**What the code actually did:** `game_text.cpp` ended its glyph handling with
`default: break;  // unmapped extended glyph: drop`. An unmapped byte emitted **nothing**, so
"Senor" (with the tilde) decoded as "Seor" — the same way "Cuchulainn" read as "Cchulainn" before
`0x81` = u-acute was added by hand. **The mod could not turn an accented letter into a plain one.**
Worth recording because the report and the mechanism disagree, and chasing the report's wording
would have led to a conversion bug that does not exist.

**Refuted at 1.00 while looking:** no `CP_ACP` anywhere in `src/`; no `MultiByteToWideChar` anywhere;
no default-char argument at either `WideCharToMultiByte` call (both `CP_UTF8`, both log-only, both
narrowing a *separate copy* after speaking); no transliterate/strip-accent helper; `/utf-8` is set
(`CMakeLists.txt`). The speech path is `std::wstring` from `GameText::Decode` all the way to
`Tolk_Output(const wchar_t*)`.

**Cause and fix:** the mod only ever mapped 62 characters structurally plus 17 hand-won punctuation
marks plus one accented letter. `font00.dat` — the game's own glyph table, beside the font texture —
carries the character for every slot, and `codec byte = slot + 0x20`. Generated into
`src/core/game_glyphs.h`. See `GameArchitecture.md` for the record layout.

**Two traps in that file:** the character field is **UTF-8 bytes packed little-endian into a u32**,
not a codepoint (it reads as a codepoint for ASCII and then goes strange); and the map is
**per-locale** — there are five font directories, not twelve, and all 224 single-byte slots differ
between them.

**`IsMostlyPrintable` had to widen in the same change.** It required 60% ASCII and one `A-Z`/`a-z`
letter, which held only *because* accents were dropped — whatever survived was ASCII by
construction. With accents decoding, a short accented string could fail the gate at ~30 call sites
and go **silent**. A fix that turns partial speech into none is worse than the bug.

**KEYWORDS: diacritics accents n-tilde e-acute dropped glyph unmapped extended glyph font00.dat
glyph table game_glyphs.h codec byte slot 0x20 UTF-8 packed per-locale IsMostlyPrintable silence**

---

### Session 130 — SOLVED: `t` spoke a conversation that had ended hours earlier

**Not a scope bug.** `t` was already dialogue-scoped: only three call sites ever wrote `g_lastLine`
(the obtained-item toast, the menu system-message panel, and each dialogue page, all through
`MessageReader::NoteSpoken`). Menu rows, entity descriptions, the combat log, gil, LP and party
status never touched it.

**A lifetime bug.** Nothing cleared the string — not end-of-message, not a map change, not
`Shutdown()` (which clears `g_confirmPrompt` and not this). So `t` repeated a finished conversation
forever, anywhere.

**Fix — two independent guards.** A gate (`DialogueReader::IsBoxLive()`, the message-window registry
`DAT_0215f200` asked "does any slot hold a window", OR-ed with `MenuState::IsChoicePopup` /
`IsConfirmWindow`, plus a birth/destruct latch for the toast, which has no state left to query), and
a clear (`MessageReader::ForgetLastLine()`) hung on the SAME events `DialogueReader` already drops
its page key on.

**`MenuState::IsAnyMenuOpen()` was NOT used and must not be** — `*DAT_0208ebc0` is written once and
never cleared, so it answers "open" forever after the first menu.

**Open, unmeasured:** whether the game nulls its `DAT_0215f200` slots when a conversation ends. If
it does not, the gate answers "live" forever and only the clear is doing the work. The `t` log line
distinguishes the two — press it in the field after a conversation and read which guard it names.

**KEYWORDS: t key re-read last spoken line lifetime g_lastLine ForgetLastLine IsBoxLive
message window registry DAT_0215f200 IsAnyMenuOpen unusable stale guard outlived its object**

### Session 130 (follow-up) — the Polish patch: the font metadata LIES, so the mapping came from the text

The Polish fan translation `PL_ff12_v1.3` repaints ~16 accented glyph slots to Polish letters. Its
`font00.dat` differs from stock in **20 bytes**, and **not one of them is a character field** — all
ten changed records changed only their *advance width*. So the file that is authoritative for a
stock install is actively wrong for this one, and every repurposed slot still claims the stock
letter it used to draw.

~~**Autodetection is not available.**~~ `instaluj.bat` repacks the archive in place
(`ff12-vbf.exe -r ff12data ..\FFXII_TZA.vbf`) and patches `FileSizeTable_US.fst`. No loose file, no
marker, no version string. ~~Hence a mod-menu row (**Text glyphs — Standard / Polish translation**,
default Standard) rather than a probe.~~

**STRUCK (S147, and the strike re-affirmed S177).** The premise is right about the DISK and the
conclusion does not follow: what needs identifying is not a file but **which atlas the game loaded**,
which is in memory as soon as the font manager exists. The marker is in this very paragraph — "ten
changed records changed only their *advance width*" IS a deterministic fingerprint.
`GameText::DetectVariantOnce` reads it back, the `Text glyphs` row is gone, and there is nothing for
the player to pick. ⚠ **The detection as first written did not work** — two wrong constants meant it
never fired on any build from V0.6.3 to V0.7; fixed S177, see the BACKLOG item below.

**How the mapping was recovered:** decode the patch's own shipped `ps2data` text with the STOCK
table and read the Polish. A repurposed slot shows up as a stock letter standing in a position
Polish orthography forbids, and the correct letter is the one that makes the word:

```
"Jù¿RùùùJùù nie moêe dosiègnàç celu."   ->  nie może dosięgnąć celu
"zamienia siè w kamieñ."                ->  zamienia się w kamień
"PÊ czèéciowo odnowione."               ->  PŻ (Punkty Życia) częściowo odnowione
"minè¿y róêne przypad¿oéci"             ->  minęły różne przypadłości
"BROŃ JEDNORĘCZNA" / "Bezimienne Źródło" / "Pani Życia i Śmierci"
```

**L-STROKE IS THE TRAP.** Lowercase `ł` sits at `0x94`, a PUNCTUATION slot, not in the accented
block — so the block rule cannot place its capital, and the obvious guess (`0x93`, the neighbouring
inverted-exclamation slot) is **wrong**. `Ł` is at **`0x81`** — the very byte the stock table maps to
`ú`, which this project had hand-derived years ago for "Cúchulainn". Found by asking which unmapped
byte behaves like a word-initial capital: 173 hits, witnesses `Łatwo` / `Łowca` / `Łupieżca`, plus 42
item and enemy names (`Arkadyjski Łucznik`, `Cesarska Łuska`, `Deszcz Łez`).

**The in-block rule, stated over its five confirmed pairs and no counterexample:** capital = lowercase
− `0x18` (ę/Ę, ś/Ś, ż/Ż, ź/Ź, ń/Ń). `ą`→`Ą` (`0x54`) and `ć`→`Ć` (`0x59`) follow from it and are
marked **rule-derived, not witnessed** — capital A-ogonek is essentially unattested in Polish and
capital C-acute is word-initial only in rare proper nouns, so neither appears anywhere in the
corpus. That is expected, not alarming, and it is recorded so a later session does not mistake it
for a measurement.

**End-to-end validation:** decoding the patch's name pool with the shipped table yields **1,351
clean Polish item and enemy names** — `Adamantowy Żółw`, `Agatowy Pierścień`, `Anielska Pieśń`,
`Arkadyjska Armia Żołnierz` — and **zero** names still containing a stock accented letter, which is
the check that proves no repurposed slot was missed.

`ó` needs no entry: the stock atlas already carries it at `0x7C` / `0x64` and the patch left both
alone (`Podróżnik`, `Żółć`, `Ósma` all decode correctly unmodified).

**KEYWORDS: Polish patch spolszczenie PL_ff12_v1.3 fan translation glyph override font00.dat
advance width repaint l-stroke 0x81 u-acute collision block rule 0x18 text glyphs mod menu
game_glyphs_pl.h autodetection impossible VBF repack in place**

## OPEN — a route to a MOVING target aims at where it stood when the key was pressed (S139)

**Reported from play 2026-08-05:** routing to a patrolling NPC walks the player to the position the
target held at REQUEST time, not to where it is on arrival. The tester's workaround is to press the
route key again once there; they explicitly deferred the fix.

**The diagnosis is STRUCTURAL, not a defect in one call path.** `PathPlanner::Request` takes a fixed
`FVec3`, and `RequestReplan` re-runs against the `g_objTarget` SNAPSHOT — there is no
`g_objSceneObj`, so no live position is ever re-read by anything. Every route in the mod has always
been to a point, and for exits and seams (which do not move) that is exactly right.

**The fix, when it is taken:**
1. `Request` gains an optional `void* sceneObj`, stored as `g_objSceneObj` beside the objective and
   cleared on map teardown — a scene-object pointer is only valid inside the map it was read on.
2. `OnGameFrame`, for an objective that carries one, re-reads `PlayerState::ReadSceneObjectPos`.
3. Past a hysteresis threshold (start ~1.5–2 m, i.e. 2–3 steps) it updates `g_objTarget` and takes
   the existing SILENT replan path, which already re-arms the beacon without speaking.

`EntityList::GetCurrentTarget` already returns `outSceneObj`, so half the plumbing exists;
`NavCommands::RouteToCurrent` is the call site. Fixed targets pass `nullptr` and keep today's
behaviour byte for byte.

**Why it is not a tail-end change.** `path_planner` is the most regression-prone subsystem in this
project (the whole S84–S124 arc) and it interacts with the funnel, the corridor march, the surface
goal, auto-walk and the beacon. A replan that fires too eagerly would stall the beacon or fight
auto-walk. It needs its own session with play evidence.

## BACKLOG — carried to a future session (recorded 2026-08-05, S140)

Four items, all deferred at the tester's direction. Each is written up so the next session starts
from evidence rather than from a re-investigation.

### 1. ~~The Polish diacritics fix is NOT working~~ — SOLVED (Session 177), TWO WRONG CONSTANTS

~~**The tester reports the S130 glyph work did not solve accented-character reading on the
PL_ff12_v1.3 fan patch.** Check whether the **Text glyphs** mod-menu setting is actually on
`Polish` in their `mod_settings.txt`; whether the mapping is applied on the read path their text
actually takes; and whether more slots moved than the 144 bytes S130 recovered.~~

**STRUCK 2026-08-30.** The investigation plan above is obsolete in every particular: S147 deleted
the `Text glyphs` setting, the mapping was correct, and no additional slots moved. **The glyph table
was never the problem — variant DETECTION was, and it had never once fired on any build.** Both
faults are in `game_text.cpp`'s `DetectVariantOnce`, both date to S147, and both shipped in every
release from **V0.6.3 through V0.7**:

1. **`RVA_FONT_MGR` was `0x1EE11F8`; the font manager is `DAT_01f811f8`, i.e. RVA `0x1E611F8`.** A
   transposed 6/E. The old value resolves to ABS `0x020011F8`, an unrelated global. Because that read
   is only a non-null "is the manager up yet" gate and is never dereferenced, it did not crash — it
   just gated detection on a garbage word.
2. **Every entry of `kFpSlot` was one too high** — `60,61,62,84,85,86,98,117,118,179` where the
   records are `59,60,61,83,84,85,97,116,117,178`. Counted from one against a zero-based file.

Either fault alone is sufficient: the scan reads the wrong ten records, no vector matches, and the
code logs `font atlas UNRECOGNISED` and settles on `Standard` — which is also the correct answer for
every non-Polish install, so nothing anywhere looked wrong (**L-83**).

**How it was settled, entirely offline and with no tester artifact:** both `font00.dat` files are in
the repo tree (`PL_ff12_v1.3\…\font\us\` and `FFXII-Decompile\extracted\…\font\us\`). Byte-diffing
them gives 20 differing bytes in 10 records, and each record states **its own ordinal at `+0x00`**.
`FUN_002ac2f0` shows the argument to `FUN_0017f8c0` is `codec byte − 0x20`, which fixes the index
space. Cross-check: eight of the ten corrected ordinals map to bytes `kGlyphPolish` already
overrides; under the old ordinals one mapped to `0x5E`, which the patch never touched (**L-82**).

**Also fixed:** a no-match no longer latches. "Manager not up yet" and "loaded but unrecognised" were
indistinguishable, so one early call could pin a Polish install to English glyphs for the whole
session — with no manual override left to escape through. It now retries up to 64 times before
settling, and logs the give-up once at the cap.

**Confidence 0.99** on both constants (the RVA is `FUN_001b5fa0`'s literal return, and the base
convention `abs = RVA + 0x120000` holds across ~25 constants in this codebase; the ordinals are a
direct file measurement corroborated by an independently derived table). **Play-confirmation on a
Polish install is still OPEN** — nobody on this machine runs the fan patch. The one line that
settles it is `[TEXT] font atlas DETECTED: Polish fan patch` in the tester's log.

**AND S130's TOGGLE IS BACK** (same session, user's instruction), **restored from `367b10f^`, not
rebuilt**: `SettingId::TextGlyphs`, two values, `text_glyphs` key, default 0 — byte-identical to the
original, with only the spoken label changed to **`Diacritics override`**. (My first attempt
reimplemented it as a new three-value row with a new key; the user caught it. The invented key would
have orphaned every tester's existing `text_glyphs=1` — see **L-84**.)

**How it arbitrates with detection**, which S130's toggle never had to contend with — the user's
rule, implemented inside `SetVariant` so the call site stays original:
- **`Polish translation`** forces the variant and stands the detector **down**.
- **`Standard`** — the default, and what every untouched install carries — means **"no override"**,
  not "force stock": it rebuilds to stock and **re-arms** the detector.

The asymmetry is what lets a two-valued row do the job of three: value 0 is not a decision, it is
what a player who never opened the menu has, so reading it as one would force stock on everyone and
detection would never fire. Deferring is free because detection's own fallback IS Standard.

**KEYWORDS: Polish diacritics detection font atlas RVA_FONT_MGR kFpSlot off-by-one DetectVariantOnce
DAT_01f811f8 FUN_0017f8c0 advance width fingerprint Diacritics override SetVariantOverride S177**

### 2. MP cost of magicks — BATTLE MENU ONLY

**Scope is the requirement, not a detail.** MP cost belongs to the game's **Battle Menu** (`F`),
where the player is committing an action and the cost decides whether it can be cast. It must NOT
appear on the **Party Menu**'s Magicks screen, which is a browsing surface where the number is noise.

That scoping is structural rather than a flag: the battle menu is already a separate system in this
codebase, releasing on its row DRAW (`FUN_00276be0`) with no pane-replay path
(`ingame_menu_reader.cpp:49`, `:160`).

**VOCABULARY (revised Session 158).** The game's own Controls screen says **"Battle Menu"** and
**"Party Menu"** (`Docs/Controls.md:100-110`, captured verbatim). It does not say "command menu".
This project now uses the game's words throughout: the `R` menu is the **party menu**, and its first
command — the membership screen — is the **Party screen**, never "party menu". The Session 93
convention that called the `R` menu the *field menu* is retired; the collision it was written to
prevent never actually caused one.

### 3. ~~Item quantity is BROKEN IN BOTH menus~~ — SOLVED, and "BOTH" was WRONG (Session 146)

~~**Tester report: the count is not announced in EITHER the party menu or the Battle menu.**~~
**STRUCK 2026-08-05.** The field side was **never broken**. The tester re-tested on the current
build and the log says it plainly:

    [INV]    item: "Potion 32"   "Antidote 5"          <- PARTY menu, count SPOKEN
    [INGAME] command: 0x52 "Potion"   0x5B "Antidote"  <- BATTLE menu, count MISSING

The 2026-08-03 tester log had already said the same thing — `"Wind Stone 6"`, `"Bone Fragment 7"`,
`"Silken Shirt 2"`, each matching the shop reader's independent `"6 in inventory"` to the digit.
**Only the battle menu was ever missing a count**, and that half shipped this session.

**HOW THE WRONG HALF OF THE REPORT SURVIVED A SESSION.** S143 read `PERF … InventoryReader::TryFocus
calls=1` in a log with zero item lines and concluded the reader was bailing out early. Those two
calls were the **title screen and the save-slot list** — that log never opened an item list at all.
*A counter proves a function ran; it does not prove it ran on the surface you are thinking about.*
The seven early-outs this section used to tabulate as "where to look first" were a hunt for a bug
that did not exist, and the instrumentation they called for was never written. **Before instrumenting
a refusal, confirm the surface was even visited** — one grep for the announcement the working half
would have printed (`[INV] item:`) answers it in a second.

**THE RULE, unchanged and now applied in BOTH menus: announce the quantity only when it is 2 OR
GREATER.** A row exists only because you own at least one, so a bare name already means exactly one.
(`inventory_reader.cpp:237`; `ingame_menu_reader.cpp`'s `ItemCountSuffix`.)

**THE BATTLE MENU IS ITS OWN SYSTEM** — it never goes through `FUN_005655f0`, and the field row's
`+0x0E` does not apply to it. Its count and its MP cost are **the same word**, the u16 at
`panel+0x512+row*8`, and only the gate at `panel+0x50C` says which (`3` = count). Two other
discriminators were shipped and measured away first — the draw callback (the Items list shares
`FUN_0027ce70` with the magick list) and the list kind (both are case `0xB`). Full model, and why
the gate must not be replaced by "is the number non-zero", in `GameArchitecture.md` under the battle
command menu.

### 4. The `<n>` dialogue macro renders empty — TRACED, one unknown left

**The number exists and is reachable.** The mod reads *"Only  Bhujerban heeds your words."* (double
space) and *"Bhujerbans heed your words."* (the leading `<n> ` gone entirely); *"No one heeds your
words."* is a separate message with no macro and reads correctly.

**THE MESSAGE BYTES.** In `byu_a01.ebp` (message base `0x41C00`), entries 5 and 6 both carry the
same six-escape run exactly where the number belongs, and entry 7 has none:

    0F 28 81 A4 | 0F 29 80 95 | 0F 2E 80 91 | 0F 29 80 80 | 0F 28 81 98 | 0F 3C C1 FD

**THE DECODER FRAMES THEM CORRECTLY AND EMITS NOTHING.** `game_text.cpp`'s `EscapeParamCount` gives
all six 2 parameters (`0x29` takes its `k = at(1) & 7 == 0` branch), so the run is consumed cleanly.
**This is not a framing bug** — the substitution is simply not implemented.

**WHERE THE VALUE LIVES** (traced this session): `setmesmacro` is native `0x1A8` → impl
`FUN_0034CF20` → `FUN_002E1B70(slot, index, valA, valB)`, which clamps `slot` to `[0,7]` and writes

    *(u32*)(DAT_0215F540 + (slot*0x20 + index)*8) = valA
    *(u32*)(DAT_0215F544 + (slot*0x20 + index)*8) = valB

so the macro table is at abs `0x0215F540` = **RVA `0x203F540`**, 8 slots × 32 entries, 8 bytes each.
`updatemesmacro` is native `0x1A9`.

**THE ONE REMAINING UNKNOWN:** which of the six selectors is the "print macro" one, and how its two
parameter bytes map to `(slot, index)`. Find it with a **Ghidra xref pass on `DAT_0215F540`** — a
recursive grep over the 33k-file decompile times out. The escape dispatcher is `FUN_002AC5F0`
(already named in `game_text.cpp`'s comments) and the generic 2-param handlers it lists are
`FUN_003FFAB0 / 003FFC60 / 003FFDA0 / 003FFEE0 / 003FFB90`; the reader is one of those.

**WHY THIS IS DONE PROPERLY RATHER THAN QUICKLY.** The fix belongs in `GameText::Decode`, the choke
point for **all** text in the mod — menus, dialogue, item names, everything. A wrong change there
breaks every surface at once. Done right it is a general win: every macro'd line in the game starts
reading correctly, not just this minigame's. It would also hand us the civilian earshot for free,
since the heed count is ground truth for who actually heard a shout.

### 5. Routing to a moving target

See the entry above this block — cause, fix and risk are already written up there.

## Session 147 — what shipped, and the two things that need a play to settle

**KEYWORDS: tester report status R1 character switch instance letter Dire Rat B libra o key autodetail
F7 polish diacritics automatic font00.dat detect exit reachability water bit23 strict flood hunt
reward panel power conduit dungeon interactables friction log corpus tester logs**

### OPEN — the dungeon device ("power conduit"): a control-surface cost, measured

**Tester, 2026-08-10:** *"Takes a little bit of fumbling around in the dungeons to get to this power
conduit thing. It's a little bit of a pain in the ass but it's doable."*

Logged rather than fixed, at the user's direction: the dungeon needs its own pass and there is no
save near it (`Saves\` holds one file, `FFXII_005`). Recorded so the next session starts from evidence
rather than a re-investigation.

**What it costs today, in keypresses.** A dungeon device lands in `Category::Object`
("Interactables") through the `kind == 5` branch of `ClassifyByNameKey` (`entity_classify.cpp:205-217`)
— *provided* it is named or interactive at scan time, because `entity_scan.cpp:330-347` drops nameless
non-interactive objects (`s_dropKind5` is the counter that says when that bites). Reaching it is then
`=` x8 (or `-` x3) around the eleven-category ring, `]` xN through everything in that category, and
`\`. That is the "fumbling".

**What is NOT available as a shortcut.** The engine keeps **exactly one** interaction target and
offers no cycling (`interact_target.h:15-19`, 0.98) — so a "next device" key cannot come from the
game; it would have to come from the mod's own list. And `;` already names the engine's chosen target
but deliberately does not route to it (`interact_target.cpp` calls no `PathPlanner`).

**Neither Raithwall's Tomb nor Barheim appears anywhere in `Docs\`** — this is new ground, not a
regression in something already understood.

### The tester's five other reports, and where each landed

| Report | Outcome |
|---|---|
| Status menu does not update on R1 | **FIXED.** One missing event: `FUN_002c2c50` (RVA `0x1A2C50`). The cycle was never observed because it happens inside category `0xa`, which the reader filters out as per-frame — correctly. |
| Instance letters not spoken in the targeting menu | **FIXED.** `battle_target_reader.cpp` had a SECOND enemy-naming path that decoded `actor+0x18` inline while `;` and `p` used `BattleState::DisplayNameForActor`. Only the latter ever grew the letter. One path now. |
| Libra support for viewing enemies | **BUILT** on `o` (and volunteered by autodetail). The flag Session 32 could not find is `*(u32*)(P + 0x10F68) & 2`. Weaknesses **are** included — `BtlChr+0x40`, suppressed for Libra-proof marks and bosses. A first pass wrongly reported them unobtainable; see below. |
| Autodetail | **BUILT** on `F7` + the `F8` menu, default Off, exactly to the spec in `Controls.md`. |
| Polish diacritics automatic | **BUILT** — detected from the loaded font atlas. See below; this does NOT on its own close the tester's diacritics defect. |
| Hunt rewards not read | ~~Surface **found** (it was the function we already hooked)~~ **STRUCK S178** — it was never that function; the panel is `FUN_003f4330` (`questresultwindow`). **SOLVED S178, play-confirmed 2026-09-15.** |
| Exits/doors you cannot reach | **Instrumented, not changed.** See below. |

### SOLVED — enemy elemental weakness is `BtlChr + 0x40` (Session 147, after getting it wrong first)

**The first answer this session gave was: "they are not obtainable, and the game does not show them
under Libra either." Both halves were wrong, and the user said so.** Libra draws a `Weak:` row of
element icons on the target panel; the mask behind it is a single byte on the BtlChr; and it was
already arriving in the vitals snapshot this mod's own hook receives, at snapshot `+0x89`. The full
chain, the Libra gate, and the `????` boss flag are in `GameArchitecture.md` §Session 147.

**THE LESSON, and it is the second time this shape has cost a session: a negative result is only as
good as the shape you searched for.** The three checks below are all still TRUE. Every one of them is
also irrelevant, because each asked about a structure the answer does not have — a four-mask quartet,
and a field on the enemy record. The answer was one byte on the combatant. Nothing in the search
would ever have found it, and "I looked hard and found nothing" felt like evidence anyway.

The correction that would have worked, and is the rule now: **when a search comes back empty,
re-derive what the DISPLAY reads.** The game draws the thing, so something reads it — start at the
draw call and walk backwards. That takes minutes and cannot produce a false negative. In this case
`FUN_002bfd20` hands `panel+0x149` straight to `FUN_00295d90`, which emits message `0x2331`
(*"Weak: "*) and then one element sprite per set bit; two hops back from there is `bc+0x40`.

Kept below because they remain correct, and because the next session should not re-run them:

- The Weak / Absorb / Half / Immune quartet the project already holds (record `+0x3C..+0x3F`) is on
  the **equipment** record. Its only consumers are `FUN_00374280` / `FUN_003745c0`, which copy it into
  the equip-preview scratch globals `_DAT_02ae96a0..ac` — the 0x36F-0x374 preview UI, not the battle
  path.
- The per-actor enemy record at `actor+0xE68` carries no affinity field; its reads in the damage range
  (`FUN_00388660` `+0x20`, `FUN_00390ab0` `+0x12`) are unrelated.
- Element multipliers are applied inside the ~110 `FUN_0038c9xx`-`FUN_0038dxxx` formula functions and
  are not preserved (already recorded at 0.98). The action record `+0x13` is the only element source
  for an ATTACK — which is a different question from the target's weakness, and conflating the two is
  part of how the wrong conclusion got its confidence.

> ⚠ A fourth bullet stood here and is **STRUCK**: ~~"And the game itself does not show them under
> Libra. Libra reveals HP/MP numbers, level and traps. Speaking a weakness would be inventing a fact
> the screen never states."~~ It does show them. Worse, this was the bullet that turned three
> offset-level negatives into a *design* justification, which is what made the answer feel finished.
> **A claim about what the GAME displays needs the same evidence as a claim about an offset** — and
> this one was written from recollection, not from the draw call.

Settled in passing, at 0.99: `result +0x42..+0x51` and `target +0x17C..+0x18A` are **per-status
countdown timers**, eight i16 slots indexed by `statusMasterRec+0x06`. That **STRIKES**
`notes\combat_re_2026_07_20_damage.md:302`, which labels them "resistance/affinity overrides" at 0.90
— a wrong lead that would have eaten this session had it been trusted.

### OPEN — the exit reachability contradiction, now measurable

**The tester asked for a filter; the user reframed it as a correctness bug, and the archive agrees.**
Our own dev logs have been printing the contradiction on adjacent lines for weeks:

```
[NAV-DIAG] raw=0x0FA00000 eff=0x0FA00000 type=0 count=3077  *** UNWALKABLE (bit23) ***
[NAV-DIAG] routable? "Exit, Garamsythe Waterway: East Waterway Control"
                     at (235.0,9.0,36.0) poly=99 eff=0x1FA00000 walk=1 reach=1
[NAV-DIAG] exits: controllers=1 surfaces=1 listed=1 | dropped: ... unreachable=0
```

`0x1FA00000` has bit 23 set. The census calls that poly class unwalkable; the routability line calls
the exit standing on it reachable. `NavMesh::Walkable` has been the poly TYPE mask alone since S96, and
`NavReach` inherits it — so the flood crosses the flooded channels, and **`unreachable=` is `0` in
every archived log while `reachability filter disabled:` has never appeared once.** The listing filter
has never dropped anything, in any session, ever.

**AND THE SAME ARCHIVE KILLS THE OBVIOUS FIX.** `"Exit, Bhujerba: Travica Way" poly=443
eff=0x0FA00000 walk=1 reach=1` — identical flags, a map with no water, in a log where four routes
succeeded. Bit 23 also marks ledge and out-of-bounds geometry under perfectly good exit seams. So
re-arming it is struck twice over: S96 did exactly that and had to revert it (399 of 690 floor prims
refused on map 311, and it cost the tester a working exit), and Travica Way is a live counterexample.

**What shipped is a measurement, not a change.** `NavReach` now runs a SECOND flood beside the
permissive one that also refuses `NavMesh::TerrainRefused` polys, published separately as
`ReachableStrict` and read ONLY by the routability line, which gained `terrain=` and `strict=`.
Nothing filters on it.

**The decision rule is written down in advance so the next session is not an argument:** if
`strict=0` tracks the exits the player genuinely cannot reach **and stays `1` on Travica Way and the
other working bit-23 exits**, it becomes the listing filter's second gate. If it fails either half it
is the wrong instrument too, and the next candidate is named from that log rather than from a theory.

**Still missing:** no archived dev log contains a ROUTE REQUEST to a Waterway exit — those sessions
listed the exits but never pressed `\` on one. So the listing half is measured and the routing half
needs one Garamsythe pass (East Spur Waterway, route to Central Spur Stairs and to No. 10 Channel).

### The Polish diacritics defect is NOT closed by making detection automatic

`GameText::DetectVariantOnce` removes the most likely cause — **a setting the player never set reads
as Standard** — and it strikes the "autodetection is not available" claim with a measurement (twenty
bytes, ten advance widths; see `GameArchitecture.md` §Session 147). It does **not** on its own prove
the mapping works.

The other two candidates from the S140 backlog stand: whether the mapping is applied on the read path
the tester's text actually takes, and whether more than sixteen slots moved. **This is the one item
where a tester log is the right evidence** — the dev machine has no fan patch installed, so our own
logs structurally cannot show the defect. Per the log-corpus rule in `CLAUDE.md`, that satisfies the
first half (a tester reported it) and still needs the second: **ask the user to point at the log**
rather than sweeping `Tester Logs\`.

On the dev machine the confirmation is one line: `[TEXT] font atlas DETECTED: standard (advance field
at record+0xNN, 10/10 slots matched)`. If it instead prints `font atlas UNRECOGNISED` with a dump of
values, the loader's record layout is not the file's and that dump is the fix.

### The log-corpus rule, learned the hard way this session

`<game>\x64\logs\` is the dev archive and the default evidence for everything. `Tester Logs\` is
opened only on a reported issue AND an explicit pointer from the user. This session reached for the
tester folder first, found no Waterway routing, and concluded the reachability defect was "not
measurable from any archived log" — while the measurement sat in nineteen of our own twenty logs.
Reaching for the wrong corpus did not merely waste the search; it produced a confident wrong answer.
Now recorded in `CLAUDE.md` under Key Paths and Auditing rules.

## FIXED — stale entities never left the tracker (S147 reported, S148 fixed it on the third try)

> **RESOLVED 2026-08-10 — and the framing was the fix.** The user, after two wrong diagnoses:
> *"you're still tracking it as if kills matter, when what we want is a stale entity pruner… entities
> will appear on the map but will not disappear when consumed or teleporting away or leaving."*
>
> **`sceneObj+0x14` bit `0x40` = present in the world.** Set for live party, live enemies **and
> treasure chests**; clear for a defeated enemy and for never-spawned reserve slots. Because
> treasures keep it set, it is a PRESENCE test rather than a liveness test, so one rule prunes a
> corpse, a departed NPC, a consumed chest and a cleared trigger alike. Applied in BOTH the
> handle-table walk and the actor-pool walk — the pool walk only sees what the handle walk did not
> list, so pruning in one alone would have re-admitted everything through the other. Every prune
> logs itself. Full derivation: `GameArchitecture.md`, *"sceneObj + 0x14 bit 0x40"*.
>
> **Three diagnoses, in order: an HP gate (S147), a stale pointer (S148), a presence bit (S148).**
> The first two were mechanisms that fit the symptom and were never measured; both were proposed
> with an ally caveat and a code comment that made them read as finished. What ended it was the
> user refusing the frame — twice — and a counter that had been shipped alongside the failed fix.
> **Neither wrong answer was wrong about a fact; both were wrong about the QUESTION.** "Why does a
> dead enemy stay?" has no good answer. "Why does anything stay?" has exactly one.

> **PLAY RESULT, 2026-08-10, on the S148 build — READ THIS FIRST.** The fix below is real and stays,
> but it is **not** what holds a corpse in the list. The new counter settled it in one press:
> `[NAV] grace: carried=0 evicted=0 filtered=2`, once in a whole session, while `Enemy=1` persisted
> across 25 rescans and a dismissed Belias answered `]` for 13+ seconds. **`carried=0` means the
> grace window never saw these entities at all** — `EntityScan::Build` is producing them on every
> single scan.
>
> So S147's first half was right after all: **the scan keeps listing them.** My S148 replacement
> premise (a stale pointer kept alive by the merge) was wrong for these two cases in the same way
> S147's was — asserted from a mechanism that fit, not from a measurement. **Two sessions running,
> the cause was picked before the counter existed.** The counter now exists; use it.
>
> **Candidate, measured only, in the object dump the user captured:** within the character
> containers `sceneObj+0x14` reads `0xF0` for the live party, a live Giza Rabbit **and Belias**, and
> `0xB0` for the DEFEATED Hyena and four never-spawned reserve slots. Bit `0x40` is the difference,
> and `0x20` beside it is already known as "model loaded". Treasures read `0x70` and are listed
> correctly, so `0x40` can never be a global gate — combatants only. **It does not explain Belias**,
> which still reads `0xF0` after dismissal. `entity_scan.cpp` counts it and filters on nothing.
>
> **The measurement still missing:** a `'` dump taken WHILE an Esper is summoned, diffed against one
> taken after dismissal. No log in the archive has a before/after pair.

**KEYWORDS: dead enemy tracker not disappearing killed corpse entity list grace window lastSeenMs
RefreshPositionsLocked kEntityGraceMs KIND_DEAD s_poolKind5 ACTOR_ACTIVE_BIT BC_CURHP buffer growth
Category::Enemy entity_scan BuildLocked actor pool HP gate struck stale pointer**

**Report (the user, in play):** *"when killing enemies, they are not disappearing properly from the
enemy tracker… we don't want a massive buffer of enemies building up over time, even if they are
cleared on map change."*

### THE FIX (S148): `lastSeenMs` had two writers, and the wrong one won

`RefreshPositionsLocked` (`entity_list.cpp`) now **skips any entity the latest scan did not produce**
— no transform read, no stamp — leaving `RescanLocked` as the field's only writer. The 2 s grace
window then measures what it was written to measure and a killed enemy ages out. Counters
`carried=/evicted=/filtered=` were added to a `[NAV] grace:` line, because nothing in the codebase
had ever reported what that merge did.

### ~~"There are TWO independent defects here"~~ — HALF STRUCK. There was ONE, and it was defect 2

**The user, correcting S147:** *"the entity itself disappears, so there's no need to hang on to it…
you just need to ensure the live delta is tracking and getting rid of stale entities. HP gate is the
wrong solution."*

**~~Defect 1 (no HP test in the actor-pool walk) and its proposed fix are STRUCK.~~** The
measurements below are all still true — the `KIND_DEAD` skip really does tally zero in 602/602
rescans — but they were assembled on top of a premise stated from recollection rather than measured:
*"a corpse is a live scene object with a perfectly readable transform"*, i.e. that the pool goes on
reporting the dead. **It does not.** The scan drops the corpse on its own; what kept it listed was
the merge putting it back. An HP gate would have added a second liveness test to fix a bug in a
third, and `feedback_hook_arity` aside, it would also have needed an ally exemption that nothing
would ever have exercised.

**The lesson is S147's own, applied to S147:** a negative result is only as good as the shape you
searched for — and a *positive* claim about what the engine keeps alive needs the same evidence an
offset does. Dressing it in a design rationale ("and here is the ally caveat") is what made it read
as finished.

**What follows is kept as the record of what was measured.** Read `1.` as struck.

**1. ~~Nothing in the field scan ever asks whether a combatant is alive.~~ STRUCK — see above.**

`EntityScan::BuildLocked`'s actor-pool walk (`entity_scan.cpp:169-224`) admits a combatant on three
gates: the slot has a `def` pointer, `ACTOR_ACTIVE_BIT` is set, and the scene-kind nibble is not
`KIND_DEAD`. There is **no HP test at any point**, and the kind test is measurably inert:

```
602 x  "0 actor-pool entr(ies) skipped as KIND_DEAD(5)"      <- every rescan, all 20 archived dev logs
  0 x  any non-zero value
```

That counter exists precisely because the line was suspect — its own comment says *"If this tally is
ever non-zero on a field map, that premise is false and the skip has to go."* It has never been
non-zero, which settles the opposite point: **the skip has never removed anything, so it is not what
drops a dead enemy, and nothing else is.** While the corpse's actor slot stays active, `Build`
re-lists it as `Category::Enemy` on every single rescan.

Compare `battle_target_reader.cpp`, which has had the right liveness test all along and uses **two**
signals — scene-kind `KIND_DEAD` **and** `curHP == 0`. The nav side uses neither usefully. And the HP
is already one guarded read away inside the loop that needs it: `def` IS the BtlChr (`ACTOR_DEF_PTR`
= `+0x698`), so `SafeReadU32(def, BC_CURHP)` sits beside the `DEF_KIND_BYTE` read that is already
there.

**2. Even if the pool DID stop reporting it, the grace window could never age it out.**

`RescanLocked` (`entity_list.cpp:98-114`) carries an entity that has stopped being reported until
`now - lastSeenMs > kEntityGraceMs` (2000 ms). But `RefreshPositionsLocked` (`:152`) stamps
`it->lastSeenMs = GetTickCount64()` for **every** entity whose transform still reads, every field
frame. So `lastSeenMs` never goes stale and the 2 s window never expires.

**THIS IS THE WHOLE BUG, and it needed one correction to be right about WHY.** ~~"a corpse is a live
scene object with a perfectly readable transform"~~ — struck. The corpse is *gone*; what still reads
is the mod's own **stale scene-object pointer**, into memory the engine has released but not yet
recycled. Same observable, opposite mechanism, and the difference decides the fix: an object the
engine still owns argues for asking it whether it is alive (the HP gate), while a dangling pointer
argues for not consulting it at all. The shipped fix skips carried entities entirely — which also
stops the mod reading coordinates out of freed memory for up to two seconds, a second defect nobody
had noticed because the first one hid it.

**This exact failure is already written down twice in this codebase, for a different case.**
`entity_list.cpp:105-110` and `entity_scan.h:216` both say: *"a filtered object is a LIVE engine
object, so RefreshPositionsLocked keeps reading its transform and keeps stamping `lastSeenMs`, so it
can never age out of the grace window. Once carried in, it stays for the life of the map."* The fix
adopted there was `EntityScan::WasFilteredThisScan` — an **explicit "we removed it" signal** that
bypasses the window rather than an absence-based timeout. A dead enemy needs the same shape, and for
the same reason (S83's lesson: *the grace window re-admitted everything every filter deleted*).

### Why it is bounded by the map, and why that is still not good enough

`g_entities.clear()` on map change (`entity_list.cpp:292`) is the only thing that empties the list, so
growth is bounded by the number of distinct combatant scene objects visited on one map. On a map where
the player fights repeatedly — or one with respawns — that is exactly the "massive buffer" the report
describes, and every stale entry is one more `]` press between the player and a live target.

### What the archive did NOT have, and the instrument that shipped instead

**No dev log contained a kill.** `Enemy=` reads `0` in 582 rescans and `3` in 20; nothing showed the
post-kill state, so the observed behaviour was the user's play report and nothing else.

The instrument S147 specified was ~~`N combatant(s) admitted with curHP==0`~~ — an instrument for the
struck defect, and it would have measured zero forever. What shipped is the counter for the merge,
which is where the bug actually was:

```
[NAV] grace: carried=N (within 2000ms) evicted=N (aged out) filtered=N (explicit)
```

Silent when all three are zero. `evicted` going non-zero after a kill, and `carried` coming back to
zero, is the falsifier this defect never had.

**The instrument must match the defect, not the hypothesis.** S147 was right that a counter had to
come first and still specified the wrong counter, because it had already committed to a cause. The
one that shipped reports what the code *does* (how many entries the merge kept, dropped, refused)
rather than what a theory predicts, which is why it stays useful now that the theory is struck.

### Rules that survived the strike

1. **Do NOT delete the `KIND_DEAD` skip** because it measures zero. It is inert on the field, but the
   constant is owned by the combat track and `phyre_types.h` already labels its name wrong. Removing
   it is a separate, unmeasured change. Left in place; S148 did not touch it.
2. **No HP gate in the nav scan, ever.** Beyond being unnecessary, the same walk files `KIND_ALLY` as
   `Category::NPC`, and a KO'd party member is revivable and still worth listing. A comment in
   `RefreshPositionsLocked` says so at the point where somebody would next be tempted.
3. **An explicit removal signal beats a timeout for a deliberate drop.** `WasFilteredThisScan` stays,
   and its justification is now the right one: the filters delete a LIVE object the scan keeps
   finding, so without the signal every rescan re-admits it. (It was previously justified by "it can
   never age out", which was true only because of the bug fixed here.)

## FIXED — the Status screen's L1/R1 switch was cut off by "Regen" (S147 shipped it, S148 fixed it)

**KEYWORDS: status screen L1 R1 character switch Regen interrupted ailment grid focus 2C390A00
FUN_002c2c50 menuCtx+0x110 StatusReader TryFocus claim block two speakers interrupt=true**

**Report (the user, in play):** *"your fix for pressing l1/r1 to switch characters on the status
screen worked, but it's being interrupted by 'regen'… the mod is reading one of the active status
effects just after the character name, which is undesired."*

The log had it verbatim, same millisecond, on every switch:

```
[STATUS] page: owner=…2C390840 "Balthier"
[SPEAK-OUT] Balthier
[READER] focus owner=…2C390A00 index=0
[READER]   item: "Regen"
[SPEAK-OUT] Regen
```

**Two speakers, two policies, one surface — the notice-board failure again.** `FUN_002c2c50` refills
the ailment grid (`menuCtx+0x110`) as part of the screen's refresh; the game answers by re-firing
focus index 0 on that pane; the **generic** `MenuReader::OnFocus` speaks it with `interrupt=true`
(`menu_reader.cpp:342`), beating `StatusReader`'s name, which also used `interrupt=true`. Neither
speaker is wrong on its own and neither looks wrong in isolation, which is why nothing in the code
reads as a bug — the same shape as the notice board's Status column vanishing to a 125 ms race.

The content was redundant besides: those statuses are already group 3 of `StatusReader`'s own virtual
buffer.

**Fix: arbitrate at the one documented claim point.** `StatusReader` had no claim predicate at all;
`menu_reader.cpp:217-235` is where a reader takes a surface (`PrimerReader::OnHuntFocus`,
`SaveReader::TryFocus`). Added `StatusReader::TryFocus`, claiming a **one-shot** armed by
`HookedStatusRefresh` after it announces, consumed by the first focus event that follows, and
rejected unless the owner is not our own container and the index is 0.

**It is a transition latch, not speech dedup, and it must stay one.** The ailment pane is
player-navigable — the archive reaches `index=1` on it — so a blanket mute would have silenced real
navigation. The latch also disarms on the first focus event of any kind and on `Deactivate`, so it
can never lie in wait across a press. Every consumption logs
`[STATUS] ailment focus swallowed: owner=… index=… (ctrl=… ailmentGrid=…)` — one per switch and none
per navigation is the proof, and the two pointers are printed side by side so a later session can
tighten the one-shot into a plain structural test if they always match.

## License board availability + a runaway tutorial repeat — SOLVED (Session 149, 2026-08-10)

**KEYWORDS: license board prerequisites adjacency cell 0x18 flags 0x1000 reachable FUN_0055e090
FUN_0055cd40 0x8001 confirm invalid action sound buzzer FUN_00249c60 FUN_00323600 no adjacency test
can learn wrong tutorial box button prompt never ending repeat press AAAA widget 0xC0 level not event
oscillates FUN_002a8c50 534-536 FUN_002e16b0 0x1C16B0 re-arm end latch idled**

### 1. "Can learn" on a node the game refused

**Symptom (user, Nomad Village save):** navigated to a node the mod called learnable, pressed
Confirm, the game played the invalid-action sound.

**Cause:** `license_reader.cpp` derived the spoken status from `FUN_00323600`. That function's body
`FUN_00323d10` checks the learned bitmask, the node-type specials and `LP < cost` — and **nothing
about reachability**. FFXII only lets you buy a node adjacent to one you already own, so every
unreached node with enough LP was announced "can learn".

**Fix:** read `cell+0x18`, the one word `FUN_0055cd40`'s Confirm branch (case `0xc` / sub-message
`0x8001`) tests: `0x2000` learned · `0x1000` prerequisites met · `0x4000` affordable. `FUN_0055e090`
is the adjacency flood that sets `0x1000` from every learned cell's four orthogonal neighbours. Full
bit table in `GameArchitecture.md`.

**The lesson: DON'T MODEL THE VERDICT — READ THE WORD THE VERDICT IS READ FROM.** A second model of
"can this be bought" can drift out of step with the game's; a read of the same flag word agrees by
construction. A negative result about `FUN_00323600` ("it doesn't say") was available in its own
68-line body the whole time.

**The status answers ONE question, by user decision: "will Confirm do anything here?"** Three states
— learned / can learn / **silence** when the prerequisites are not met. Two words were cut:

- **No "prerequisites not met" phrase.** There is no game wording to borrow (node state is icon
  colour; a cell's only codec is its category word), so inventing one would be a fabricated label.
  Silence also makes "can learn" a claim the mod only ever makes when Confirm really responds.
- **No "not enough LP".** *"that's up to the player to decide… we already have LP displayed per
  license and a key to check total LP."* Affordability is arithmetic the player already has (the cost
  is in the line, `U` reads the total) — and it is **not** the no-op case: `FUN_0055cd40` answers a
  reachable-but-unaffordable Confirm with the game's own `FUN_002ce2f0(board, 10)` message, so the
  game gives that information itself at the moment it matters. `CELL_AFFORDABLE` is still named and
  still visible in the logged `flags=` word; it is never spoken.

**THE SECOND LESSON: DON'T MIX WHAT THE PLAYER CANNOT WORK OUT WITH ARITHMETIC THEY ALREADY HAVE.**
A status line earns its place by answering the question the screen cannot.

`LockedUpper` ("Locked") still owns the blank `id == 0xFFFF` tile. `LockedLower` and `NotEnoughLP`
were deleted with the switch they served.

`FUN_00323600` survives as a **log-only** cross-check: `node[flags=0x%04X status=%d]:` prints both
answers per node, so one board sweep shows where the old model and the game disagree.

### 2. A tutorial box repeated its line forever at a button prompt

**Symptom (tester, no log):** *"the box starts and is read correctly until a button prompt should
play… a never ending 'clone' of that key input… sounds like 'press AAAAAAAAAAA'… even hitting NVDA
key doesn't stop it, it only stops if the line is skipped/over."*

**NOT the button icon.** `0x0F 0x40-0x6B` takes one parameter and emits nothing, which is correct
for speech and is what the readme promises. The 462-escape naming gap is untouched and still open.

**Cause: `widget+0xC0` is a LEVEL, not an event.** `FUN_002a8c50` sets it to 1 at the codec
terminator (`:136-146`); the next call takes the skipped path and sets it back to 0 with `mode = 1`
(`:534-536`); mode 1 passes the `:139` guard, so the call after that latches again. A **finished box
that stays on screen** therefore reads `1, 0, 1, 0` at frame rate — and `dialogue_reader` wiped the
page key on every `1`. The next frame read `0`, found no key, and re-emitted: speech every two
frames with `interrupt=true`, each utterance cut off after ~33 ms, and stopping the reader only
clearing the way for the next. It also killed `t` on such a box (`ForgetLastLine` ran on every wipe).

Ordinary dialogue never showed it: those pages park at a `0x03` break *before* the terminator and
the box is torn down when the script advances. A tutorial banner runs to the terminator and then
waits for a dismissal press — which is exactly "when a button prompt should play".

**Fix:** `PageKey` gained `ended`; the latch is handled **once per key** and `base`/`off` are
**kept**, so the emit path's own equality check holds on every `ended == 0` frame. The re-arm the
wipe used to provide moved to the game's own event — `FUN_002e16b0` (RVA `0x1C16B0`), the writer of
the `DAT_0215f200` slot. That hook **speaks nothing and must never be made to**: it fires ~18 times
in 140 ms at area load. It is NOT a restoration of the S52 content-setter reader.

**Instrument:** the inert repeats are counted per slot and logged once at 64 —
`end latch idled 64x on wnd=…`. Presence proves the loop was real; **absence on the reported
tutorial box means this diagnosis is wrong** and needs re-testing on the tester's build.

**PLAY-CONFIRMED 2026-08-27 (user, relaying the tester): the repeat at a tutorial button prompt is
gone.** Corroborating: `end latch idled` appears **zero times** in all three of our own 2026-08-27
sessions on V0.6.5 (`9e9a00f`). The diagnosis stands and the instrument may be retired the next time
`dialogue_reader.cpp` is opened for other reasons — it has now served its purpose.

**PLAY-CONFIRMED (same session), and the spoken word is `available`, not `can learn`.** The user:
*"license board works perfectly, even announces the game's own 'insufficient license points' when
failing to learn."* That settles the one thing left open — the not-enough-LP popup is
`FUN_002ce2f0(board, 10)`, not the `FUN_0057c480` surface `message_reader` covers, so whether it
spoke was unknown. It does, in the game's own wording, at the moment it matters. `Phrase::Id::CanLearn`
became `Available`: "can learn" promises an outcome the player's LP might not support, and the mod
deliberately does not read the LP.

## Treasure never leaves the nav list — the presence bit answers a question these objects do not answer (Session 150, 2026-08-11) — SOLVED, PLAY-CONFIRMED

> **RESOLUTION.** Fixed by hooking the game's own treasure AWARD (`FUN_002fafd0`, RVA `0x1DAFD0`) and
> identifying the collected object by the coordinates the award record was handed — the same floats
> the placement wrote. **Play-confirmed 2026-08-11**, which also settles the one link the decompile
> could not: the scene transform reads those floats back exactly, so `kMatchTol` (0.25 m) is slack
> for the round-trip and **not** a search radius. See `GameArchitecture.md` → *Treasure — spawn,
> award, and how a COLLECTED one is detected*.
>
> **Related fix in the same session:** the `0x40` ABSENT pruner had been deleting a **Save Crystal**.
> The bit was measured on COMBATANTS and applied to the whole handle table; it is now gated on
> `isCharacter`. Play-confirmed at 98 drops, all `+0x14=0xB0 kind=1` named enemies, nothing
> collateral. **A measurement's scope is part of the measurement.**

**Symptom (tester, no log pointer):** *"once you pick one up they are still there and can make
tracking them down a pain in the ass."* Collected treasure stays in the F5 entity list forever.

**STRIKES a claim that was standing as fact in three places** — `Docs\GameArchitecture.md` (the
`+0x14` bit table), `src\navigation\nav_rva.h` (`READY_PRESENT_BIT`), and
`src\navigation\entity_scan.cpp` (the stale-entity pruner block). All three asserted that the
Session 148 presence bit prunes *"a consumed chest"* by the same test that prunes a corpse.

**It was never measured.** It was inferred from the fact that UNOPENED treasure reads `0x70` — i.e.
from the bit being SET on a live one, which says nothing about what happens when it is taken. This
is the same shape as the two diagnoses S147 and S148 each had struck: **a mechanism that fits the
symptom, asserted without measuring.** Third session running.

**The refutation was already sitting in our own log archive.**
`x64\logs\FFXII-Screen-Reader-2026-08-10_12-08-35.log:1133-1145` dumps two treasure slots the game
**never placed at all** — world origin `(0,0,0)`, `layers=0 raw=0` — and both read `r14=70`, bit
`0x40` **SET**. The never-spawned *enemy* reserve slots in the same dump read `r14=B0`, bit clear.
So on treasure/gimmick objects `0x40` is set unconditionally and carries no placement or presence
information whatsoever; bit `0x80` is what actually separates combatants from gimmicks
(`0xF0`/`0xB0` have it, `0x70` does not). Corroborating count: `Treasure=5` flat across 30 rescans
in that session, `Treasure=4` across 20 in another, `Treasure=3` across 39 in a third — never once
decrementing.

**Consequence:** the S148 pruner already runs on every treasure, in BOTH walks
(`entity_scan.cpp` `ScanCombatants` and `BuildLocked`), and **provably cannot ever fire on one.**
Widening or re-testing that bit is wasted work.

**Why a treasure survives the scan's own admission rule:** `entity_scan.cpp:366` drops an object
only when `!named && !interactive`. Treasure is `named` (npcdic 434 "Treasure", 468 "Urn"), so a
cleared interaction flag never drops it — `entity_classify.cpp:152-154` says that is deliberate, so
a named gimmick stays listed "even if its interaction flag is momentarily clear".

**Do not call these chests.** There is no opening animation; they are collectable world objects and
the game's own script vocabulary is `setuptreasure` / `talktreasure` / `settreasureflag`. The mod
has always called them `Category::Treasure` and the docs now match.

## Equipment offhand list read nothing — SOLVED same session: it is EMPTY-vs-EQUIPPED (Session 150, 2026-08-11)

> **STATUS: HALF FIXED, ROOT CAUSE STILL OPEN.** The off-hand candidate list now **speaks on entry**
> (play-confirmed) via an announce gated on class `0x2DDFE0` — the one the unclaimed-pane census had
> already specified. **Navigating within that list is still silent** and is not diagnosed.
>
> **TWO ROOT-CAUSE CLAIMS WERE MADE AND BOTH WERE REFUTED IN PLAY (2026-08-11) — do not revive them:**
> 1. *"It is empty-vs-equipped, not shields."* **REFUTED.** An unequipped **helm** slot reads
>    correctly on entry AND while navigating, and the off-hand still fails **with a shield
>    equipped.** Equipped state is not the variable.
> 2. *"The off-hand uses a different window class."* **REFUTED.** One object address (`…BF63DC0`)
>    serves both `"SHIELDS"` and `"WEAPONS"` in the same session — same instance, not merely the same
>    class.
>
> **The live hypothesis is the category, `0x41`** — the only one whose rows `FUN_0057cf20` builds on
> a bespoke path (stub first switch, then a two-pool merge of shields + ammunition, with two exits
> that free and NULL the array). Same window, different BUILD. This hypothesis was raised early,
> wrongly struck in favour of (1), and is restored.
>
> **Instrument in place for the next pass:** `focus msg on a NON-cursor pane: owner=… val=N -> cursor
> pane=… class RVA=…`, capped at 12 and keyed on the value so it cannot collapse. `FUN_003fdfe0`'s
> own handler opens with `val * 0x20 + container[+0xE0]`, so a focus message for this list carries a
> usable row index; the open question is which window it arrives on.
>
> **Three throttled diagnostics were mistaken for measurements first:** `unclaimed pane` is once per
> DISTINCT CLASS (`s_seenCls[12]`), `[DESC]` is a paint dump capped at 10, and a category-shaped
> hypothesis (`FUN_0057cf20` case `0x41`) fit every symptom and was wrong. **Read a log line's
> emitter for a cap or a dedup before treating it as evidence.**

**Symptom (tester, both keyboard AND controller, so not an input defect):** *"in the equipment menu
with offhand Shields. When you're going to select one, it doesn't read them."*

**NOT a slot-dispatch bug: the mod has no equipment-slot dispatch at all.** The candidate list is
claimed by struct SHAPE via `InventoryReader::TryFocus` (`menu_reader.cpp:472`). Slot to category is
entirely game-side — `FUN_003fd360` writes `category = slot + 0x40`, so **`0x41` is the offhand** —
and `FUN_0057cf20` case `0x41` is the ONE category with a bespoke branch: it merges two item pools
(shields *and* ammunition, `FUN_00252fa0(2)` + `FUN_00252fa0(4)`) and has two distinct paths that
hand back a **NULL row array**.

**NOT DIAGNOSED — and the reason it could not be is the finding.** `OnCategoryRefresh`
(`inventory_reader.cpp`) had **six early exits that were bare `return`s with no log line**. A
category that failed to announce left nothing behind at all, so "the claim misfired", "the game gave
us a null list" and "the mod never reached this screen" were indistinguishable. Our own log corpus
(21 archived logs + Latest) contains **zero equipment-screen play**, so nothing in it could
discriminate either. Three candidate mechanisms, none better than 0.35 — well under the 0.98 bar, so
nothing was fixed.

**Shipped instead: the instrument.** Each of the six exits now names which gate closed and carries
`owner / rows / scroll / table / raw180 / tab index / tab count / src / textId`, and the
`empty category -- claimed and SILENT` line gained its owner and index. Log-only, and the
`(owner, gate)` cache is a CACHE not a counter cap — a cap gets spent early and is then dead for the
one rejection that matters hours in (the repair `dialogue_reader.cpp:116-119` already needed).

**The discriminator for the next log**, opening the offhand slot with shields owned:
1. `category: … "SHIELDS"` then `empty category -- claimed and SILENT` → the game handed back a NULL
   `+0xE0` for `0x41`, or the claim is misfiring on a populated list. Note `FUN_005655f0` **nulls
   `+0xE0` at the top of every refresh** before rebuilding it, so a focus delivered inside that
   window reads null on a list that is about to have rows.
2. `category: … "SHIELDS"` then `[READER] focus … index=N` with no `item:` → the row read failed.
3. **No `category:` line, and the new line names the gate** → the mod never reached the surface.

**Corrected while here:** `equip_compare.h` claimed the Equipment screen's stat preview "is announced
automatically on each highlight". `FUN_003fe720` has exactly two call sites — `FUN_002c2320:240`
(the 4-row action list) and `FUN_002c2cd0:20` via `FUN_003ff360:91` (the 5-slot list) — and
**neither is the candidate-item list** (`FUN_003fdfe0`, `menuCtx+0x150`). The preview announces per
SLOT, and is silent while the player cursors the actual candidates.

## Gate crystals deleted by the presence pruner — SOLVED (Session 153, 2026-08-12)

**KEYWORDS: gate crystal dropped Rabanastre Crystal Weather Eye absent pruner READY_PRESENT_BIT
READY_POPULATION_BIT sceneObj+0x14 0x30 0xB0 LooksAbsent OldRuleWouldDrop spared isCharacter
insufficient scene category 5-7 S148 S150 third time Gate=0**

**Report:** *"gate crystals are being dropped."* Correct, and understated.

**The log already had it** — `absent: [0:17] +0x14=0x30 kind=4 "Rabanastre Crystal" at
(115.0,-10.0,151.0)` next to `rescan: … Gate=0 …` on a map that has one.

**Byte-shape histogram of every `absent:` drop in the reporting session:**

| `+0x14` | drops | what |
|---|---|---|
| `0xB0` | 44 | `kind=1` "Hyena" — correct, the corpses the filter is for |
| `0x30` | 153 | "Rabanastre Crystal" `kind=4`; `kind=5` "Weather Eye", "Chocobo Aficionado", "Horne", "Rabanastran", one nameless — **all wrong** |

**Root cause.** S148 measured bit `0x40` on combatants; S150 caught it deleting a Save Crystal and
scoped it to `isCharacter` on the reasoning that *"a crystal, a gate, a door and a treasure are
not [characters]"*. **That is false — a gate crystal is scene category 5-7**, so the gate never
excluded it. `0x40` alone separates nothing: clear on `0xB0` (drop) and on `0x30` (keep).

**Fix.** `EntityScan::LooksAbsent` tests the byte *shape* the ABSENT state was measured in — `0x80`
set and `0x40` clear, i.e. `0xB0`'s high nibble — instead of reading `0x40` alone. `0xF0`→keep,
`0xB0`→drop, `0x70`→keep, `0x30`→keep. One predicate, both walks. `nav_rva.h` had already recorded
*"bit 0x80 is what actually separates the two populations"* in S150; nothing acted on it until now.

**Instrument shipped with it (L-39).** `OldRuleWouldDrop` emits a capped `spared:` line naming every
object the old rule deleted and this one keeps, plus a `spared` count on the drop-census line. No
`spared:` line in a session that visits a gate crystal ⇒ S153 fixed something else.

**Play-confirm gate:** at the Rabanastre gate crystal — `Gate=1`, reachable with `=`/`\`, a
`spared:` line naming it, and `ABSENT` still rising when a Hyena dies.

**Tried & Failed, recorded so it is not retried:** *scoping the pruner by what kind of object it is*
(S150's `isCharacter`). It is not a measurement of the population, and the population disagreed.
Scope by the measured data instead.

## `o` spoke Libra instead of ability descriptions in battle — SOLVED (Session 156, 2026-08-12)

**KEYWORDS: Libra not active describe key battle menu magick technick description SpeakTargetDetail
TextCapture CurrentHelpText BattleCommandActive P+0x10F78 OFF_GATE committed acting ordering**

**Report (twice):** *"o when highlighting a magick or technick should read its description, not say
'libra not active'."*

**Two compounding faults, and the first hid the second.**

1. **`P+0x10F78` is NOT "target selection active".** That name was inferred and is **STRUCK**. It is
   true while the battle command menu is open, so the S155 gate on it changed nothing. The reporting
   log shows `ResolveTarget: "Zombie Warrior D" committed/acting enemy` — a spell already executing
   — being treated as an aiming cursor.
2. **THE ORDER WAS THE REAL DEFECT.** `SpeakTargetDetail()` ran first and returned true, so
   `TextCapture::CurrentHelpText()` was **never called**. The log has four "Libra not active" lines
   and **zero** `describe:` lines, which looks like "there is no description available" and is
   nothing of the kind — *the question was never asked.* Any fix that only gates the Libra branch
   leaves the outcome resting on that gate being correct.

**Fix — ask the description FIRST.** `MenuReader::DescribeHotkey` now runs: help text (which is
generation-gated to the current focus, `g_helpTextGen == g_helpGen`, so it cannot leak a stale
description into the target cursor) → refuse Libra while `IngameMenuReader::BattleCommandActive()` →
Libra → a log line naming which came back empty. A silent `o` is never ambiguous again.

`BattleCommandActive()` stores the battle panel and **re-validates it against the window class on
every read**, so a freed or repurposed panel stops answering true on its own.

**`ResolveTarget` was deliberately NOT changed** — its commit-first order is load-bearing for `p` and
`;` (2026-07-21 regression note). The residual (while aiming it can name a stale commitment rather
than the unit under the cursor) now emits a log line ONLY when the two disagree.

**Tried & Failed:** gating the Libra branch on `P+0x10F78` (S155). The flag does not mean what its
name said, and gating alone would not have fixed the ordering fault underneath it.

## A gambit row with a condition and no action read as "empty" — SOLVED (Session 158, 2026-08-13)

**KEYWORDS: gambit half-set row condition no action empty rec+0x15 class byte incomplete rec+0x10
rec+0x12 0xFFFF FUN_00567b60 FUN_0056a1d0 picker stale category paint cache FUN_0056b4d0
gambit_picker_reader ability summary double speak**

Reported: *"an empty gambit row is correctly being announced as empty; however, when a condition is
selected, that row remains empty, even when browsing the individual columns."* Both halves of the
report were in our own logs, and the second one — the action picker — turned out to be the worse
defect of the two.

### 1. The class byte meant INCOMPLETE, and the reader read it as EMPTY

`gambit_reader.cpp` decided a row was empty on `rec+0x15 == 2` **before decoding anything**. The
builder `FUN_00567b60` writes the condition id and the real condition name into the record first and
only then sets that byte to 2 when EITHER id is `0xFFFF` — so a row with a condition and no action is
class 2 as well, and its condition is on screen the whole time (`FUN_00568bb0` assigns `rec+0x00`
into the condition sprite for every row; class 2 only dims the colour). Full write-up, with the
commit and write-back functions, in `GameArchitecture.md`.

Fixed by testing the two ids separately (`rec+0x10`, `rec+0x12`), so an unset half contributes the
word "empty" and the row reads `"Foe: party leader's target, empty, off"` with every column
answering for its own field. `cls=` now rides on the reader's log line: the byte is evidence, not a
gate.

**A CORROBORATED CLAIM CAN STILL BE SCOPED WRONG.** S94's own probe output already showed the class
byte taking 0, 1 **and** 2 on a single screen, and its pass criterion — "ids read `0xFFFF` on exactly
the rows whose class byte is 2" — was recorded as confirmed. It was true of every row that run saw,
because that run never edited a row. The measurement was sound; the population was three quarters of
one.

### 2. The picker was being read from the paint cache, which is always one event behind

The condition/action chooser had no reader (S94 declined it as unmeasured), so it fell to the generic
painted-row path — and that resolves a row's text out of `TextCapture`'s per-paint cache. The focus
for a category switch arrives BEFORE the new rows are drawn, so the player heard:

    [READER] item: "Attack"     <- landed on Technicks
    [READER] item: "Horology"   <- then moved down INSIDE Technicks
    [READER] item: "Traveler"

and, on opening the action list over the condition list, `"Foe: party leader's target"` where
`"Attack"` was highlighted. A blind player was choosing gambit actions from a list that reported the
wrong row.

`ui/gambit_picker_reader.{h,cpp}` reads the picker's own row array instead. Both rebuilds
(`FUN_0056a890` / `FUN_0056bc70`) fill that array before they move the list cursor, so the timing
question disappears rather than being tuned. It claims the focus only when it actually spoke, so an
unrecognised shape still falls through to the path that covered the surface before.

**Tried & Failed — not attempted, and here is why.** Deferring the announcement to the next paint
(the `PrimerReader` idiom in `menu_reader.cpp`) would have worked only as long as the row happens to
repaint inside the retry budget, and it would have left the mod reading a cache when the game's own
array was sitting right there. Reading the source removes the failure mode; deferring only narrows
its window.

### 3. The same screen was speaking twice

`[LICENSE] summary: "Cure, unavailable"` twice in the same millisecond, from one owner, mid-edit: the
license board's ability page controller is driven by this picker too, and driven twice per event.
`AnnounceEntry` now stands down while `GambitPickerReader::IsLive()` — arbitration between two paths
that cover different cases, which is the sanctioned shape; the alternative (deleting a path, or
adding a same-as-last-time filter) is the mistake this project has already paid for twice.

### The category NAME — ASKED, ANSWERED, CLOSED (same day, play-confirmed)

There isn't one. A diagnostic build walked all eleven action tabs live and refuted every candidate:
`DefName(0x15, family)` gives only the four battle commands (all five magick tabs share family 1);
`DefName(0x15, tabIndex)` gives nonsense ("NOT USED concentration", "Summon", "Foecraft");
`DefName(0x18, family)` is the magick-SCHOOL table but the family byte is not a school id; and
`picker+0x580`, which the tab-step functions clear, is the 180-frame *"you cannot pick this"* error
banner. The tabs are a gambit-specific grouping (1 Attack, 5 Magicks, 3 Items, 2 Technicks) drawn as
icons, with no string table behind them.

**So any category label would have been invented. Tester's call: announce nothing** — the corrected
first row of the tab just entered already distinguishes all eleven. The diagnostic and its four
`DefName` calls per switch were removed with the question they answered, and the finding is written
into `GameArchitecture.md` and the reader's own header so it is not re-derived.

**Two corrections fell out of the same walk.** Row `+0x10`'s not-acquired value is **`0x10`**, not
the `0x0F` the decompile suggested. And `ingame_menu_reader.cpp`'s chooser constants had their names
reversed: **`0x18` is the magick-school table, `0x15` is the battle-command table.** The values and
the branch were always right — each is passed with an id from the chooser's own rows — so nothing
behaved wrongly; the comment simply named the wrong table, which is exactly the kind of claim that
gets built on later.

### PLAY-CONFIRMED 2026-08-13

Tester: *"gambit fix for the empty action on a condition is confirmed working."* The picker half is
confirmed from the log of the same session: eleven category switches, each speaking the new tab's own
first row (`Attack` → `Cure, unavailable` → `Protectga` → `Aero, unavailable` → … → `Traveler`), zero
stale items, zero duplicate `[LICENSE] summary` lines, and the picker class constant right first
time.

## SOLVED — the Draklor lift was a NUMBER, not a list (Session 163, 2026-08-24)

**KEYWORDS: Draklor Laboratory 66th Floor North Lift Terminal Select destination floor picker
silent highlights numeric field 0F 2D mode 4 widget+0x54 widget+0xA2 digit width no 0x0E block
choice_reader HookedChoiceTick FUN_002b35a0 SOLVED**

**Reported 2026-08-24:** the lift prompt speaks *"Select destination: F (Current location: 68F)"* —
the floor number missing before the `F` — and moving the highlight says nothing at all.

**The reader was hunting the wrong kind of surface.** This prompt hosts the message widget's
**numeric field**, not an option list. `FUN_002a8c50` reaches a `0F 2D` escape, puts the widget's
`+0xB0` low byte into **state 4** and hands off to `FUN_002b35a0`; there is no `0x0E` block on the
page and `window+0xC0` (the child option-list window) is never created. `OptionCodec` finding nothing
was **correct behaviour**, not a parse failure.

**What made it read as a two-option list.** In state 4 the same bytes carry different meanings:

| logged | field | what the reader assumed | what it is |
|---|---|---|---|
| `b=2` | `widget+0xA2` | option count | **digit width of the maximum** (floors 66–70) |
| `a=0` | `widget+0x58` | row cursor | **packed spinner state**, three 6-bit fields |
| `wait=66` | `widget+0x54` | park reason | **the selected value** — floor 66 |

Every one of those is a plausible number, so nothing anywhere looked wrong. The full mode table is in
`GameArchitecture.md` under PAGINATION.

**Fixed three ways, all in one build:**
1. `ChoiceReader::HookedChoiceTick` reads the `+0xB0` mode **before any other field** and takes a
   numeric branch that speaks `widget+0x54` on change. `+0x54` is authoritative in both flavours the
   field can take (free range / pick-from-candidates), so the reader needs no index of its own.
2. `GameText`'s decode loop renders `0F 2D` when a `NumericFieldScope` is open — purely additive,
   identical advance, so every page without one decodes byte for byte as before. `DialogueReader`
   opens it in mode 4, which is what puts the floor number into the spoken sentence.
3. `NotePage` now carries the value the page line just spoke and **seeds** the tick's baseline, so
   entry says the whole sentence once and each move says the new number. It is a seed, not a filter:
   a move back to the starting floor differs from the value last spoken and is announced.

**Also fixed:** `OptionCodec` had seven false returns all reported as *"no 0x0E block"*. Each now
names itself. A diagnostic that collapses seven causes into one sentence is what let this look like a
parse bug for a session.

**Tried & Failed, recorded so it is not retried:** scanning further for the marker. `widget+0xA2`
reading 2 was taken as proof the game had found a `0x0E` header (`choice_reader.h` says that field is
written from it), which pointed at the page-offset scan as the culprit. It was not — in mode 4 that
field is never touched by the block walk at all.

## SOLVED — "No path" to a target the game was offering an ACTION on (Session 164, 2026-08-25)

**KEYWORDS: C.D.B. Draklor Laboratory 67th Floor no path off-mesh goalPoly=-1 goal=-1 frontier
suppressed off-mesh-nothing-in-reach reach 0.50 radiusMin class 1 class 3 gimmick volume
ReadReachFor ReadBandFor ObjectClass FUN_0025be50 FUN_0025bad0 FUN_003a1960 kNoRadiusApproach
engineRadius NoteFallback fallbackPoly interaction cylinder terminal console lift**

**Reported (2026-08-25):** on Draklor Laboratory: 67th Floor the interactable `"C.D.B."` answers
`"No path"`, *"and as you can see from the log, there clearly is a path to it"*. Reproduced from
across the floor and from directly beside it.

**The log named it in three lines:**

```
request: interaction band=[-1e9,1e9] reach=0.50
ends: start=1790 walk=1 | goal=-1 walk=0
frontier: goal unreachable (off-mesh-nothing-in-reach); ... ending at poly 1775 (107.98,0.00,11.69), 0.7m short
```

**Root cause, two facts stacked.**

1. **The target's point is OFF the navmesh** (`goal=-1` on every attempt) — a console against a wall.
   That case is already handled: `PathSearch::NoteFallback` records the first polygon A* pops that the
   party can stand on and interact from, and the search finishes there.
2. **The reach it was given was 0.50 m and the nearest walkable point is 0.69 m away** — refused by
   0.19 m. With no fallback there is no goal at all, so the plan came back `Frontier`, and S96's rule
   ("partial routes are not spoken") turned that into `"No path"` — the same answer from 0.7 m and from
   48 m, which is exactly what a target with no reachable goal looks like at every distance.

**Where 0.50 came from — this is the part worth remembering.** `InteractTarget::ReadReachFor` runs the
**class-3** ellipse arithmetic on every target regardless of class. `"C.D.B."` is class 1 (the same
`kind=4 flags=0x2134` shape as the Draklor lift terminals), and the class-1 scorer has **no horizontal
radius in it at all** — see `GameArchitecture.md`, "The class-1 scorer has NO horizontal reach". So
0.50 was the player's own body radius plus three reads off a layout that node does not use. `ReadBandFor`,
the function immediately above it in the same file, has branched on class since S76.

**Fixed:** `Reach::engineRadius` (false for class 1, ellipse fields left at zero rather than filled with
the wrong layout); `nav_commands.cpp` supplies `kNoRadiusApproach = 3.0f` in that case — a sanity bound,
not a discriminator, because A* pops in distance order. A `route reach: <m> source=<class3-radiusMin |
class1-no-engine-radius | transition-arrive | none>` line goes in before every request so a bad route can
be attributed rather than guessed at. **Play-confirmed 2026-08-25.**

**It cannot break a route that works today, and the reason is structural, not empirical:** `IsGoal` is
tested and breaks BEFORE `NoteFallback` in the A* loop; the fallback is consumed only under
`reached == kNoPoly`; and the early break on a found fallback is guarded by `goalOffMesh`. The reach only
ever fires where the old build returned nothing.

### CORRECTED — the `"Direct Lift"` refusal was on 67F, once, from one standing spot

**This was first written up here as *"Direct Lift on 66F still answers No path"*, an open defect, and
carried into the session log, the memory index and the commit message. Both halves were wrong.** The
tester caught it: *"direct lift on 66 is not no path, unsure where you got that. I was able to path to
it just fine."*

**Wrong floor.** The refusal is `drain seq=10` at 04:06:18, which falls between the `mapId=1019`
announce at 04:05:39 and the next map change at 04:08:42 — the **67th Floor**. Every route taken on 66F
that session (`seq=1`–`6`, all to North Lift Terminal) came back `plan=Route`.

**Wrong scope.** It happened **exactly once**, from `(80.41,0.00,51.63)` — the same standing spot that
produced C.D.B.'s `seq=9` and `seq=11`. Three of the log's four Frontier results came from that one
position. **One refusal from one position is not a property of the target** (L-01), and the tester
routes to it without trouble.

**What survives, at its real scope:** from that spot the search gave up on an **on-mesh** goal whose own
oracle line said the mesh connects it to the start — `goal poly 588 is IN the start poly 601's adjacency
component (1953 polys) -- the mesh connects these two, so this is the SEARCH giving up, not an
unreachable goal` — after `attempts=4 banned=2` with validation never passing. That is **one observation
of the breach / repair-ladder path giving up**, not a reproducible defect, and it is untouched by S164.
Anyone picking it up needs a repro first, and the standing position is the variable to vary.

## SOLVED — the interactable the mod could see but could never place (Session 166, 2026-08-26)

## ⇒ BEFORE CHASING "THE MOD CANNOT SEE THIS INTERACTABLE" — THE CHAIN OF EVIDENCE

**Read this before writing a line of code. It exists because six sessions (S166–S171) were spent
forcing an object to appear that the game had not placed, and the work regressed shipped behaviour.**

### Step 1 — IS THE GAME OFFERING IT AT ALL?

**An object the mod cannot see may simply not be there yet.** Field signs, dig spots, quest props and
most script-placed interactables are created by the map script at a specific story/quest step, and
until that step the object exists as an inert shell: same enable bit, same `(0,0,0)`, same event ids
as a live one. **Nothing on the object distinguishes "placed" from "not placed."**

- **If the examine prompt does not appear when the player is standing there, the mod has nothing to
  surface. STOP — there is no defect.**
- The Westersand `Dynast-Cactoid` case: placed only while quest `0x35` reads step `0x28`; the winner's
  own `talk` sets it to `0x32` and removes them all.

### Step 2 — ASK THE PLAYER WHAT QUEST STEP THEY ARE ON (`Lessons.md` L-77)

**One sentence, and it is free.** Do NOT build a quest-flag reader, and do NOT add a runtime probe to
discover it — that is a map-specific gate that does not belong in the mod, and the person playing the
save already knows the answer. This is not the "never ask the user to find something in the world"
rule inverted: reading a step from their own journal costs them a sentence; locating an unseen object
is the thing the mod exists to do for them.

### Step 3 — ONLY NOW ask why the mod cannot see something the game IS offering

And if the answer requires admitting a new population of objects into the entity list, go to the
guard below before writing it.

---

## ⇒ ADMITTING A NEW POPULATION INTO THE ENTITY LIST — THE MANDATORY GUARD

Anything that makes the scan keep objects it used to drop **must** be checked against these, because
they are shared, fragile and already have recorded failures:

| what it collides with | why it is fragile |
|---|---|
| **`TagDoorwaysAndDropSignTwins`** — the `doorway` tag, and therefore the whole Door/Shop split | **a single 2.5 m NEAREST-WINS proximity test** against the `+0x70` table. One extra candidate inside 2.5 m steals a record from a real portal. **Session 92 caught it wrong in BOTH directions on Rabanastre — the gate crystal a false positive, and "South Gate" and "Lowtown", the map's two actual portals, false negatives.** |
| **`ApplyFallbackLabels`** | anything left unnamed takes a generic word. A stolen doorway tag turns a real exit into "Sign 1" / "Sign 2". |
| **the origin drop** | objects at `(0,0,0)` are dropped for a reason. Keeping them admits unspawned shells as phantom NPCs with no interaction component. |

**THE HARD RULE: an exit whose destination resolves must NEVER fall back to a generic word.** The
`Sign` fallback is for the one authorised case — the North End sign the game itself renders as "???"
— never for a portal that has a name. **If a labelled exit starts reading "Sign N", a new candidate
has stolen its `+0x70` record; look there first.**

**And do not settle for writing the risk down.** S166 stated the doorway collision precisely in its
own Open section and shipped anyway; the tester hit that exact failure on the South Gate six sessions
later (`Lessons.md` L-78).

---

> ## ⛔ THE WHOLE CACTUS LINE WAS REVERTED — Session 172, 2026-08-27
>
> **There was no defect. The tester was not on the correct quest step.** These field signs exist only
> while quest `0x35` reads step `0x28`, so the cactus genuinely had no interact component; the mod was
> already pulling interact prompts correctly. Everything from S166 to S171 chased a symptom whose
> cause was save state.
>
> **It also regressed shipped behaviour**, which is why it is out rather than parked:
> phantom NPCs in Rabanastre and elsewhere (the origin-drop change admitted a population the scan had
> always correctly filtered), and labelled exits relabelled "Sign 1"/"Sign 2" — more objects taking
> doorway tags, which is **precisely the risk S166 wrote into its own Open section and shipped
> anyway**: *"a script-placed sign standing near a doorway record could take a tag a real door would
> have had."* A risk you can state that precisely is not covered by writing it down.
>
> `src/` is byte-identical to V0.6.5 `9e9a00f`. `script_place.{h,cpp}`, the origin-drop branch,
> `FindStandablePolyAt`, `ResolvePlacementsToGround` and the S165 diagnostics are all gone; the 1080-
> line diff is saved at `<scratchpad>/cactus_line_S165_S170.patch`. **134 script-placed field signs
> game-wide are invisible again — an accepted cost.** If revisited, the first question is the
> doorway-tag collision, not the placement parse.
>
> What follows is kept as the record of the investigation. **None of it describes the mod as it is.**

**The report:** Dalmasca Westersand carries `Dynast-Cactoid` cacti you examine to dig for a buried
item. The mod's scan ADMITTED them — they carry a bound ACTION script, `act=2` — and then dropped
every one of them, on every map, silently until Session 165 added the `unspawned` line. Nothing in
the Interactables list, nothing to route to, and the user cannot walk over and check by hand.

**Session 165 ruled out, each against a control that would have shown the answer had it been there:**
the object's map-data record, the descriptor at `*(obj+0x40)`, all 64 floats of its transform node,
and literal `(x,y,z)` triples in the map files. The position is in none of them, and its reading —
"that rules out proximity streaming and points at a **state gate**" — was right.

**The cause: the object never has a position, and never will.** The MAP SCRIPT places it, by calling
the `setrect` native with literal coordinates out of the script's own float-constant pool at
`blob+0x24`. `setrect` sets the script's interaction rect; nothing writes the scene object's
transform, so reading the object gives (0,0,0) forever.

**MEASURED, not argued.** `x64\logs\FFXII-Screen-Reader-2026-08-25_15-30-55.log`: `[0:10]` and
`[0:11]` on map 347 sit at (0,0,0) continuously from 13:09:38.9 — before the map-347 announce — to
13:09:49.4, with the player standing at (219.14, 48.92, 370.26). Eight seconds in, the script long
since run, and the transform node is all zeros bar the cone (6.28) and band (-1.00/0.50). **That kills
"it has not spawned yet" for these two objects**, and it means the Session 165 spawn watch could
never have fired on them.

Reading it needed the offline disassembler to work first; `FFXII-Decompile\notes\ebp2_disasm_fix.md`
is the account of what was wrong with it.

### The fix

`navigation/script_place.cpp`. Before dropping an origin object the engine is offering an interaction
on, ask the script where it is: walk the routine table, decode each routine (exactly `record+0x0c`
instructions — no guessing), and take `setrect`'s (x, y, z). The object is joined to its routine by
**pointer identity** — a scene object's `+0x48` IS its routine's entry table inside the blob — so no
authoring order and no slot arithmetic is involved. Its LABEL comes from the same routine's
`fieldsignmes`, decoded from the container's message table with the mod's own codec.

Positions recovered (all four cross-checked against the maps' own arrival tables for plausibility):

| map | slot | routine | position | radius | caption |
|---|---|---|---|---|---|
| 347 Shimmering Horizons | 10 | `サボテン_ハズレ２` | (401.25, 72, 381.80) | 6.5 | Dynast-Cactoid |
| 347 | 11 | `サボテン_ハズレ３` | (411.50, 76, 412.25) | 6.5 | Dynast-Cactoid |
| 349 Windtrace Dunes | 9 | `サボテン_アタリ_砂塵` (winner) | (176.80, 48, 305.30) | 6.5 | Dynast-Cactoid |
| 349 | 10 | `サボテン_ハズレ_砂塵` | (191.70, 50, 274.60) | 6.5 | Dynast-Cactoid |

**Not a special case:** 290 routines across the 769 map scripts place themselves with a literal
`setrect`, and 134 are examinable field signs — "Bottle of Spirits", "Notice Board", "Quiet Shrine",
"Batahn's Technicks", "Pilika's Diary", "Suspicious-looking Wall". Every one was invisible for this
reason.

### ⚠ OPEN — a script placement can be GATED, and nothing on the object says so

The placement runs only if the map's `常駐監督` routine lets it. On both cactus maps, identically:

```
if (v0 >= 1540 && (0x00350000 | getquestscenarioflag(0x35)) == 0x00350028)
    REQEW each cactus's ＦＳ配置 entry
```

and the winner's `talk` ends with `setquestscenarioflag(0x35, 0x32)` plus a `REQ` of the other
cactus's `ＦＳ終了`, which removes both signs. **The mod does not read that gate.** Since the object
looks identical either way — same enable bit, same (0,0,0), same `act=2` — a placement whose routine
never ran will still be listed, and walking there finds nothing.

Deliberately shipped anyway: before this, the object was never listed under ANY game state, so a
gated-off false lead replaces a permanent blind spot rather than a working feature. **What would
close it:** find where `reqenable` / `showfieldsign` land at runtime, and require that evidence
before using the placement. Do NOT close it by reading quest flag `0x35` — that number is one map
pair's gate, not a rule, and hard-coding it is exactly the map-specific keying this reader forbids.

### PLAYED 2026-08-26 — listed and named ✅, position undone by the per-command refresh ❌

First play on map 347. Two of the three questions above came back yes immediately: **both cacti are
listed, and both speak as "Dynast-Cactoid"** — the first time either has ever appeared. The third
came back no, and the same log says why.

**The symptom:** both cacti announced `Dynast-Cactoid N. South, 574 steps (below)`, and the route key
answered `No path`.

**The arithmetic, which is what identifies it — player at (219.14, 48.92, 370.26):**

| observation | what it means |
|---|---|
| 574 steps × `g_unitsPerStep` 0.75 = **430.2 m** | √(219.14² + 370.26²) = 430.2 — the distance to the **world origin** |
| **both** cacti said 574 | they are 32 m apart; one number for two objects is one shared position |
| "(below)" | the cacti are at y = 72/76, i.e. 23 m **above** the player |
| `tgt=(0.0,0.0)  goalPoly=-1` | the planner's own line. The goal was the origin, off the navmesh |
| `nearDist=418.7m`, `expands=2951` | A* flooded the map toward a goal it could never reach |

**The cause — a second writer of the field.** `EntityScan` resolved the placement correctly; the
scan-time diagnostic in the SAME log proves it (`sign g0[5] ... nearest "Dynast-Cactoid" 39.20m`,
which is exactly right for a sign at (406.94,·,343.02) and a cactus at (401.25,·,381.80)).
`EntityList::RefreshPositionsLocked` then re-reads `sceneObj+0xB8` before every command and
overwrites `e.pos`. **For these objects that read does not fail — it SUCCEEDS and returns (0,0,0)**,
which is the whole reason the script had to place them. The resolved position survived less than a
frame, and every consumer downstream (bearing, steps, route goal, beacon) got the origin.

**The fix:** a script-placed entity is marked `fixed`, whose contract is already exactly this —
"fixed world pos, do not refresh via +0xB8". The scene pointer stays set, so the stale-entity pruner,
`IsInteractionAvailable` and the doorway/sign filter all keep working on the object; only the
position refresh is skipped. `fixed` previously implied "no scene node at all" (exits, map-jumps,
item drops); it now covers two populations and the field comment in `entity_scan.h` says so.

That the diagnostic was right and the announcement was wrong **in the same log** is `Lessons.md`
**L-74**: an instrument at the point of computation certifies the arithmetic and cannot see a
downstream overwrite.

### PLAYED 2026-08-27 — routed 200 m, then broke in the last 19 m ❌ FIXED (unplayed)

The position fix worked: `Northwest, 243 steps (above)` and a real 23-leg route, which the player
followed from (219.14, 48.92, 370.26) to about (383, 82, 383) — the far side of the Shimmering Sands.
Then it fell apart. The mod kept saying "North" into ground the player could not walk, replanned 15
times, recorded 3 blocked spots, and never arrived.

**The tell is the search cost, not the verdict.** Same log, same map, same session:

| target | player distance | `expands` |
|---|---|---|
| ordinary targets (seq 3,4,6,7,8,9) | 2–11 steps | **1, 5, 3, 2, 12, 2** |
| Dynast-Cactoid, from 240 m | 240 m | 2940 |
| Dynast-Cactoid, from **19 m** | 19 m | **2684** |
| Dynast-Cactoid, from 14 m | 14 m | 2679 |

**The cost never falls as the player approaches.** A goal 19 m away that costs a full-mesh flood is
not 19 m away in the graph.

**The cause, stated by the router's own lines:**

```
ends:  start=2474 eff=0x00240000 walk=1 | goal=2389 eff=0x07841000 walk=1 | class=0
cost:  corridor pays terrain=6000 (terrain > 0 => crosses ground the party's class may not stand on)
costed: ... refused eff-flags: 0x07841000 x1001  0x0F841000 x1090 ...
REVERSAL: leg 3 -> 4 turns 135 deg -- the polyline doubles back; the route geometry is wrong
```

The goal poly's effective flags are `0x07841000` — **bit 23 set**, the leader's terrain refusal — and
that exact word is the most-refused flag in the search. The start poly the player was standing on
reads `0x00240000`, bit 23 clear. **The goal was on ground the party may not stand on.**

Nothing errored, and that is the point. **Terrain refusal is a PRICE, never a graph cut** — correct
and hard-won (S96 proved cutting over-refuses: 399/690 prims on map 311, and it cost an exit; it has
been reverted twice). So A* did not report an unreachable goal. It breached its way there, paid
`terrain=6000`, produced a corridor that doubled back 135°, and the mod spoke it. See `Lessons.md`
**L-75**.

**Why the goal was there:** `setrect` gives the interaction volume's REFERENCE height, not a ground
height — a fact this project established itself in S166 and then fed straight to the router.
`NavMesh::FindPolyAt` takes the containing floor NEAREST the Y it is handed, with no terrain test and
no vertical tolerance, so y=72 selected a poly ~10 m under the terrain the player was standing on.

**The fix — at the goal, not in the router.** The pricing model is right; the input was wrong.

| | |
|---|---|
| `nav_mesh.{h,cpp}` | new `FindStandablePolyAt(x, yHint, z)`: the floor at (x,z) the **leader can stand on**, nearest `yHint`. `FindPolyAt` and it now share ONE cell walk (`FindPolyAtImpl`) differing only by a flag, so they cannot drift into two notions of "the floor here". Terrain refusal is decided from the effective flags' bit 23 rather than by calling the engine, because the scan is memory-only by policy — the same reason `IsInteractionAvailable` replicates `FUN_002675c0` instead of calling it. |
| `entity_scan.cpp` | `ResolvePlacementsToGround` runs once per map over every script placement: (x,z) is authoritative and never moves, only Y moves, and only onto a standable floor at that same (x,z) within 25 m. No such floor → the placement is left exactly as the script wrote it, i.e. the behaviour that shipped before. |

**Non-regression, established rather than assumed:**

- `FindPolyAt` is *statement-identical* to its previous body — 29 executable statements, mechanically
  diffed against `HEAD`, zero differences; with `requireStandable=false` both new guards are no-ops.
- `FindStandablePolyAt` has exactly **one** caller in the whole tree, and that caller only ever runs
  on script placements — a population that did not exist in the entity list before S166.
- The new query can never refuse a route. It only *prefers* a standable floor over a refused one at
  the same (x,z); with nothing to prefer it returns `kNoPoly` and the caller changes nothing. **This
  is deliberately not the S96 lever** — that made bit 23 a graph cut in `Walkable` and refused
  ankle-deep water the player walks through. Nothing here is wired into `Walkable`, and no route is
  ever declined on terrain grounds by this change.
- The funnel/stats log formats are untouched, so the project's own working-route-invariance check
  (diffing those lines across logs) still applies.

### PLAYED 2026-08-27 (2nd) — the Y hypothesis was WRONG, and the instrument said so in one line

S168 shipped the Y correction together with the one diagnostic that would say whether its premise
held. It did not. All four cacti, both maps, first play:

```
placement ground: routine 10 at (401.25,381.80) script y=72.00 KEPT -- NO floor the leader can stand on
placement ground: routine 11 at (411.50,412.25) ... KEPT -- NO floor ...
placement ground: routine  9 at (176.80,305.30) ... KEPT -- NO floor ...      (map 349)
placement ground: routine 10 at (191.70,274.60) ... KEPT -- NO floor ...      (map 349)
```

**There is no standable floor at a cactus's own (x,z) at ANY height.** The Y was never the problem.

**What S168 got right, and it still stands:** the goal poly is terrain-refused, and because refusal is
a PRICE and never a cut, nothing errors — A* breaches to its own goal and the mod speaks the result.
That is `Lessons.md` **L-75** and it is confirmed twice over, on two maps, with the *same flag word*.

**What S168 got wrong:** the cause of the refusal. It was not a Y that selected an under-terrain poly.
**The object occupies its own coordinates.** `setrect` gives an interaction VOLUME — a centre and a
radius — and the centre is where the cactus IS, which is precisely the one place the party cannot
stand.

**Map 349 shows the endgame at three metres**, player at (177.65, 47.39, 302.62), goal (176.80, 48.00,
305.30):

```
ends: start=133 eff=0x00100000 walk=1 | goal=45 eff=0x07841000 walk=1
mesh: polys=2 portals=1 corners=2          <- start and goal are ADJACENT
firstLeg=(176.8,305.3)                     <- the one leg drives straight at the centre
validate: ... volHit=1 volWalked=1 ... OK
corridor march: CLEAR over 2 hop(s)
```

Validation passed it, the march called it clear, and the character hit the cactus and stopped dead
2.8 m short while the mod repeated "North 4." forever. Map 347's goal poly carries the identical
`0x07841000`.

### Where the interact component actually is — asked and answered

The obvious next question is whether the cactus has an engine-side interaction object with a real
position to route to, instead of a geometric guess. **It does not**, and three independent checks say
so:

- **The engine's field-sign `+0x70` table is NOT it.** 18 records on map 347, 12 on map 349, all
  `destIdx` 1-3 area/doorway signs; the nearest cactus to any of them is **33-88 m** away. The mod
  already dumps this table on `'` and every record reads `TOO FAR, unclaimed`.
- **The scene object carries no volume.** S165 checked its map-data record, the `*(obj+0x40)`
  descriptor and all 64 floats of the transform node. The node's class-3 ellipse semi-axes read 0.01
  — i.e. unused — and its only live interaction fields are the cone (`6.28`, a full circle, so facing
  is unconstrained) and the band (`-1.00 / 0.50`).
- **A class-1 target has no engine reach to route to.** `interact_target.h` records this from the
  decompile: `FUN_0025be50` gates on the vertical band, the mode bit `node+0x60 >> mode`, the facing
  cone, and then a bare squared distance kept only to MINIMISE — **nearest wins; nothing is rejected
  for being far away.** That is why the mod logs `route reach: 3.00m source=class1-no-engine-radius`.

**So the interact component IS the script's rect** — `setrect`'s centre plus `setwh`'s extent, which
the mod has read since S166 (`r=6.50 circle` on all four cacti). There is no second object to find.
Routing to the interact component therefore means routing to *walkable ground inside that volume*,
which is what the fix below does.

### The fix — route to the interact volume, not to the object's own point

`setwh` already gives the mod the radius and it already reads it: `r=6.50 circle` for all four cacti,
a field that until now was marked *"diagnostics only — no caller consumes it."* Now it does.
`ResolvePlacementsToGround` resolves each placement once per map:

1. **centre standable** → take its floor height (a rect's Y is a reference height, not ground);
2. **centre not standable** → take the **closest walkable ground inside the interaction volume**
   (16 directions, rings one player step apart, first hit wins). The goal becomes ground the party can
   actually stand on, so A* routes **around** the obstacle instead of pricing its way through, and the
   player lands as near the object as the walkmap allows. Hugging matters because `setwh`'s args 3-4
   are still unidentified, so whether 6.5 is a radius or a full width is open — the nearest hit is
   inside the volume on either reading;
3. **nothing standable in range** → leave the placement exactly as the script wrote it — the
   behaviour that shipped before any of this existed.

The ring search is up to 96 mesh queries per placement, so the result is cached per map and reused
across rescans; the map id is only stamped once the mesh was actually up, which is what makes a scan
that ran too early retry rather than cache a miss.

**Non-regression, established rather than asserted** (the user's standing requirement — *"causing
regression is unacceptable"*, restated as *"no regressions in the rest of the pathfinder"*):

- **Not one router file is modified.** `path_search`, `path_planner`, `path_funnel`, `path_corridor`,
  `path_validate`, `path_march`, `path_repair`, `path_surface_goal`, `nav_reach`, `nav_footprint`,
  `map_query` — all clean. The routing algorithm, its costs, its funnel and its validation are
  byte-identical.
- **`FindPolyAt` is statement-identical to its old body** — 29 executable statements, mechanically
  diffed against `HEAD`, zero differences.
- **`FindStandablePolyAt` has exactly two callers**, both inside `ResolvePlacementsToGround`, which
  runs only on script placements — a population that did not exist in the entity list before S166.
- **The new query can never refuse a route.** It is not wired into `Walkable` and cannot decline a
  crossing; it only chooses *where a placement's own goal point sits*. **Deliberately not the S96
  lever**, which made bit 23 a graph cut and refused ankle-deep water the player walks through.
- Only the position of a script-placed field sign changes, and only ever from a point the party
  cannot stand on to one it can.

### ⚠ OPEN

- **`placement ground:` is `Log::Write("NAV-DIAG", …)` — LOG ONLY, never spoken.** It is a
  developer line and no player hears it; there is no speech path anywhere in the resolver.
- **Unplayed.** The `placement ground:` line now names the branch outright: `STANDS`, `is NOT
  standable … goal moved to (x,y,z), N.NNm out`, or `KEPT — no standable ground anywhere within`.
  The first two are the fix working; the third means the disc really is solid and the answer is a
  wider search or an honest "No path".
- **The stand point is chosen nearest the CENTRE, not nearest the player** — deterministic and stable
  across rescans, which the announced bearing needs. If it lands on the far side of the obstacle the
  route simply goes around, which is correct, just longer.
- Whether `setwh`'s 6.5 is a radius or a diameter is still unestablished (S166 left args 3-4 of
  `setwh` unidentified). It is used here only as a SEARCH BOUND, and the search takes the nearest hit,
  so a factor of two would change how far out it is willing to look and nothing else.

---

## SOLVED 2026-08-29 (S174) — Config "Battle Speed" spoke its label and no value

**Symptom (user, from their own log):** on the game's Config screen, `Battle Speed` announced the row
name and nothing else — silent on highlight *and* on a left/right change. Every other row on the same
screen ("Battle Mode: Wait", "Controls", "Graphics Settings") read correctly.

**The log had already named the cause**, one line, at the moment the row was highlighted:

```
[READER] config row REFUSED (no per-option labels -- not an enum): row=...2CE24C40
         class=0x23D6B0 kind=2 sel=5 bytes=[70 61 72 74 79 74 6F 70 5F 34 5F 63]
```

- `class=0x23D6B0` → `FUN_0023d6b0` → `ValueRow::EnumD6B0` (`kind=2` ✓).
- The bytes are ASCII **`partytop_4_c`** — a sprite name, not codec text.

The row is drawn as **gauge blocks**, so walking to "the selected option's label" lands on a texture
id that is the SAME for every option. S148's `HasPerOptionLabels` guard caught that correctly (it
refuses a row whose option 0 and option 1 decode identically) and stopped the mojibake it was written
to stop — `Battle Speed: æÏèêïêäæÍUÍÒ`. It then had nothing else to offer, so the row went silent.

**What made it a five-minute fix instead of an RE session: `sel=5` was in the refusal line.**
`SelectedIndex` locates the selected child by its flag bit and had been right the whole time. The
value was never unreadable — it was unspoken. `ConfigReader::GaugeReadout` now counts the blocks
rather than decoding them: `sel + 1` of the option count at `row+0xD2`, joined with the phrasebook's
existing `Phrase::Id::OfJoiner`, so nothing new was invented to say it.

**Scope of the fallback:** `EnumD6B0` / `EnumDB40` only. Those two carry the count at `OFF_ROW_CCOUNT`
(`0xD2`), which is what makes "of 6" true rather than guessed. `EnumE770` has no count field and keeps
refusing — a bare index with no range is a number the player cannot act on.

**The diagnostic stayed**, reworded from `REFUSED` to name which branch it took, with the same 8-row
dedup. A row that reaches it and is *not* a gauge is the next defect, and this log line is how it will
be found. Only one row took the path in the reported log.

**⚠ Carried, stated in the code:** on the value-change path `RowValueAtNewValue` assumes `nv` is the
display index. That assumption is already load-bearing for the working enum rows of this same class,
but it is unverified for a gauge. If a change ever speaks a number the highlight then contradicts,
drop that line and let the value ride the next paint.

**Not the same defect as S163's Draklor lift**, though it rhymes (a field whose meaning depends on
state). That one is the field-message/choice widget family (`+0xB0` mode byte, `+0xA2` digit width);
this is the config row-widget family (`FUN_0023xxxx`), which has no mode byte and dispatches purely
on the class pointer at `row+0`. Do not carry offsets between them.

## The private airship destination map was silent because it cannot send a focus index (Session 176)

**Symptom.** Boarding the Strahl, the destination screen read nothing. The route to the desk, the desk
conversation and the board / leave Yes-No prompts all worked.

**NOT a regression — the surface had never been built.** `plan.md:251` `- [ ] World map / fast travel`
under `## v2 (deferred)`; `Strahl` and `airship` appeared zero times in the repo. Establish this
before treating a silent surface as a defect: **"the mod stopped reading X" and "the mod never read
X" need different work, and only one of them is a bug.**

**Diagnosis.** `FUN_005528c0` (obj[0] RVA `0x4328C0`) **never calls `FUN_00247510`** and has no
`case 0xc`. The `0x8000` focus dispatch every list reader hangs on is never sent — because the
destinations are a **node graph** with screen coordinates walked by direction, so **there is no row
index for a focus message to carry.** No guard added in `menu_reader.cpp` could have fixed this.

**Fix.** `src\ui\airship_reader.{h,cpp}` — hooks the pane's own handler and reads `pane+0x9F40`, the
hovered node, with a change-check guarding that per-frame tick. Layout in `GameArchitecture.md`.

### Tried & Failed / recorded so it is not repeated

- **The `unclaimed pane` log line names a suspect in its own text, and it was wrong here.** It prints
  "(candidate: the on-screen CONTROLS panel)" — a fixed string in `menu_reader.cpp:767`, not a
  measurement. **A diagnostic that embeds a guess will have that guess quoted back as a finding.**
  Settled by a corpus sweep instead: the everyday unclaimed panes appear in 20–21 of 21 logs, while
  `0x4328C0` appears in **one log, once**, timed to the boarding. **Rarity plus timing is the test,
  not the sentence the log prints.**
- **`TextCapture::DumpRingToLog` had ZERO callers since Session 112.** It was written to diagnose
  exactly this class of problem and had never once run. **A diagnostic nothing invokes is not
  insurance, it is dead code that looks like insurance** — and its first run returned only stale
  dialogue, because it fires at pane construction before the surface's own text is drawn.
- **Do not copy a decompiled signature into a detour.** Ghidra infers parameters from body usage, not
  the ABI: in this call graph the base handler decompiles as `FUN_005c5230(void)` and is called with
  two arguments one line later. The detour takes **four** and forwards four. Over-declaring is safe
  on x64 (extras ride in R8/R9); under-declaring is the S129 shop crash.

### Open

- **✅ PLAY-CONFIRMED 2026-08-30 — SOLVED.** `CENSUS nodes=34 named=34 distinct=yes`, 21 destinations
  spoken and reaching `SPEAK-OUT`, zero `distinct=NO`, zero hook failures. `node+0x48` is the name.
  **The runtime distinctness gate is what made it safe to ship a field identified from one record,
  and it cost nothing when the field was right** — cheaper than the second play pass it replaced.
- **⚠ The pane lists the WHOLE WORLD MAP, not flyable ports** — Garamsythe Waterway, Barheim Passage,
  Henne Mines and Lhusu Mines are all in it. The reader speaks whatever marker the cursor lands on,
  which is right for a map cursor; it does not claim you can fly there. If a player asks why an
  unreachable place is announced, that is this, and the answer is in the unmodelled flags.
- `node+0x54` / `+0x130` flag semantics unmodelled and unused; the cursor stops on nodes the render
  flag calls hidden, so they are not a selectability filter.

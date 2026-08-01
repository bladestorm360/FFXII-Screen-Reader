# File-size audit & centralization ledger

`CLAUDE.md` has always named this file as the home for the file-size audit and centralization debt.
It did not exist until Session 51; the debt it was meant to track accumulated unrecorded, which is
how three `.cpp` files passed the 500-line rule and four copies of the actor-pool offsets appeared.

Last audited: **2026-07-21 (Session 51)**.

## The rules being tracked

From `CLAUDE.md`:
- **No `.cpp` over 500 lines.** At 400, plan a split.
- **Headers under 150 lines.**
- **Always centralize** — shared helpers, never duplicated logic.

## Current state

> **CORRECTED (Session 93).** This section said "**Every `.cpp` is under 500**" and listed four files in
> the 400-500 band. That was false when written and had drifted further since: `battle_state.cpp` was 584
> and absent from the table entirely, `path_search.cpp` had reached 667, and two UI readers were over.
> **An audit that reports a rule as satisfied is worse than no audit** -- it is the thing a later session
> greps to find out whether it may add lines, and this one licensed 700-line files. The numbers below are
> measured, not remembered; re-measure them rather than editing them by hand.

**Four `.cpp` files are OVER the 500 cap**, all pre-existing and none of them touched by the Session 93
pathfinder work:

| File | Lines | Note |
|---|---|---|
| `ui/ingame_menu_reader.cpp` | 648 | Was 739; the character chooser moved out to `char_select_reader.cpp` in S93. Still over -- the clean remaining seam is the battle-command half (`RVA_BCMD_*`), which would land both halves near 320. S94 added only a comment. |
| `navigation/entity_scan.cpp` | 583 | Scan + classify + the handle-table walk |
| `ui/menu_reader.cpp` | 570 | Hooks + speech decisions; grew past its S51 split |
| `navigation/entity_postscan.cpp` | 546 | Sign/doorway tagging + the twin filter |

Re-measured **2026-07-30 (Session 94)**. The same four files are over, and none of them grew materially:
`menu_reader.cpp` took the two-line gambit dispatch branch and `ingame_menu_reader.cpp` a corrected
comment, both of which had to land in those files -- the dispatch is the one choke point for `0x8000`,
and the mislabel was in the row-chain table. The gambit reader itself went into a **new** file
(`ui/gambit_reader.cpp`, 190) rather than into either of them, which is what kept this table from
drifting again.

**Paid off in Session 93** (each split on a seam the file already had, no logic change):

| File | Before | After | Split into |
|---|---|---|---|
| `navigation/path_search.cpp` | 667 | 499 | `path_funnel.{h,cpp}` (string-pull, corner inset), `path_validate.{h,cpp}` (leg validation) |
| `battle/battle_state.cpp` | 584 | 468 | `battle_state_diag.cpp`, `battle_state_names.cpp`, `battle_state_internal.h` |
| `navigation/map_query.cpp` | 499 | 402 | `map_seams.{h,cpp}` (the map-jump sweep + its cache) |

Revisit the four over-cap files when one of them next needs new behaviour -- splitting for its own sake
has broken working readers here before (S31's chooser consolidation silenced board navigation).

**Session 95** split `path_search.cpp` again, and this one was forced rather than tidy: the file sat at
499 of 500 and the frontier fix needed real code. `path_corridor.{h,cpp}` (83 + 107) took corridor
reconstruction from A*'s parent links plus the frontier route built on one -- a genuine seam, since the
frontier's whole defect was that it reused the corridor built for a *different* end poly. `path_search.cpp`
came out at **495** with more behaviour than it went in with (the banked proven prefix, the two-candidate
frontier choice). `path_validate.cpp` 71 -> 74, `path_planner.cpp` 453 -> 488 (the beacon's objective
snapshot); `path_planner.cpp` is the one to watch -- the next feature it takes needs a split first.

### Header exceptions (deliberate, not oversight)

| File | Lines | Why it stays over 150 |
|---|---|---|
| `navigation/nav_rva.h` | 572 | ~80% provenance comments. Was recorded as 279; it is the registry every RE finding lands in, so it grows with the project. |
| `navigation/map_rva.h` | 273 | same, split out of nav_rva.h in Session 51 (recorded as 221) |
| `navigation/entity_scan.h` | 203 | struct layouts + their derivation (S94 added `Entity::factionVerdict` and the reason it has to exist) |
| `navigation/player_state.h` | 155 | the nav-safe gate's contract, corrected in S93 |
| `battle/battle_state.h` | 160 | was exactly 150 after the S93 diag/name split; S94's `CharacterName` and the thread-safety note that explains why it exists beside `DefName` push it over. The next addition here should take the split. |
| `navigation/map_query.h` | 146 | under, listed to be watched |
| `navigation/nav_mesh.h` | 145 | under, listed to be watched |

Both are RVA-documentation headers: for each address they record which reading was **STRUCK**, what
the evidence was, and what replaced it. That commentary is precisely what stopped past sessions
re-deriving and re-shipping the same wrong addresses (the `FUN_00353490` / `+0x70` / `mapData+0x54`
cycle). **Do not trim comments to hit 150** — the line count is not the goal, the not-repeating-a-
three-session-mistake is. If the limit is enforced mechanically some day, exempt `*_rva.h`.

## Centralization ledger — what is single-source now

| Concern | Owner | Was |
|---|---|---|
| SEH-guarded memory reads | `core/mem_read.h` | + private copies in `title_reader.cpp`, `menu_observer.cpp` |
| Wide→UTF-8 + log lines | `Log::WriteW` / `Log::ToUtf8` (`core/logger.h`) | `LogLine` written out 3× verbatim; 6 more open-coded `WideCharToMultiByte` |
| Actor pool / BtlChr / scene-kind offsets | `core/phyre_types.h` | 4 copies: `nav_rva.h`, `battle_target_reader.cpp`, `battle_state.cpp`, `party_status.cpp`, `combat_events.cpp` |
| BtlWork pointer, master-data reloc base | `core/phyre_types.h` | 3 and 2 names respectively (see below) |
| Menu surface identity / focused pane / row class | `ui/menu_state.h` | private to `menu_reader.cpp`'s anonymous namespace |
| Config row values | `ui/config_reader.h` | inline in `menu_reader.cpp` |
| Field liveness | `navigation/player_state.h` | (already single-source) |
| Battle state, names, factions | `battle/battle_state.h` | (already single-source) |
| Field-object scanning | `navigation/entity_scan.h` | inline in `entity_list.cpp` |
| Map names / map exits / walkmap geometry | `map_names.h` / `map_exits.h` / `map_query.h` | one 747-line `map_query.cpp` |

The three state modules are now symmetric: **`PlayerState`** (field) · **`BattleState`** (battle) ·
**`MenuState`** (menu). All three are read-only, SEH-guarded, and call no game function.

## How the duplicates were found

Grepping duplicate constant **names** finds the easy half. The valuable pass was grepping duplicate
**values** — the same RVA under different names, which no name-based check catches:

```sh
grep -rhn "constexpr uint32_t [A-Z_0-9]* *= *0x[0-9A-Fa-f]\{5,\}" --include=*.cpp --include=*.h . -o \
  | sed 's/.*constexpr uint32_t //' \
  | awk -F'= *' '{gsub(/ /,"",$1); v=tolower($2); gsub(/;.*/,"",v); print v, $1}' \
  | sort | awk '{if($1==prev) print "DUP",$1,":",prevname,"/",$2; prev=$1; prevname=$2}'
```

That turned up `0x2D9F190` under **three** names (`RVA_BTLWORK`, `PARTY_MGR_PTR`,
`FIELD_STATE_BLOCK`) — and that global is the one whose pointer-vs-struct confusion made the 4/5/6
party keys silent for two sessions. Re-run this after adding addresses.

## Open debt

1. ~~The `0x2D9F190 + 0x5A7E` contradiction.~~ **RESOLVED** — it is roster list 3; the "field-sign
   category tables" were the party roster misread. Same base/offset/stride/width as
   `BtlChrForSlot`, and `0x5A7E + 9*2 == 0x5A90` makes "table B" the next roster list. Struck in
   `nav_rva.h`, documented on `BTLWORK_PTR` in `core/phyre_types.h`.
2. ~~`ui/menu_observer.cpp` is dormant.~~ **RESOLVED — deleted (Session 51).** Phase-0 scaffolding
   that inferred menu focus from cursor X/Y. Only `Init`/`Shutdown` were ever called;
   `SetFocusChangeCallback`, `LatestSnapshot`, `ReadRegistry` and `RegisterController` had **zero**
   call sites, so it ran a detour on `FUN_00241d40` and dispatched to a callback that never existed.
   Superseded by `menu_reader`'s `FUN_00247510` msg-`0x8000` path, which reads the focus INDEX
   directly instead of inferring it from pixel coordinates. In git if ever needed.
3. **Composite `__try` blocks stay as they are.** `ingame_menu_reader.cpp` (`ReadStatusSlot`,
   `ReadBcmdDraw`) and `map_query.cpp` keep multi-step pointer walks inside a single guard. That is
   a correctness constraint, not duplication — a walk split across guards can fault between them.
   Do not "centralize" these into `MemRead` calls.
4. **`map_query.h` is 157 lines, over the 150 header ceiling (Session 74).** Introduced by the
   map-jump vertex list, `NearestPointOnSurface` and `CachedMapJumpSurfaces`. Phase 1 of the
   pathfinder rebuild moves the floor-span reader out into `nav_voxel`, which takes this back under
   on its own — do not restructure it separately in the meantime.
5. **~1,000 lines of exit/entity diagnostics are now unreachable (Session 74).** Stripping the `'`
   key left `EntityList::LogDiagnostic` / `EntityDiag::DumpLocked`, `ExitDiag::DumpCoverage` (and
   through it `MapScript::DumpCaptureDiag`), `MapExits::DiagScanScriptMapjumps` and
   `PlayerState::ReadMoveFrame` with **zero call sites**. They still compile and still link.
   **Deliberately not deleted:** they are the instrument trail for how the exit mechanism was pinned
   down across Sessions 46-64, and the exit saga cost six sessions of wrong models before that. If
   they are to go, that is a decision to take on its own, not a side effect of a key being re-keyed.
   The cheaper alternative is to re-key them onto a second diagnostic key.

6. **The grid pathfinder is gone (Session 75).** `nav_grid.h/.cpp` deleted; `path_search.cpp` fell from
   ~700 lines to 201 and `path_planner.cpp` from 482 to 322 once the grid-era diagnostics went with
   it (the direct-line step probe, the 33x33 terrain field and the per-leg step profile all measured
   `kMaxStep` / `kStepDiscont`, gates that no longer exist). Routing now costs ONE short raycast per
   expanded edge instead of thousands per route -- the failed Highhall route logged `rays=2985`.
7. **`map_query.h` is 157 lines, over the 150 header ceiling.** Carried from Session 74. The navmesh
   reader went into its own `nav_mesh.h` rather than growing this further; trimming it is still open.

8. **`entity_scan.cpp` split (Session 77).** It passed the 500-line ceiling at 545. The passes that
   run over the FINISHED list -- `ObjectHandle`, `LogObjectDump`, `TagDoorwaysAndDropSignTwins`,
   `ApplyPlayerLabels`, `NumberDuplicateLabels` -- moved to `entity_postscan.cpp` (327 / 251 lines).
   Signatures live in `entity_scan.h` so the two units cannot drift apart.

## Session 81 — file-size debt (logged, not paid)

| file | lines | limit | note |
|---|---|---|---|
| `src\navigation\nav_rva.h` | **541** | 150 (header rule) | Pre-existing 3.5x breach, +15 this session for the scene-category constants. It is the canonical offset registry, so the constants belong here; the file needs its own split (by subsystem: scene object / walkmap / actor pool / map jump). |
| `src\navigation\entity_scan.cpp` | **457** | 500 (hard), 400 (plan a split) | Past the planning mark. Obvious seam: the diagnostic/tally block (`s_*` counters, `LogPartyBodyOnce`, the `inclusion:` line) into `entity_scan_diag.cpp`. Deliberately not done in a bug-fix change. |

`entity_labels.cpp` shrank (273 -> 264) and `entity_labels.h` (75 -> 88, prose rewritten). Everything
else is comfortably inside the limits.

## Session 96 — file-size debt (logged, not paid)

| file | lines | limit | note |
|---|---|---|---|
| `src\navigation\path_search.cpp` | **749** | 500 (hard), 400 (plan a split) | Session 93 split this file at 667 and it has grown again. Obvious seam: the ATTEMPT LOOP body (funnel → validate → repair ladder → re-cost) is self-contained and reads as one unit — it belongs beside `path_corridor` as `path_attempt.cpp`, leaving `Run` as the A* pass plus the outcome block. Deliberately not done inside a regression fix, and after this session that restraint is the point: three global changes in one build is what caused the damage being undone here. |
| `src\navigation\nav_rva.h` | **~555** | 150 (header rule) | Unchanged debt from Session 81, plus prose this session for the struck bit-23 note. Still wants a split by subsystem. |

`path_validate.cpp` (307), `path_funnel.cpp` (283) and `nav_mesh.cpp` (478) are inside the limit;
`nav_mesh.cpp` is close enough to watch.

## Session 97 — file-size debt (PARTLY PAID)

**`path_search.cpp` 749 → 805 → 711**, split on the seam Session 96 named. The repair ladder came out
whole as `path_repair.{h,cpp}` (112 + 76 lines), and it is a real seam rather than a line-count trick:
it answers one question — *"the chord across this corridor did not walk; is there another polyline
through the SAME corridor that does?"* — and needs **none** of `Run`'s search state to answer it. No
A*, no ban list, no centroid cache, no frontier. Its whole input is the corridor, the polyline drawn
across it, and the validator's verdict on that polyline.

The extraction is behaviour-identical by construction: same rungs in the same order, same guards, same
`InsetCorners` call, same log format, and the probe budget still decrements between rungs (a local that
the caller subtracts once). `Run` keeps only what is its business — spending the budget and adopting
the result.

| file | lines | limit | note |
|---|---|---|---|
| `src\navigation\path_search.cpp` | **711** (was 749) | 500 (hard), 400 (plan a split) | Still over, and **the next cut is not a good one yet.** What remains is essentially one 600-line function, and its bulk is the A* pass: the edge loop and its lambdas (`Centroid`, `IsGoal`, `NoteFallback`, the refusal tally) close over a dozen of `Run`'s locals, so lifting them means inventing a context struct — moving code for line count rather than on a seam. Two candidates, both weaker than the one just taken: the FRONTIER + outcome tail (~100 lines, but it needs `PassResult` promoted to a header for 13 inputs) and the refusal DIAGNOSTICS (`refNoPoly`/`refUnwalkable`/`refEdge`/`refBanned`/`refBlocked` + the `costed:` histogram) into a `Refusals` struct with `Note`/`Format` — cohesive and matching the project's `*_diag.cpp` pattern, but only ~45 lines. Take the diagnostics one next time this file is open for a reason. |
| `src\navigation\path_validate.h` | **173** (was 144) | 150 (header rule) | Newly over. All of the growth is the struck-premise note on the volume test and the `volHit`/`volWalked` contract — load-bearing prose, not declarations. Pay it off with `path_search.cpp`: the S96 premise note belongs in `GameArchitecture.md` (where it now also lives) and can be cut to a pointer once the finding has been play-confirmed. |
| `src\navigation\nav_rva.h` | **~555** | 150 (header rule) | Unchanged debt from Session 81. Still wants a split by subsystem. |

`path_validate.cpp` (333), `path_funnel.cpp` (312), `path_funnel.h` (135) and `nav_mesh.cpp` (495) are
inside their limits. **`nav_mesh.cpp` is now 5 lines from the hard cap** — it was "close enough to
watch" last session and is now the next one to trip. Watch it, or take the `BodyFitsAt`/`EdgePassable`/
`EdgeClearSpan` block out to `nav_edges.cpp` at the next opportunity that is not a regression fix.

## Session 116 — `path_search.cpp` breaks 1000 lines, and the named seam is now overdue

The corridor-march change added ~145 lines to `path_search.cpp` and the file is now **1108**. That is
more than twice the hard cap, and the seam Session 96 named is still the right one and is now the
only sensible cut:

> the ATTEMPT LOOP body (funnel → validate → **corridor march** → repair ladder → re-cost) is
> self-contained and reads as one unit — it belongs beside `path_corridor` as `path_attempt.cpp`,
> leaving `Run` as the A* pass plus the outcome block.

| file | lines | limit | note |
|---|---|---|---|
| `src\navigation\path_search.cpp` | **1108** (was 711) | 500 (hard), 400 (plan a split) | The attempt loop is now ~330 lines on its own and has grown a second decision point (which SOURCE names the crossing to price). Take `path_attempt.cpp` at the next opportunity that is **not** a regression fix — the restraint is the same one recorded in S96 and S100, and this change is unconfirmed in play, so it must stay revertable as one commit against one file. |
| `src\navigation\path_corridor.h` | **130** (was 83) | 150 (header rule) | Inside the limit but the growth is all `CorridorMarch`'s reasoning. Once the finding is play-confirmed, the "A\* certifies crossings, never the travel between them" paragraph belongs in `GameArchitecture.md` and this can be cut to a pointer — the same trade `path_validate.h` already owes. |
| `src\navigation\path_corridor.cpp` | 149 | 500 | Fine. |

Everything else is unchanged from Session 100's table; `nav_mesh.cpp` is still 5 lines from the cap
and still wants `nav_edges.cpp`.

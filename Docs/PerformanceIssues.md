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

**Every `.cpp` is under 500.** Four are in the 400-500 "plan a split" band:

| File | Lines | Note |
|---|---|---|
| `navigation/map_exits.cpp` | 451 | Four independent exit sources; splits cleanly by source if it grows |
| `ui/battle_target_reader.cpp` | 431 | Nameplate hook + snapshot hook + the `p` cache |
| `ui/menu_reader.cpp` | 416 | Down from 667; now hooks + speech decisions only |
| `navigation/path_planner.cpp` | 404 | A* + string-pull + the game-thread request drain |

Nothing here is urgent. Revisit when one crosses 450 with new behaviour rather than pre-emptively.

### Header exceptions (deliberate, not oversight)

| File | Lines | Why it stays over 150 |
|---|---|---|
| `navigation/nav_rva.h` | 279 | ~80% provenance comments |
| `navigation/map_rva.h` | 221 | same, split out of nav_rva.h in Session 51 |

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

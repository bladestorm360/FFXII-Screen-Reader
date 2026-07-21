# FFXII-Screen-Reader — Session Log (Sessions 51–current)

Continues `sessions_001_050.md`, which is closed at **Session 50** (release 0.1-shotgun-build).

Entry format is mandatory: `## Session N — YYYY-MM-DD — [track] <title>`, where `N` is a single,
global, monotonically increasing integer shared by all parallel tracks. Never a date-only header,
never a letter suffix — the number is what bounds this file to 50 entries and drives the next split
(at Session 100, rename to `sessions_051_100.md` and start `sessions_101_current.md`).

Every entry carries a **KEYWORDS** line for grep discoverability.

---

## Session 51 — 2026-07-21 — [menus] No-dedup rule; strip all non-per-frame dedup; centralization pass

**KEYWORDS:** dedup deduplication debounce speech silent menu re-entry focus battle-command
status-chooser title-screen MaybeAnnounce centralization phyre_types MenuState ConfigReader
entity_scan entity_diag map_names map_exits map_rva file-split Log::WriteW MemRead aliased-RVA
BTLWORK_PTR PerformanceIssues.md

### Part 1 — the no-dedup rule, and removing every non-per-frame instance

**The bug the user reported:** leaving a menu pane and coming back was SILENT. Cause: `OnFocus`
compared `(owner, index, text)` against a cache, and the active-pane gate *above* it returns early
without refreshing that cache — so the stale entry survived the excursion and swallowed the return.
Two more of the same shape: the battle command list deduped on spoken TEXT alone (back out of
Attack, reopen → silence), and the Status chooser on `(ctrl, slot)`.

**Rule, now in CLAUDE.md + `feedback_no_dedup_rule` memory:** speech is never deduplicated. Two
exceptions only — (1) the user asks in the current conversation, (2) it guards a **per-frame /
per-draw** game function, and the comment **names that function**. A repeat from an event-driven
hook is a redundant-call-path bug; fix the path. Explicit carve-outs so a later session doesn't
over-apply it: state-machine latches that detect a transition (`combat_events`), collection dedup
(`AlreadyListed`), and log-only volume control.

**Removed** (all event-driven): `menu_reader` `OnFocus` gate; `ingame_menu_reader` `SpeakIfNew`,
battle-command text gate, Status `(ctrl, slot)` gate; `Speech::MaybeAnnounce` (zero callers — it
existed only to make the pattern convenient).
**Split rather than removed:** `title_reader`'s `g_lastRow` sat on a path shared by the `0x8000`
focus event AND the per-draw `HookedRow`. Replaced with a one-shot `g_replayPending` consumed by
`HookedRow`, so the guard covers only the per-frame path.
**Kept, now documented as the sanctioned exceptions:** `battle_target_reader::g_lastHandle`
(`FUN_002bfd20` → `FUN_00329220`, per render) and `entity_list::s_lastArea` (`FUN_0022a770`, the
field tick).

`FUN_00285a10` evidence: all 8 decompile call sites are in `FUN_00284ec0` / `FUN_00285190` /
`FUN_00285290` / `FUN_00285b20` — open / cursor-set / close handlers, none per-frame. If the Status
vitals line ever doubles per highlight, two of those fired for one input: narrow the hook, don't
re-add a filter.

### Part 2 — centralization audit (user-requested, "before it gets too bulky")

**Phase A — shared utilities.** `Log::WriteW` + `Log::ToUtf8` (`LogLine` had been written out 3×
verbatim, plus 6 open-coded `WideCharToMultiByte`). `title_reader` and `menu_observer` gave up their
private SEH readers for `core/mem_read.h`. New **`core/phyre_types.h`** — the actor pool, BtlChr
layout and scene-kind nibble had **four** copies (`battle_target_reader.cpp`'s own comment admitted
it "mirrors nav_rva.h"). CLAUDE.md's documented layout had always listed `phyre_types` under
`core/`; it had just never been created.

**Phase B — `MenuState`, the missing third state module.** `PlayerState` (field) and `BattleState`
(battle) existed; menu state was private to `menu_reader.cpp`'s anonymous namespace, so nothing else
could ask it. Now public, plus `FocusedOwner()`/`IsAnyMenuOpen()` and a `ValueRow` enum so callers
never resolve a row RVA. `ConfigReader` took the ~180 lines of value plumbing.
`menu_reader.cpp` 667 → 416.

**The find worth remembering:** grepping duplicate constant *values* rather than *names* exposed
three globals under multiple identities — `0x2D9F190` as `RVA_BTLWORK` + `PARTY_MGR_PTR` +
`FIELD_STATE_BLOCK`, `0x1F6E688` as `ACTOR_POOL_BASE` + `SCENE_POOL_BASE`, `0x1E63530` as
`RVA_RELOC` + `MAPJUMP_RELOC_BASE`. Three names for one pointer is three mental models of it, and
this is the exact global whose pointer-vs-struct confusion made 4/5/6 silent in Session 48.
Merging them surfaced a contradiction left **UNRESOLVED on purpose**: `nav_rva.h` called
`0x2D9F190+0x5A7E` a "field-sign category table"; `battle_state.cpp` reads the same address as
`OFF_ROSTER_L3`, the party roster — and the roster reading is confirmed in play while the field-sign
one was never used by any `.cpp`. Probably wrong, but "probably" is below 0.98, so it is recorded as
a question on `BTLWORK_PTR` rather than silently decided.

**Phase C — file splits.** `map_query.cpp` 747 → `map_names` + `map_exits` + `map_query` (234).
`entity_list.cpp` 883 → `entity_scan` (Build takes its destination by reference, so the scanner
holds no state) + `entity_diag` (the mutex deliberately stayed with the list it protects;
`DumpLocked()` requires the caller to hold it) + `entity_list` (385). `nav_rva.h` 472 → 279 +
`map_rva.h` 221. **Every `.cpp` is now under 500.**

**Phase D — docs.** Created **`Docs/PerformanceIssues.md`**, which CLAUDE.md has always required and
which never existed — that absence is why this debt accrued unrecorded. It carries the size table,
the centralization ledger, the duplicate-VALUE grep, and the open debt (the `0x5A7E` contradiction;
`menu_observer.cpp` being dormant — 226 lines whose focus callback is never registered).

Both `*_rva.h` headers stay over the 150-line rule **deliberately**: they are ~80% provenance
comments recording which readings were struck and why, which is what stopped past sessions
re-deriving the same wrong addresses. Recorded as a tracked exception, not ignored.

### Follow-ups (same session, after review)

**The `0x5A7E` "contradiction" was closed, not left open.** It was never a live conflict — it was a
wrong label on dead code, which is worse than no label because the next session builds on it. The
"field-sign category tables" ARE the party roster lists:
- the removed field-sign code read `SafeReadU8(mgr, 0x5A7E + slot*2)`; `BtlChrForSlot` reads
  `SafeReadU8(W, OFF_ROSTER_L3 + slot*2)` — identical base, offset, stride and width;
- `0x5A7E + 9*2 == 0x5A90`, so "table B" is exactly where list 3's nine u16 entries end and the next
  list starts (`OFF_LEADER` 0x5AA4 sits just past the second);
- the roster reading drives the party-vitals keys and works in play; the field-sign reading never
  did, was deleted from `entity_list.cpp` back in `e093c74`, and exits were solved a different way
  entirely (`__MJ_CTRL<N>`, Session 46).
STRUCK in `nav_rva.h`, documented on `BTLWORK_PTR`.

**`ui/menu_observer.cpp` DELETED.** Phase-0 scaffolding that inferred menu focus from the cursor's
X/Y pixel position. `Init`/`Shutdown` were its only call sites — `SetFocusChangeCallback`,
`LatestSnapshot`, `ReadRegistry` and `RegisterController` had **zero** — so it paid for a detour on
`FUN_00241d40` plus SEH reads and a map lookup per menu message, then dispatched to a callback that
was never registered. Superseded by `menu_reader`'s `FUN_00247510` msg-`0x8000` path, which receives
the focus INDEX rather than inferring it from pixels. In git if ever wanted.

### Status

Builds clean and deploys after every phase (four separate commits, so a regression bisects to one).
**Not yet play-tested** — the log checks in the plan's verification section still need a run:
re-enter a pane / reopen the Attack list / reopen the Status chooser should each speak again, the
Status chooser should show exactly ONE `status:` line per highlight, and the per-frame exemptions
(battle target, `Entering <area>`) must still fire once per change, not per frame.

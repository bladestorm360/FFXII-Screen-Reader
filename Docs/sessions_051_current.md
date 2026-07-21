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


## Session 52 — 2026-07-21 — [menus] Field-menu entry announce on the menu's SHOW message; dialogue pagination; `;` restored; per-area NAV-DIAG stall moved off-thread-path

**KEYWORDS:** field menu party menu entry announce speaks-on-keypress menu-not-ready
FUN_00280de0 0x160DE0 cat 0x13 SHOW message cat 0x11f ACTIVATE never-sent ArmPaneEntry
HookedFieldPaneWnd HookedFocusSet FUN_00244830 g_panePending battle-command parity
OnRowChainFocus wnd-first diagnostic dialogue pagination 0x03 page-break message_reader
telop NextPage semicolon `;` target HP battle_target_reader browsing acting NAV-DIAG
per-area flood DiagnosticDump backtick opt-in EnumerateFieldSignExits EnumerateMapJumps
DiagScanScriptMapjumps 400ms-fallback draw-callback removed

Continues Session 51 the same day (the second, behavioural half after the centralization pass).
Everything below is **play-tested and confirmed by the user** unless marked otherwise.

### 1. Field/party-menu entry announce now fires when the menu is VISIBLE, not when it starts loading

**Symptom:** opening the party/field menu spoke the focused row (`"Status"`) instantly on key-press,
while the menu itself appeared noticeably later. The BATTLE command menu never had this problem.

**Root cause (the one difference):** both menus stash the entry focus and replay it; they differ
only in *what releases the stash*.
- battle command: released by `HookedBcmdDraw` — the panel's own row draw (`FUN_00276be0`).
- field pane (before): released by `HookedFocusSet` — `FUN_00244830`, the focus **assignment**,
  which fires at the **start** of construction. That is why it spoke during load.

**Fix — make the field pane wait for its own "menu is visible" event, mirroring the battle menu.**
`FUN_00280de0` (RVA `0x160DE0`, == `ROW_CHAIN[0]`, the field command column, `rowOff 0xD8`) has a
message **`case 0x13` = SHOW**: it creates the info window, plays the open SE `FUN_00249c60(4)` once,
and clears the "hidden" bit `0x80` on the menu's UI resources (battle_4_p / s_font_c / targetline_p /
shape / mini_face_c). That is the frame the menu becomes visible. We trigger on the **message**; the
SE call is only corroboration. **Confirmed:** `"Status"` now lands with the menu.

Implementation is a one-to-one mirror of the battle path (`src/ui/ingame_menu_reader.cpp`):
`g_panePending{Owner,RowOff,Index}` ⇄ `g_bcmdPending*`; `IngameMenuReader::ArmPaneEntry` (called from
`HookedFocusSet` only for `IsFieldPaneOwner(o)`) ⇄ the stash in `OnBattleCommandFocus`;
`HookedFieldPaneWnd` releasing on `cat 0x13` ⇄ `HookedBcmdDraw`. Consume one-shot under the lock,
speak outside it, on both. **Only the `0x160DE0` class defers**; every other pane (submenus, config,
pop-ups) opens with the shell already up and still speaks immediately in `HookedFocusSet`. No
fallback — if the row never resolves, silence, exactly like the battle menu only replaying on a real
draw.

**Four "menu is ready" signals were tried and refuted BY MEASUREMENT before landing on 0x13** — all
recorded in `debug.md` so none is retried:
1. first UI string drawn (`TextCapture` DrawCallback) — the field HUD paints text every frame, so it
   fired the very next frame (`99ce38a`).
2. the focused row's OWN text drawn — measured **31 ms** after the focus event; drawing ≠
   presentation (`eaea451`).
3. window ACTIVATE `cat 0x11f / msg 0x8000` — **NEVER SENT** (0 occurrences in a whole session). This
   one SHIPPED AS SILENCE (`1600244` → user got no speech). A bounded `wnd:` sequence log is what
   proved the absence.
4. a 400 ms timeout fallback — removed as a band-aid that speaks at the wrong time and hides which
   signal is right (`58071b2`); it also let a per-string DrawCallback sit on the game's paint path.

The whole draw-callback plumbing (`TextCapture::SetDrawCallback` / `DrawCallback` /
`IngameMenuReader::RowChainText`) was deleted with the fallback. The `wnd-first:` diagnostic that
remains in `HookedFieldPaneWnd` logs each category **once per open** with Δt from the arm, keyed on
category (so pointer-valued msgs can't flood it), count-capped but **NOT time-capped** — the old
40-line/+110 ms cap is exactly what hid `0x13` last time.

Commits: `1b821d3`, `5ff045b`, `216d8b2`, `99ce38a`, `eaea451`, `1600244`, `58071b2`, `991b323`,
`6fd347a` (the SHIP).

### 2. Multi-page dialogue is paginated, not read as one block

The telop setter hands the reader an ENTIRE message (all pages concatenated), so it read a whole
conversation in one breath. **Codec `0x03` is the page break** — proven from shipped data
(`tools/ebp_find_pagebreak.py`: 95.6% of 3309 `0x03` occurrences sit at page boundaries across 17,268
messages), NOT guessed from the decompile. `GameText::DecodeToPages` now splits on `0x03`;
`message_reader` holds the page list and advances on the observed Confirm. **First attempt shipped
inert** — the `0x03` branch was silently dropped by a scripted edit and only a data re-check caught
it (`eeed6bc` fixed `a357354`). `kMaxCodecBytes` raised to 4096 (was truncating the hunt tutorial).

### 3. `;` (target HP) restored

Regressed earlier this session: `SpeakTargetStatus` bailed on `t.browsing`. Removed the
`|| t.browsing` guard and gated the `", queued"` suffix on `!t.browsing && !t.acting`
(`battle_target_reader.cpp`). Confirmed with a live Dire Rat + committed Attack. `71f43ee`.

### 4. Per-area NAV-DIAG flood moved off the area-change path (a real game-thread stall)

Every area transition wrote **~1,250 `NAV-DIAG` lines synchronously on the game thread** (one burst,
single timestamp: `EnumerateFieldSignExits` + `EnumerateMapJumps` + a ~98 KB script scan, all under
`Log::Write`). It was pure diagnostic — results discarded, and the exit FEATURE enumerates
independently with `logRaw=false`. Moved verbatim into `NavCommands::DiagnosticDump` (the `` ` ``
key, which already does rescan + object dump), so it is now **opt-in**, in whatever area the player is
standing in. The area-change branch keeps only the `"Entering <area>"` announce and its one-line
context. `entity_list.cpp` shed its now-unused `MapExits`/`kExitMaxDist` includes. `77c2b47`.

### Documented-only (NOT fixed this session — see debug.md)

Dialogue-choice pop-up options; full-screen system-notification banners; item name missing on
tutorial item pop-ups (`0x0f 2e` substitution slots dropped by the decoder); pathfinding accuracy
degrading near a target. Commits `371664c`, `2e5d8c3`, `7ec640d`.

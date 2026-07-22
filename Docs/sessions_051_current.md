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

## Session 53 — 2026-07-22 — [licenses] License Board + Job system reader: char-select, job ring, board nodes, LP, `o` details, `F` summary pages, confirm prompts

**KEYWORDS:** license board job system Zodiac char-select FUN_00560910 0x440910 menuCtx+0x158
job ring FUN_00557db0 0x437DB0 board grid FUN_0055cd40 0x43CD40 menuCtx+0x320 cell struct 0x38
FUN_0055bff0 grid builder cell pointer val FUN_00247510 0x8000 node status FUN_00323600
FUN_00323d10 status codes learned can-learn not-enough-LP locked LP save+0x190 job1 +0x1c3
job2 +0x1c4 viewed +0x1c5 FUN_003242f0 swap job names FUN_002f9860 job+0x3ED desc job+0x838
granted entries FUN_00559e30 kind rec+0x23 ids rec+0x26 technick 0x9e..0xb5 category cell+0x10
U key LP DIK_U 0x16 ProvideHelpText ability summary overlay FUN_002c53b0 0x1A53B0 FUN_002c3b90
0x1A3B90 menuCtx+0xDE7 mode entry 0x20 stride flags 0x20000 placeholder 0x4C7 empty
confirm prompt FUN_002cdf20 0x1ADF20 menuCtx+0x2e8 FUN_0057c480 surface +0x1B0 TakeConfirmPrompt

Built the whole License Board / Job feature. **Everything below is play-tested and confirmed by
the user** except where marked. Three surfaces + an overlay + the confirm prompts.

### 1. The flow, and the three controllers

Party Menu → **Licenses** (command `0x4b5`) → `FUN_005601b0` creates the **character-select**
`FUN_00560910` (RVA `0x440910`) at `menuCtx+0x158`. Confirm → an established character opens the
board (`FUN_00561300` → `FUN_00561390` → `FUN_0055c690`); a jobless one routes through stage
`FUN_00558fb0`, which spawns the **job ring** `FUN_00557db0` (`0x437DB0`), and after commit the
**board grid** `FUN_0055cd40` (`0x43CD40`). Ring and grid share slot `menuCtx+0x320` (mutually
exclusive in time — tell them apart by `obj[0]`).

- **Char-select is NOT the Status/Equip chooser** (`FUN_00285290` at `menuCtx+0xf8`). The mod's
  long-deferred `FUN_00285a10` hook targets that *other* surface and can never fire here — which
  is why this screen was silent. It gets its own window-proc hook: **SHOW (cat 0x13) = entry**,
  **cat 0xc / 0x8000 = move**. Highlighted member read from the global `menuCtx+0xde0` (set FIRST
  by `FUN_00285f20`) rather than `ctrl+0xd0`, which `FUN_00560ee0` only fills later — that
  ordering is what makes the *entry* announce reliable.
- **Board node moves ride the existing `FUN_00247510` 0x8000 hook**, but its `val` is a **POINTER
  to the focused cell**, not a row index (`FUN_0055bff0` fires
  `FUN_00247510(board, 0x8000, cellBase + (w*row+col)*0x38)`). `HookedDispatch` already carried
  `val` as a full 64-bit value, so no signature change.

### 2. Node reads

Cell (stride `0x38`, array at `board+0x120`): `+0x00` name codec (variant-selected), `+0x08` u16
node id (`0xFFFF` = not-yet-reachable), `+0x0b` type, `+0x0c` LP cost, `+0x10` **CATEGORY** codec
("Weapon"/"Magick" — *not* a description; this was a real bug), `+0x18` flags, `+0x20/0x21`
col/row, `+0x30` category id.

**Status** = `FUN_00323600(charId, nodeId, 0)` → `FUN_00323d10`, fully decoded from the decompile:
`1` learned · `2` not enough LP (`charBlock+0x190` < cost) · `0`/`9` can learn · `3/4/5/8` locked ·
`6/7` null/invalid. `FUN_0055bff0` **zeroes** locked cells to `id=0xFFFF`, so a named cell is
always `{0,1,2,9}`. Blank tiles announce **"Locked"** (user-requested; the game draws neither icon
nor info panel there, so we say a license exists without leaking what it is).

**`o` detail = category + granted entries + a description only where the game draws one.** The
entry list comes from `FUN_0035d330(0x19,nodeId)` → kind `rec+0x23`, 8 ids `rec+0x26..0x34`
(copy them BEFORE resolving again — one shared scratch record `DAT_022ca520`), resolved by kind:
`0`→`FUN_0035d330(1,id<<16)` gear, `1`→`0x14` magick, `2/3`→`0x1d` technick. The description
(`rec+0x08`) is rendered by the game **only** for kind‑1 ids in the technick block
`(ushort)(id-0x9e) < 0x18` — screenshots confirmed: Telekinesis shows one, Cure/Blindna and gear
do not. Gating on anything looser leaks text a sighted player never sees.

### 3. Two-job boards are SEPARATE, not merged

`FUN_003242f0(charIdx)` toggles the *viewed* board (`record+0x1c5`) between `job1 (+0x1c3)` and
`job2 (+0x1c4)`; `board+0x558` is the single viewed job the grid is built from. **Blank tiles are
never "where job 2 goes"** — a hypothesis raised and disproved this session.

### 4. `F` ability-summary overlay — a different subsystem entirely

Not part of the license module: a shared party-member detail overlay the board forwards pad input
into (`FUN_0055c740` → `FUN_002c1a80`). Mode byte **`menuCtx+0xDE7`**: `2` = Technicks/Mist/Remedy
Lore/Espers, `1`/`3` = Magicks, `0` = closed. Two controllers, one entry format; each has a single
focus routine firing on open **and** every cursor move, so one hook each covers the screen:
`FUN_002c53b0` (`0x1A53B0`, entries `obj+0xE0`, idx `obj+0x7A0`, section `obj+0x7A4`) and
`FUN_002c3b90` (`0x1A3B90`, entries `obj+0xC8`, idx `obj+0xAE8`). Entry `0x20`: `+0x00` name,
`+0x10` description, `+0x18` flags (bit `0x20000` = learned/bright).

**Bug worth remembering:** the Magicks page was *entirely silent* and the RVAs were fine.
`GameText::IsMostlyPrintable` requires `alpha >= 1`, and the game's unlearned placeholder
(`FUN_002f9860(0x4C7)`) is `"?"` — no letters — so every unlearned row decoded to empty and was
dropped. Placeholder rows are now detected explicitly and spoken as **"empty"** (a literal `?` is
commonly dropped by TTS punctuation settings, which would re-silence it). Section heading is
announced on **page switch only** (owner change), not on every section crossing.

### 5. Confirm prompts ("Obtain Accessories 1?", "Choose this license board?")

Class `FUN_002cdf20` (`0x1ADF20`, registered `menuCtx+0x2e8`) — **not** the known confirm window
`FUN_00241d40`, so it fell through to the generic content path and spoke **stale item text from a
recycled owner address**. Now recognised as a pop-up (itself or the list widget it owns), so
Yes/No come from the game's own ids 1000/1001.

Its body is **not stored on the object**: it is composed into a local and handed to a
`FUN_0057c480` surface. The composed string is readable only at that surface's **case‑1 birth** —
which `message_reader` already decodes and logged, muted, all along. Reading it later at
button-focus time returns nothing (tried; failed). `MessageReader::TakeConfirmPrompt()` now stashes
it at birth and `PopupReader::BodyText()` consumes it, so it flows through `OnFocus`'s existing
`preambleSpoken` path — body first, button queued after. `kSpeakSurfaceConfirms` stays `false`
(speaking at birth gets cut off by the Yes/No focus milliseconds later).

### 6. Keys / structure

`U` (DIK `0x16`, free per Controls.md) reads current LP; LP also announced on board entry.
`TextCapture::ProvideHelpText()` added so a reader can supply `o` text for surfaces the game does
not feed to the description bar (the board **clears** it: `FUN_00291d80(0,0)`).

Split to respect the 500-line rule: `license_reader` (480), `ability_summary_reader` (143),
`popup_reader` (41), `menu_reader` back to 499.

**Not yet verified in play:** the `F`-overlay "empty"/page-heading changes and the confirm-body
wiring landed at end of session — built and deployed, untested.

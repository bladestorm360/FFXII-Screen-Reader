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

## Session 54 — 2026-07-22 — [nav] Gates into Interactables; F5 story-gate filter; exit parse unwindowed; world-absolute directions

**KEYWORDS: town gate Rabanastre interactables FLAG_TALK misclassification kind nibble 0x0E
FUN_002675c0 FUN_0025bad0 story gate 0x10 FUN_0026ba60 script native F5 availability filter
BLOB_MAX 0x18000 silent return false routine table world-absolute directions egocentric withdrawn
CompassFaceDeg 180 flip npcdic odd slot struck Rabanastran ordinals duplicate interactables OPEN**

Reported: the Rabanastre town gates appear in no category; the NPC list is a wall of identical
"Rabanastran"s so the gate guard cannot be picked out; and route/crow-flies directions reorient
whenever the camera moves. Tester's classification rule for this work: **press-Enter-to-use ⇒
Interactables; step-on-coordinates ⇒ Exits.** All RE below was done OFFLINE (decompile + extracted
data) per the ≥0.98 bar — the runtime log was used only as the symptom record.

### 1. The engine's own interaction classifier (the root cause)

`FUN_002675c0` (RVA `0x1475C0`) is "can the player interact with this right now", and it *is* the
tester's rule: ACTION path returns `(obj+0x0E & 0xF) == 5`, TALK path requires `== 1`. Corroborated
by `FUN_0025bad0` (kind 1 talk-only, 4 both, 5 action-only, 7 talk-only, else rejected). So
**`sceneObj+0x0E & 0xF` is the object KIND: 5 = ACTION gimmick (gate/door/switch/chest), 1 = person.**

**STRUCK:** `ClassifyByNameKey`'s `if (flags & FLAG_TALK) return NPC` *as the first test*. The engine
branches on TALK and ACTION *independently on the same object* and never equates talk-target with
person. A gate with a confirm prompt is a talk target, so every one was filed under NPC — `Object=0`
in East End (37 NPCs, zero interactables) and Muthru Bazaar (26/0).

**REGRESSION, caught in play the same session — and it partly refutes the finding above.** The first
fix ordered the classifier `kind == 5 → Object` *before* the character test, and **every NPC was
reclassified as Interactables**. Tester: "treasure, NPCs, save crystals should still go in their own
categories; interactibles like switches, levers, wells should still be interactibles." So:

- **REFUTED: "kind 1 ⇒ a person".** Field NPCs evidently do not carry kind 1, so any kind test placed
  ahead of the character test swallows them. Two engine functions agreeing on a shape did not make
  that shape a classifier for the population being scanned.
- **Survives (~0.9):** `kind == 5` spots the ACTION gimmick *among non-characters* — `FUN_002675c0`'s
  ACTION branch literally returns `(obj+0x0E & 0xF) == 5`.
- **The reliable person test was already there:** the scene CHARACTER class `+0x03 & 0x1f` in 5-7.

Shipping order: npcdic gimmick band (Treasure / Gate Crystal / Save Crystal keep their own
categories) → `isCharacter` ⇒ NPC → non-character `kind == 5` ⇒ Interactables → `FLAG_TALK` ⇒ NPC →
Interactables. The kind-5 *inclusion* widening is likewise non-characters only. Net effect versus the
pre-session build: **only** a non-character kind-5 object moves; everything that classified correctly
before still does.

The `'` dump never logged the kind nibble, which is why nothing caught this before it shipped — it
now prints `kind=` and `en=` (the story-gate bit) per object.

**Worse, `+0x1C` is MODE STATE:** `FUN_0025ad10`/`FUN_0025ae00` rewrite those bits, and a disabled
object has **zero** flags — so the inclusion test dropped story-gated gates outright. `kind == 5` is
now an inclusion reason in its own right.

### 2. Story gate + `F5`

`sceneObj+0x0E & 0x10` is the interaction-ENABLED bit. Setter `FUN_0026ba60` (RVA `0x14BA60`), whose
only caller `FUN_0034afe0` (RVA `0x22AFE0`) has **zero in-binary callers** ⇒ a script-VM native: the
map's own script opens and closes it. `EntityScan::IsInteractionAvailable` replicates `FUN_002675c0`
memory-only; `F5` toggles **All ⇄ Story-gated** through the one predicate every command already goes
through (`FilteredSortedLocked`). Default All — nothing is ever hidden unasked. Non-interaction kinds
are always `available` so props never pad the gated list.

### 3. Exit parse — the 96 KB window nobody knew was a precondition

`MapScript::SnapshotBlob` copied a fixed `0x18000` prefix and bounds-checked the header offsets
against the COPY, so on a bigger map `ReadExitDests` returned false **before its first log line**;
every door logged `-> no controller (arrival point)` and `Exit=0` across all of Rabanastre. The tell
was an absence — no `==== field-script exits ====` header — while `EnumerateMapJumps` read 14 doors
from the same blob. Migelo's Sundries, a small shop, already had `routineTable=+0xE350`.

Now reads the live blob directly (SEH-guarded, `OFFSET_MAX` sanity ceiling, no window); per-rescan
cost drops from 96 KB to ~2 KB. **Every bail logs its reason**, and a clean parse with 0 controllers
dumps the routine names. Offline note: `__MJ_CTRL` is plaintext in **none** of the 20 extracted
`.mpk`s, including Nalbina where it is proven present at runtime — the pool is packed on disk.

### 4. Directions — reversed THREE times; the answer was the ROUTE SMOOTHER

1. Tester: routes "constantly switching directions" → withdrew the relative frame for world-absolute.
   **Wrong** — absolute is stable but useless, you cannot push "north".
2. Restored the frame but swapped in literal ego words ("ahead", "behind-left"), theorising that
   compass-words-on-a-relative-frame was the confusion. **Also wrong**, and backwards: *"most prefer
   north/south/east/west … essentially north=forward whether it's true north or not, east=right."*
3. **Actual cause**, tester's words: *"we're not telling the player to walk 30 northwest when the arc
   is actually 18 north, 7 west … then if the player walks 10 west by accident the directions flip to
   northeast because the player has gone past the north leg."* The planner's greedy string-pull
   collapses an L across open ground into one diagonal chord — the player is sent along a line the
   route never takes, and any drift comes back inverted.

**SHIPPED — the diagonal rule.** A diagonal word may only describe a genuinely diagonal stretch (a
fine alternation whose net line really is 45°); a route with shape is spoken as its actual legs.
Pipeline: raw cell path → **Ramer-Douglas-Peucker at one grid cell** → runs → staircase collapse →
merge same-word → absorb sub-2-step legs → **cap 5 legs then "then N more"**. Both extremes were
measured wrong: the smoothed chord gave one invented diagonal, the raw path gave a **19-leg** readout
in play. RDP is deviation-bounded, so unlike string-pull it cannot cross a corner.

**Vocabulary as shipped:** compass words on the RELATIVE frame — North = forward, East = right,
South = behind, West = left. The literal-ego wording is retained unused and renamed per the tester:
`forward / forward-right / right / backward-right / backward / backward-left / left / forward-left`.

**`PlayerState::ReadCameraForwardStable`** (live camera → last good → leader facing) replaces the raw
getter everywhere, and **always** yields a reference: a distance with no direction is useless to walk
on, so there is no direction-less path. The old code declared `float facingRad = 0.0f`, ignored
`ReadCameraForward`'s bool, and `CompassFaceDeg(0) == 180` — every direction spoken **reversed**.

### 4b. Phantom "NPC 1 … NPC 37" — unplaced reserve slots at the world origin

Root-caused from the new `dup-label` dump in one session: East End listed **37** entities ALL at
`pos=(0,0,0)`, unnamed (so labelled by category word, then numbered), `kind` 1 and 5. Slots the map
allocates at load and positions later; still at the origin means never placed. Off the walkable map,
so `\` correctly answered "No path" — the log shows `NPC 1. North, 197 steps` then `NPC 1. No path`.
`ScanCombatants` always had this guard ("unplaced reserve unit"); the handle-table walk never did.
Fixed with the same exact-zero test. **This is where East End's "NPC=37 Object=0" always came from —
not one was a real NPC.**

### 5. Same-named NPCs

**STRUCK before building:** npcdic's odd slot is not a yomi — `FUN_00263990` selects it via
`FUN_0032a930`, a live per-id bitfield at `FUN_002ef2b0()+0x13B4` — but decoding `npcdic.bin`
directly (2282 slots / 1141 ids) shows **odd == even** in the US build. There is no second name. The
duplication is the game's data: **109 ids all render "Rabanastran"** (554 distinct names overall).
So `NumberDuplicateLabels` appends a stable ordinal (npcdic id, then scene handle) to same-label
entries only. **No name is ever substituted** — `ResolveObjectName` speaks the game's own string
verbatim, and the numeric suffix is the only edit any label receives.

**Retracted same session (tester correction):** "gate guards are already distinct: npcdic 362 =
Imperial Guard" was written as fact and is an **unverified hypothesis**. Id 362 does exist and does
read "Imperial Guard", but it was found by grepping the extracted dictionary for guard-ish words —
which proves the string exists, not that any object at the Rabanastre gates stamps it at `+0x102`.
The tester reports the gate guards are **not** Imperials. Below the 0.98 bar; no code keys off it.
Confirm properly with `'` at a gate and read `nameIdx` on the objects near the logged player position.

### 6. Also recorded / OPEN

- `phyre_types.h`'s `KIND_DEAD = 5` is a **misname** (5 is the field-gimmick kind). Left in place —
  the combat track owns it, and the actor pool holds no gimmicks so the skip is a no-op today.
- **OPEN: every interactable is listed twice** (reported mid-session, "at least all of the shops").
  Not diagnosed, deliberately not guess-fixed. Instrumented instead: `dup-label` groups dump once per
  map with pointer/handle/kind/flags/pos, and the `'` dump now prints the container's own interaction
  spans (`grp1 = +0x1DE/+0x1D6`, `grp0 = +0x1DA/+0x1D2`) that `FUN_0025b820` walks instead of the raw
  array. Same position + different pointers ⇒ shadow registration; different positions ⇒ two real
  objects. See debug.md.

### 7. Structure

`entity_scan.cpp` hit 512 lines, so the per-object judgement layer split out to
`entity_classify.{h,cpp}` (151/38): `CategoryWord`, `ResolveObjectName`, `InGimmickBand`,
`ClassifyByNameKey`, `IsInteractionAvailable`. entity_scan.cpp back to 382 — it finds objects,
entity_classify decides what each one is. `entity_scan.h` includes the new header, so no caller
changed.

**Built and deployed; not yet verified in play.**

## Session 55 — 2026-07-23 — [nav] Planner recovery (start-snap/bridge/near-goal); exit source = +0x54 ∪ +0x70; sign-twin dedup; reachability filter; NoPath root causes

**KEYWORDS: pathfinder No path startFloor touched 2157 islanded doorway kMargin 0.5 relaxed retry
bridge near-goal snap-start reachability flood fill NavReach exit union +0x70 field sign __MJ_CTRL
slot 7 fieldsignmes setfieldsignlocationjumpinfo sign twin dedup Interactables 12 to 7 kind 4 door
kind 5 NPC dup-label latch NOT USED placeholder Pharos at Ridorana coverage matrix +0x8c dest table
MapRef DAT_02099d88 exit_diag exit_scan path_search nav_reach file split**

Tester report on the S54 build (Rabanastre East End 291 / North End 289): duplicate interactables,
exits named "Pharos at Ridorana: NOT USED", exits with no path, missing east gate. Root-caused from
the mod's own log + the `'` dump; no runtime discovery.

### Planner — two NoPath causes, both mod-side (path_search.cpp, split from path_planner.cpp)
- **Start side:** player's own fine cell had no floor sample (`startFloor=0 expands=1`) — the goal got
  a walkability snap, the start never did. Added `SnapToWalkable` on the start (`snap-start:`).
- **Goal side:** `touched=2157` identical from four starts and both targets — goal cell walkable but
  islanded by the 0.5 m lateral-clearance test at a narrow archway. Added a margin-0 **bridge flood
  from the goal** (≤256 cells) that splices into the strict search, plus a **near-goal fallback** (route
  to the closest reached cell within 6 m). Every NoPath now logs `pass=` and `nearDist=`.

### Exit source — union of two tables (exit_scan.cpp, split from entity_scan.cpp)
`__MJ_CTRL` controllers alone are NOT the exit list. East End slot 3 has a controller and no sign;
**slot 7 has a sign and no controller** — a transition the game files as an exit that the mod could not
see. Exit = a `+0x54` slot claimed by a controller OR a field sign. **STRUCK:** the "+0x70 empty on
every map" verdict (25 records on East End). New `map_exits.EnumerateFieldSignRaw` + `SignRec`.

### Duplicate interactables — the fieldsign discriminator
`'` dump: each shop is two objects, identical `cat=21 kind=4 en=1 flags=0x2134 ACT`, both inside the
container action span `[19,35)`, 6–15 m apart. Both are field signs (`nameIdx=-1` → `sceneObj+0xf8`
via `fieldsignmes`). The one bound to a jump (`setfieldsignlocationjumpinfo`, a `+0x70` record within
~2 m) is the doorway; its same-named twin is a plain sign → dropped by exact-label match. Interactables
12 → 7. **STRUCK:** S54's `kind==5 = ACTION gimmick` — on East End kind 5 = NPC, kind 4 = doorway,
kind 1 = player, so `IsInteractionAvailable` never evaluated a door. Fixed the dup-dump latch (fired on
map-id change, before objects streamed in) to latch on `(mapId, population high-water)`.

### Reachability filter (nav_reach.cpp) — fail-open
One budgeted flood-fill per map (60 cells/frame, ~1 ms, STALL_SCOPE'd), published via shared_ptr swap.
Drops an exit only when ready AND no cell within 4.5 m is reachable, AND never more than half a map's
exits (`reachability filter disabled: would drop N of M`).

### Regressions this build introduced (fixed in S56, recorded here): placeholder detector (≥3-ids
multiplicity — see S56) and the exit union over-matched shop arrival points (see S56). Diagnostics
authored: `exit_diag.cpp` (coverage matrix, both candidate pairings, `+0x8c` dest table, MapRef
records) on the `'` key.

**Files split for the 500-line rule:** path_search.{h,cpp}, exit_scan.{h,cpp}, nav_reach.{h,cpp},
exit_diag.{h,cpp}. Built + deployed.

## Session 56 — 2026-07-23 — [nav] Direction 180° flip diagnosed = game camera (accepted+documented); placeholder detector fixed; exit union arrival-point exclusion

**KEYWORDS: direction reversed 180 Northeast Southwest camera flip DAT_02aedf30 FUN_004742a0 movement
basis camera-relative accepted README limitation ref src dref log placeholder NOT USED multiplicity
struck Aerodrome No 10 Channel exit union shop arrival point signTaken objectOnSign kSignObjectDist
camera lock rejected battle targeting travel anchor rejected**

Tester walking to Southern Plaza: directions reversed mid-walk — "Northeast" became "Southwest".

### Diagnosis — the frame flipped exactly 180°, and it is the game's camera
Log: same target, same route, five legs with identical lengths and **every word exactly 180° opposite**
across `seq=7` → `seq=11`. Player Y went 0.00 → 0.50 (stepped onto the shop terrace) on the flip frame,
new value held 4 s. The direction reference is `ReadCameraForward` (row 2 of `DAT_02aedf30`), which the
game block-copies every frame from the active camera (`FUN_00202c70`, `decompile_all.txt:509454`). We
read it correctly; the game changed it.

### Decision — accept, document, do not engineer around
Movement is camera-relative, so a mid-walk camera swing sends the held stick input the wrong way *at
that instant* — no read can undo it, because the mod learns of the flip no sooner than the player.
Three fixes designed and **rejected** (recorded so not re-attempted):
- **Camera lock** (write world axes into `DAT_02aedf30` at `FUN_004742a0`'s entry): rejected — that
  matrix orients the character onto the battle target, and `FUN_004742a0` keeps cross-frame lock-on
  state (`param_1[2]` + 22.5° threshold), so freezing it breaks targeting. Also crosses read-only rule.
- **Travel-anchored frame**: rejected — stops the words flipping but the held input is already wrong and
  the readout stays silent until the next query. Symptom swap.
- **Spoken "Camera angle changed." notice**: rejected — the battle camera reorients on every target
  change, so it would fire almost continuously in combat.

**Shipped:** a `ref=<deg> src=<live|held|face> dref=<deg>` line on every route drain and entity
announce (`ReadCameraForwardStable` gained a `srcOut` overload), a `README.md` "Known Issues" paragraph,
and a corrected `Docs/Controls.md` (S54's "egocentric words" note was wrong — `RelativeWord` returns
`kCardinal`; egocentric vocab exists but is unshipped).

### Regression fixes from S55
- **Placeholder detector** — the ≥3-ids multiplicity test is refuted by its own first run: "NOT USED"
  is shared by 2 ids (293/294) while genuine "Aerodrome" is shared by 5 and "No. 10/11 Channel" by 3.
  Replaced with an exact match against `kPlaceholderNameUS = L"NOT USED"` (the one locale-specific point;
  the shared-name scan stays log-only to name the token on other locales). "Pharos at Ridorana: NOT
  USED" gone; Aerodrome et al. keep their names.
- **Exit union over-matched** — exits went 6 → 14; the 7 extras were shop *arrival points* (signs
  destIdx 20-26 + the North End arrival slot 0). A sign justifies a jump slot only if it is not already
  claimed by a controller door (`signTaken`) AND has no scene object on it within `kSignObjectDist`
  (`objectOnSign`) — an interior doorway sign has the press-Enter object sitting on it. East End back to
  7 exits (slots 1-7).

Built + deployed. **Not yet verified in play.** Deferred: `__MJ_CTRL` slot-N+1 pairing walk test,
Eastgate search (Southern Plaza / Muthru Bazaar never dumped), `+0x8c` story-variant dest recovery.

## Session 57 — 2026-07-23 — [nav] ROOT CAUSE: +0x54 is the ARRIVAL table not the exit set; exits = __MJ_CTRL controllers only; drop unnamed; S57 zone-test capture diagnostic

**KEYWORDS: mislabel Southern Plaza Bazaar exit destination +0x54 arrival table not exits FUN_00264b90
FUN_00259b30 getmapjumppos nowjumpindex jumpIndex arrival slot on destination map mapjump literal
__MJ_CTRL zone test 0x202d native +0x84 parallel table few steps short transition tile N+1 struck
revert union controllers only drop unnamed HasRealAreaName capture DumpCaptureDiag Explore agent**

Tester on the S56 build (Rabanastre): the "Southern Plaza" exit LOADED Muthru Bazaar; East End listed
13 exits not 3; a 20-leg route validated through a wall; and exits land a few steps short of the
transition (in Nalbina too).

### ROOT CAUSE (Explore-agent decompile trace, 0.9–0.95) — supersedes the S46 N+1 model
**`+0x54` is the party ARRIVAL table, not the exit set.** `+0x54[K]` = x/y/z/angle only (no dest field),
read by `getmapjumppos(nowjumpindex)` (`FUN_00264b90`) and used by party-spawn `FUN_00259b30` to place
you ON ARRIVAL. `mapjump(dest, jumpIndex, flags)`'s `jumpIndex` is the **arrival slot on the DESTINATION
map** (`FUN_003145e0` stores it as `nowjumpindex` at map-state `+0x1048`), NOT the source slot. No
engine loop tests `+0x54` vs the player — the "stepped on a transition" test is a **script zone check
(VM native 0x202d)** inside the `__MJ_CTRL` routine, keyed by script-literal zone ids. So **no
`+0x54`→destination binding exists in the blob**; S46's `__MJ_CTRL<N> owns slot N+1` paired two
independent tables and only held on Nalbina's tiny maps. That is the mislabel, and routing to `+0x54`
(arrival, set back from the edge) is the "few steps short" — same data everywhere, so Nalbina too.
Reliable data = the `mapjump` literal (destination). Missing data = the true transition-tile position.

### +0x84 parallel table — unresolved contradiction the capture settles
`FUN_00264b90` reads `+0x54` OR `+0x84` (hdr words 0x2a/0x42) by a flag; party-spawn reads `+0x54`
(arrival), so **`+0x84` is the leading candidate for the transition tiles**. The mod's own S46-era
`map_script.cpp` comment claims the OPPOSITE — flagged, unresolved, dumped for decode.

### Shipped this build (capture + cleanup)
- **Exit source reverted to `__MJ_CTRL` controllers only** (`exit_scan.cpp`) — the S55 `+0x54 ∪ +0x70`
  union was wrong (pulled shop-arrival slots in → Exit=13; its object-list exclusion was empty at
  area-entry). Deleted the signTaken/objectOnSign machinery. **Drop unnamed** (placeholder 293/294 →
  `HasRealAreaName` false). East End 13 → 3, robust from frame 0, no object-streaming dependency. Interim
  labels stay N+1 (may be swapped) per tester — real fix next build.
- **S57 capture** (`MapScript::DumpCaptureDiag`, on the `'` key): dumps BOTH `+0x54`/`+0x84` tables raw
  + un-deduped, and every `__MJ_CTRL` routine's full bytecode with each CALLACTPOPA native annotated
  (mapjump 0x008d / zone-test 0x202d + operands). Needed because East End's routine table sits at
  `+0x29A00`, past the old 0x9000 blob-hex cap. **Tester presses `'` on East End + Muthru Bazaar.**

### NEXT build (real fix, after decode): correct exit reader
Destination from the routine's `mapjump` literal + transition-tile position from `+0x84` (if proven the
tiles) or the routine's `0x202d` zone test — same routine, no N+1, no `+0x54`, no cross-map. Ships behind
a safe fallback. **HARD CONSTRAINT: must not break the Nalbina prologue** — tester runs a full prologue
tutorial as the release gate.

Deferred: level-aware walkmap (multi-level Muthru routed y=−9→y=0 through a wall). Built + deployed.

## Session 58 — 2026-07-23 — [nav] `+0x84` edge-pairing decodes the door binding; route target marched to the transition seam

KEYWORDS: +0x84 edge record, edge-pairing, ResolveControllerArrivals, door binding, N+1 struck, arrival
table, transition seam, SeamTarget, SeamTargetFrom, trigger bearing, route to map edge, Southern Plaza
loads the Bazaar, map_script_internal.h, map_script_diag.cpp, exit_scan.cpp, ExitDest::edge

### What this session fixed

The S57 capture (`'` on East End + Muthru Bazaar) was decoded offline. It answered the open question
outright, and both tester symptoms fell out of the same one fact.

**`+0x84` = the `+0x54` arrival table with ONE extra "edge" record inserted per map-jump controller.**
An edge is a `+0x84` record that appears in no `+0x54` record; it is the transition TRIGGER's reference
point, sitting off the walkable mesh out past the map boundary. **Controller `i` owns the record
immediately AFTER the i-th edge.**

That replaces the S46 rule (`__MJ_CTRL<N>` owns `+0x54` slot `N+1`), which paired two structurally
independent tables and only ever held on maps where every arrival happens to have a controller — i.e.
Nalbina's 2-3-loader maps. The moment a map has a controller-less arrival (a shop spawn point), the two
rules diverge, and East End has several.

**East End (6 controllers), edges at `+0x84[1,3,6,8,10,12]`:**

| ctrl | dest | OLD `+0x54[i+1]` | NEW `+0x84[edge_i+1]` | atlas dir | ✓ |
|---|---|---|---|---|---|
| 0 | 289 North End | (125,21) | (125,21) | north | ✓ |
| 3 | 290 Muthru | (107,56) mid-map | **(33,56)** | west | ✓ |
| 4 | 292 S.Plaza | (33,56) west | **(123,135)** | south | ✓ |

Muthru Bazaar matches too (ctrl0→289 north at (36,24.5), ctrl2→291 East End at (48,56)). It also
reproduces the tester's walked ground truth: arriving from the Bazaar you spawn at `+0x54[2]=(26,58)`,
and the Bazaar exit's true position is `+0x84[4]=(26,58)` — the same point. The mod had been showing
`+0x54[4]=(107,56)`, ~80 units away, which is the reported "10 steps north, actually a couple of steps
behind me". **"Southern Plaza loads the Bazaar" was this and nothing else.**

### Change A1 — the door binding (`map_script.cpp`)

`ResolveControllerArrivals()` reads both tables, marks every `+0x84` record absent from `+0x54` as an
edge (exact float match on x/y/z/angle — the arrivals are byte-identical copies), and hands controller
`i` the record after the i-th edge. `ExitDest` gained `edge`/`edgeOk` alongside `pos`/`posOk`.

**Consistency gate → safe fallback.** If `#edges != #controllers`, or a record after an edge is missing
or is itself an edge, the whole map falls back to `+0x54[N+1]` and says so in NAV-DIAG. A map that
breaks the pattern behaves exactly as it did before rather than emitting a wrong position. (On Nalbina
the two rules are expected to *agree*, because the divergence needs a controller-less arrival; the
prologue tutorial run is the gate that confirms it.)

### Change A2 — route to the seam, not the arrival (`exit_scan.cpp`)

Tester: *"routing to the arrival point often leaves the player feeling around for the exit because
there's no indication of which way to walk to get to the map edge."*

The arrival is a couple of steps INSIDE the map. The fix marches it out to the boundary — but **the
direction cannot be found geometrically**: an unwalkable sample is a building wall and a map edge alike,
so "march out until the floor ends, in whatever direction ends soonest" walks the player into the
nearest shopfront. (This was written and discarded before the data answer was found.)

The data has the answer. The paired **edge record's BEARING** from the arrival is exactly the direction
the player crosses the seam — north to (125,−78), west to (−39,·), south to (·,255) on East End's three
real exits. So `SeamTarget()` marches the walkmap along that bearing (0.5 m pitch, ±20° fan of 5
branches, capped at 14 m *or* the trigger distance, whichever is shorter) and takes the last sample that
still has floor; the fan branch that gains most **along the true bearing** wins, since that is the one
aimed down the gap. Overshooting is harmless by construction — the transition fires when the player
crosses it, before they reach the target.

Guards: resolved **once per map** and cached (a few hundred walkmap queries, and the answer cannot
change while a map is loaded); nothing is cached until `MapQuery::HasWorld()`, because the exit scan runs
from the first frame on a new map and caching a pre-stream miss would freeze every exit on its arrival
for the whole area; a seam the `NavReach` flood cannot reach is discarded in favour of the arrival; and
**reachability is still judged on the ARRIVAL**, before the march, so the march can never hide an exit.

### Housekeeping

`map_script.cpp` hit 523 lines. Split into `map_script_internal.h` (blob layout constants + the guarded
blob readers, shared so the reader and its capture dump cannot drift apart), `map_script.cpp` (337) and
`map_script_diag.cpp` (137). `map_script_internal.h` also records the now-SETTLED `+0x54`/`+0x84`
identities in place of the S57 "DISPUTED" note.

### Verification (tester)

1. East End, `=` to Exit → 3 real transitions at their correct doorways: North End **north**, Muthru
   **west**, Southern Plaza **south**. Walk each; `announce: mapId=` must match the label.
2. Routing to an exit should end AT the transition — the closing legs point into it, no feeling around.
3. NAV-DIAG shows `arrival (…) edge (…)` per controller, `door binding: +0x84 edge-pairing`, and one
   `route target -> seam (…), N.Nm along the trigger bearing` line per exit.
4. **Release gate: full Nalbina prologue tutorial run.** Confirm the log says `+0x84 edge-pairing` (not
   the fallback) and that every prologue exit still names and reaches the right sub-map.

### Still open

Eastgate (East End has no loader to 305); Change B (routes must never validate through an obstacle) is
still deferred pending the retest above; the direction camera-flip is accepted + documented (S56).

## Session 59 — 2026-07-23 — [nav] Seam march REFUTED in play and removed; speak the crossing direction at the exit

KEYWORDS: seam march refuted, walkmap boundary is not the transition, script zone 0x202d, crossRad,
hasCross, At the exit Walk east, kAtExitDist, arrival Y projection, GroundAt floorY, blob dY nominal,
CardinalOfHeadingRelative, path_planner short-circuit, Muthru Bazaar 290, multi-level tier

### The refutation

S58's `+0x84` edge-pairing is CONFIRMED in play — the log reads `door binding: +0x84 edge-pairing
(4 controllers)` on Muthru Bazaar with correct destinations. The **route target** half of that build is
what failed. The seam march found a real map boundary **zero times out of two**:

| exit | arrival | march result |
|---|---|---|
| ctrl000 → North End | (36, 0.0, 24.5) | (36, 0.0, 10.5) — ran its **full 14 m cap**, no boundary |
| ctrl002 → East End | (48, 0.0, 56) | (51, **−8.0**, 56) — 3 m east and **8 m down**, onto another tier |

**The premise was wrong, structurally, and S57 already said so:** the transition is a script ZONE (VM
native `0x202d`), which the collision mesh does not model. No walkmap probe can find it. The march also
had a real bug — it sampled `GroundAt` with no floor-continuity check, so it stepped off a ledge.

The play consequence: target (51,−8.04,56), player pinned at x=48.40–48.41 across **ten** `\` presses,
told "East 3. 3 steps" every time and cycling East / Northeast / Southeast / North without converging —
a confident route into a wall. Tester: *"the exit is in the right area, but I am not told which way to
walk to get to the actual transition point."*

### Change 1 — the march is gone

`exit_scan.cpp`: `SeamTarget()` / `SeamTargetFrom()` deleted. The route target is the `+0x84` arrival —
the tile the engine itself places the party on, so it is known walkable and adjacent to the trigger. Two
seam-finding attempts are now struck (S58's geometric 16-heading probe, discarded before shipping; this
march, shipped and refuted); both are recorded together in `debug.md` so a third is not attempted.

### Change 2 — the crossing direction, spoken on arrival

The player never needed a better target. They needed to be told **which way to step**, and the game has
that exactly: every `+0x84` trigger record sits far off-mesh **directly along the crossing axis**.

| ctrl | arrival → edge | crossing |
|---|---|---|
| 000 | (36, 24.5) → (36.1, −74.2) | north |
| 001 | (48, 64) → (195.8, 64.2) | east |
| 002 | (48, 56) → (198.1, 56.0) | east |
| 003 | (42.9, 144.9) → (43.0, 258.6) | south |

Exactly axis-aligned on every transition of both test maps. `Entity::crossRad`/`hasCross` carry the
bearing (`atan2(dx,−dz)`) from `exit_scan.cpp` → `GetCurrentTarget` → `PathPlanner::Request`. In the
drain, **before** the A*: if `hasCross` and the player is within `kAtExitDist` (3.0 m, X/Z only), speak

> "Exit, Rabanastre: East End. At the exit. Walk east."

instead of another two-metre leg. Checking before the search also means standing on an exit can never
produce "No path", and costs one distance compare otherwise. `kAtExitDist` = two fine nav cells; the
measured need is 0.78 m (where the tester stood while being fed 3-step legs). Y is deliberately excluded
— on a multi-level map it is unreliable even after Change 3, and a doorway at your feet must not fail the
test over a metre of slope.

Spoken **only** at the end of a route (tester's call) — not on `/` describe, not while cycling, so exit
announcements stay as short as they are.

### Change 3 — arrival elevation from the walkmap, not the blob

Muthru's East End arrival carries `y = 0.0` while the player walks that ground at `y = −9.0`, which is
why the describe announced "East, 11 steps **(above)**" for a doorway at the player's feet. The exit's Y
is now `GroundAt(x, z)` when a floor exists there, keeping x/z; the blob's Y survives only when there is
no sample. That makes `(above)`/`(below)`, `Distance3D` and the at-the-exit test honest.

### Change 4 — one diagnostic line per exit

`__MJ_CTRL002 (48.0,0.0,56.0) floorY=-8.04 (blob dY=8.04) cross=east dest=291 -> exit "…"`

`blob dY` answers the one open question below without another capture round-trip.

### Verification (tester)

1. Muthru Bazaar at the East End doorway, `\` → "…At the exit. Walk east." Walking it crosses.
2. From across the map, `\` → normal legs to the arrival, then the crossing sentence inside ~3 m. No
   more oscillation between 3-step legs.
3. North End exit on the same map → "Walk north". East End (291) → north / west / south.
4. Log: the new `floorY=` line per exit; `+0x84 edge-pairing` still present; no `route target -> seam`.
5. **Release gate: full Nalbina prologue tutorial run** — every exit names and reaches the right
   sub-map, and the binding line says `+0x84 edge-pairing`, not `FALLBACK +0x54[N+1]`.

### Open

- **Is Muthru's ctrl002 arrival really 9 m above the street?** Its blob Y is 0.0, the player walks at
  −9.0, and ctrl001 (dest 294 "NOT USED") sits at (48,−9,64) heading east as well. The destination
  binding is independently confirmed — ctrl002 passes `entrance=2`, and East End's `+0x54[2]` = (26,58)
  is exactly where the tester lands coming from the Bazaar — so only the HEIGHT is in question. Change
  4's `blob dY` settles it in the next log; ~0 means there is nothing to fix.
- Change B (a path with an obstacle in it must never validate) still deferred — re-assess now that the
  target is the arrival rather than a point on another tier.
- Eastgate (East End has no loader to 305); the camera direction flip (accepted + documented, S56).

## Session 60 — 2026-07-23 — [nav] The `+0x84` bearing is NOT the crossing direction — refuted in play; NavTrace measures where transitions actually fire

KEYWORDS: crossing direction refuted, walked east into a wall, NavTrace, breadcrumb, TRANSITION FIRED,
walk field ASCII dump, LogWalkField, walked box, floorY -9.00 blob dY nominal confirmed, kOnExitDist,
stop asserting unverified directions

### Refuted, in one line of log

`drain seq=3: at exit (1.4m), speaking crossing dir cross=90.0deg ref=-176.4deg` → "Walk East", five
consecutive presses, camera reference stable across all five (−176.4 / −177.9 / −178.0 / −176.4 /
−176.4), and **the player's x never once passed 48.41** in the entire session (positions span
x∈[38.36, 48.41], z∈[45.68, 68.25]). The relative-frame math is correct and self-consistent with the
route legs — at ref = −176.4 the route to the exit said "North 4" and the crossing said "East", and both
check out through `CompassFaceDeg`. **The direction itself is simply wrong: the `+0x84` record's bearing
is not the direction you cross the transition.** Whatever that record is, it is not the trigger's heading.

That is the FIFTH model of the transition trigger derived from the blob and refuted on the ground:
`+0x54[N+1]` (S46) → the `+0x54`∪`+0x70` union (S55) → the nearest walkmap boundary (S58, pre-ship) →
the trigger-bearing march (S58, shipped) → the edge bearing as the crossing direction (S59, shipped).

### What the log DID settle

`__MJ_CTRL002 (48.0,0.0,56.0) floorY=-9.00 (blob dY=-9.00) cross=East` — S59's open question is
answered: **the blob's arrival Y is a nominal, not a tier.** The East End arrival is on the street at
y=−9, the same surface the player walks. There is no upper level and nothing to fix there; the walkmap
projection added in S59 is doing its job. (Muthru's North End arrival really is up: `floorY=-0.25`.)

### Stop asserting the direction

The at-exit state stays — it is what stopped the ten-press "3 steps" churn — but it no longer names a
crossing direction. It says "At the exit.", plus the relative bearing and step count when the player is
more than `kOnExitDist` (1 m) away, since standing *beside* a doorway rather than in it is a real
difference. `crossRad` is still computed and LOGGED, flagged `UNVERIFIED ... (not spoken)`, because it is
the hypothesis under test. Silence beats wrong speech.

### NavTrace — measure it instead of inferring it (`nav_trace.{h,cpp}`)

Five refuted models is a pattern, and the cause is structural: **the trigger is a script zone (`0x202d`)
whose geometry is nowhere in the blob we can read.** So the mod now measures it. A 64-crumb ring records
the player's path (a crumb per ~1 m of movement, piggybacked on the position read `PathPlanner::
OnGameFrame` already does for NavReach), and on a map change it dumps:

```
TRANSITION FIRED: mapId 290 -> 291; the trail below ends where the player crossed
==== trail of the map just left: mapId=290 crumbs=41 walked box x[38.4..48.4] z[45.7..68.3] y[-9.0..-7.8] ====
  ...
  [40] (48.41,-9.00,56.12)   <== LAST POSITION ON THIS MAP
```

The last crumb is within ~1 m of where the trigger fired — ground truth for the trigger position, on
every map, for free, and the oracle any future model must reproduce before it ships. The `walked box`
is the other half: a player who spent a minute failing to get past x=48.41 has said so in data, without
ever being asked to look at anything.

### `'` diagnostic: the walk field (`exit_diag.cpp`, `LogWalkField`)

A 21×21 ASCII picture of the walkmap centred on each exit's arrival, 1 m per cell, north up:
`#` floor at the arrival's tier, `~` floor at another tier, `.` no floor, `A` the arrival. Plus
`NavTrace::DumpTrail()` for the current map. Between them: which way out of the doorway is even open.

### Verification (tester)

1. On Muthru Bazaar, press `'` — the walk field for both exits and the trail so far.
2. **Cross into Rabanastre: East End by any means you like**, then check the log for `TRANSITION FIRED`.
   That single block is what the next build's model is built on.
3. Routing still works and no longer names a crossing direction.

### Open

The trigger model is now OPEN, not merely uncertain. It will be rebuilt from `TRANSITION FIRED` data
rather than from another blob reading. The `+0x84` edge-pairing is still correct for WHICH doorway each
controller owns (destinations verified via `entrance`), and remains shipped; only the claim about its
bearing is withdrawn.

## Session 61 — 2026-07-23 — [nav] A doorway is a PASSAGE: exit markers move into the transition; the phantom slot is dropped

KEYWORDS: DoorwayPassage, PassageEnd, passage test replaces name filter, exit marker inside the
corridor, +0x54[3] phantom, walk field corridor, kPassReach 12m, measured trigger 6.3m 8.4m,
(0,0,0) position gate, reach fill 1 cell

### What the ground truth showed

NavTrace recorded two Muthru→East End crossings: **(55.05,−6.94,65.43)** and **(56.35,−6.31,64.01)**,
both walked east out of the spawn point `+0x54[2]` = (48,−9,64). The mod was marking `+0x54[3]` =
(48,0,56). Tester: *"there was no exit there whatsoever. that is not an exit, I don't know what it is."*

The `'` walk field explains both halves in one picture (centred on `+0x54[2]`, 1 m/cell, north up):

```
z=+63  ################~~~~~
z=+64  ##########A#####~~~~~   'A' = +0x54[2]; corridor runs 10+ m east, ramping up (~ = other tier)
z=+65  ################~~~~~
z=+56  ##############.......   +0x54[3]: floor stops 3 m east. Nothing there.
```

So the walkmap distinguishes a real doorway from a dead slot perfectly well — we were just never asking
it. **A real doorway has somewhere to go.**

### Change 1 — the passage test replaces the name filter (`exit_scan.cpp`)

`DoorwayPassage()` / `PassageEnd()`: march the crossing axis (the `+0x84` trigger bearing) out of the
arrival, validating every 0.5 m step with `MapQuery::SegmentTraversable` — the same floor-continuity /
no-cliff / no-wall / body-height oracle the planner validates its own routes with. Keep the doorway iff
a body can get `kPassMin` = 4 m out; the marker becomes the far end of that march.

- **`kPassReach` = 12 m, set by measurement**: the two recorded triggers were 6.3 m and 8.4 m out, so
  12 m reaches them with margin while stopping the march wandering across open ground. Narrowness is
  NOT available as a doorway test — a Rabanastre district transition *is* open ground (tester: "it's
  literally a completely open area the character walks into") — so the length cap is what bounds it.
- **This is not the struck S58 boundary march.** That one asked "where does the floor end", which
  cannot tell a building from a map edge, and it ignored elevation so it stepped off a ledge. This asks
  "how far can a body actually walk this way".
- **Overshooting the trigger is desirable**: the map changes when the player crosses it, so a marker
  past the trigger means the route leads *through* the transition instead of stopping short of it.

**The placeholder-name filter is GONE.** It dropped any controller whose destination resolved to "NOT
USED" (ids 293/294). That deleted the real East End corridor — its blob-order label is "NOT USED" —
while keeping the phantom that had a name. An unresolvable name now costs the entry its destination
WORD, never its existence; position and direction are measured, and those are what the player walks on.

Consequence to watch: maps may list more exits than before (East End could go 3 → 6), and the
destination word on some is still bound by blob table order, which is known to mis-assign it. **Per the
tester, labelling is explicitly NOT this build's problem** — being led to the actual transition is.

### Change 2 — reject the origin position (`path_planner.cpp`)

For ~1 s after a map load the player position reads exactly `(0,0,0)` and still passes
`IsFieldNavSafe`. The reachability flood burned a whole pass on it every single transition
(`reach: fill complete -- 1 cells reachable from (0,0)`, four times in one log) and NavTrace recorded a
phantom crumb that skewed its own walked box. No real field position is exactly the origin, so both
consumers now wait a frame longer for the truth.

### Verification (tester)

1. Muthru Bazaar: the East End exit marker should now sit **in the corridor east of the spawn point**,
   not 8 m southwest of it. Routing to it should walk you east and cross.
2. The phantom at (48,0,56) should be gone — log line `NO PASSAGE (3.0m of 4.0m needed) ... dropped`.
3. East End: the Muthru exit should sit west of (26,58) — the way you actually walked back.
4. Log: `passage N.Nm from arrival (…) -> marker (…)` per surviving exit; no more
   `fill complete -- 1 cells reachable from (0,0)`.
5. **Release gate: Nalbina prologue run.**

### Open

Destination WORDS are still bound by blob table order and are known-wrong on at least Muthru and East
End. Deferred by tester direction. The verified binding channel exists (the `+0x54` slot you arrive on
is the door back to where you came from — confirmed both directions of Muthru↔East End in the S60/S61
logs) and is the basis for fixing labels once positions are trusted.

## Session 62 — 2026-07-23 — [nav] Destinations resolved by the ARRIVAL RELATION, learned from neighbour maps and persisted

KEYWORDS: exit_links, arrival relation, mapjump entrance names the NEIGHBOUR's slot, arrivalSlot,
verified destination binding, blob table order struck, map_links.txt, LOCALAPPDATA, kFormatVersion

### The problem after S61

S61's passage test fixed WHERE exits are — the Muthru phantom at (33,56) is dropped, the real corridor
survives. But the destination WORD was still bound by blob table order, so the outcome was inverted:
**the real exits read "Exit 1" / "Exit 2" while the surviving mis-bound ones carried area names.**
Tester: *"apply the destination resolver to the real exits."*

### The binding rule (the only one ever verified on the ground)

> **Map M's doorway at `+0x54` slot S leads to map D  ⟺  D's script contains `mapjump(M, S, 0)`.**

`mapjump(dest, entrance)` means "you will arrive at DEST's slot `entrance`", and the slot you arrive on
is the doorway you would walk back out of. **So a map's `mapjump` literals do not name its own doors —
they name its neighbours'.** This is S46's "arrival relation", and the S60/S61 NavTrace logs confirmed
both directions of one pair by walking them:

- Muthru's `mapjump(291, 2)` ⇒ East End slot 2 → Muthru. The tester spawned on East End at
  `+0x54[2]` = (26,58), walked west, and arrived in Muthru ✓
- East End's `mapjump(290, 2)` ⇒ Muthru slot 2 → East End. The tester spawned in Muthru at
  `+0x54[2]` = (48,−9,64), walked east, and arrived in East End ✓

### What is STRUCK, and must not come back as a fallback

Every local rule was tried and refuted: **blob table order** (labelled Muthru's dead slot "East End"
and the real corridor "NOT USED"), the routine's **`0x011E` id** (it is just `ctrlIndex + 1`), and
reading **`entrance` as a local slot** (three East End controllers claim slot 2). A doorway with no
established destination is now left UNNAMED — position and direction are measured and still spoken. A
wrong area name is what walked the tester into a wall, repeatedly; no name is strictly better.

### Implementation

- `MapScript::ExitDest::arrivalSlot` — the `+0x54` index of the paired arrival, recovered by matching
  the `+0x84` record against `+0x54` (byte-identical copies, so an exact match is exact).
- `exit_links.{h,cpp}` — the store. `NoteMapControllers(mapId, dests)` records what THIS map says about
  its neighbours; `Resolve(mapId, slot, dest)` answers for this map's own doorways.
- **Persisted** to `%LOCALAPPDATA%\FFXII-Screen-Reader\map_links.txt` — deliberately not beside the
  game, which the mod treats as read-only. Plain `<mapId> <slot> <destMapId>` lines behind a `version`
  header; a version mismatch discards the file and relearns, so a future rule change cannot be poisoned
  by links an older rule wrote.
- `exit_scan.cpp` names an exit only from `ExitLinks::Resolve`.

### Coverage, by design

Arriving anywhere names the way back **immediately** — you were just on the map whose `mapjump` names
that door. Other doorways on a first-visited map stay unnamed until the map on their far side has been
seen, and then stay named permanently. One Muthru↔East End round trip names both.

### Verification (tester)

1. Muthru → East End and back. Both maps' shared doorway should now read
   "Exit, Rabanastre: Muthru Bazaar" / "Exit, Rabanastre: East End" **on the passage that actually
   works**, not on a dead slot.
2. Log: `exit links: learned map <M> slot <S> -> map <D>` lines, and per exit
   `slot=N marker (…) -> "Exit, …"` or `[destination not established yet …]`.
3. `%LOCALAPPDATA%\FFXII-Screen-Reader\map_links.txt` exists and survives a restart.
4. **Release gate: Nalbina prologue run.** Expect forward exits unnamed until first crossed; the way
   back is always named.

### Open

First-visit forward exits are unnamed. Elimination (one unbound doorway + one unassigned destination
⇒ forced) is available but deliberately NOT implemented — the doorway/destination sets are not in
bijection once placeholders and passage-failures are removed, so it would be a guess, and guesses are
what this session removed.

## Session 63 — 2026-07-23 — [nav] Learned-cache removed (rule violation); mapctrl natives NAMED; `0x202d` struck; capture v2

KEYWORDS: ExitLinks removed, no learned cache, dbg_symbols_mapctrl, nativeId+5140, setmapjumpgroup,
0x202d struck, istouchuc, istouchucsync, setmapidmj, walkmap poly classes, Director routine,
capture v2, full 32-byte records, blob header dump

### The violation, removed

S62 shipped `ExitLinks` — a runtime store that discovered exit destinations by visiting maps and
persisted them to `%LOCALAPPDATA%`. That is against the project's rules (discovery belongs in the
decompile; the mod reads the game's own data), and `GameArchitecture.md` already specified that the
exit reader resolves "on the first frame of any map with no cross-map data, **no cache and nothing
learned by playing**". **Deleted — not kept as a fallback.** Exits keep their measured position and
crossing axis and are UNNAMED until the real resolver lands.

### The natives are now NAMED — and that broke a five-session-old "fact"

From the archived `.dbg` symbol table (`..\FFXII-Decompile\notes\dbg_symbols_mapctrl.csv`):

> **`dbgIndex = nativeId + 5140`**, anchored on `mapjump` (`0x8D` → index 5281) and cross-validated
> **15/15** against a whole `__MJ_CTRL` routine, which decodes to a coherent fade-and-jump:
> `reqenable(12) · setmapjumpgroup(K) · clearmapjumpstatus · sysucon · spotsoundtrans(40,0) ·
> fadelayer(6) · fadeprior(255) · fadeout(2,12) · setmapidmj(1,1) · ucmove ×4 · wait(12) ·
> stopspotsound · pausesestop · fadesync · wait(2) · mapjump(dest,ent,0)`

Valid in this band only; the standing "`dbg_idx − 5140` is BROKEN" warning is about index math across
the whole symbol file and still applies elsewhere.

**Consequences:**

1. **`0x011E` = `setmapjumpgroup(K)`**, not S58's "authoring-order id, no new binding info". Its value
   is `ctrlIndex + 1`, but it is the controller's identity in the engine's map-jump GROUP system.
2. **STRUCK: "the transition trigger is VM native `0x202d`" (S57).** mapctrl native ids run
   ~`0x0000`–`0x06BA`; `0x202d` (8237) is outside the table and indexes past the end of the symbol
   file. It was asserted once and then quoted as fact in three documents across five sessions.
   **No `__MJ_CTRL` routine contains a zone test at all** — all 15 natives decode and none reads
   player position.
3. The real trigger natives are named: **`istouchuc`** (`0x026D`/`0x0529`), **`istouchucsync`**
   (`0x0525`/`0x052A`), `settouchwh`, `touchradius`, `setnochecktouchheightflag`. The trigger is a
   **touch volume**, tested in the map's **Director** routine — which we had never dumped.
4. **`setmapidmj` sits beside `setmapidfloor` / `setmapidwall`** ⇒ the WALKMAP's polygons carry ids and
   one class is the MAP-JUMP surface. `WALK_POLY_STRIDE` is `0x20`; `ReadCellFloor` reads bytes
   `0x00–0x11` and uses only the **low 3 bits** of the flags word. The upper 29 bits and the
   `0x12–0x1F` tail are unread — the leading candidate for a first-frame, local, no-cache trigger.

### Capture v2 (`'` key)

- **Whole 32-byte records** of `+0x54` and `+0x84`, hex + the tail as u32s. Every previous capture read
  only the first 16 bytes, so half of every record in both tables had never been seen.
- **Blob header words** `hdr+0x00 … +0xC0`, each with its first word, so an unopened table announces
  itself by starting with a plausible count.
- **Director routines** dumped (any routine whose name contains "director", case-insensitive; matched
  on the suffix, never a map id) at the full span cap, plus a one-line index of **every** routine so
  nothing stays invisible again.
- **Natives annotated BY NAME**; unknown ids print as raw hex rather than inventing a label — which is
  exactly how `0x202d` became a fact.
- **`LogWalkPolyClasses`**: sweeps the walkmap grid, groups floor polys by their FULL flags word, and
  reports each class's count, sample positions and unread `0x12–0x1F` tail. Ground truth makes the
  decode a glance: on Muthru the East End transition fires at ~(56, 64.3), so whichever class has
  members there is the map-jump surface. Bounded at 200k prim reads, one-shot.

### What the tester needs to do

Press `'` once on **Muthru Bazaar** and once on **Rabanastre East End**. That single capture is what
the destination resolver gets built from.

### Open

Exits are unnamed in this build by design. `exit_diag.cpp` is at 464 lines — plan a split before it
grows further.

## Session 64 — 2026-07-23 — [nav] SOLVED: transitions are walkmap surfaces tagged with their map-jump group

KEYWORDS: map-jump surface, setmapidmj, WALK_POLY_MJ_SHIFT, setmapjumpgroup, ExitDest::group,
ReadMapJumpSurfaces, transitions vs doors, 305 teleport not a door, six models retired

### The rule

> **A walkmap floor poly whose flags carry a non-zero value above the 3 type bits is a MAP-JUMP
> SURFACE. `group = (flags >> 3) & 0x1F` is the map-jump GROUP id. The `__MJ_CTRL` routine calling
> `setmapjumpgroup(K)` with `K == group` owns it, and that routine's `mapjump` literal is where it goes.**

One routine, both halves — geometry and destination — so they can never be mismatched again. Local,
first frame, no cross-map data, no cache, nothing learned by playing.

Verified against every crossing NavTrace has ever recorded: Muthru group 3 → 291 East End at
(50–56, 60–65) vs crossings (55.1,65.4)/(56.4,64.0); East End group 4 → 290 Muthru at (11,54) vs
crossing (19.7,58). North End lands north on both maps, Southern Plaza south. Muthru's two "NOT USED"
controllers have **no surface at all** — the map has nothing to walk onto for them.

### Transitions and doors are different systems

The tester has said this since Session 56. This is the mechanical proof:

| | TRANSITION | DOOR |
|---|---|---|
| lives in | walkmap poly tagged `setmapidmj` | scene object, `kind == 4` |
| fires when | you walk onto it | you press Enter |
| named by | the owning `__MJ_CTRL`'s `mapjump` | a `+0x70` field sign |
| read by | `exit_scan.cpp` | `entity_scan.cpp` |

**Every refuted model looked for transitions in door-shaped places** — `+0x54[N+1]`, the
`+0x54` ∪ `+0x70` union, field-sign pairing, the `+0x84` edge bearing, blob table order, and both
geometric marches. Transitions were never in the blob's position tables. The two readers stay separate.

### Shipped

- `MapScript::ExitDest::group` — parsed from `4f <K> 5d 1e 01` (`setmapjumpgroup`) in the routine.
- `MapQuery::ReadMapJumpSurfaces` — one walkmap sweep, grouping tagged floor polys into
  `{group, centroid, bbox, polyCount}`; a poly contributes its base vertex, not a grid cell (East End's
  cells are 8 m — far too coarse to aim at a doorway).
- `NavRva::WALK_POLY_MJ_SHIFT/MASK` beside the existing `WALK_POLY_TYPE_MASK`, layout documented.
- `exit_scan.cpp` rewritten: position = the surface centroid (Y snapped to the floor), destination =
  the routine's own literal. **Two drop rules, both from the game's own data:** no surface for the
  group → nothing to walk onto; destination name unresolved → the game itself says "NOT USED", so it
  leads nowhere. Every listed exit now has a real surface *and* a real name.
- `DoorwayPassage`/`PassageEnd` deleted; `crossRad`/`hasCross` retired to `Entity::isTransition` (the
  route now ends ON the trigger, so there is no crossing direction left to derive — and the one we
  shipped in S59 was refuted anyway).

### Also settled

Map **305 (Eastgate)** is in the Director's `mapjump(…, 0, 0x0A)` list — `flags == 0x0A` is the
world-map teleport menu, not a walk-through door. There was never a door to find. The Director routines
are that teleport list and nothing else; no further script decoding is needed.

### The lesson

`group` lives in a flags word `ReadCellFloor` has been reading for thirty sessions and using **three
bits of**. Six models were invented to replace data already sitting in memory, unread. **Dump the whole
record before inventing a model** — every unread byte of a structure you already parse is cheaper to
look at than one hypothesis is to test. Same still applies to `+0x54`/`+0x84` (stride 0x20, 16 bytes read).

### Verification (tester)

1. **Muthru Bazaar: exactly 2 exits** — "Exit, Rabanastre: North End" (north) and
   "Exit, Rabanastre: East End" at ~(50–56, 60–65). Route to the latter; walking it must cross.
2. **East End: exactly 3 exits** — "Exit, Rabanastre: Muthru Bazaar" far west (~x 11–20, z 54–58,
   where you actually crossed), North End north, Southern Plaza south.
3. **Every listed exit is named.** A bare "Exit" now means something is wrong.
4. Both correct on the **first frame** of a freshly loaded save, no prior visit, no file on disk.
5. Log: `__MJ_CTRL### group=N -> "<name>" (id) at (x,y,z) | P polys, box x[..] z[..]`. A future
   `TRANSITION FIRED` position must fall inside the box of the exit claiming that destination.
6. **Release gate: full Nalbina prologue run.**

### Session 64 follow-up — confirmed working in play, committed, plus one open cosmetic issue

The transition reader is **confirmed by the tester in play** and committed (`27fc6d3`, branch
`combat-system`), covering Sessions 54–64.

**Open, logged not fixed:** North End announces `Interactables. North, 10 steps (below)`. That is the
category word being spoken as a proper NAME for a scene object the game itself leaves anonymous
(`kind=4`, `nameIdx=-1`, empty sign string, `act`/`talk` both `0xFFFF`; in-game it reads `???` /
"(You're not sure what this sign is for.)"). Full evidence and the ready fix are in `debug.md` →
Known Issues. It breaks four separate speech rules, so it should land before release, but it is
independent of the exit work and was deliberately kept out of that commit.

## Session 65 — 2026-07-23 — [nav] Focus clamp, stable NPC numbers, player labels (F6), sign naming

KEYWORDS: focus clamp, CursorMatch label identity bug, entity_labels, stable duplicate numbers,
F6 clipboard label, kEntityGraceMs, Sign, entity_commands split, NVDA no longer blocked

### The focus bug — one cause, two symptoms

Tester: *"I was trying to track Rabanastran 7, but it kept disappearing and dropping back to
Rabanastran 5."*

`NumberDuplicateLabels` assigned the suffix as an **ordinal within the group the current scan saw**, and
`CmdNext` rebuilds the whole list on every keypress. The log shows the settled map oscillating
**NPC=14 ↔ NPC=15 across 118 rescans** — one NPC flickering in and out of the handle table renumbered
everyone after it. That alone is cosmetic; the damage came from `CursorMatch` requiring
`e.label == g_cursor.label` in **both** match tiers, so a renumber made the focused NPC unrecognisable,
`FindFocusInViewLocked` returned −1, and `CycleLocked` restarted at `view[0]` — the nearest.

Sorting was already by distance and there is no distance or count cap anywhere in `entity_scan.cpp`:
the list churned, it never truncated.

### Fixes

1. **Focus identity is the OBJECT.** `CursorMatch` exact-tier is now `e.sceneObj == g_cursor.obj`
   (fixed exits match on `nameIdx`, which encodes the controller). The re-lock tier keeps the
   name/label/category test for when the pointer genuinely changed. A nearer entity can no longer
   change anything the cursor looks at, so it cannot steal focus — the clamp the tester asked for.
2. **Grace window** (`kEntityGraceMs` = 2 s). `RescanLocked` carries over an entity that has only just
   stopped being reported, and `RefreshPositionsLocked` no longer deletes on a single failed transform
   read. A real despawn still leaves; streaming noise removes nobody.
3. **Stable numbers** (`entity_labels.{h,cpp}`). The suffix is assigned ONCE per object and kept —
   across rescans, streaming, reloads and sessions. Keyed on `mapId · container · slot`, with `nameIdx`
   stored as a validation field so a reused slot is logged and ignored rather than mislabelling a
   stranger.

### Player labels — F6

The player names anything the game leaves anonymous ("gate guard"), and **a label outranks everything**:
game string, duplicate number, category word. Applied before duplicate numbering, so naming one NPC
does not renumber the rest.

Entry is by **clipboard**, not typing: the mod passes the DirectInput buffer to the game as `const` and
never swallows a key, so there is no way to run a text field in-game without breaking the read-only
input rule. Type the name anywhere, copy, focus the entity, press **F6**; an empty clipboard clears the
label. Stored in `%LOCALAPPDATA%\FFXII-Screen-Reader\entity_labels.txt`, plain text and hand-editable,
reloaded on every area change, behind a `version` header so a key-scheme change discards cleanly.

**Not the Session 62 mistake.** That store had the mod *discover game facts by playing* and present them
as truth. This one holds the player's own words and which ordinal we already handed out — presentation
state we authored, never a claim about the game. Every game fact is still read fresh every scan.

### Sign naming

An unnamed object carrying a `+0x70` field-sign record (`Entity::doorway`, set by
`setfieldsignlocationjumpinfo`) **is a sign** and is now labelled "Sign" instead of the category word
"Interactables". Shop doorways carry the same record but resolve a real name, so they are untouched.
North End `[0:19]` is covered; `[0:15]` (`door=0`, `act`/`talk` both `0xFFFF`) is still unidentified,
**stays listed** per instruction, and can be named by the player with F6 meanwhile.

### Housekeeping

`entity_list.cpp` hit 575 lines. Split into `entity_list_internal.h` (shared private state + helpers),
`entity_list.cpp` (392 — the list and the per-frame tick) and `entity_commands.cpp` (206 — the hotkey
commands over it).

### Documentation corrected

The tester confirmed **NVDA's own key commands now work while the game runs**, contradicting a
long-standing note in `Controls.md` and `GameArchitecture.md` that they "remain blocked under the game's
exclusive grab". Both updated to record the observation and its date. **The cause is deliberately NOT
stated** — the tester suspects a mod-side issue since fixed, and nobody has traced it; it is written as
an observation, not a mechanism. The read-only input rules are unaffected.

### Verification (tester)

1. Focus a distant Rabanastran, walk past nearer NPCs, press `/` repeatedly — it must stay on them.
2. Note a number, walk away, come back, **quit and reload** — same number.
3. Cycle repeatedly while standing still — no entry may vanish between presses.
4. Copy "gate guard", focus the NPC, press F6 → confirms, and it keeps that name after a reload.
5. North End announces "Sign", not "Interactables".
6. **Release gate: full Nalbina prologue run.**

## Session 66 — 2026-07-23 — [nav] Eastgate found: it is off SOUTHERN PLAZA. Progress unblocked.

KEYWORDS: Eastgate 305, Southern Plaza 292, East End has no loader to 305 CLOSED, progress unblocked,
ready for shotgun build

The tester found the east gate **in the Southern Plaza**, and progress is unblocked.

That closes an item open since Session 55: *"Eastgate — East End has no loader to 305."* It was never a
defect. **East End genuinely has no door to 305 because the door is on Southern Plaza (292).** The exit
reader was right every time it said so; the search was on the wrong map.

**Corrects Session 64.** That entry concluded "305 appears only in the Director's teleport list …
there was never a door to find". The teleport-list half is right (`flags == 0x0A` is the world-map menu,
not a walk-through transition). The second half was too strong — there IS a walk-through transition to
305, on a map we had not looked at.

**The generalisable lesson:** *"this map has no door to X"* is a statement about **that map**, never
about X. Several sessions went into hunting a missing door that was simply somewhere else. When a
neighbour is expected and absent, check the adjacent maps before doubting the reader.

No code change — this is a finding. The exit work that made it findable is `27fc6d3` (transitions from
walkmap map-jump surfaces) and `826c25f` (focus clamp + player labels).

**State:** working tree clean, all navigation work committed on `combat-system`. Next session opens with
the release build; the **full Nalbina prologue run is still the gate** for it.

## Session 67 — 2026-07-23 — [release] 0.1.1-shotgun-build: F6 naming + the wall-camera flip documented

KEYWORDS: release 0.1.1-shotgun-build, ReadMe F6 clipboard labelling, F5 availability filter, U License
Points, wall camera flip known issue, NVDA keys known issue withdrawn, License Board no longer "not
implemented", four-file zip, Tolk x64 8664

Release prep only — **no code change**. `dinput8.dll` built fresh from `661432d`; the tree was clean
before and after.

**ReadMe, asked for:**
- **F6** now has its own section: numbering of duplicate names, copy-then-select-then-press, that the
  name replaces the game's everywhere the mod speaks it, empty clipboard clears it, names persist in
  `%LOCALAPPDATA%\FFXII-Screen-Reader` (not the game folder), and that **map exits cannot be named** —
  they come from the map script, so there is no object to key on and F6 is silent. The clipboard is
  explained as a *consequence of the read-only input rule*, not as a workaround.
- **The wall-camera flip** as its own Known Issue. The general camera paragraph (added at 0.1) already
  described the mechanism; the tester meets it in alleys and doorways, so it now names walls as the
  common cause and says what to do — step off the wall, re-press the directions key.

**ReadMe, corrected while in there (both would have shipped as false):**
- *"The License Board … not implemented yet"* — implemented in Session 53. Dropped from the list.
- *"NVDA's own keyboard commands do not work while the game has focus"* — **withdrawn**, per Session
  65's play confirmation. `Controls.md` and `GameArchitecture.md` were corrected then; the
  player-facing file was missed, and it is the one that would have told a blind player their screen
  reader keys do not work. The readme states the observation and explicitly does **not** assert a cause.

**ReadMe, gaps filled:** `F5` (availability filter) and `U` (License Points) shipped in Sessions 54 and
53 and had never reached the readme. `F4` is left out deliberately — it is a dev A/B toggle, not a
player feature.

**Artifacts:** `Releases\V0.1.1-shotgun-build\` + `Releases\FFXII-Screen-ReaderV0.1.1-shotgun-build.zip`,
four files, zip root flat. All three DLLs verified PE machine `8664`. TTS pair carried over from
`V0.1-shotgun-build`. No tag, no push — the user ships by hand.

**State:** working tree clean on `combat-system`; two readme commits (`548086f`, `661432d`). The **full
Nalbina prologue run remains the gate** — this release is built, not validated in play.

## Session 68 — 2026-07-24 — [nav] Pathfinder elevation: root cause = STEP-DISCONTINUITY (no slope limit); tight edge test shipped

KEYWORDS: pathfinder elevation cliff pit ledge impassable not moving kMaxStep kStepDiscont kEdgeSubStep
step discontinuity walk-type flags FUN_0022cc50 FUN_00231900 FUN_0033bc80 0.3 step no slope limit
GroundInfoAt route-profile diagnostic Dalmasca Estersand Rogue Tomato slope gate dropped

**The S67 progress-blocker, root-caused and fixed (pending one tester round).** On elevation-varied field
terrain the pathfinder routed the player onto a descent they could not traverse (char holds the stick,
footsteps, no movement). Four prior fixes failed; the last (a 0.6 m / ~50° dense edge check) left the
route unchanged.

**RE result that reframes it (first-hand decompile trace, ~0.9):** the FIELD walkmap movement has **NO
walkable-slope limit.** `FUN_0022cc50` (the move-across-walkmap per-poly handler) decides walkability
purely from baked **walk-type flags** (poly+0xC & 7: 0 walkable, 1/4 conditional; walls block) — no
cosine / normal.y / angle threshold anywhere. `FUN_00231900`/`FUN_00231890` only guard `B > 0.001`
(a divide-by-~0 guard, not a slope cap). Because the player can walk **any continuous slope**, stairs and
hills work. The only geometric movement blockers are (1) **walls** — `SegmentClear` (mask=4) already
tests them, and they are CLEAR along the impassable descent — and (2) a **step-height DISCONTINUITY**:
`FUN_0033bc80:23-28` samples `GroundAt` ahead and reacts when `ABS(groundY − currentY) >= 0.3`
world-units. So the real limit is a ~0.3 m step, and our A* `kMaxStep = 1.5` was ~5× too loose — it
stitched routes across a 0.3–0.6 m ledge/lip (invisible to the floor+0.9 m wall feeler and to the loose
0.6 m dense check).

**DROPPED (do not retry):** the poly-normal **slope gate** (reject cells whose floor poly is too steep).
The RE null result proves a slope cap would be **wrong** — it would reject continuous slopes the player
can legitimately walk. The fix is a step-**discontinuity** test, not a slope test.

**Shipped (built + deployed; combined fix + diagnostic per user choice):**
- `path_search.cpp` `passable()`: replaced the loose dense-edge block with a **fine step-discontinuity
  check** — sub-sample `GroundAt` every `kEdgeSubStep = 0.25 m` along any edge with a real height change
  (`> kStepTrigger = 0.15 m`) and reject a sub-step that JUMPS more than `kStepDiscont = 0.35 m` (a
  ledge; a continuous slope / ramped staircase passes). `kMaxStep = 1.5` kept as the coarse cliff gate so
  continuous slopes survive. Flat/near-level edges skip it (city fast-path).
- Same cap applied to the string-pull validator (`SegmentTraversable(kEdgeSubStep, …, kStepDiscont, …)`)
  so the smoother can't straighten a leg back across a rejected ledge.
- `map_query.{h,cpp}`: added `GroundInfoAt(x,z,&y,&cosSlope)` (diagnostic-only; centralized the poly
  scan of `ReadCellFloor` into a shared `ScanTopFloorAt`).
- `path_planner.cpp`: added `LogRouteProfile(rawPoly)` — dumps the ROUTE's OWN per-leg max sub-step ΔY +
  floor-Y span + poly slope (the directline/route-field dumps only sample the STRAIGHT line, missing the
  route's descent). `NAV-ROUTE route-profile leg N: … maxStep=X.XXm … WORST step=…`.

**`kStepDiscont = 0.35` is PROVISIONAL** — not shipped as fact (the `0.3` role in `FUN_0033bc80` is only
~0.5 conf). The route-profile dump brackets the real value: the descent's WORST step (the tester can't
follow) vs the WORST step on a route they DO walk (city/stairs). Tune in one round if needed.

**NEXT (tester round):** on Dalmasca Estersand "The Stepping", route to the Rogue Tomato (`\`/`p`) and
walk it; also route a couple of Rabanastre targets incl. across a staircase. Read `NAV-ROUTE`:
route-profile WORST step on the tomato route (the ledge height) vs the city/stairs route (walkable bound
+ whether stairs are ramped). Confirm the tomato route no longer jams (or NoPath if no walkable descent)
and city/stairs still route. Adjust `kStepDiscont` and rebuild once if the bracket says so. Then move
S67 pathfinder from Known Issues → Solved and resume the deferred shops/gambits menu plan.

## Session 69 — 2026-07-24 — [menu] Shop reader (Buy/Sell/Bazaar: name+price+inventory + quantity/step) + `g` gil key — SHIPPED, play-confirmed

KEYWORDS: shop buy sell bazaar quantity selector 1x 10x step inventory owned count gil g key sideways
FUN_0056e5d0 FUN_0056d370 FUN_0056e140 FUN_0057b890 FUN_00253690 shop_reader gil_reader panel container
highlight price total DAT_02092758 DAT_02ca9790 DAT_02ca9798 0x2B89790 0x2B89798 0x1F72758

**SHIPPED + confirmed in play (user: "works perfectly").** Shops now read on highlight, the quantity
selector vocalizes, and a new `g` key speaks gil.

**Item reader (`src/ui/shop_reader.{h,cpp}`).** Buy, Sell AND Bazaar share ONE class pair (built by
`FUN_0056dd50`): container `FUN_0056e140` (0x44E140) holding item panel `FUN_0056d370` (0x44D370). Hooking
the container highlight/refresh handler **`FUN_0056e5d0` (0x44E5D0)** covers all three (it also sets the
tooltip → why `o` already worked on Sell). Speaks "‹name›, ‹price› gil, ‹owned› in inventory": name codec
@ row+0x00, id @ +0x08, owned/INVENTORY (u16) @ +0x0E, price (u32 &0x7FFFFFFF) @ +0x10, stride 0x20, row
array @ panel+0xC8; idx from the scroll grid @ container+0xD8 (`(s8)+0xED + ((s16)+0xF4 + (s16)+0xF2) *
(u8)+0xEC + (s8)+0xEE`). Change-guard on (container, itemId) — the sanctioned no-dedup exception naming
FUN_0056e5d0 (per-focus redraw fires ~2×); resets on container change so re-entry re-announces.

**STRUCK (offline inference):** "Sell reuses the Buy 0x8000 read path." It does NOT — the sideways Sell
grid refreshes via the container on msg 0x13, never `FUN_00247510` 0x8000, so a 0x8000-only reader saw Buy
but never Sell. The single `FUN_0056e5d0` hook is the correct point for both.

**Quantity selector (2nd hook: panel `FUN_0056d370`, per-frame).** After picking an item: `panel+0xE4`
bit1 = quantity mode; qty (u16) @ +0xDC, max @ +0xDE, gil snapshot @ +0xE0, selected row @ +0xD0; total =
(price&0x7FFFFFFF)*qty. Announces "‹qty›, ‹total› gil" on entry + each change; Left-arrow +1/+10 step =
`panel+0xE4` bit **0x400000** → "1x"/"10x" (**PROVISIONAL**: one live sample, play-confirmed). Per-frame
change-guard on (qty, step) naming FUN_0056d370; resets on leaving qty mode.

**`g` gil key (`src/ui/gil_reader.{h,cpp}` + input wiring).** "‹N› gil" anywhere a save is loaded, silent
on the title screen. Mirrors getter `FUN_00253690` = `*(u32*)(DAT_02092758+8)` as a pure memory read
(base = *(RVA 0x1F72758); gil = *(u32)(base+8)) — no game call, safe off the game thread like `U`.
DIK_G=0x22 → WM_GIL → `GilReader::Announce`. `g` free in game + mod bindings.

**RVA correction:** shop controller `DAT_02ca9790` = RVA **0x2B89790**, container `DAT_02ca9798` =
**0x2B89798** (the agent's "0x1AA9790" was bad arithmetic; abs−0x120000). Gil base 0x1F72758 verified live.

Probe archived `../FFXII-Decompile/frida/probe_shop.js`; RE notes
`../FFXII-Decompile/notes/shop_sell_re_2026_07_24.md`. Files: shop_reader.{h,cpp}, gil_reader.{h,cpp}
(new); input_tracker.{h,cpp}, menu_reader.cpp, CMakeLists.txt.

## Session 70 — 2026-07-24 — [menu] Inventory item QUANTITY + CATEGORY switching

**KEYWORDS: inventory items quantity row+0x0E category tab switching left right L1 R1 0xCA0
tabCount<2 FUN_005655f0 0x4455F0 FUN_00563ec0 0x443EC0 FUN_00564300 0x444300 FUN_00564e10 0x444E10
win+0xE0 win+0x180 win+0xE8 FUN_002f9860 probe_inventory KEY-ITEMS LOOT struck-equipped-count**

**SHIPPED (built + deployed, awaiting play test).** New `src/ui/inventory_reader.{h,cpp}`;
`shop_reader` gains `OwnsSurface`; `menu_reader.cpp` wiring; CMakeLists.

### The one family

Pause item lists, the Equipment list and the Shop are **ONE tabbed-container family** — same row
record, same `+0x180` bitfield, same two nav primitives, same refresh. Four window classes seen
(`0x4436F0` weapons/armor/accessories, `0x443930` **ITEMS and LOOT**, `0x443B10`
magicks·technicks, `0x443D20` **KEY ITEMS**) and **one class serves several screens**, so the reader
claims a window by STRUCT SHAPE (`+0xE0` rows, `+0xD8` scroll, `+0xE8` tab table, sane row count) —
never by a class table.

### Item quantity — `row+0x0E`, array at `*(win+0xE0)`, stride 0x20

The **same record `shop_reader.cpp` already decodes**; the shop just holds it at `panel+0xC8` and
`FUN_0056e410:54` copies `container+0xE0` across. Name codec `+0x00`, id `+0x08`, quantity `+0x0E`.
Probe-confirmed: `Potion 5`, `Rat Pelt 3`. **Item id `0x0` is VALID** (Potion) — only `0xFFFF` is the
empty sentinel.

### STRUCK — `row+0x0C` is NOT the equipped count

Offline it was labelled "equipped count (equipment lists only)". **REFUTED in play by the tester:**
the probe logged `Dagger qty=1 equipped=0` while that Dagger **was equipped to Vaan**. Every
equipment row observed had `+0x0E==1, +0x0C==0`, so nothing discriminates the candidate meanings.
The field is **not read and not spoken**. Settling it needs a save owning ≥2 of one equipment item
with some equipped and some spare. (`FUN_0057dad0:26-32` sets it from `master+0x20`;
`FUN_00563560:54-59` draws `+0x0E − +0x0C` and `+0x0E`.)

### Category switching

`FUN_00564e10:24` masks the pad with **`0xCA0` = L1|R1|LEFT|RIGHT** — LEFT is hard-aliased to L1 and
RIGHT to R1 (measured live: `held=0x20(RIGHT)`, `held=0x80(LEFT)`). Two REAL reasons it goes silent,
both the game's own behaviour, not a mod gap:
1. **`FUN_00564e10:22-23` returns early when tab count < 2.** This exactly explains the tester's
   report — ITEMS 1 tab (silent), MAGICKS·TECHNICKS 2 (active), WEAPONS 2 / ARMOR 3 (active),
   KEY ITEMS / LOOT / ACCESSORIES 1 (silent).
2. `win+0x180 & 0x100` (use-on-target sub-mode) routes to `FUN_00564d30`, which has no L/R handler.

`win+0x180`: bits[20:16] = tab COUNT, bits[25:21] = tab INDEX. Prev/next = `FUN_00563ec0`
(0x443EC0) / `FUN_00564300` (0x444300) — **a CLOSED SET: exactly 3 call sites in the whole 47 MB
dump** (items `FUN_00564e10`, equipment `FUN_003fdad0` 0x2DDAD0, shop `FUN_0056ded0` 0x44DED0).

### The hook: `FUN_005655f0` (0x4455F0), on ENTRY

Fires on screen **OPEN and on every category change**, all three families. Hooked on **entry**, for
two reasons: `FUN_00564010:56-70` populates the tab table and `+0x180` *before* calling it, and the
original's `FUN_002d47c0:15-20` re-fires `FUN_00247510(child, 0x8000, idx)` *during* the call — so
announcing first gives **category, then item**. (On exit the order inverts; the probe log proves it.)

Name = `FUN_002f9860(*(u32*)(*(win+0xE8) + srcIdx*8 + 8))`, `srcIdx = *(s8*)(win+0xF6+tabIdx*8)`
clamped at 0. **Two independent read paths agreed 14/14** in the probe. The reader prefers
`TextCapture::StringById` (already cached from the game's own resolver) and only calls the getter
the first time an id appears.

**This hook is also what makes a ONE-ITEM list speak at all** — entering it moves no cursor, so a
0x8000-only reader is silent. Tester caught this; it would have shipped as a bug.

### Confirmed: empty categories never occur

Every `[cat]` line reported `rows=yes` with n≥1, and tab lists are filtered live per screen state
(ARMOR 3 tabs, ACCESSORIES 1) — the `gateId` at `entry+6` removes empty categories before they are
tabbed. Tester confirmed independently. **The "speak the category alone" branch was designed and
then dropped; do not implement it.**

### Speech

Row = `<name>` or `<name> <count>`, the count spoken **only above 1** (a row exists only if you own
≥1, so a bare name means exactly one — this is what keeps Key Items and Magicks from reading "… 1").
**No comma.** Category spoken first, item **queued** behind it (`Speech::Output(line, false)`) — not
a dedup, it suppresses nothing; without it the item's interrupt cuts the category off mid-word.
Left/Right on a 1-tab screen: **silent**, matching the game.

`ShopReader::OwnsSurface()` added so the two readers never both announce the same shop row.

Probe `../FFXII-Decompile/frida/probe_inventory.js`, log `../FFXII-Decompile/notes/probe_inventory.log`,
RE notes `../FFXII-Decompile/notes/inventory_qty_category_re_2026_07_24.md`.

## Session 71 — 2026-07-24 — [menu] Status screen reader: wrong hook STRUCK, real controller found, probe authored

**KEYWORDS: status screen reader wire up FUN_0057edb0 0x45EDB0 save load pane STRUCK FUN_002c2320
0x1A2320 status equipment shared container cmd 0x4b4 0x4b6 ctrl+0x160 ctrl+0x168 bit0 menuCtx+0x140
menuCtx+0x138 FUN_003fe5d0 attribute labels FUN_002f9860 0x4A90 member block 0xAC8 blk+0x90 EXP
blk+0x94 next blk+0xB0 LP blk+0xBA level probe_status_data.js virtual_buffer Home End combat log**

**Task:** "wire up the status screen reader" — `status_reader.{h,cpp}` existed but was not in CMake
and `StatusReader::Init()` had zero callers.

**The wiring was never the blocker — the reader hooks the wrong function.** `status_reader.cpp:27`
targets `0x45EDB0`, which is the **save/load file-detail pane**: `FUN_00583040` creates it only after
a 200-slot save-table scan, as a sibling of the slot list `FUN_0057fe80`, and its seven sub-panels
read a 4-byte packed save-preview record. Full strike in `debug.md`. Adding it to CMake would have
shipped a reader that fires on the save screen. Its `CAT_SHOW = 0x13` has no case in that function
either — copied from `FUN_00280de0`.

**Real chain found offline (≥0.98):** Status = pause command `0x4b4` → `FUN_00281ed0` →
**`FUN_002c2320` (RVA `0x1A2320`)**, the container **shared with Equipment** (`0x4b6`) and
distinguished by `*(int*)(ctrl+0x160)` / `ctrl+0x168` bit0. Selected character `menuCtx+0xDE0`;
member block `*(menuCtx+0xAC8 + member*8)`; nine attributes at `panel+0xC8+row*4` off `menuCtx+0x138`.
Details + category map in `GameArchitecture.md` § "Status screen".

**Two findings that shrink the work:** (1) the five vitals offsets marked PROVISIONAL in
`status_reader.cpp:48-52` are **correct** — two independent witnesses (`FUN_00283e40` draw,
`FUN_00329220` fill); (2) the member block already caches EXP `+0x90`, Next `+0x94` (the game itself
stores `FUN_002f8f20(level) - EXP`) and LP `+0xB0`, so **the save-record read can be deleted** and no
EXP curve needs reimplementing. (3) The nine attribute **labels are game-supplied** —
`FUN_002f9860(0x4A90 + row)` — so no English needs hardcoding.

**Probe (two runs, FRIDA-FIRST).** `frida/probe_status_data.js` rewritten against the new chain and
moved out of `archive\` (the launcher auto-discovers `frida\*.js`, so the move *is* the
registration); the superseded copy is kept as
`archive/probe_status_data_SUPERSEDED_wrong_hook_0x45EDB0.js`. Run 1 confirmed the controller, the
Status/Equip gate, all 15 page-1 values and all 9 labels. Run 2 (rewritten to cover the other two
pages) delivered everything else. Console capped at 150 lines; dumps file-only to
`notes/probe_status_data.log`.

**THE STATUS SCREEN HAS THREE PAGES**, selected by `menuCtx+0xDE7` (0 Attributes / 1 or 3 Magicks /
2 Technicks-Quickenings-Remedy-Espers). Pages 2/3 are the *same two page objects* the license board's
`F` overlay uses, and are parked in menuCtx: Magicks at `+0x120` (class `+0x1A3560`, 81 slots),
abilities at `+0x128` (class `+0x1A4460`, 54 slots). Section headings decode live as **Technicks /
Quickenings / Remedy Lore / Espers** — *Quickenings*, not "Mist" as `ability_summary_reader.h`
claimed.

**STRUCK: ~~"all three pages are static displays with no browsable cursor, so enumerate 2/3 into the
buffer"~~.** Pages 2/3 have a real in-game cursor here, exactly as on the license board (tester). A
three-buffer enumeration was built, shipped and then **removed the same session**; do not rebuild it.
`status_reader` now covers **page 1 only** and declines every key while `menuCtx+0xDE7 != 0`, so the
game's cursor is never fought; `ability_summary_reader` speaks 2/3 unchanged from the board path (its
`StatusReader::IsActive()` stand-down was removed with the buffers).

**FIXED, same session: the section heading now announces on every CROSSING**, not only on a page
switch. `ability_summary_reader` tracked `g_sumOwner` only, so cursoring from Technicks into Remedy
Lore said just "Blind" — the tester read that as the mod reporting the wrong thing. It now also
tracks the game's own section index (`obj+0x7A4`) and re-announces the heading when it changes. The
old ~~"re-announcing on every crossing was noise"~~ note is STRUCK: it fires once per crossing, not
per row, and the section is what makes a bare status name mean anything.

**SHIPPED + deployed.** `status_reader.{h,cpp}` rewritten and added to CMake; wired into
`MenuReader::Init/Shutdown`. Page 1 groups: Character (name, Level, HP, MP, LP, EXP, Next) / the nine
Attributes with the game's own labels / Status effects. New shared header
**`src/ui/ability_entry.h`** holds the entry layout + decode rules once, used by both readers.

**Two things deliberately NOT done.** (1) `"(No status effects.)"` is **layout art, not a message
id** — `FUN_002c5900` merely hides the rows — so it cannot be read back; the group is omitted when
empty rather than fabricating a line. (2) LEVEL/HP/MP/LP/EXP/NEXT are drawn as art too and stay
mod-emitted under the CLAUDE.md gauge-label carve-out; every *value* is live game data.

**Robustness:** `OnMenuNavKey` re-validates that our container is still parked at `menuCtx+0x140`
before consuming a key, so a missed `0x12` teardown can never leave the buffer holding Home/End for
the session. Buffers are pre-rendered snapshots built on the game thread; the input thread does two
guarded memory reads and touches nothing else.

**User decisions this session:** the status buffer **claims Home/End** while the page is open — a
deliberate, user-instructed exception to the "combat log usable everywhere" requirement, now recorded
in `Controls.md`. Left/Right stay as group nav (user: the game has no arrow-key function on this
screen).

**STRUCK, same session — "there is no event for mode 2→0, pressing a nav key reports position".**
That claim was wrong and was called out immediately: a game does not back out of a submenu and then
sit stale waiting for input. The event exists and is **`FUN_002c1a80` (RVA `0x1A1A80`)**, the overlay
page state machine and the *only* writer of `menuCtx+0xDE7` — `:24` sets 1 (Magicks), `:42` sets 2
(Technicks), **`:87` sets 0 = closed back to the Attributes page**. Its return value classifies the
event: **2 = dropped back to page 1**, 1 = opened/switched/consumed, 0 or −1 = idle. The reader now
hooks it and acts only on `ret == 1 || ret == 2`, so the idle per-frame path is one integer compare
and every page change — including the back-out — rebuilds and announces immediately. The two
page-focus hooks were dropped entirely; the page objects come from their parked pointers.

**Process lesson:** the evidence was already in the probe log — run 2's category list gained
`0xf, 0xc, 0x14` that run 1 never had, and run 2 was precisely the run where the overlays were opened
and closed. The `[mode]` transition lines were themselves emitted from *inside* the container's own
handler, which alone proves the container is notified. The conclusion "no event" was asserted without
checking data already in hand. **When about to claim a game does not fire an event, grep the trace
first.**

**Next:** play-test all three pages, including backing out of a summary page onto the Attributes
page (should re-announce the first entry) and out of Status entirely.

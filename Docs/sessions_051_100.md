# FFXII-Screen-Reader — Session Log (Sessions 51–100)

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

**CONFIRMED IN PLAY (tester, end of session): "it works perfectly as intended."** All three pages
read correctly — the Attributes buffer on the arrow keys, the two cursor pages via
`ability_summary_reader` with the section heading now announcing on every crossing, and the
back-out to the Attributes page re-announcing off `FUN_002c1a80`. Committed `ed67c4c`.

**Left uncommitted on purpose** (other tracks' work, per the one-track-per-commit rule):
`src/ui/ingame_menu_reader.cpp` (S67 `o` stale-in-field fix) and the S67/S68 pathfinder + `o`-fix
entries in `Docs/debug.md`. `debug.md` was committed via the reset-to-HEAD / apply-only-mine /
restore dance, so only the S71 strike went in.

## Session 72 — 2026-07-24 — [nav+combat] Ground-loot Items category; enemy "begins casting" realtime; EXP/LP on the defeat line

**KEYWORDS: ground loot drop pool DAT_02ec0fa0 0x2DA0FA0 stride 0x60 marker table DAT_022be7f0
0x219E7F0 FUN_00319ca0 0x1F9CA0 spawn FUN_00319920 0x1F9920 collected FUN_003197b0 0x1F97B0 discard
Category::Items item_scan.cpp core/item_names.cpp FUN_00272cb0 0x152CB0 msg 0x0D begins casting
realtime FUN_00312280 0x1F2280 reward batch BtlChr+0x18C EXP +0x190 LP +0x1C2 level defeated line
7 slots not 4**

**Task (tester, pre-ship):** two blockers — (1) "items dropped by enemies in the field are not
accessible on the pathfinder, need an items category"; (2) two combat-log gaps — when an enemy
starts preparing an ability, and how much EXP/LP a kill gave.

**Straight to C++ by explicit user instruction** — no Frida probe gate this session. The two facts
that would normally have been probe-gated were closed offline instead (below).

### 1. Ground loot is a SECOND OBJECT POOL — that is why the scanner never saw it

The pathfinder walks the scene-object handle table (`DAT_02098e10`). Dropped loot is not in it. The
engine keeps ground drops in their own **10-slot pool `DAT_02ec0fa0` (RVA `0x2DA0FA0`, stride
`0x60`)**, with world positions in a **parallel marker table `DAT_022be7f0` (RVA `0x219E7F0`, stride
`0x20`)** — `+0x00` alive flag, `+0x10` float xyz, exactly the two fields the engine's own getter
`FUN_002fb310` reads. Record: `+0x04` state (0 = free), `+0x20`…`+0x57` payload.

**The payload is 7 entries, not 4.** `combat_system.md:1007` said 4; both `FUN_003180f0` (the roll)
and `FUN_00319920` (the award) loop 7 times — 5 normal drop slots + 2 rare. **Struck.**

New `src/navigation/item_scan.{h,cpp}`, modelled on `exit_scan.cpp` (fixed position, no scene node).
`Category::Items` inserted **immediately after `Enemy`** — the tester's explicit request, since the
cycle is a plain modulo and one `=` press then flips between the enemies and their loot.

**The thread split is the whole design.** Three hooks — spawn `FUN_00319ca0` (`0x1F9CA0`), collected
`FUN_00319920` (`0x1F9920`), discarded/expired `FUN_003197b0` (`0x1F97B0`) — run on the **game
thread** and are the only place an item id becomes a name, because that is a game call. The spawn
hook renders all 10 slots' labels into a cache (a full pass, not a diff, so the pool's LRU eviction
path is covered too). `ScanDrops()` is pure memory reads and only ever reads that cache. Same idiom
as the combat log rendering its text at append time.

**The hooks are also the refresh event.** `EntityList::OnFieldFrame`'s container-mask edge cannot see
this pool, so without them a drop would only appear when the player happened to press `` ` `` or flip
category — the design the project forbids. Each hook sets an atomic; `OnFieldFrame` consumes it
**before** the mask early-out, so the edge cannot be swallowed.

**`FUN_00272cb0` is NOT an item-id resolver** — corrected en route. It is
`handle → FUN_003588b0 → FUN_00263990`, and `FUN_00263990` reads `+0x102` / `+0xf8` with the
`0xffffbfff` npcdic mask: the *scene-object* name chain `EntityScan::ResolveObjectName` already
replicates. Ghidra dropped the register-passthrough arg, so its input semantics are not established
offline — but the call is play-confirmed in the battle item sublist, so it is reused **as a game
call, from the game thread only**, rather than reversing the 14-class dispatch behind
`PTR_FUN_01eebd08` on an inference below the bar. Centralized into **`src/core/item_names.{h,cpp}`**;
`ingame_menu_reader.cpp`'s copy now delegates to it.

### 2. Enemy begins casting — one line, and a limit accepted on purpose

Messages `0x0D`/`0x0E`/`0x0F` were already flowing through the Tier-1 hook; they were simply filed as
the spam tier. `combat_format.cpp`'s `ShouldSpeakNow` now returns true for **`0x0D` only** —
`FUN_00469af0` maps action category 1 (magick) to it and is faction-gated `& 0x0A` (guest|foe), so a
party member can never reach it. `0x0E` "readies" / `0x0F` "uses" stay log-only.

**Accepted limit, tester's call:** `0x0D` carries render style `0x01`, which is **not** cull-exempt,
so the bus drops it beyond ~24 world units — a caster hanging far back announces nothing. Hooking
the emitter instead would fix that and would also name the target (the message never does), but it
was deliberately NOT taken, to keep the game's own verbatim wording in all 12 locales. Recorded in
the code comment so it is not "fixed" later.

### 3. EXP / LP folded into the defeat line

FFXII has **no end-of-battle results screen**. Rewards are granted per corpse and shown as floating
`+EXP`/`+LP` **sprite digits** — no text exists anywhere in the binary, so the sentence is ours; the
numbers are the game's.

New hook on **`FUN_00312280` (RVA `0x1F2280`)**, the reward batch, one call per enemy death off
`FUN_0030e360` case 0. Snapshot `BtlChr+0x18C` (EXP) / `+0x190` (LP) across roster list 3 slots 0–8 —
**the same list the function itself walks** (`FUN_00320ab0(i, 3)`, capped at 9) — call through, then
take the largest delta. EXP is divided among survivors so every living member sees the same share and
the max *is* that share, while a KO'd or absent member reads 0 and cannot drag it down.

`"<enemy> defeated. 34 EXP, 2 LP."` — one line, `speakNow=true`. Both deltas 0 (non-party kill, Trial
Mode) degrades to the bare `"<enemy> defeated"`; **never "0 EXP, 0 LP"**.

**Why the diff and not the popup:** `FUN_0028fb80(actorId, exp, lp)` looked cheaper — its args are
literally the drawn numbers — but Ghidra renders that call site with what look like the gil
accumulator in the value slots (dropped register args), leaving the argument identity at ~0.85. The
diff needs no such inference; it observes what the game actually wrote. `+0x18C`/`+0x190` are
corroborated by `license_reader.cpp` (already reads `+0x190` as current LP in shipped code) and by the
status-menu member block.

**The enemy-defeated line MOVED.** It used to be emitted from `CheckVitals`, i.e. off a damage
*calculation* one step before the HP write and long before rewards existed — the two could not be
joined there. It now comes from the real death event. The `l.low` latch stays: it is what re-arms the
party 20 % warning on revive.

### ⚠ The one sub-0.98 item, and what to watch on first playtest

That `FUN_00312280` is reached for **every** enemy death is **0.95** — its sole caller is
`FUN_0030e360:204` case 0, the KO funnel for any cause, but "sole caller" came from a static xref,
not from observation. **Failure mode is loud:** a kill that announces nothing at all. Watch the first
fight, including a poison/doom death. Fallback if a path misses it: keep `CheckVitals`'s enemy branch
as the trigger, stash the pending name, and let the reward detour flush it with the numbers attached.

**Built and deployed clean. NOT yet play-confirmed.**

### Reported this session, NOT diagnosed — two silent Clan/Hunt surfaces

The tester reported two more surfaces that read nothing, with screenshots. **Documented in
`debug.md` § "Clan / Hunt surfaces", no RE done, deliberately not guess-fixed:**

1. **The multi-item reward panel** — a titled panel (the bill name, e.g. `Red & Rotten in the
   Desert`) over one row per reward: `300 gil`, `Potion x 2`, `Teleport Stone x 1`. Distinct from the
   single-item obtained toast the mod already reads (`FUN_0035e070` + `widget+0xC8`) and from battle
   messages `0x24`-`0x26`: it has a heading and a separate quantity column, and has **no cursor**.
2. **The hunt notice board** — `"Which bill would you like to read?"` over a three-column cursored
   list (`Mark` / `Rank` / `Status`, e.g. `Thextera | I | Available`), with `Done` as a row in the
   same list. This one **does** have a cursor, so the first question is why the universal focus
   signal `FUN_00247510` 0x8000 does not already reach it.

**Do not assume the two share a controller** — one is cursored and one is not. Session 71's wrong
hook came from exactly that kind of shape assumption; follow each creation chain separately.

---

## Session 73 — 2026-07-24 — [pathfinder] Navigation is elevation-blind: `AllFloorsAt` + the reach-phrase lie

KEYWORDS: elevation, stacked floors, AllFloorsAt, ScanTopFloorAt, topmost floor, NavGrid 2D,
IsWithinReach, ReachPhrase, right next to you, above, Montblanc, Clan Hall, plinth, interaction
gates, facing cone, FUN_003a1bb0, FUN_0025bad0, FUN_0025b820, DAT_0209a2b8, height band, layered
grid, cross-level routing

### The report

Tester could not progress the story: the mod said **"Montblanc. right next to you"** and routed
**"Northeast 1. 1 steps"**, but Confirm always talked to a Clan Member instead. Measured from the
mod log (Clan Hall, map 302):

| | X | Y | Z | horiz from player |
|---|---|---|---|---|
| Player | 37.00 | **0.00** | 54.60 | — |
| Montblanc (slot 29) | 37.04 | **6.92** | 55.10 | 0.50 |
| Clan Member 4 (slot 26) | 38.86 | **0.00** | 54.74 | 1.86 |

Numbering came from `%LOCALAPPDATA%\FFXII-Screen-Reader\entity_labels.txt` (`302 0 26 135 4 Clan
Member`), not from guessing which duplicate was which.

### Root cause — every floor query in navigation is `f(x, z) -> y`

Four layers, all elevation-blind, each confirmed from our own source:

1. `MapQuery::GroundAt` is a **game** function taking `(x, z)` only. It cannot be asked "the floor
   nearest MY height."
2. `ScanTopFloorAt` (`map_query.cpp:190`) — `if (!found || y > bestY)`. It already iterated **every**
   floor poly in the cell and **discarded all but the highest**. In a stacked column it therefore
   described the surface *above the player's head*.
3. `NavGrid` is a **2D** grid (`WorldToCell(wx, wz, col, row)`), one `floorY` per cell — so a target
   directly overhead maps to the player's own cell: `tgtCell=(24,36) nearest=(24,36) nearDist=0.0m`.
4. `nav_common.cpp` — all three `DescribeDirection*` early-returned a bare `L"right next to you"`
   **above** the line appending `ElevationSuffix`. **The one phrase implying "you can interact now"
   was the only phrase that could never say "(above)".**

The mod had already MEASURED the gap and thrown it away: the same route logged
`route-profile leg 0: maxStep=6.93m(up)`, `WORST step=6.93m`, `directline wmBlock=1(step>max)` —
and `path_planner.cpp:122` labels that dump *"diagnostic, log-only"*.

### Shipped this session

- **`MapQuery::AllFloorsAt`** (`map_query.h/.cpp`) — every walkable floor in a column, ascending,
  near-coplanar polys merged at `kLayerMerge = 0.35` (deliberately the same magnitude as
  `kStepDiscont`: the height at which the engine stops letting you walk across a change is the
  height at which two surfaces become different levels). Pure memory reads, no game call, same cost
  as `ReadCellFloor`. The scan loop is **unchanged** — only what it does with each hit differs.
- **`ScanTopFloorAt` is now a wrapper** over the collector and tracks its top/cos **independently of
  the layer array**, so its answer is byte-identical even if the array overflows. No existing caller
  changed behaviour.
- **`NavCommon::ReachPhrase`** — the reach short-circuit keeps its wording (a bearing genuinely does
  swing wildly at melee range, which is why the early return exists) but re-attaches
  `ElevationSuffix`. On level ground it returns `L""`, so flat maps are byte-identical.
- **C++ elevation diagnostic** in `entity_diag.cpp` (the `'` dump): the player's column and every
  dumped object's column now report
  `cell=(c,r) layers=N raw=M [y0,y1,...] top=… nearest=… dY=±… <== STACKED`.
  `top=` is what the old single-answer path handed the router; `nearest=` is the level the entity is
  really on. **`top != nearest` is the bug, printed.** Deliberately does NOT call `GroundAt`
  (game-thread-only per `nav_grid.h`); `top=` is its memory-only equivalent.

### Interaction system — RE'd, recorded in `GameArchitecture.md`, NOT built on yet

Three independent geometric gates, all of which must pass before an object is even scored:
horizontal distance (`FUN_003da5a0`, **Y excluded — it is a cylinder**), a vertical band
(`FUN_0025bad0:77-80`), and a **facing cone** (`FUN_003a1bb0`, 0.99). The engine keeps exactly **one**
chosen target (`DAT_0209a2b8` + `DAT_0209a2bc`), reset per frame by `FUN_0025d650` — so
**"cycle interaction target" is not a thing the engine can do**; do not design a key for it.

### Struck / corrected in-session

- ~~"the 15:16:32 telop was Montblanc"~~ — **WRONG**, tester confirmed it was Clan Member 4 (the
  interaction icon reads "clan member"). It was offered at 0.9, below the bar, and should not have
  been stated at all. It also *confirms* the diagnosis: the engine never selected Montblanc.
- ~~"route stays silent when there is no walkable route"~~ — **REVERSED by the tester mid-session.**
  Turn-by-turn directions must reach **every** destination and nothing may go silent. `No path`
  becoming *more* common after the layered grid lands is a regression, not a truer answer.
- **Frida-first was explicitly waived by the tester for pathfinding** ("much easier to diagnose in
  C++"). A `probe_walkmap_layers.js` was written and then retired to `frida/archive/` unrun. Per
  CLAUDE.md the standing rules never override an explicit instruction in the current conversation.

### Still open

- **NOT play-confirmed** — built and deployed, no tester round yet.
- The layered grid itself (`col,row` -> `col,row,layer`, layer-aware snapping, inter-layer edges via
  the existing `kStepDiscont`) is **designed, not built**. `AllFloorsAt` is the primitive it needs.
- **Unproven at 0.90:** that scene-object Y and player Y share a reference frame. The new `player
  floor:` line settles it — if the player's column reports a layer at ~6.92, the frames match.
- Two silent Clan/Hunt surfaces from S72 remain undiagnosed.

### Session 73 addendum — containment fix validated, layered grid STRUCK, `;` interact readout

**Containment fix validated in play.** Montblanc `pos.y=6.92` → `layers=1 raw=1 [6.93] dY=+0.01`;
player column `raw=103 → raw=1`. Every named NPC lands on a real floor (Krjn 6.00→[6.00], an object
0.75→[0.75]). The `(i+1)%3` winding assumption for `DAT_00908de8` is validated — a wrong table gives
`layers=0`, not exact matches. **Object Y and player Y share a reference frame: SETTLED at 0.99.**

**STRUCK: the layered grid.** Post-fix histogram over the whole dump — `layers=1` ×28, `layers=0`
×16, **`layers>=2` ×0**. No stacking exists in that room; the walkmap is a single-valued height field
with a step. The "stacking" was plane extrapolation. Do not build a layer dimension for a case the
data does not show.

**REAL cause of "1 steps" found:** `expands=0 touched=1`. Player and Montblanc are 0.50 apart
horizontally and `kFineCell` is 1.5, so both land in ONE fine cell; A* short-circuits as "already
there" and **never tests an edge, so `kStepDiscont` never runs**. Fix is a resolution-independent
**goal-surface check** + approach cell, NOT a layer dimension. Designed, not built.

**`;` interact-target readout shipped** (tester's key choice — no new binding). `;` was structurally
silent in the field, so that dead slot now reads the engine's OWN chosen target
(`DAT_0209a2b8`/`DAT_0209a2bc` → "Talk: X" / "Action: X", silent when nothing is in reach).
`BattleTargetReader::SpeakTargetStatus()` now returns whether it spoke; `OnNavKey` falls through only
on false, so battle behaviour is byte-identical. New `navigation/interact_target.{h,cpp}`.
The `'` dump gained `LogChosen()` (ground truth) plus a per-object replica of the three gates
(distance / vertical band / cone) that logs its RAW inputs, so a wrong offset in the replica is
visible rather than producing a plausible FAIL.

### Session 73 addendum 2 — interaction band VALIDATED, approach-cell routing built

**Band validated in play.** `gates "Montblanc": dist2D=0.51 | band FAIL py=0.00 in [4.63,8.02]
centre=6.92 sc=1.00 up=1.10 dn=0.50 pad=1.79 | cone PASS |d|=0.255 half=1.571`. Both bounds
reproduce exactly from the replica, so the offsets are measured, not inferred. **`half=1.571` = π/2 —
the cone is a 180° hemisphere**, so a "face the target" key would have fixed nothing; and distance is
a cylinder (0.51 passed from 6.92 below). For anything on a dais the band is the ONLY gate that
decides where to stand.

**STRUCK (mine): "rotating the camera cannot orient the character."** The tester corrected this —
camera facing DOES orient the character, settled in an earlier session; movement is camera-relative
(see `project_camera_relative_directions_session37`). It was also irrelevant: the cone was passing.

**Approach-cell routing built.** Goal is now a SET of cells whose floor lies inside the target's
interaction band, within 3 cells / 4 m. Multi-goal A* (min-over-goals heuristic, membership
termination, `tc/tr` adopt the reached cell). Non-regression by construction for ground NPCs,
exits, `p`, and any target with no readable band. **Built and deployed, NOT play-confirmed.**

### Session 73 addendum 3 — approach cell lands in-band but out of reach (OPEN)

Tester round on the goal-set build: routing gets you *near* the target but not close enough to
interact. `goal-set: 12 cell(s) ... primary d=0.9m` yet `reached alternate approach cell ...
nearDist=2.1m` — a nearer cell existed and the search did not take it, because a min-over-goals
heuristic makes every goal equally attractive and A* stops at the first one popped.

**Shipped a provisional fix:** the remaining gap is now a terminal cost,
`h(n) = min_g(euclid(n,g) + 4.0 * g.d)`. Admissible, more informed (fewer expansions), and it
changes only the PREFERENCE among goals, never the goal SET — so it cannot make Montblanc
unreachable, which the tester called out as the thing not to break. The `goal-set: reached
alternate approach cell` line now also logs the gap in metres.

**Real fix, next session:** `kApproachRadius = 4.0f` is made up; it should be the engine's true
horizontal interaction reach from `FUN_003da5a0` — two of its four extents are constants at
`playerNode+0x5C` and `targetNode+0x7C`. Measured bracket: reach is between 0.51 (passed) and 1.70
(band+cone passed, engine still chose nothing). **Do not tighten it blind** — an empty goal set falls
back to the target's own cell, i.e. the unreachable dais.

**NOT play-confirmed.** Built and deployed at end of session; the tester shipped the build without a
verification round.

---

## Session 74 — 2026-07-27 — [pathfinder] 3D rebuild Phase 0: `'` reduced to two gates, the interaction reach solved

**KEYWORDS: nav probe span gate A gate B AllFloorsAt stacked column voxel sparse span graph Recast
compact heightfield interaction reach FUN_003da5a0 FUN_003da730 FUN_003a1d30 ellipse radius
kApproachRadius made up 2*dist2D-score DAT_0209a2b0 self-check exit centroid near edge overshoot
Upper Apartments Highhall Lower Apartments 280 282 279 stacked exits kGoalGapWeight does not work
NearestPointOnSurface CachedMapJumpSurfaces XFORM_POS_OFFSET_FLAG 0x107 band bug NAV-PROBE**

### Why

The tester reported three things: `\` turn-by-turn walks 3-4 steps **past** where `;` starts
announcing an interactable; a tracked exit announced as "25 steps north" fires after about five; and
in Upper Apartments, routing to the Highhall exit walks to the **Lower Apartments** exit instead.

They then directed that the pathfinder be rebuilt on a true 3D world model rather than patched, since
"a* is not sufficient for true 3D pathing" — ramps, steep slopes, stairs, cliffs, balconies, plinths.
Decisions taken: **sparse surface spans** (not a dense voxel volume), **slope measured and logged but
NOT gated**, and **no interim 2D fix build** — the destination fixes fold into the new search.

Note on framing, agreed with the tester: A* is the *search*, voxels are the *world model*. Recast /
Detour, Minecraft-style navigators and UE's own 3D navigation all still run A* over the voxel graph.
What is being rebuilt is the **state space and edge model**; the search and the spoken legs survive.

### Root causes, all three established from the tester's own log — not inferred

**Upper Apartments (map 280), `FFXII-Screen-Reader-Latest.log:7781-7782`:**

```
__MJ_CTRL000 group=1 -> "Nalbina Fortress: Lower Apartments" (279) at (53.5,-1.9,27.0) | box x[52.1..55.0] z[24.3..29.8]
__MJ_CTRL001 group=2 -> "Nalbina Fortress: The Highhall"    (282) at (50.4,+7.8,29.6) | box x[49.0..51.7] z[29.5..29.8]
```

The two exits are **~4 m apart horizontally and ~9.7 m apart vertically** — stacked in one footprint.
`NavGrid` stores **one height per 1.5 m cell** (a single `GroundAt` sample, `nav_grid.cpp:39-51`),
`SnapToWalkable` hunts up to 9 m for a walkable cell, and the "At the exit" short-circuit compares XZ
only with Y deliberately ignored (`path_planner.cpp:304-305`). A single-height grid cannot tell the
two apart. **This is a representation limit, not a regression in the Session 73 work.**

**STRUCK — "`kGoalGapWeight = 4.0` fixes the approach-cell overshoot" (Session 73, addendum 3).**
It does not. From the same log:

```
goal-set: 8 cell(s) in band [-2.49,2.50], primary (40,28)->(40,28) d=0.7m
goal-set: reached alternate approach cell (39,26), gap to target 4.0m   ... nearDist=3.4m
```

A 0.7 m goal existed and the search finished on a 4.0 m one. The terminal cost lives in the
**heuristic**, but `isGoal` (`path_search.cpp:295-299`) still fires on **any** goal popped, so a far
goal that happens to be popped first ends the search regardless of its terminal cost. The real fix is
the one the source comment already predicted: cap the goal set at the engine's true reach, so every
goal is interactable by construction and the weight becomes unnecessary.

**Exit overshoot** is `exit_scan.cpp:142`, `e.pos = surf->centroid` — the mean of **every** tagged poly
vertex in the group. Southern Plaza's seam is 28 polys spanning `z[132.0..140.0]`.

### SOLVED — the interaction reach is exactly replicable, and self-checking

`path_search.cpp:96-99` and `debug.md:1712-1728` both record `kApproachRadius = 4.0f` as invented,
with the reach bracketed only as `0.51 < r < 1.70` and two of the four extents written off as
"direction-dependent shape queries we do not replicate". **They are replicable.**

`FUN_003da730` is an **ellipse radius along a direction** and `FUN_003a1d30` is `sqrtf(fabs(x))`:

```
w = cos(-yaw)*dz - sin(-yaw)*dx ;  u = sin(-yaw)*dz + cos(-yaw)*dx
r^2 = (u*u + w*w) / ( u*u/(A*A) + w*w/(B*B) )          // 0 when the points coincide
```

`FUN_0025bad0` calls `FUN_003da5a0(out, playerXform+0x50, playerXform, targetXform+0x70, targetPosAdj)`
and rejects unless the return is `< 0`; that return is `dist2D - reach` with

```
reach = ellipse(playerShape -> target) + playerXform[+0x5C]
      + ellipse(targetShape -> player) + targetXform[+0x7C]
```

Each `*_SHAPE` is `{semiAxisA, semiAxisB, yaw, extraRadius}`. **All four terms are plain memory reads.**

**And it self-checks.** `FUN_0025bad0:139` stores `DAT_0209a2b0 = (dist2D - reach) + dist2D`, where the
second term is `param_1[3]` — the distance `FUN_003da5a0:36` writes out. The mod already reads that
global as `Chosen::score`, so `reach = 2*dist2D - score` is the **engine's own answer** for the same
quantity. `InteractTarget::ReadReachFor` returns both and `NAV-PROBE` prints them side by side.
Replica confidence offline **0.97**; the runtime identity is what lifts it over 0.98.

### Found in passing — a real bug in shipped code

`FUN_0025bad0:72-76` adds `xform[0x10..0x12]` to the target's position when the byte at `xform+0x107`
has bit 0 set, **before both** the distance gate and the vertical band test.
`InteractTarget::ReadBandFor` does not add it, so its band — which is what `PathSearch` routes to — is
wrong for any target carrying the flag. `ReadGatePos` now applies it; `ReadBandFor` is corrected in
Phase 3, and `NAV-PROBE` reports the flag so we learn how often it is actually set.

### Shipped this session (Phase 0 — instrumentation only, no routing behaviour change)

- **`'` stripped.** The old `DiagnosticDump` (~165 lines) emitted the move-frame snapshot, the wall
  self-test, the grid cross-check, the `+0x70`/`+0x54` legacy exit tables, `ExitDiag::DumpCoverage`
  and `MapExits::DiagScanScriptMapjumps` — the last alone hex-dumping 0x9000 bytes as ~1,152 lines.
  None of it measured anything still open.
- **`nav_probe.h/.cpp`** — the key now runs on the **game thread**, drained from the field-frame hook,
  because Gate B needs `MapQuery::GroundAt` and `debug.md:1608` records that calling it from the old
  input-thread dump was a bug. Emits, under the new flushed `NAV-PROBE` category:
  - **SPAN-PROBE** — an 11x11 block of fine columns around the player and around each seam (capped at
    4 seam blocks): every walkable layer with its slope cosine, next to `GroundAt` for the same point,
    plus an ASCII span-count field per block.
  - **INTERACT-REACH** — the replica beside the engine's measured value, marked CONFIRMED or MISMATCH.
  - **EXIT-AIM** — per group: centroid (what exits aim at today) vs nearest vertex, in metres and steps.
  - The walked trail, whose crumbs now each carry their distance to the nearest seam.
- **`NavTrace`** — every crumb now records its distance to the nearest seam vertex *and* to that
  group's centroid, measured when the crumb is laid down (at transition time the old map is gone). The
  `TRANSITION FIRED` dump therefore states outright how far the player was from the seam when it fired
  versus what the mod was telling them — automatic, with nothing for a blind tester to observe.
- **`MapQuery`** — `MapJumpSurface` carries the seam's own vertices; `NearestPointOnSurface`;
  `CachedMapJumpSurfaces` centralises the once-per-map sweep that `exit_scan` had privately (three
  subsystems now share it instead of paying ~15k reads each).

### The two gates that must close BEFORE any graph code

- **GATE A — does `AllFloorsAt` ever report >=2 layers?** Across every log so far it never has
  (`layers>=2` x0, zero `STACKED` lines), which is exactly why Session 73 struck the layered grid. But
  that diagnostic only sampled the player's column and each listed object's column, and Upper
  Apartments' two seams sit in **different** columns 3.1 m apart — the stacking in that room was never
  sampled. SPAN-PROBE sweeps a grid. **If every column still reports one span, the span graph is
  isomorphic to today's flat grid and the rebuild must be re-derived, not built.**
- **GATE B — is span height trustworthy?** The existing cross-check reports **100% walkability**
  agreement (416/416, 404/404, 520/520) but height agreement as low as **41/45**. Span Y is what the
  whole rebuild keys on, so the disagreements must resolve to a rule (topmost / nearest / lowest)
  first. SPAN-PROBE reports which span index `GroundAt` actually lands on, per column, and the
  tie-break tally deliberately counts only multi-span columns — on a flat map "top" would otherwise be
  the answer by construction.

**Build clean, zero warnings, deployed. NOT play-confirmed** — Phase 0 exists to be run and read.

### Debt recorded

- `map_query.h` is 157 lines against the project's 150 ceiling. Phase 1 moves the span reader into
  `nav_voxel`, which takes it back under.
- `'` no longer reaches `EntityList::LogDiagnostic` / `EntityDiag::DumpLocked`,
  `ExitDiag::DumpCoverage` (and through it `MapScript::DumpCaptureDiag`),
  `MapExits::DiagScanScriptMapjumps`, or `PlayerState::ReadMoveFrame` — roughly 1,000 lines now
  unreachable but still compiled. Deliberately **not** deleted: they are the record of how the exit
  mechanism was pinned down across Sessions 46-64. Delete or re-key them as a separate decision.

---

## Session 75 — 2026-07-27 — [pathfinder] The walkmap is a NAVMESH: routing rebuilt on the engine's own poly adjacency

**KEYWORDS: navmesh poly adjacency +0x16 +0x18 +0x1A FUN_002327d0 FUN_002324f0 FUN_0022f9b0 nav_mesh
NavMesh FindPolyAt EdgePassable FloodFrom nav_grid DELETED kMaxStep kStepDiscont SnapToWalkable
near-goal bridge REMOVED WALK_POLY_MJ_MASK 0xF not 0x1F Highhall wrong direction movement class 4
FUN_00230a40 FUN_00230c10 FUN_00232020 flag override table DAT_0209a3e0 GroundAt is not a floor query
FUN_0026e3c0 climb-and-drop mask 0xFFFF volume prims 0x4000 0x5000 dynamic obstacle closed gate
kAtExitDy seam near edge all three verts**

### The finding

**FFXII has no navigation grid. The walkmap is a NAVMESH.** Every 0x20-byte floor triangle carries the
index of its neighbour across each of its three edges at `+0x16/+0x18/+0x1A` (`< 0` = none), and the
character mover `FUN_002327d0` keeps a CURRENT POLY INDEX across frames, stepping onto the neighbour
when the position leaves the triangle:

```c
iVar8 = FUN_002324f0(param_1,param_5,&local_238);                 // edge crossed, -1 = still inside
sVar9 = *(short *)(param_1[2] + 0x16 + ((longlong)param_5 * 0x10 + (longlong)iVar8) * 2);
if (iVar8 < 0 || sVar9 < 0 || FUN_00230a40(param_1,sVar9,*(undefined2 *)(param_2 + 0x50)) == 0) { blocked }
```

`param_1[2]` is `ctx+0x10` (the poly array) and `param_5*0x10*2 == param_5*0x20`, so the address is
`polyArr + poly*0x20 + 0x16 + edge*2`. **Verified by reading the function directly**, corroborated at
`FUN_0022f9b0:86`. Confidence 0.99.

The mesh has been in front of us since Session 33 — the mod has been reading it for height all along.
What was never known is that the triangles are LINKED. We were using a navmesh as a heightfield.

### Why every symptom followed from that

The mod threw the connectivity away and rebuilt its own: a uniform 1.5 m grid, ONE height per cell,
sampled with `MapQuery::GroundAt`, with invented `kMaxStep = 1.5` and `kStepDiscont = 0.35` edge gates.

- **Levels collapsed.** Upper Apartments' Highhall (7.8 m up) and Lower Apartments (1.9 m down) exits
  are 4 m apart horizontally, so one height per cell put them in the same place. The tester got
  `"Southeast 3, Southwest 10, Southeast 6. 19 steps"` and `"...Southeast 7. 20 steps"` — one step
  apart, both "South".
- **Connectivity was invented.** The engine has NO step limit between adjacent polys; walkability is a
  flag test. Our gates refused edges the game walks, so A* reported unreachable for places the player
  walks daily. Any staircase with a riser over 0.35 m was unroutable — almost certainly the Waterways.
- **The heights were not floor heights.** `FUN_003208c0` → `FUN_0026e3c0` is two-stage: topmost floor,
  then **climb up to 30 units and cast back down with mask 0xFFFF, returning whatever it hits**. Cells
  could hold a wall, a ceiling or a rooftop. Confidence 0.97.
- **And it lied about failing.** The Highhall "route" was `pass=near-goal`, a fallback that stopped
  3.0 m short and was spoken as a normal route with no caveat.

### STRUCK — `WALK_POLY_MJ_MASK = 0x1F`

The map-jump group field is **four** bits, not five. Verified personally in two functions:

```c
FUN_00232020:  uVar2 = (ulonglong)((param_1 >> 3 & 0xf) + 0x40);
FUN_00230a40:  uVar3 = (ulonglong)((uVar5  >> 3 & 0xf) + 0x40);
```

Any seam poly with bit 7 set computed as `group + 16`, matched no `setmapjumpgroup(K)`, and was
**silently dropped**. The Highhall seam therefore read as 2 polys spanning a 0.3 m depth with a 1.9 m
rise — not a surface anyone can stand on, because it was a fragment. That is the "wrong direction"
half of the bug, independent of the routing half. The Session 64 note "the exact width of the field
cannot matter" is struck with it.

Compounding it, `ReadMapJumpSurfaces` recorded only `WALK_POLY_VERT0` per triangle — one corner of
three. Now records all three, plus the poly ids themselves.

### RESOLVED — the movement class, and what walkability actually is

`FUN_00230c10(ctx, from, to, out, class, radius)` takes the class as arg5. Both actor movers
(`FUN_0032bcc0:44-50`, `FUN_0032ca70:62-67`) pass **4** normally and `0xffff` only as an unstick mode
when already inside a volume or jammed within 0.27 units of a wall.

Class 4 matches **none** of `FUN_00230a40`'s 0/1/2/3/5 branches, so it falls through to
`uVar5 = uVar4 ^ 1 = 1`. **For the party, floor walkability is exactly `(effectiveFlags & 7) == 0`** —
no per-class opt-out bit. That 4 is the same value the mod has passed as `MAP_MASK_WALK` since S33.

`effectiveFlags` is `FUN_00232020`: two banks of `{u32 mask, u32 value}` at `DAT_0209a3e0`
(RVA `0x1F7A3E0`, 0x50 entries), indexed `(flags>>13)&0x1F` and `((flags>>3)&0xF)+0x40`. Floor
FINDING bypasses it (`FUN_00231900` uses raw bits — which is why `AllFloorsAt` was always right);
MOVEMENT does not. Both are now replicated, each where it belongs.

### Shipped

**New `nav_mesh.h/.cpp`** — the poly graph: `FindPolyAt` (containing triangle nearest a given Y),
`Neighbor`, `Walkable`, `MapJumpGroup`, `PolyHeightAt`, `EdgePassable`, `FloodFrom`.

**`nav_grid.h/.cpp` DELETED.** With it went `kMaxStep`, `kStepDiscont`, `SnapToWalkable`, the bridge
recovery pass and the near-goal fallback — every one of them existed to paper over invented
connectivity, and the last one is what spoke a failed search as a route.

**`path_search.cpp` rewritten** as A* over polys. Both endpoints are located WITH THEIR Y, which is
the single fact that separates the Highhall seam from the floor beneath it. Goal acceptance samples
the centroid AND all three corners against the interaction band and the measured reach, so the route
stops where `;` starts answering instead of walking onto the target. No partial routes: if the mesh
says there is no path, it says so and the log explains.

**Where the raycast survived.** Floor adjacency knows nothing about blockers — static volumes are
prims `0x4000-0x5000` and doors/platforms are `>= 0x5000` — so the floor under a CLOSED GATE is still
adjacent to the floor before it. `EdgePassable` casts one short walk-class segment straddling the
shared edge. That is the only raycast left in routing; the failed Highhall route used to burn 2,985.

**`nav_reach`** floods the mesh instead of the grid (256 polys/frame of memory reads, no raycasts).
**`exit_scan`** aims at the seam's nearest vertex with the seam's own Y, no longer at the centroid
with a `GroundAt` height. **`kAtExitDy = 3.0`** added, deliberately revising "never test 'am I at this
exit' in 3D" — correct when an exit's Y came from the blob, wrong since S64 made it a floor polygon.
**`nav_probe`** rewritten around the mesh: the player's poly with its flags and three neighbours, a
flood of the component, and per seam whether its polys are in it.

Build clean, zero warnings, deployed. **NOT play-confirmed.**

### The one thing still unmeasured

Whether the mesh is CONNECTED from the player to each seam on a given map. Everything says it should
be, but that is an assertion about map data, and asserting things about exit data is how six sessions
of the exit saga went wrong. The probe's `FLOOD` and `seam group=N ... inPlayerComponent=` lines
answer it in one press, and name the polys if the answer is no.

---

## Session 76 — 2026-07-27 — [pathfinder] One destination for both keys; TWO interactable classes found

**KEYWORDS: expands=1 search never ran IsGoal too generous centroid arrival mismatch destinations
disagree Waterways save crystal interaction anchor xform+0x107 FUN_002646c0 class 1 class 3
FUN_0025be50 FUN_0025bad0 score units mixed reach identity class-3 only ClosestPointOnPoly
ProjectPointToNavigation DQ7R InteractionCollisionArray hasBoxComponent ObjectClass**

### The report

In the Waterways, targeting the Save Crystal: `/` crow-flies said **"North, 6 steps"** (correct),
`\` turn-by-turn said **"South 1. 1 steps"**. Tester: *"the turn-by-turn directions need to route to
the same destination as the crow flies directions, exactly."*

### Root cause — the search never ran

20 of 30 routes in the log:

```
mesh: start=41 goal=33 end=41 polys=1 expands=1 touched=0 rays=0
pass=mesh-reach startPoly=41 goalPoly=33 endPoly=41
say="Save Crystal. South 1. 1 steps"
```

`start=41`, `end=41`, `expands=1` — A* terminated on its **first pop, the player's own polygon**.
Two errors from Session 75, both mine, compounding:

1. **`IsGoal` was far too generous.** It accepted a poly when ANY of {centroid, 3 corners} was in the
   interaction band and within `reachRadius`. A navmesh triangle is often a whole corridor, so the
   triangle the player stands on nearly always has a corner within 1.6 m of a target six steps away.
   The goal test passed before the search moved. I added the corner sampling deliberately, to be
   generous about large triangles; it was generous enough to swallow the entire search.
2. **The point tested and the point arrived at were different.** On a non-goal finish the polyline
   ended at `Centroid(reached)`, not the corner that passed — so the destination was the centroid of
   the triangle the player was standing in, which is why the direction pointed south at a target to
   the north and swung around as they moved.

### Fix — one destination, and it is the target

`IsGoal` is now `p == goal`, and reconstruction **always appends `to`**. Both keys therefore name the
same `FVec3` from the same source, and they cannot disagree by construction. Overshoot is impossible:
a path that ends at the target cannot end past it, which was the original Session 74 complaint.

For a target the player genuinely cannot stand on (Montblanc's dais), the qualifying poly is recorded
**during** the main search — first one A* pops — and used only if the goal poly is never reached. The
point tested IS the point arrived at (`NavMesh::ClosestPointOnPoly`), which is the correction for
error 2. No second search; nothing changes in the common case.

`NavMesh::ClosestPointOnPoly` is our reimplementation of what DQ7R gets free from Unreal's
`ProjectPointToNavigation`: closest point on the triangle in XZ, Y from the poly's own plane.

### RE — THERE ARE TWO INTERACTABLE CLASSES, AND THEY DO NOT SHARE FIELD OFFSETS

`FUN_0025b820` runs **two** loops over two index ranges of the same container, scored by two different
functions. The discriminator is `sceneObj+0x03 >> 5`:

| | class 3 — characters/NPCs | class 1 — gimmicks/volumes |
|---|---|---|
| scorer | `FUN_0025bad0` | `FUN_0025be50` |
| interaction point | `pos + node[+0x40/+0x44/+0x48]` when `node+0x107 & 1` | plain `pos` |
| band | `node.y ± node+0xC8/+0xCC` scaled by `node+0x24`, skip `+0xDD` | `node.y + node+0x68` / `- node+0x6C`, **no scale**, skip `+0x5C` |
| cone half-angle | `node+0xBC` | `node+0x50` |
| cone aims at | the position | **`node+0x10` / `node+0x18`** |
| score written | `2*dist2D - reach` | `FUN_003a1960(player,node)` — a plain distance |

**Reading one class's offsets on the other returns plausible-looking floats that are simply wrong**,
which is exactly the failure mode that survives review. `InteractTarget::ObjectClass` now gates every
interaction read.

### STRIKE — the reach identity is CLASS-3 ONLY

Session 74 recorded `reach = 2*dist2D - score` as a self-checking identity, and
`InteractTarget::ReadReachFor` applied it unconditionally. **`DAT_0209a2b0` mixes units between the
two scorers** — verified by reading `FUN_0025be50` directly:

```c
fVar5 = (float)FUN_003a1960(param_3,lVar1);
if (fVar5 < DAT_0209a2b0) { ... DAT_0209a2b0 = fVar5; }
```

For a class-1 winner the identity yields a confident, meaningless number — printed next to the word
CONFIRMED, which is how a wrong value gets promoted to fact. `haveMeasured` is now gated on class 3.
`ReadBandFor` is likewise class-aware; the band it reported for a class-1 object was reading
character-band fields.

### Partial refutation of a subagent finding — recorded so it is not re-derived

A subagent reported class 1's interaction point as `node[+0x10/+0x14/+0x18]` (an absolute world
position). **Reading `FUN_002646c0` — the engine's own interaction-point getter — directly refutes
that:**

```c
bVar2 = *(byte *)(param_1 + 3) >> 5;
if (bVar2 == 1) { pfVar1 = *(float **)(param_1 + 0xb8);
                  *param_2 = *pfVar1; *param_3 = pfVar1[1]; *param_4 = pfVar1[2]; }   // PLAIN POS
```

`pfVar1` is `float*`, so those are bytes `0x00/0x04/0x08`. `+0x10`/`+0x18` is where `FUN_0025be50`
aims the **facing cone**, not where the interaction point lives. `FUN_0026bb00`'s class-1 setter does
write bytes `0x10/0x14/0x18`, so there is a real field there — but it is not what the getter returns,
and the two are not reconciled. Treat class-1 interaction point as **the plain position** (0.98, read
directly) and the `+0x10` field as **unidentified**.

### The interaction anchor, applied centrally

`PlayerState::ReadSceneObjectPos` now returns the engine's interaction anchor — a replica of
`FUN_002646c0`. It is the one function feeding `e.pos` in `entity_scan` (x2) and `entity_list`'s live
refresh, so `/`, `\` and `;` all name the same point.

**Not asserted:** what writes the offset or what it means is unestablished (~0.9). The justification
does not depend on it — "report the point the engine measures from" holds regardless — and the build
counts how often the flag is actually set (`anchor offset (xform+0x107): applied=N plain=N`), so the
next log says whether this is a real change or a no-op instead of us assuming.

### From DQ7R (read for the idiom, per tester instruction)

Its rule: **the interaction point is authored game data, not geometry the mod computes.**
`RefreshEntityPosition` overwrites `entry.pos` with the interaction volume's cached world position and
everything downstream consumes that one field — which is the "both keys retarget" design chosen here.
Also worth having, not yet ported: `if (e.hasBoxComponent) { e.reachable = true; return true; }` —
an entity with an authored interaction point is reachable **by definition**, do not ask the walkmap.

Build clean, zero warnings, deployed. **NOT play-confirmed.**

### What to watch in the next log

- **`expands=1` must disappear** for any target outside the player's own polygon. That single number
  is the tell for this whole class of bug: a search that never ran.
- `anchor offset … applied=` — whether the anchor change does anything at all.
- `gates "…" class=` — which class the Save Crystal and other common targets actually are.

---

## Session 77 — 2026-07-27 — [pathfinder] Funnel string-pull; and the twin filter that deleted four NPCs

**KEYWORDS: route reversal doubles back funnel algorithm simple stupid funnel string pull portal
midpoints EdgePortal TriArea2 not elevation sign-twin dropped Nomad Village missing NPCs name
equality no proximity kStackedDist NumberDuplicateLabels contradiction character inclusion mode state
flags zero KIND_DEAD counted entity_postscan split raw dump re-keyed REVERSAL invariant**

---

### 1. Route reversals — portal midpoints, not elevation

**Symptom.** `"Dire Rat 1. South 2, North 7, West 5, North 5, West 14, then 3 more. 36 steps"` —
legs that send the player one way then straight back. Tester's hypothesis was elevation.

**It was not elevation, and the log says so three ways:**
- Searches are healthy: `pass=mesh`, `nearDist=0.0m`, `startPoly=522 goalPoly=1234 endPoly=1234`,
  an 11-polygon chain found in 12 expansions.
- The routes are flat — target `(23.56, 0.00, 177.16)`, player on the same level.
- The *same* start and goal produced the route both with and without a spurious leading leg
  (`"North 6, West 5, ... 33 steps"` vs `"South 2, North 7, West 5, ... 36 steps"`) purely as the
  player shuffled **inside one triangle**. Height cannot do that; geometry inside a triangle can.

**Root cause — MINE, from Session 75.** `path_search.cpp` built the polyline as
`[from] + midpoint of every shared edge + [to]`, and `outPoly = rawPoly` — no smoothing at all. The
grid pathfinder string-pulled its cell staircase; when the grid was deleted the smoothing went with
it and the placeholder shipped. On a mesh whose triangles are often whole corridors, successive edge
midpoints sit at opposite ends of their portals and the line saws between them. It also explains the
spurious FIRST leg exactly: player near one end of their own triangle, first portal midpoint behind
them. `PathDirections` cannot recover from it — RDP *preserves* shape, and the zigzag IS the shape.

**Fix — the funnel algorithm** ("simple stupid funnel") over the portal sequence. Keeps a left and
right bound, emits a corner only when the funnel inverts, and returns the shortest path inside the
corridor — which **cannot** double back. That is a property, not a heuristic. O(n), no raycasts,
cheaper than what it replaced.

- `NavMesh::EdgePortal` returns an edge's two ENDPOINTS; `EdgeMidpoint` is now diagnostics-only and
  carries a comment saying it must not go back into routing.
- Left/right is decided by the sign of a 2D cross product against the travel direction, **not** by
  trusting the mesh's winding. The containment test implies consistent winding, but that is an
  inference and two multiplies buys not depending on it.
- Y rides on the portal vertices, which are real mesh vertices on their own surfaces, so a stair
  climb still describes correctly without the funnel reasoning about height.
- `from` stays first and `to` stays last — Session 76's one-destination guarantee is untouched.

**Guard shipped: the REVERSAL invariant.** `PathDirections::Describe` now logs when two consecutive
legs turn 135° or more. Same lesson as `expands=1` last session: the tell should announce itself in
the log, not wait for a tester to walk into it.

---

### 2. Four NPCs deleted by the sign-twin filter

**Report:** "at least 2 NPCs missing, one required to advance the story." **The log already named
them, and there were four** — no diagnostic dump needed:

```
sign-twin dropped "Nomad": [0:40] (45.97,0.00,67.37) repeats doorway [0:38] (53.80,0.00,38.80) 29.6m away
sign-twin dropped "Nomad": [0:41] (61.00,0.00,58.00) repeats doorway [0:38] 20.5m away
sign-twin dropped "Nomad": [0:42] (30.50,0.00,53.60) repeats doorway [0:38] 27.6m away
sign-twin dropped "Nomad": [0:43] (37.30,0.00,43.10) repeats doorway [0:38] 17.1m away
```

**Root cause.** `TagDoorwaysAndDropSignTwins`, two compounding defects:
1. Any object within 2.5 m of a `+0x70` field-sign record was tagged a **doorway** — including an
   NPC who happened to stand near a shop sign.
2. The twin test was `out[j].doorway && out[j].label == cur.label` — **name equality with NO
   proximity check at all.** The distance in those log lines is computed only for the message.

So one mis-tagged "Nomad" erased every other "Nomad" on the map, at any distance.

**The codebase already knew.** `NumberDuplicateLabels`, the *other* duplicate handler, says in its
own comment: *"two at different positions are two real objects that share the game's own name (which
is normal — 109 npcdic ids all read 'Rabanastran')"*. That pass NUMBERS same-named objects; this one
DELETED them, and it runs first. The same map proves it: `dup-label "Nomad Youth" x4`,
`dup-label "Cockatrice" x6`.

**Fix (scope, per the tester's rule):**
- **Doorway tagging never applies to a person.** A character near a sign is not a doorway, so it can
  never become the anchor that deletes others.
- **Interactables only** for the doorway-twin removal — the shop-sign case it was written for: same
  name, the other one carries the location jump (`Entity::doorway`), this one does not.
- **NPCs dedupe only when literally stacked** — `kStackedDist = 0.05 m`, i.e. the same coordinates.
  `j < i` so the first of a stacked pair survives; without it both would erase each other.
- **Every drop is logged unconditionally.** It used to log only on a new population high-water mark,
  so a twin removed on a later rescan was silent. This filter deleted a story NPC once.

---

### 3. Second suspect — instrumented, not assumed

`entity_scan.cpp`'s inclusion gate had both the `gimmick` and `named` routes written `&& !isCharacter`,
so for a person the ONLY way into the list was non-zero `+0x1C` flags — a word this project's own
`debug.md` records as **mode state that reads zero on a disabled object**: *"a disabled object has
zero flags … therefore dropped story-gated gates entirely … Include by KIND; use the flags only for
'what can I do with it right now'."* That lesson was applied to gimmicks in Session 54 and never to
people, so a story NPC whose talk hook the script has not armed is invisible by construction.

A character that resolves a name is now included. **But this shipped on inference, not evidence** —
there is no log proof it fires — so it counts itself:

```
inclusion: N character(s) admitted by NAME ONLY (no interaction flags) | M actor-pool entr(ies)
           skipped as KIND_DEAD(5), which debug.md records as NPC
```

The `KIND_DEAD` skip in `ScanCombatants` is **counted, not flipped**. `phyre_types.h` labels that
constant "NAME IS WRONG" and `debug.md` records kind 5 = NPC on the field, but the combat track owns
it and the pool is documented to hold no gimmicks. If that tally is ever non-zero on a field map, the
premise is false and the skip goes.

---

### 4. The raw dump is reachable again

`EntityList::LogDiagnostic` / `EntityDiag::DumpLocked` — the raw handle-table walk showing objects
the scan **rejected** — lost its only caller when I stripped the `'` key in Session 74. That is why
four deleted NPCs went unnoticed: the log could only show what passed. Re-keyed onto the `'` probe.
Its filter is deliberately wider than the scan's, so a rejected object still appears with its flags,
kind, category and position.

**LESSON: when a diagnostic is orphaned, the bug it would have caught becomes invisible, not absent.**

---

### 5. Housekeeping

`entity_scan.cpp` passed the 500-line ceiling (545). Split: the passes that run over the FINISHED
list — `ObjectHandle`, `LogObjectDump`, `TagDoorwaysAndDropSignTwins`, `ApplyPlayerLabels`,
`NumberDuplicateLabels` — moved to **`entity_postscan.cpp`** (327 / 251 lines). Signatures declared
in `entity_scan.h` so the two units cannot drift.

Build clean, zero warnings, deployed. **NOT play-confirmed.**

### What to watch in the next log

- **No `REVERSAL:` line.** If one appears it names the legs and the angle.
- **`twin dropped (…)`** — the reason string must never be a person unless it says "stacked".
- **`inclusion: N character(s) admitted by NAME ONLY`** — whether the widening does anything.
- Leg counts: an 11-polygon chain used to yield 8 legs; the funnel should give 2–4.

---

## Session 78 — 2026-07-27 — [pathfinder] The reversal is a PASSED WAYPOINT, and it is older than the navmesh

**KEYWORDS: reversal leg0 leg1 passed waypoint drop leading corner mobile target static target immune
Dire Rat Rogue Tomato 64% 0% log forensics 1059 routes 109 reversals grid era pass=strict 86
dual-polarity funnel measured not derived funnel length invariant portal crossing edge cost camera
relative ref swing geom dump**

### The tester was right, and the archive proves it

They said the reversal predates the navmesh. Forensics over all 20 archived logs, **1,059 spoken
routes, 109 with an immediate reversal (10.3%)**:

- **OLD GRID era: 86 reversals / 998 routes.** Every single one from **`pass=strict`** — a fully
  completed search, up to 601 expansions and 55,628 rays. Not one came from a recovery pass; the
  24 `pass=near-goal` routes in the archive contain **zero** reversals, and no `bridge:` line exists
  anywhere in any log.
- **NAVMESH era: 23 / 61.**

**Two different bugs wearing one name.** The difference is magnitude, not position:

| era / position | n | median wasted | max wasted |
|---|---|---|---|
| GRID leg0 | 62 | 4 | **20** |
| GRID mid-route | 17 | 7 | **20** |
| NAVMESH leg0 | 19 | 3 | 6 |
| NAVMESH mid-route | 4 | 3 | 5 |

The grid produced *route-scale* detours you could walk into — `"Rogue Tomato. South 24, Northwest 20,
North 22. 66 steps"`. The navmesh produces *stub-scale* jitter. "North 7, South 25" is a grid-era
memory; the navmesh has never produced a reversal that large.

### THE MECHANISM — a waypoint the player has already walked past

**87 of 109 reversals (80%) are leg 0 -> leg 1**, in both eras. And the target distribution names the
cause outright:

```
Dire Rat 1     14/22  (64%)        Save Crystal        0/36  (0%)
Rogue Tomato   38/113 (34%)        Stair to Lowtown    0/55  (0%)
Montblanc      25/183 (14%)        Rabanastre exits    0/41  (0%)
```

**Static targets are essentially immune; moving ones dominate.** The target's motion is not the cause
— it is that a moving target makes the player re-press *while walking*. The route's first corner is a
fixed point; the player drifts across it; and from one step past it, leg 0 points BACKWARDS to that
corner and leg 1 immediately turns around.

The archive caught it red-handed — the *identical* path (`firstLeg=(46.5,160.0)`, same `expands=38`)
spoken four ways in six seconds as the player rocked back and forth over that one waypoint.

**Fix: drop leading waypoints the player has already passed.** Project the player onto the
corner->next-corner segment; a positive parameter means they are beyond it, so it is history, not a
waypoint. Repeated, because they may have passed several. Independent of the funnel, of the corridor,
and of which era's pathfinder is underneath — which is why it addresses the grid-era memory too.

### Also shipped, from this session's own log

**The funnel was not string-pulling.** Every route logged `portals=6 corners=7`, `7->8`, `8->9` —
exactly one corner per portal, three for three. A working funnel collapses a corridor to a handful.

I derived the left/right convention (FFXII has north at -Z, which flips the handedness against every
reference implementation), traced both branches against Mononen's twice, and they looked correct. The
log disagreed. **So the sign is no longer an argument to win:** the funnel now runs with BOTH
polarities and keeps the shorter path. The correct polarity is the shortest path through the corridor
by definition; the inverted one is the zigzag. O(n) twice, impossible to get wrong, and it logs which
won so the next session can collapse it to one branch **on evidence**.

**The invariant that would have caught it in one line:** the funnel's output can never be longer than
the portal-midpoint path through the same corridor. Logged, plus a corridor-quality check (taut length
vs straight line) that separates "the funnel is wrong" from "A* chose a wandering corridor" — the
exact distinction three sessions of reading code could not make.

**A\* edge cost now measures the crossing, not the centroid hop.** It was `Dist3(centroidA, centroidB)`;
on a mesh where a triangle is a whole corridor, centroid hops are a poor proxy for walking distance.
Now `centroid -> portal midpoint -> centroid`. The heuristic stays Euclidean, so still admissible.

**Route geometry is dumped** — corridor poly ids, corner coordinates, portal left/right coordinates.
Every session on this bug ended with me wanting those numbers.

### Corrected: the REVERSAL comment's attribution

`path_directions.cpp` credited the bug to Session 77. The archive shows 86 grid-era reversals three
days earlier, from completed searches, and considerably worse. Comment amended. The detector itself
postdates every archived log, so none of that history was ever caught by it.

### Separately — the camera is not the geometry bug, but it IS half of what the tester sees

Across the six presses in this session's log, `ref` swung **-24.2 -> -72.3 deg** (48°) while the leg
distances stayed fixed at 11, 11, 6, 8. Directions are camera-relative, so identical geometry gets
different words each press. That is the documented, accepted limitation (Session 56 rejected the
camera lock, the travel-anchored frame and the spoken notice) — but it masks the real defect, so when
re-testing, hold the camera still between presses.

Build clean, zero warnings, deployed. **NOT play-confirmed.**

### What to read in the next log

- **`funnel: polarity=...`** — if one polarity wins every time, that is the evidence needed to delete
  the other branch. If it flips between routes, the portal ordering is position-dependent and the real
  fault is upstream.
- **`<== LONGER THAN MIDPOINTS`** — the funnel is still broken.
- **`corridor is N.Nx the straight line`** — A* is choosing badly; the fault is the search, not the pull.
- **`dropped N leading waypoint(s)`** — the leg-0 fix firing.
- **`corners` vs `portals`** — must drop well below 1:1.

---

## Session 79 — 2026-07-27 — [navigation] A whole KIND of NPC was invisible on every map

**KEYWORDS: missing NPC Nomad Village elder tent slot 55 include by KIND isCharacter clause global not
per-map loaded model READY_MODEL_BIT presence test blast radius per-kind tally shadow registration
stacked exact position entity_labels key struck container slot nameIdx -1 collision position anchor
claim seq self-inclusion bump F6 never bound DIK_F6 0x40 g_extraDown 21 merchant negative shop name
Frida-first probe_shop_name**

### The bug: `&& !isCharacter`

A tester was told by the Nomad Elder to find a woman behind his tent, got sighted assistance to
confirm she is there, and the mod never listed her. The dump found her:

```
[0:55] cat=66 kind=5 en=1 r14=70 flags=00030000     nameIdx=-1  ""            pos=(46.00,0.00,57.70)
[0:37] cat=66 kind=5 en=1 r14=70 flags=00030004 ACT nameIdx=239 "Nomad Elder" pos=(46.80,0.00,58.70)
```

1.28 m from the elder. `cat` prints in **hex**: `0x66` masks to 6, so she is class 3 — a CHARACTER.
`en=1` (story gate open), `r14=0x70` (model loaded, ready). All four inclusion routes failed at once,
and the decisive one was `gimmick = (kind == 5) && !isCharacter`.

**That clause is not map data. It rejected this class of NPC on EVERY map in the game.** Nomad Village
is only where a tester happened to need one. `debug.md` had already stated the rule — *"a disabled
object has zero flags … Include by KIND; use the flags only for 'what can I do with it right now'"* —
and it had been applied to gimmicks in Session 54 and to NAMED characters in Session 77, but never to
people without names.

**FIX: presence = an interaction KIND (1 or 5) + a loaded model.** Both kinds deliberately: she is
kind 5, but kind 1 is the same defect wearing the other kind. Purely ADDITIVE — the old `gimmick` term
is kept verbatim so a model-less trigger volume cannot start falling out as a side effect.

### The blast radius could not be measured, so it is instrumented

The object dump is Session 77 code and **all 20 archived logs predate it**, so Nomad Village's is the
only dump that exists. Rather than guess what a crowded city costs, `inclusion:` now counts what the
KIND route admits that nothing else would, **split by kind**, plus how many are characters. If kind 1
sweeps townspeople the tally says so and that half comes out — with a number, not an argument.

### STRUCK — this comment's own claim

*"Character objects (cat 5-7) are deliberately NOT surfaced … unflagged ones are left to the combatant
scan or the talk-flag path."* Both escape hatches are fictional: the combatant scan reads the BtlWork
pool, which holds no field NPCs, and the talk-flag path needs a flag the engine clears on anything the
script has not armed. A character with neither was reachable by **no path at all**.

### Shadow registrations

Widening admits second registrations: `[0:39]` sits at (53.80, 0.00, 38.80), the **identical**
coordinates of `[0:38] "Nomad"`. New `DropShadowRegistrations` drops an unnamed character stacked on a
named object. **EXACT only (`kStackedDist` = 5 cm)** — the same map has real NPCs 0.62 m and 1.22 m
from a named neighbour, and the woman this whole pass exists to surface is 1.28 m from the elder. Any
radius loose enough to feel like "nearby" deletes her.

### Numbering — STRUCK: `mapId . container . slot`

The slot is assigned at map load in script order, so it is **not stable across loads**; a re-slotted
object looked new and took a fresh number. Map 243 held five Nomads numbered **1, 3, 5, 6, 7**, and
`NumberFor` could only ever move a number UP because the "lowest free" scan counted the record it was
renumbering as taken.

New identity: **`{mapId, baseLabel, nameIdx}`**, with a **position anchor only when that triple
repeats**. The anchor is not optional: every anonymous object carries `nameIdx = -1` and one category
word, so `{map, "NPC", -1}` is the same key for all of them — and the widening above admits precisely
those objects, so keying on `nameIdx` alone would have broken the people it just added. A unique
triple matches on identity alone, so a *wandering* NPC with its own npcdic id stays stable wherever it
walks. Added a per-pass claim so two live objects can never collapse onto one record, and
`Entity::baseLabel` so the key survives the " 2" suffix.

**Version 1 -> 2 cost nothing: the store held 127 records and ZERO player labels.**

### F6 had never been bound

Handler, clipboard read, persistence, apply-before-numbering — all built in Session 65, and both
`Controls.md` and `README.md` documented the key. **`DIK_F6` was never defined and no edge was ever
registered**, so `case VK_F6:` was dead code. The zero-label store is the independent proof. Added
`DIK_F6 = 0x40`, grew `g_extraDown` to `[21]`, registered the edge.

### The merchant — a clear negative

No per-NPC merchant marker exists. `sceneObj+0xCC`/`+0xDC` are slots of an 18-entry mode-indexed array
holding **event indices** (every Nomad Village NPC reads `0xFFFF`, `5`, `8` or `9`); npcdic has no
merchant band (the candidates straddle the gimmick band); the NPC->shop binding lives only in compiled
map script. **A learned NPC->shop binding was deliberately NOT built** — it is the Session 62 mistake
by definition.

The shop's own NAME is readable, and the chain is now decompiled: `shopId = *(u8*)(DAT_02ca9790+0xC0)`
-> master table `DAT_02ebf158` -> npcdic id (**odd** slot, `FUN_003eabe0`). **No C++ was written** —
FRIDA-FIRST applies to a new behavioral feature. `probe_shop_name.js` authored instead.

**Also corrected while writing it:** the plan claimed `FUN_0057c010` was the shop-OPEN event. It is a
**dialog callback** (`local_18 = FUN_0057c010`, registered by `FUN_0057a4e0` into `FUN_003f47e0`), so
the open hook point is still unestablished. The probe reads from the already-confirmed
`FUN_0056e5d0` instead.

Build clean, zero warnings, deployed. **NOT play-confirmed.** Still untested from Session 78: the
passed-waypoint reversal fix — every route in that log was 1.1 m.

## Session 80 — 2026-07-27 — [navigation] `+0x1C` is an 18-MODE mask; the classifier is `+0xC8`, not the flags

**KEYWORDS: NPC classification interact component script resolution interaction mode mask 18 bits
sceneObj 0x1C bit index equals mode index 0xC8 u16 array FUN_002652d0 FUN_0026b4a0 FUN_0025d5e0
FUN_00266bd0 default mask scene category cat 7 talk cat 5 6 action interact icon superseded native
table offline Ghidra dump_script_native_table probe_interact_modes delta 5140 two anchors
setmapjumpgroup 0x11E mpk no EBP2 address order refuted no code written**

Picked up the Session 79 hand-off: classify NPCs from the interact component or from map script.
**No C++ was written** — this is RE plus two USER-RUN artifacts.

### `sceneObj+0x1C` is an 18-bit interaction-MODE mask (conf 0.99)

The mode index **is** the bit index, and `sceneObj+0xC8` is a parallel `u16[18]` of per-mode event
indices. Read from `FUN_00269a90` / `FUN_00269ba0` / `FUN_00269ad0` (`(flags >> mode) & 1`),
`FUN_0026b4a0` (arm: `|= 1 << mode`), `FUN_0025d5e0` (disarm), and `FUN_002652d0` (the getter, bound
`mode <= 0x11`, with a map-record fallback when the inline slot is `0xFFFF`).

The mod's two shipped constants fall out as *derivations*: mode 2 = ACTION = `0x004` = `+0xCC`;
mode 10 = TALK = `0x400` = `+0xDC`. **It has been reading two of eighteen bits.** The correspondence is
confirmed at **eight distinct modes** (0, 2, 7, 10, 11, 14, 16, 17), including the same `0xE -> 7`
alias in `FUN_00269860` that `FUN_002652d0` carries. Offsets and the full table are in
`GameArchitecture.md`.

### The correction that saved this from being a wrong classifier

`FUN_00266bd0` sets the mask's **default from the scene CATEGORY alone** — cat 0 -> `0`, cat 1/2 ->
`0x38`, cat 3/4 -> `0x40`, cat 5/6 -> `0x00030004`, cat 7 -> `0x00034c85`. **Category 7 is the only one
born with TALK.** So the mask carries nothing `sceneCat` does not already give, and a classifier built
on it would have been category laundered through eighteen bits.

It does corroborate Session 79 independently: the Nomad Elder's `flags=00030004` *is* the cat-5/6
default, and the missing woman's `00030000` is that default with bit 2 cleared by her script. She was
never an odd object — she was a standard character with her one default mode switched off.

**What is per-object is the `+0xC8` array** — static map data, present before the player acts. That is
where a classifier can be built, and it is database resolution, not a learned binding.

### The interact ICON is superseded, not refuted

Session 79 nominated the icon as the cheapest classifier. Walking the two interaction predicates upward
found the mode array first, and it is cheaper (18 `u16` reads in a scan the mod already runs) and
static. The icon was never located; the hand-off section in `debug.md` is marked superseded, not struck.

### Answering "why not finish it offline like the exit chain?"

The remaining unknown is **native id -> handler function**. It lives in the exe's `.data` — the
`mapjump` handler pointer sits at abs `0x1EEE8B0` — and the decompile export contains function bodies
and **no `.data` bytes**. Three offline routes were tried and are recorded as dead ends in `debug.md`:
interpolating ids from handler addresses (**refuted**: it would make `FUN_00355540` = `sethpmenu`); the
extracted `.mpk` map controllers (**zero `EBP2` magic** across all 20 — map data, not bytecode); and
`output\mapctrl_ebp_disasm.txt` (**mis-based**, decoding data as instructions).

So the offline route is a **Ghidra script — the same class of artifact as `dump_mapjump_native.java`,
which produced that `0x1EEE8B0` anchor in the first place.** No play session required.

Also measured: the delta `dbgIndex = nativeId + 5140` gains a **second anchor** — `setmapjumpgroup`,
native `0x011E` = 286, dbg index 5426. Still not global; `setshopname` is ~1,050 indices away.

### Artifacts (authored, USER-RUN, neither has been run)

- **`ghidra\dump_script_native_table.java`** — the offline answer. Validates the `mapjump` anchor and
  **stops rather than emit a plausible wrong table**; searches for the exe's own native NAME table in
  two layouts so natives are named **with no delta assumed**; joins the `.dbg` list as a second opinion
  and **cross-checks, reporting every disagreement**; reverse-looks-up which native calls the
  arm/disarm/event primitives (the constant at the call site names the MODE); forward-looks-up
  `openfullscreenmenu` / `setshopname`. Syntax-checked with `javac` (only unresolvable-Ghidra-class
  diagnostics remain).
- **`frida\probe_interact_modes.js`** — secondary, not on the critical path. Dumps the mask and the full
  `+0xC8[18]` array per scene object, once per map, for breaking ties the names leave ambiguous.

### Still outstanding from Session 79 — unchanged, still not play-confirmed

The by-KIND+model inclusion widening, the per-kind `inclusion:` tally, the shadow-drop rule, the new
`{mapId, baseLabel, nameIdx}` numbering, and the F6 binding. The only archived log is from 11:54, which
predates that build. Session 78's passed-waypoint reversal fix is also still untested.

### Run 1 of the native-table script — it refuted its own assumption (same session)

The user ran `dump_script_native_table.java`: *"1197 natives, delta=5140"*. The anchor validated (a
pointer to `FUN_00355350` really is at `0x1EEE8B0`), and everything downstream of it is wrong, because
the script **assumed a dense qword array** with `mapjump` at index `0x8D`. Its own output kills that:

- **776 gaps of exactly 3** across 4096 slots — a pointer every 4th qword, not a dense array.
- **The emitted names fail against handlers known by behaviour**: `FUN_00355830`, which disarms
  interaction mode 2, came out `@SWCOD_000162` (a compiler switch label); `FUN_00346020` came out `sin`
  while its body is a wait-poll structurally identical to the one named `waitv`.
- **`FUN_00355540` (arm mode), `FUN_003558f0` (disarm mode) and `FUN_00351f40` were not in the window at
  all** — and all three are certainly natives: four-arg signature and **zero references anywhere in
  `.text`**, so only a dispatch table reaches them. Wrong window -> wrong base -> every id wrong.
- **No exe-side native NAME table** at either probed layout, so naming still depends on the `.dbg` join.

`output\script_native_table.txt` is kept as evidence; **nothing in it may be cited but the anchor**.

**Two claims made earlier this session are STRUCK, both mine:**

1. *"the delta now has TWO anchors."* Circular — `setmapjumpgroup`'s id `0x011E` came from bytecode and
   was then *named* using the 5140 delta, so re-deriving 5140 from it counts one fact twice. `mapjump`
   remains the only true anchor (its handler was identified by behaviour, not by any delta).
2. *"handlers are not laid out in native-id order."* That refutation was computed from the same bad
   stride-8 assumption. Run 1 shows addresses **mostly ascend** with slot order (895/1179 adjacent
   pairs). Ordering is **unresolved**, not refuted — and moot, because the stride is now measured.

### `ghidra\dump_native_slots.java` — measures the stride instead of assuming it

A native is identifiable **without** its id: four-argument signature and zero `.text` callers. The new
script finds the `.data`/`.rdata` slot holding a pointer to each of ~26 such handlers, takes the **GCD of
the sorted slot deltas as the stride**, anchors the base on `mapjump`, and then **self-checks**:
`FUN_00355540` and `FUN_003558f0` provably arm and disarm the same bitfield, so their names *must* form a
matched enable/disable pair. Coherent -> the mapping is usable and the natives that call arm/disarm with
a constant name the interaction modes. Not coherent -> it prints **NOT COHERENT** and withholds the ids.
It also dumps the raw record bytes around the anchor so the field layout is read, not inferred.
Syntax-checked with `javac`. **Not yet run.**

### Run 2 — the stride is 32, and the self-check actually PASSED there

`dump_native_slots.java` reported `stride=8 self-check=NOT COHERENT`. The **verdict was right** — it
stopped a wrong mapping being adopted, which is what it was for — but the stride is wrong, and its own
slot list contains the correction. Three independent samples sit exactly `0x20` apart: ARM
`FUN_00355540` / DISARM `FUN_003558f0`; fire `FUN_0034e5c0` / fire-all `FUN_0034f380`; and
`FUN_003537b0` registered **four times** at `0x1eeed30/50/70/90`.

That quad is one handler serving four *adjacent* natives — the exact shape of the
`keyscan`/`keyscanr`/`keyscant`/`keyscantr` family in the `.dbg` list. And **ARM and DISARM one record
apart is the `reqenable`/`reqdisable` adjacency the self-check was testing for. It passed at stride 32
and only read as a failure because the script measured 8.**

**Why the GCD said 8:** the handler's offset *within* a record varies across samples — `0x00` for
ARM/DISARM/fire/fire-all, `0x10` for `mapjump` and the quad, `0x08` for `FUN_00351f40` and
`FUN_00355830`. A record holds several function pointers; the samples hit different fields; a GCD over
mixed fields collapses to the pointer size. **New lesson: a GCD of address deltas measures stride only
when every sample is the SAME field.**

**And the record's second field is a POLL function**, which explains every nonsense name at once:
`FUN_00346020` (`sin`) and `FUN_003453d0` (`waitv`) are the identical wait-poll shape, and
`FUN_00342f50` (`settalkiconstatus`) **takes no arguments** and merely polls then yields — a `set…`
native with no argument cannot exist. These are the Athena VM's blocking-native continuation slots, so
reading them as natives could only ever yield unrelated names. `FUN_00356990` (`lastjumpindex`) pops an
argument and writes, which no getter does.

**Nothing about the id numbering is established. Cite no id or name from either output file.** What is
established: 32-byte records, at least three function-pointer fields each, and the arm/disarm adjacency.

`ghidra\dump_native_raw.java` (authored, syntax-checked, USER-RUN) dumps three windows we know the
contents of — around ARM/DISARM, around the `mapjump` anchor, and around the quad — as raw 32-byte-aligned
rows with every qword resolved to a function, string or value. It asserts nothing; the field order is to
be READ. That is the step both previous scripts skipped.

### Run 3 — SOLVED. The native table, and the `-24` that beat two scripts

`dump_native_raw.java` dumped the bytes and the structure was immediate: Athena natives can **block**, so
each has up to three implementations 32 bytes apart —

```
BASE = abs 0x1EED720 (RVA 0x1ECD720), stride 32, name = dbg[k + 5140]
simple[k] = BASE+32k      init[k] = BASE+32k-24      poll[k] = BASE+32k-16
```

**The `-24`/`-16` is the whole story.** A native's init and poll sit in the physical row *below* its
simple slot, so one row holds `simple[k]` next to `init[k+1]` and `poll[k+1]`. Both earlier scripts
treated a row as one native, blended two natives, and emitted ~1200 confident wrong rows each.

**And the tell was in run 1's output the whole time, misread as noise:** `FUN_00346020` named `sin` is a
wait-poll; `FUN_00342f50` named `settalkiconstatus` takes **no arguments**. A handler whose argument
count contradicts its name is in the wrong FIELD, not at the wrong id. That should have redirected the
search two runs earlier.

Confirmed by **13 handlers identified from their code before any name was looked up** — including
`mapjump = 141 = 0x8D`, independently reproducing Session 63's bytecode-derived id, and one sync poll
(`FUN_003537b0`) shared by four *adjacent* `voice*` natives. Delta 5140 now holds across ids 42..703, a
660-id span against the single anchor it had. Table in `GameArchitecture.md`.

### What it unlocks for NPC classification

- **`reqenable(mode)` = native 42 (`0x2A`), `reqdisable(mode)` = 43 (`0x2B`)** — the natives that arm and
  disarm the 18 interaction modes. A map script's literal argument NAMES the mode, and `map_script.cpp`
  already walks that bytecode for `mapjump`/`setmapjumpgroup`.
- **Mode 12 = map-jump / transition** — Session 63's decoded `__MJ_CTRL` routine opens with
  `reqenable(12)`. First mode named from script rather than engine code.
- **Mode 13 = the name label** — `fieldsign` (native 374) arms modes 8 and 13 *and* sets the object's
  display name, matching `FUN_00268d10`'s `& 0x2000` test that renders `FUN_00263990`'s string.
- **`talktreasure` (703) disarms mode 2** — treasure chests use ACTION.
- `sysreq`/`sysreqall` (170/171) fire a mode's event — the script-side counterpart of `+0xC8`.

**Shop natives derived but NOT confirmed:** `openfullscreenmenu` -> 1138 (`0x472`) / 1165 (`0x48D`),
`setshopname` -> 1193 (`0x4A9`). All are extrapolations past the validated band; below the 0.98 bar
until their handlers are seen to be menu openers.

`ghidra\dump_native_table_v2.java` (authored, syntax-checked, USER-RUN) asserts the layout, **re-proves
all 13 checks on every run and emits NOTHING if one fails**, then dumps the full table plus a focused
section for the interaction-mode and shop natives. Still **no C++ written** this session.

### SHIPPED — the personal-name read (C++, at the tester's direction)

The tester redirected the thread: the interact component is the useful part, this is lookup not
discovery, go straight to C++. Correct call — and following `FUN_00263990` one step further than the
mod ever had found a live bug.

**`FUN_00263990` picks the npcdic slot as `id*2 + (FUN_0032a930(id) != 0)`.** The mod always read the
EVEN slot. `FUN_0032a930` is a **live per-id bitfield** — "has the player been introduced to this
character" — written by the `settalknpcname` / `releasetalknpcname` natives and read back by
`istalknpcname` (`FUN_0034e980`, which pops an id and returns exactly that bit).

**STRUCK — Session 54's "byte-identical in the US build, even-only is correct, no second name to
mine."** It rested on a **48-id sample**: ids 0-11 and the 433-469 gimmick band — crowd filler and
crystals/urns/treasure, the two ranges that by construction hold no personal names. Decoded across
**all 1141 ids: 247 differ**, and the odd slot is the real name.

| id | even | odd |
|---|---|---|
| 221 | Nomad | **Arjie** |
| 228 | Nomad | **Lesina** |
| 239 | Nomad Elder | **Elder Brunoa** |
| 159 | Viera | **Ktjn** |
| 220 | Cockatrice | **Agytha** |

So the mod has been saying the generic word for **every NPC the player has already met**, on every map,
for the whole project. It is the game's own display string in all 12 locales — database resolution, not
a learned label, and it needed none of the native-table work.

**LESSON: a sample drawn from the ranges you already understand cannot falsify a claim about the ranges
you do not.** Those bands were sampled precisely because they were already being validated for crystals
and treasure, which is what made them the wrong evidence here.

**Changes** — `map_rva.h`: `TALK_NAME_STATE` (RVA `0x2044280`, the static array `&DAT_02164280`, **not**
a pointer to deref), `TALK_NAME_BITMAP` `0x15B4` (folding `FUN_002ef2b0`'s `+0x200` and
`FUN_0032a930`'s `+0x13B4`), `TALK_NAME_MAX_ID` `0x800`. `entity_classify.{h,cpp}`: `TalkNameKnown(id)`,
`NpcdicName(id, known)` with an even-slot fallback if the odd slot is past the table, and
`ResolveObjectName` wired through both. `entity_scan.cpp`: an `s_knownName` tally on the `inclusion:`
line — Session 79's rule that a change which cannot be sized offline ships with its own counter.

**Known consequence, documented in `entity_labels.h` rather than papered over:** `baseLabel` is part of
the store's key, so the moment a name is revealed a player label under `{map, "Nomad", 221}` is orphaned
and the object leaves its numbering group. The numbering change is the correct behaviour, and the store
holds 127 records with zero player labels, so nothing is lost today.

Build clean, zero warnings, deployed. **NOT play-confirmed.** Also still unconfirmed: Sessions 78 and 79.

## Session 81 — 2026-07-27 — [navigation] Resolve, don't track: numbers leave the store, phantom NPCs leave the list

**KEYWORDS: Cockatrice 37 numbering leak 39 records six animals NumberFor deleted stateless numbering
container slot within scan entity_labels version 3 rewrite on version mismatch repeating discarded line
party roster bodies scene category 5 phantom NPC 1..6 missing NPC findable odd npcdic slot always Dania
Lesina Masyua Nanau Jinn grace window numbered list not the spoken list idempotent label passes cursor
re-lock baseLabel**

Four defects from one play log, three sharing a root cause the tester named exactly: *"it seems as if
maybe you're tracking the interaction component for all NPCs now instead of just resolving the NPC
location to its interaction component label in the database."* **Resolve, don't track.**

The Session 80 personal-name read was confirmed working first — *"the NPC classification on introduction
is already working"* — and nothing here changes that path.

### The leak, measured on disk

`NumberFor` allocated a fresh record whenever `Match` failed, which for a ROAMING object was every time
it wandered past `kAnchorDist` from an anchor that is deliberately never refreshed. Nothing capped,
evicted or cleaned up, and the free-number search counted leaked records as taken, so the number could
only climb. **The live v2 store held 39 records labelled "Cockatrice", numbered 1..39, for six real
animals**, plus 6 `NPC`, 4 `Nomad`, and **zero player labels** — its only product was numbers, and the
numbers were wrong.

`NumberFor` is deleted. Numbering is now a dense rank within the current scan, keyed on
`{container, slot}` — the game's own name for a handle-table object, stable for as long as the map is
loaded. Session 79's strike on `container.slot` was about PERSISTENCE and does not forbid this: unstable
across loads is why it may not be stored, stable while loaded is why it is right for a number recomputed
every scan. The store keeps only the player's own words; format 2 -> 3.

### A latent bug the store had been hiding

The two label passes ran at the end of `BuildLocked`, but `RescanLocked` merges the 2 s grace-window
survivors **after** `Build` returns — so **the list that got numbered was never the list the player
heard**. Harmless while numbers were permanent; with stateless numbering a member streaming out for one
frame would leave the survivors to compact while the carried entity kept its old suffix, so two entries
would answer to one number for up to the whole window. Both passes moved into `RescanLocked` after the
merge and were made idempotent.

### Three phantom NPCs, and the woman who was findable all along

Six `NPC n` entries: three genuinely anonymous townsfolk and three scene-category-5 bodies (kind 1, no
name, stacked at one point 6 m above the floor) admitted by the `present` route. She was in the list
from Session 79 onward — just indistinguishable among six. Category-5 objects that resolve no name are
now dropped, **narrowed to `&& !named` and instrumented** because the 5/6/7 split has one map's evidence
and the engine gives categories 5 and 6 the same default mode mask. Every drop is logged with the fields
that would falsify it.

### The numbers should never have existed

Every "Nomad" on that map has a distinct personal name one npcdic slot over — 223 Dania, 228 Lesina,
229 Masyua, 231 Nanau, 225 Jinn. The mod now prefers the odd slot regardless of whether the game has
made the introduction, so five numbered entries become five real names and the group dissolves. Only
npcdic 238, whose two slots BOTH read "Cockatrice", still needs numbering.

**LESSON: when a disambiguator keeps breaking, check whether the thing it disambiguates should exist.**
Four sessions went into making a number stable across rescans, streaming, reloads and sessions. Five of
six numbered groups had real names available in data the mod was already reading, one slot over.

### Also fixed while in here

- **"store is version N -- discarded" repeated on every area change forever.** `Load()` refused without
  rewriting and `Reload()` is unguarded; once numbers stopped triggering saves nothing would ever have
  overwritten the file. `Load()` now `Save()`s on the mismatch path, which silences it *and* deletes the
  leaked records. The success line is gated on a non-empty store.
- **The cursor re-lock still compared the SUFFIXED label.** `CursorMatch`'s own comment claims that was
  fixed "in BOTH tiers"; it reached tier 2 only, so the "kept dropping back to the nearest" failure
  survived through the re-lock door — and stateless numbering makes a renumber more likely. Tier 1 now
  compares `baseLabel`, which no renumber can change (`CursorId::baseLabel`).
- `ResolveObjectName` was being called twice per object; collapsed to one (it now costs a decode pair).
- Dead `entity_labels.h` include removed from `entity_scan.cpp`.

Build clean, zero warnings, deployed. **NOT play-confirmed.** Sessions 78-80 also remain unconfirmed.
`entity_scan.cpp` is now 457 lines (past the 400 split-planning mark) and `nav_rva.h` 541 — both logged
in `PerformanceIssues.md`, neither split in this change.

## Session 82 — 2026-07-27 — [navigation] Two Session 81 claims struck; the story gate joins the KIND route

**KEYWORDS: odd npcdic slot reverted TalkNameKnown gate spoiler Nomad 2 then Dania scene category 5
party refuted unplaced origin reserve slot NPC 1..19 Rabanastre story gate en bit 0x0E 0x10
INTERACT_ENABLE_BIT present route narrowed s_gated counter en added to object dump**

Play test on Rabanastre. Two of the previous session's changes were wrong and are reverted; the
remaining complaint got the fix the Session 79 tally had pre-registered.

### REVERTED — the always-odd-slot name

*"You broke the unlabelled-until-interacted-with component… was Nomad 2 before, then Dania once
interacted with."* Correct, and reverted on sight. `NpcdicDisplayName` is back to the engine's rule,
`id*2 + FUN_0032a930(id)`. The Session 80 half — reading the odd slot at all, for characters the player
HAS met — was a genuine fix and stands; the log confirmed it working before this session began.

**LESSON: reading the wrong slot and choosing a different policy for the slot are two changes.** The
first was a correctness fix with evidence behind it. The second was a behaviour change nobody had asked
for, and bundling them let it ride in on the first's evidence — including past a question I asked and
got an answer to, because the answer was given in the abstract and the behaviour was only wrong in play.

### REFUTED — "scene category 5 = party/roster bodies"

*"These are not party members, I have no other party members currently."* The exclusion's own
falsification dump agreed: **every object it removed read `pos=(0.00,0.00,0.00)`** — the world origin,
an unplaced reserve slot that the existing guard already drops. Wrong premise *and* a no-op.

The three bodies that inspired it shared one position 6 m above the floor. **That is a fact about
POSITION and I wrote it down as a fact about ROLE.** Constants deleted from `nav_rva.h`; nothing keys on
the 5/6/7 split until something other than one map says what it means.

**What worked:** the dump shipped with the change named what it removed, on the first map that was not
the one the theory came from, and that killed the theory inside one play session.

### FIXED — nineteen bare "NPC n" on a city map

The Session 79 `present` route (kind 1|5 + loaded model) was shipped with an explicit test: a large
per-kind tally means it is sweeping crowd NPCs and the clause comes out. Rabanastre returned
`kind5=23`, with nineteen `NPC n` entries — every one `flags=0x00030000, nameIdx=-1, avail=0`, while
every named person on the map was `flags=0x00030004, avail=1`.

The route now also requires **the story gate**, `+0x0E & 0x10`. That is the engine's own interactivity
switch — `FUN_0026ba60` is a dedicated setter driven from map script, and it is the first thing
`IsInteractionAvailable` tests. It is emphatically **not** the `+0x1C` mode state Session 79 correctly
refused to filter on: those bits mean "may I act on this right this second" and flicker; this bit means
"has the script switched this object on at all".

**The Session 79 woman passes it** — her dump line reads `en=1`, which is exactly why the gate is this
bit and not `available`, which she failed. New `s_gated` tally reports what the gate holds out. **If a
city map comes back reading zero there, the theory is wrong and the next step is a `'` dump, not a
third guess.**

### `en` added to the object dump

It was the one field `LogObjectDump` lacked, and its absence is why the nineteen entries could not be
told from real story NPCs without a `'` dump that had not been taken on that map. `flags` is mode
state; `avail` folds four tests into one bit; `en` is the single honest "is this switched on".

Build clean, zero warnings, deployed. **NOT play-confirmed.** The tester also reports the story NPC is
now findable, which closes the Session 79 report.

## Session 83 — 2026-07-27 — [navigation] Phantom NPCs go by PLACEMENT and REACHABILITY; the grace window was undoing every filter

**KEYWORDS: extraneous NPC1 NPC2 phantom spawn point Y 6.06 above suffix unreachable routed to an
obstacle FindPolyAt no rejection threshold PolyHeightAt kFloatingDrop NavReach Reachable kNpcReachTol
DropUnplacedCharacters majority guard stood down grace window re-admits filtered WasFilteredThisScan
NoteFiltered story gate struck**

### The discriminator was the tester's, and it is the right kind of fact

*"Try reachability. 3 NPC classifications were right at my position when I spawned into the world, 1 was
unreachable entirely (routed to an obstacle), and one was at the same location as one of the nomads."*

Three earlier attempts asked what these objects **are** — party members (S82, refuted), roster bodies,
story-gated bodies (S82, dropped zero) — and read that meaning into fields that do not carry it.
**Where an object IS, the walkmap can answer. What it is, the game data does not state.**

All six bare `NPC n` on Nomad Village, identified:

| spoken | slot | position | what it is |
|---|---|---|---|
| NPC 1-3 | `[0:32-34]` | (37.89, **6.06**, 71.87) | spawn-point phantoms — the only floor at that XZ is 6.06 **below** |
| NPC 4 | `[0:51]` | (30.70, 0.00, 52.40) | unreachable |
| NPC 5 | `[0:55]` | (46.00, 0.00, 57.70) | **REAL — the Session 79 story NPC** |
| NPC 6 | `[0:56]` | (37.80, 0.00, 43.47) | duplicate registration, 0.6 m from a named Nomad |

The three phantoms are confirmed independently by the spoken output: NPC 1, 2 and 3 always carry
**`(above)`** and always report the same bearing and distance as each other.

### `DropUnplacedCharacters` — two questions, both answerable

Candidates are the narrowest set that exists: `Category::NPC`, `!gameNamed`, has a scene object. **The
only thing it can delete is an entry that was going to be announced as the bare word "NPC" and a
number.** Named NPCs, exits, treasure, drops and combatants are never examined.

- **Not on the floor** — `NavMesh::FindPolyAt` then `PolyHeightAt`; drop when the object is more than
  `kFloatingDrop` = 2.0 m **above** its own floor. One-sided on purpose: below the floor is a basement.
- **Unreachable** — `NavReach::Reachable(pos, kNpcReachTol = 1.5)`, the per-map flood fill that already
  filters exits. One `FindPolyAt` plus a hash lookup; **no A\***, which matters because a single failed
  search was measured at ~45,000 raycasts.

**The trap that made the height test necessary:** `FindPolyAt` resolves by XZ containment with Y only as
a tie-break and **no rejection threshold**, so an object floating 6 m up still resolves to the triangle
beneath it — `NavReach` alone calls all three phantoms reachable. It is also the mechanism behind
"routed to an obstacle": the goal snaps vertically onto whatever floor is under the object.

Fail-open three ways, copied from the exit filter: nothing until `NavReach::Ready()`; any query that
cannot answer keeps the entity; and **if it would drop more than half the candidates it drops nothing
and logs that it stood down.**

### The bug underneath: the grace window was re-admitting everything we filtered

`RescanLocked` carries an entity over when its scene object is missing from the fresh list, to survive
the handle table streaming an object out for a frame. **It cannot tell "the engine stopped reporting
it" from "we just deleted it"** — and a filtered object is a LIVE engine object, so
`RefreshPositionsLocked` keeps reading its transform and keeps stamping `lastSeenMs`, so it can never
age out. Once carried in it is permanent for the life of the map, **while the scan goes on logging the
drop on every rescan**. A filter that reports success and changes nothing.

Fixed with `EntityScan::NoteFiltered` / `WasFilteredThisScan`: every drop pass records the scene object
it erased, and the merge skips those. This was silently undoing the shadow drop too.

### STRUCK — the Session 82 story gate

`&& storyOn` on the `present` route required `+0x0E & 0x10`. It shipped with the counter that would
falsify it and the counter came back **zero** — every one of those objects reads `en=1`. Clause and
counter removed. **The counter did its job in one play session; that is the third time in four sessions
a shipped tally has killed a theory faster than argument would have.**

### Deliberately NOT done

`NPC 6` sits 0.6 m from a named Nomad and is a duplicate registration, but widening `kStackedDist`
(5 cm) to catch it would put the threshold within 0.68 m of the real story NPC, who stands **1.28 m**
from the Nomad Elder — the exact object Session 79 exists to protect, and the exact mistake Session 77
made when a loose filter deleted four NPCs. That window is too narrow to pick from one map. Left in
place; if the two new tests do not remove it as a side effect, the next step is measuring co-location
across several maps, not guessing a radius.

Build clean, zero warnings, deployed. **NOT play-confirmed.**

## Session 84 — 2026-07-28 — [navigation] One route, three defects: `present` struck; the enemy classifier gets its inputs back

**KEYWORDS: present include-by-KIND model HasModel shadow NPC bare NPC n stacked bodies party slots
enemies NPC category DropUnplacedCharacters kFloatingDrop kNpcReachTol nameless interactable icon
ApplyFallbackLabels Sign dead code NoteFiltered grace window crossing oracle exit inventory span bleed**

### The evidence: four maps, one log

The tester collected `FFXII-Screen-Reader-Latest.log` across Lowtown North Sprawl (701), Rabanastre
Eastgate (305), Nomad Village (243) and Garamsythe Central Spur Stairs (311) — the last of which
should have **no NPCs at all** and reported `NPC=4`.

Every map showed `inclusion: kind1=3`, and in every case those three were unnamed `kind=1` bodies on
one shared authored coordinate:

| Map | slots | shared position |
|---|---|---|
| Lowtown North Sprawl (701) | `[0:24][0:25][0:26]` | (92.00, −0.19, 51.77) |
| Rabanastre Eastgate (305) | `[0:75][0:76][0:77]` | (200.00, −10.00, 81.00) |
| Nomad Village (243) | `[0:32][0:33][0:34]` | (37.89, **6.06**, 71.87) |
| Garamsythe Central Spur (311) | `[0:15][0:16][0:17]` | (32.52, 0.00, 121.96) |

Nomad Village is the only one where that point is in the air. That, and nothing else, is why Session
83's floating test appeared to fix that map and no other.

### One route caused all three reported defects

`present = (kind == 1 || kind == 5) && HasModel(obj)` — Session 79. Struck. Full account in
`debug.md`. Shadows beside real NPCs; the three stacked bodies above; and **enemies in the NPC
category**, because a field enemy is a character with a loaded model, `BuildLocked` runs before
`ScanCombatants`, and `ScanCombatants` skips anything `AlreadyListed` — so the widening took the
enemy classifier's inputs away. There was never a missing enemy classifier; faction has always come
from the actor pool in `ScanCombatants`.

**Session 79's founding premise is struck with it.** The unnamed woman it was built for was listed
the whole time under her own npcdic name; what the widening added was her shadow. The tester
identified the pattern from play: *"Masui is 'Nomad2' in my game so she has a classification. She just
also now has an extra NPC shadow that never gets named, even after talking to her."*

### Shipped

- **`entity_scan.cpp`** — five inclusion routes replaced with `if (!named && !interactive) continue;`.
  `present`, `gimmick`, `InGimmickBand`, `droppedBefore` and `HasModel` all deleted.
- **`entity_postscan.cpp` / `entity_scan.h`** — `DropUnplacedCharacters` deleted with `kFloatingDrop`
  and `kNpcReachTol`. `DropShadowRegistrations` and `kStackedDist` kept; `NavReach` kept (exits use it).
- **`ApplyFallbackLabels` (new)** — the category-word fallback moved out of `BuildLocked` to after
  doorway tagging. **The `"Sign"` word was dead code from the day it was written**: `e.doorway` is set
  by a pass over the finished list, so it is always false on a fresh entity, and the word the tester
  explicitly authorised could never be spoken. Every unnamed doorway said "Interactables" instead.
- **`entity_postscan.cpp:320`** — `TagDoorwaysAndDropSignTwins` erased without `NoteFiltered`, so the
  2 s grace window could carry a twin straight back in, permanently, while the pass logged the
  deletion every rescan. Session 83 fixed this in the other two passes and missed this one.
- **`entity_classify.h`** — the header still documented the unconditional odd-slot naming policy that
  Session 82 reverted as a spoiler. Corrected; that is how a struck design gets re-shipped.

### Counters shipped WITH the change (every one a falsifier)

`nameless dropped: kind1= kind5= other=` · `of which carried a PAYLOAD ID` (**must be 0** — non-zero
means a real gimmick was deleted, the Session 54 regression) · `kept while nameless BUT INTERACTIVE`
(should be 0; each is dumped in full) · `handle-table object(s) also in the ACTOR POOL` (counter only,
no behaviour — this is how enemies-as-NPCs would come back).

### Exits — audited, then instrumented. NO new model.

The NPC work cannot have removed an exit: `ScanExits` runs after all drop passes; both NPC passes bail
unless `category == NPC`; the grace window skips null `sceneObj` and exits are built with one;
`RefreshPositionsLocked` has an explicit `fixed` branch. On 311 the scan listed everything it found.

Session 60's rule stands — no sixth transition model. Four diagnostics only:

- **Crossing oracle** (`nav_trace.cpp`): on every map change, prints the seam group crossed, **what
  the mod claimed it led to**, and **the map that actually loaded**. Mismatch = the group→destination
  binding is wrong, proven, needing no sight. `EntityScan::ClaimedDestForGroup` caches the binding per
  map because the script it came from unloads with the map.
- **Exit inventory** (`exit_scan.cpp`): `exits: controllers= surfaces= listed= | dropped: nogroup=
  notused= unreachable=`, re-logged whenever the numbers **change** rather than once per map id — the
  walkmap streams in after the map id, so a once-per-map latch printed the empty state and went quiet.
- **Surface inventory**: every walkmap map-jump group, flagging any **no controller claims** — a
  transition surface with no destination, i.e. a missing exit, previously invisible.
- **Span dump** (`map_script.cpp`): per `__MJ_CTRL`, its code span and where in that span the
  `setmapjumpgroup` and `mapjump` literals were found, plus explicit logging of the two silent drops
  (`SPAN EMPTY`, `SPAN UNREADABLE`). A routine's span runs to the next routine's offset, so a span
  that bleeds reads the NEXT controller's literals — which is what swapping would look like. Map 311
  is the one map whose controller→group mapping is not the identity (CTRL000→2, 001→3, 002→1) and the
  one the tester reports swapping on. **A lead to measure, not a conclusion.**

**Struck: "on East End `+0x70` covers a doorway no `__MJ_CTRL` routine owns."** Per the tester there is
no missing exit in East End — that was a gate, and it was not in that area. See `debug.md`.

### Play confirmation

**NPC classification CONFIRMED IN PLAY** — *"NPC classification looks good for now, will be tested
further but verified working for now."* The bare `NPC n` entries, the stacked bodies and the
enemies-as-NPCs regression are all gone from the tester's session.

Confirmed as a side effect the same session: the Session 80 npcdic naming rule, seen working live.
Interacting with a Seeq fired `settalknpcname`, his introduced bit flipped, and the mod moved from the
generic even slot to the personal odd slot — "Balzac". The dialogue box that followed read `...`, an
ellipsis-only line, so nothing was spoken. **Open, not a defect and deliberately not "fixed":** an
ellipsis-only line means the mod is silent for a whole interaction. Whether that deserves a cue is a
design question for the tester, and it needs a count of how common those lines are first.

**Still NOT confirmed: the exit work.** All four diagnostics are measurement only and have not been
read back yet — the crossing oracle needs the tester to walk through a Waterway exit, and the surface
inventory and span dump need a `'` dump from a map where exits are wrong.

Build clean, deployed.

---

## Session 85 — 2026-07-28 — [navigation] The seam cache was a full map behind: `HasWorld()` is liveness, not identity

**KEYWORDS: exit swapped mislabelled missing exit 199 steps waterway Garamsythe Central Spur Stairs
Northern Sluiceway map-jump surface seam cache stale HasWorld liveness identity PrimeMapJumpSurfaces
CachedMapJumpSurfaces InvalidateMapJumpSurfaces IsFieldNavSafe nav-safe gate teardown epoch
PublishClaims round trip crossing oracle false MISMATCH NavTrace transient mapId 0 route label
turn-by-turn destination name dropped**

### Session 84's diagnostics read back, and they solved it in one pass

Session 84 shipped four exit diagnostics as measurement-only and said they were unconfirmed. This is
that read-back. The tester walked Garamsythe Waterway 311 → 315 → 311 and reported exits correct on
the first load, then scrambled: the exit they spawned next to mislabelled, one exit gone, one
announced ~200 steps away and unpathable. **The surface inventory answered it outright** — no new
model, no RE, no Frida probe.

| log line | map | surfaces reported |
|---|---|---|
| 1937–1939 | 311 (first load) | g2 (13.3,4.3,116.4) · g1 (36.0,3.0,131.6) · g3 (49.0,−0.0,137.0) |
| 2178–2180 | **315** | **byte-identical to the three above — 311's** |
| 2298–2299 | **311 (re-entry)** | g2 (170.0,9.1,57.4) 16 polys · g1 (13.7,4.3,119.8) — **315's** |

The destination half was correct on every map (2282–2292: 311's re-entry read its own fresh blob,
`routineTable=+0x4990 count=18`, same three controllers, same three destinations as the first load).
Only the POSITIONS were stale, and since the join is `controller.group == surface.group`, stale
positions scramble which name lands on which doorway. All three symptoms are one fact:

- `2295` `group=3 dest=321 -> no map-jump surface on this map, dropped` + `nogroup=1` — **the missing
  exit** (No. 10 Channel); 315's walkmap has no group 3.
- `2317` `Exit, Lowtown: North Sprawl. right next to you` — group 1 bound to 315's surface at
  (13.7,4.3,119.8), which is where the player spawned.
- `2322` `Northwest, 199 steps (above)` — **the 200-step exit**; group 2 bound to 315's 16-poly seam
  at (170.0,9.1,57.4), off 311 entirely.
- `2281` `CROSSING ORACLE … <== MISMATCH — the group->destination binding is WRONG` — **a false
  accusation.** The binding was right; the oracle read the same poisoned surfaces. Session 84 wrote
  that map 311's non-identity controller→group mapping was "a lead to measure, not a conclusion".
  Correctly hedged: it was a coincidence, and the span dump ruled span bleed out on the same lines.

### Root cause: `HasWorld()` is a LIVENESS signal, not an IDENTITY signal

`MapQuery::CachedMapJumpSurfaces` latched on it:

```cpp
if (s_map != mapId) { s_map = mapId; s_haveMap = false; s_surf.clear(); }
if (!s_haveMap && HasWorld()) { ReadMapJumpSurfaces(s_surf); s_haveMap = true; }
```

`HasWorld()` proves *a* walkmap is resident, never that it is *this map's*. The map id flips BEFORE
the engine swaps the walkmap, so the first scan of a new map swept the PREVIOUS map's polygons and
then pinned them for the whole visit. The comment above it anticipated the *empty* case and never the
*stale* case. `exit_scan.cpp` had a second copy of the same latch on top, so fixing one alone would
have changed nothing.

### The rest of the nav stack was already right, and the log proves it

`NavMesh`/`NavReach` invalidate on the **teardown epoch** and re-read behind
`PlayerState::IsFieldNavSafe()`. Their flood counts are exact on all three loads: **139** polys on
311, **1997** on 315, **139** again on 311's re-entry (log 1949, 2190, 2311). The seam cache was the
one cache in the stack with neither mechanism. **The fix was to give it the two mechanisms that were
already there and already measured working** — not to invent a walkmap fingerprint, which was the
tempting third option and would have been a new unproven model.

### Shipped

- **`map_query.{h,cpp}`** — the cache split into one gated writer and pure readers.
  `PrimeMapJumpSurfaces(mapId)` (GAME THREAD, nav-safe frames only) is now the **only** caller of
  `ReadMapJumpSurfaces`; `CachedMapJumpSurfaces(mapId, out)` **never sweeps** and serves only when the
  cached answer was swept for that same `mapId`, so a reader can be handed this map's seams or
  nothing — never another map's. `InvalidateMapJumpSurfaces()` added.
- **`path_planner.cpp`** — `PrimeMapJumpSurfaces` called from inside the EXISTING nav-safe +
  non-origin-position block in `OnGameFrame` (ahead of `NavReach` and `NavTrace`, both of which read
  the seams); `InvalidateMapJumpSurfaces()` added to `OnMapTeardown` beside the other two. Bonus: the
  ~15k guarded reads moved off the input thread.
- **`exit_scan.cpp`** — the duplicate `CachedSurfaces` deleted outright. Not-yet-swept now logs ONE
  line instead of one "dropped" per controller; the per-controller line is kept for the real case
  (seams known, this group not among them), which is what makes a missing exit visible.
- **`exit_scan.cpp` `PublishClaims`** — made idempotent. Two independent defects, **both hit by
  exactly the A→B→A round trip the tester walks**: (1) `if (mapId == g_claimMapA) return;` latched on
  the FIRST call, so a map whose script was not readable yet published zero rows and could never
  republish — the oracle then said "the mod claimed NOTHING for that group" for the whole visit;
  (2) re-entering the map held in `g_claimMapB` skipped BOTH the prune and the `B = A` update, so old
  rows survived and new ones were appended beside them, and `ClaimedDestForGroup` returns the FIRST
  match — the stale one.
- **`nav_trace.cpp`** — `g_haveSurf` deleted; the seams are re-read per crumb. The oracle is the
  instrument the exit work is verified with; it does not get to be the last thing holding a stale
  copy. Also `mapId <= 0` now returns early: the transient 0 fired every crossing TWICE
  (`701 -> 0`, then `0 -> 311`) and the first of those CLEARED THE TRAIL, so the second had no crumbs
  and the oracle silently returned with nothing to say about the crossing that actually happened.
- **`nav_probe.cpp`** — both `'` sites now say "NOT SWEPT YET" rather than looking like "this map has
  no seams".

### Also shipped: `\` and `p` no longer speak the destination name

Tester request: *"there is no need to speak the destination name first … Destination can be inferred
from context."* Confirmed with them as **all four** planner utterances, so there is nothing to
remember about which one names its target:

| before | after |
|---|---|
| `Exit, Garamsythe Waterway: Northern Sluiceway. North 23. 23 steps` | `North 23. 23 steps` |
| `Exit, …: North Spur Sluiceway. At the exit. Northeast 3.` | `At the exit. Northeast 3.` |
| `Save Crystal. No path` | `No path` |
| `Save Crystal. Route unavailable` | `Route unavailable` |

`p` shares the drain and changes with it — confirmed with the tester. The label is still CARRIED and
now goes to the `NAV-ROUTE` drain lines (`target="…"`), because the log has to keep being able to say
which target a route was for.

### Play confirmation

**CONFIRMED IN PLAY** — the tester played the fix and reported *"works"*. That is the Waterway
transition case they reported at the top of this session: exits stay correct across a map change and
back again. Build clean, deployed, committed.

**Scope of the confirmation, stated honestly.** The tester confirmed the OUTCOME they could
experience — exits no longer scramble across a transition. The log-side checks below were **not**
individually read back, so they remain the falsifiers for this change and are the first thing to look
at if anything exit-shaped regresses:

- the `surface gN:` block after `announce: mapId=315` must NOT repeat 311's coordinates;
- `exits: controllers=3 surfaces=3 listed=3 | dropped: nogroup=0` on **both** visits to 311;
- no `<== NO CONTROLLER CLAIMS THIS GROUP` on 315;
- **CROSSING ORACLE = MATCH on both crossings** — the single strongest signal, belief against outcome;
- no `199 steps`; no `TRANSITION FIRED: mapId N -> 0`.

**Not separately exercised: the `PublishClaims` round-trip fix.** It needs a fourth leg
(311 → 315 → 311 → 315) with the oracle still reporting MATCH. The two defects it fixes are proven by
reading the code, not by this play session.

## Session 86 — 2026-07-28 — [navigation] A sign error the polarity switch hid, and the second door into NPC

KEYWORDS: funnel polarity TriArea2 dtTriArea2D portal left right winding string-pull routing through
walls impassable terrain corridor breach SegmentClear duplicate corners passed waypoint enemies as
NPCs Category::Enemy faction roster list 3 party member drop Penelo Urstrix Hyena actor pool overlap

Three defects reported from a Giza Plains session (Nomad Village 243 -> Toam Hills 239). **They were
two bugs**: the tester confirmed the "blocked exit" was the routing failure, not an exit failure
(*"same issue as 1, blocked. as in routed through impassible terrain"*). Exits were not touched.

### 1. The pathfinder routed through walls — a negated helper, papered over since Session 74

`TriArea2` (`path_search.cpp:57`) is the **exact negation** of Recast's `dtTriArea2D`, while
`Funnel`'s four comparisons were transcribed from Recast **verbatim**. Negated helper + unnegated
comparisons = the funnel's whole notion of left and right inverted. Expand both and the terms cancel;
this is arithmetic, conf 1.00.

`FunnelBestPolarity` had been masking it since the Session 74 rebuild by running both polarities and
keeping the shorter path. **The log proves that was never a measurement:** 75 routes FLIPPED, 14
as-labelled, and every one of the 14 had <=1 portal, where mirroring cannot change the length. 75 of
75 real routes flipped — a constant, not an observation.

The masking was not neutral. Portal left/right was ALSO being decided per portal by a
centroid-to-centroid side test, which only separates the two endpoints when the triangle pair forms a
convex-enough quad. On this mesh a triangle is often an entire corridor; obtuse/sliver pairs put both
endpoints on the same side. **A global mirror cannot repair a per-portal error, and shortest-wins
actively PREFERS the corrupted run** — a funnel that accepts a bound on the wrong side cuts through
the wall, so the invalid path is the shorter one. The route through impassable terrain was selected
*because* it was invalid.

**The correct labelling needed no test at all.** `PolyContainsXZDetail` pins the winding: it rejects a
point on edge `v[i]->v[j]` when `crossY <= -eps`, and `crossY == -TriArea2(v[i],v[j],p)`, so the
interior is always on the `TriArea2 <= 0` side — the RIGHT of `v[e]->v[e+1]`. Travelling parent->child
crosses right to left, giving **`left = v[e]`, `right = v[(e+1)%3]`, always**. Recorded in
`GameArchitecture.md` ("VERTEX WINDING").

Shipped: winding-derived labelling; the four comparisons flipped to match `TriArea2`'s own sign
(NOT the helper negated — its doc comment is the one that is correct for this frame); duplicate
corners collapsed at the two emit sites.

**The duplicate collapse is not cosmetic.** A duplicate at index 1/2 gave the passed-waypoint drop a
zero-length segment, tripping its `len2 < 1e-6f` guard on the first iteration and silently disabling
the whole Session 78 leg-0 reversal fix. The log shows exact duplicates on several routes.

### 2. The invariant that could never have fired

Both shipped invariants are LENGTH tests, and a path that leaves the corridor is SHORTER. They sat at
**zero hits** across the entire session that produced the bug report. Added `CORRIDOR BREACH`: walk
consecutive corners through `MapQuery::SegmentClear` — the game's own walk-class feeler — and name the
first leg the party cannot actually walk. Log-only and bounded; a breach means the geometry is wrong
and the fix belongs upstream, so rerouting here would only hide the next regression.

`FunnelBestPolarity` is kept for one release as a **self-check, not a crutch**: `as-labelled` must now
win every route with >=2 portals, and a FLIPPED win logs `POLARITY SELF-CHECK FAILED`. That is the
falsifier, and it answers the standing note in that function ("collapse this to one branch ON
EVIDENCE").

### 3. Enemies as NPCs — Session 84 closed one door of two

**Not a post-S84 regression.** One commit exists since `86365a2` (`3ddd129`, the seam cache) and it
touches no classification file. S84 struck the `present` (KIND+model) route and left the older
`named` route open, so the fix held on four enemy-free maps and failed on the first map with enemies.

Chain: `ResolveObjectName` decodes `sceneObj+0xf8` when `nameIdx < 0`, so a field enemy is *named*;
`if (!named && !interactive)` therefore never fires; `ClassifyByNameKey` returns NPC for anything
`isCharacter`; and `ScanCombatants` — the only pass that knows friend from foe — then skips it as
`AlreadyListed`. **NPC=4, Enemy=0** with a Hyena, two Urstrix and Penelo in the list.

**The falsifier S84 shipped for exactly this fired and changed nothing.** `s_poolOverlap` was declared
"COUNTER ONLY, no behaviour attached … a non-zero here on a map with enemies is how 'enemies show up
as NPCs' would come back" — and it read **4**. It now drives the fix.

Shipped: a faction override in `BuildLocked` for handle-table objects that are also live actor-pool
entries. Party members (tester's call) are **dropped**; a positive `Faction::Foe` verdict re-files to
`Category::Enemy`; everything else keeps its category. Guest/Ally/Neutral/Unknown are deliberately
left alone — this pass exists to stop enemies being called NPCs, not to re-adjudicate the map.

**Party membership is tested against roster list 3, not a kind byte.** The scene-kind nibble reads
`kind=1` for Penelo AND all three enemies, so keying on it would have filed the player's own party as
Enemy. `entity_scan.cpp`'s claim that it is "the game's own faction test" is **STRUCK**.
`ScanCombatants` still uses it for the battle-only population, which has been correct in play; the
comment now records the strike and names `FactionOf` as the replacement rather than changing a
working path blind.

`NoteFiltered` on the party drop is **mandatory**, not tidiness: a party member is a live engine
object, so its transform keeps refreshing `lastSeenMs` and the grace window would carry it back
forever while the pass logged the drop every rescan — the exact failure Session 83 hit.

Also fixed: the `inclusion:` log line measured **507 characters into a 512-byte buffer** before this
session added two counters. Bumped to 768. `snprintf` truncates silently and the counters at the end
are the falsifiers.

### Status

Built clean and deployed. **NOT play-confirmed.** The three falsifiers to read back:
`polarity=as-labelled` on every route with >=2 portals; zero `CORRIDOR BREACH` lines; `Enemy` = 3 and
`NPC` = 0 on Toam Hills with Penelo absent.

Paused for this: the four-item probe work (battle command character switch, notice board, dialogue
choice options, enemy cast logging). The battle-char-switch probe already returned good data and is
ready to wire up.

### Session 86 (cont.) — the funnel was only HALF of it: a portal is an OPENING, not an edge

The sign fix above is **CONFIRMED CORRECT** by the next play log: `polarity=FLIPPED` **0**,
`as-labelled` **26**, `POLARITY SELF-CHECK FAILED` **0**. Enemy classification also confirmed in play
by the tester.

**But `CORRIDOR BREACH` fired 20 times** — the new invariant earning its keep on its first outing.
Every one read `leg 1/1`, i.e. the whole route was a single straight segment. One example:

```
funnel: polarity=as-labelled kept=9.5m other=27.3m midpoints=13.7m straight=9.5m corners=2/5 portals
funnel: CORRIDOR BREACH on leg 1/1 -- (51.9,55.3) -> (61.0,58.0) is not walkable
geom corridor: 180 655 658 23 660 661
geom portals L|R: (53.0,60.0)|(52.0,52.0) (53.0,60.0)|(55.1,54.5) (53.3,61.7)|(55.1,54.5)
                  (55.4,67.7)|(55.1,54.5) (55.4,67.7)|(61.6,52.6)
```

`kept == straight == 9.5m` — the funnel produced the straight line, and working the intersections by
hand shows that line **does** cross all five portals in order. **The string-pull was correct; the
corridor was wrong.**

#### Root cause: A* certifies that a crossing EXISTS, not WHERE it is

`NavMesh::EdgePassable` probed a single short straddle at the **edge midpoint**. On this mesh a
triangle is often an entire corridor and the shared edges run 8-16 m. Where the taut path actually
crossed each portal, versus the one point that had been tested:

| portal | funnel crosses | probe tested | gap |
|---|---|---|---|
| 1 | (52.4, 55.5) | (52.5, 56.0) | 0.5 m |
| 4 | (55.2, 56.3) | (55.25, 61.1) | **4.8 m** |
| 5 | (59.5, 57.6) | (58.5, 60.2) | **2.7 m** |

Certified the middle, walked through the end. The obstacle sat in the untested part.

**A PORTAL IS AN OPENING, NOT AN EDGE.** Fixed in `nav_mesh.cpp`:

- `StraddleAt(p, e, neighbor, t, …)` — the probe segment at any parameter along the edge, factored
  out of the old midpoint-only `EdgePassable`.
- `EdgePassable` now samples `kEdgeSamples = 7` points and passes if **any** is clear — which is what
  adjacency should mean, and it stops a doorway whose middle happens to be blocked from being
  rejected outright. Short-circuits, so the unobstructed case still costs one raycast.
- **`EdgeClearSpan`** (new) returns the sub-span that is actually walkable. Clips to the **longest
  run** of clear samples, not to every clear sample: a pillar mid-edge leaves two gaps, and a portal
  spanning both would let the funnel thread straight through the pillar. Endpoints are the outermost
  sampled points, never the interpolated boundary — the conservative end of the interval.

`PathSearch::Run` now builds its portals through `EdgeClearSpan`, so **the funnel physically cannot
pull the path through a blocked part of an edge**. `clipped=` / `blocked=` counters added to the
`funnel:` log line.

#### And it repairs, because a diagnostic is not a guarantee

Tester's requirement is absolute: *"there should be no instance in which the character is routed
through terrain they can't walk through."* So `CORRIDOR BREACH` no longer just reports. On a blocked
leg the path falls back to the polyline through the **portal crossings** — the midpoints of the
clipped spans, i.e. the exact points `EdgeClearSpan` probed and found walkable — and re-validates.
Longer and turnier, but every leg tested. If BOTH breach, the taut path is kept and the log says the
corridor itself is wrong: that is upstream of the string-pull (an obstacle mid-triangle, where no
portal probe can see it) and it is the next session's lead.

**Status: built clean, deployed, NOT play-confirmed.** Read back: `CORRIDOR BREACH` should be zero;
`clipped=N` non-zero on maps with obstacles is the fix working, not a fault; `portal-crossing path
ALSO breaches` means the remaining fault is mid-triangle and needs a different instrument.

### Session 86 (cont.) — battle command menu: WHOSE menu is this

The tester heard only "Attack" when switching characters with left/right. Wired up, **no probe
needed in the end** — the one open question (id -> name) was answered by the decompile.

**`FUN_002778c0` (RVA `0x1578C0`)** is the controller: sole creator of the command panel
`FUN_0027ad70`, and the only thing reading the pad directly. Cases `0xa`/`0xb` turn pad RIGHT
(`0x20`) into `+1` and LEFT (`0x80`) into `-1`, **gated on `FUN_0035d4e0() -> *(int*)&DAT_022c8478 >
1`** — the game's own "2 or more party members" test, which is exactly how the tester described the
feature. It stashes the direction at `ctrl+0x2DB6` and sends itself message **`0x23`**, whose handler
resolves the new character via `FUN_0027c280(currentId, dir)` and writes it to `parent+0x2FE0`
(`parent` = `ctrl+0xD0`; the controller's own msg `0x2d` hands that field back).

**`parent+0x2FE0` IS A SCENE HANDLE — conf 0.99, from the game's own comparison.** `FUN_0035bc50`
builds the party record table (`&DAT_022c8080`, stride `0xC0`, count `DAT_022c8064`) and does:

```c
DAT_022c806c = thunk_FUN_003590d0();              // the LEADER SCENE HANDLE accessor
if (*(int *)(record + 0x04) == DAT_022c806c)      // -> this slot is the leader
    DAT_022c8074 = DAT_022c8064;
```

`FUN_0027c280` returns exactly that `record+0x04`. So the id is the same kind of value as
`DAT_022c7fe0`, and `BattleState::ActorForHandle` -> `NameForActor` resolves it with pure memory
reads — no game call, no new name plumbing.

**STRUCK before it was written: "we need a probe to learn the name path."** The probe was authored
for it, but the answer was in `FUN_0035bc50` the whole time. Resolve identity by finding what the
game COMPARES a field against, not by watching it at runtime.

Shipped in `ingame_menu_reader.cpp` (which already owns the battle command panel):
- Hook `FUN_002778c0`. On msg `0x23`, **after** the original (that is when `+0x2FE0` is written),
  speak the new character's name.
- On msg `0x01` (construct) arm `g_bcmdNeedName`, so the name is spoken when the menu **becomes
  active**, before the initial command focus — the tester's explicit ordering.
- `g_bcmdQueueNext` makes the command announcement that follows use `Speech::SpeakQueued` instead of
  interrupting, so the two land in order rather than the command cutting off the name.

Both flags are **transition latches, not dedup** (the CLAUDE.md carve-out): neither suppresses an
event, they only order two announcements. `CurrentBattleCharName` returns empty when the handle will
not resolve, and the caller then says nothing about the character rather than guessing.

**NOT play-confirmed.**

#### On the story-gated exit

The tester reports the Gizas North Bank exit is not open yet and routing still crosses blocked
terrain. **The log that showed it predates the portal-clipping fix** — that build had the breach
DETECTOR only. With clipping, a closed gate blocks all seven samples on the edge beneath it, so
`EdgePassable` returns false, A\* cannot cross, `reached == kNoPoly`, and `PathSearch::Run` returns
`Plan::NoPath` (`pass="unreachable"`, no partial route by deliberate design) -> the mod says
**"No path"**. That is the correct answer for a story gate, and it is what the old single-midpoint
probe was too coarse to produce reliably: one clear sample at the middle of a 16 m edge was enough to
declare the whole gate open.

### Session 86 (cont.) — CORRECTION: the breach detector was measuring the floor

**Routing is CONFIRMED WORKING in play** (tester: *"your pathfinder fix did work"*). The sign fix plus
portal clipping did the job.

**But `CORRIDOR BREACH` was firing on 100% of routes, and every one was a FALSE POSITIVE.** STRIKE
the earlier claim in this session that "20 breaches" evidenced obstacles in untested parts of the
portals -- the number was real, the reading of it was not.

`MapQuery::SegmentHit` flattens both endpoints to `from.y`, so handing it raw path points casts the
ray **along the ground**, where it clips the terrain the path is standing on. The tell was in the log
and unmistakable once read properly:

```
CORRIDOR BREACH on leg 1/3 -- (49.1,84.1) -> (50.0,84.0) is not walkable
```

A **0.9 m** leg starting at the player's own feet, on a route the tester then walked to the end. And
`portal-crossing path ALSO breaches` on every route -- both paths "failed" because both were tested
at ground level.

Fixed: `firstBreach` now lifts both endpoints by `kBodyPad = 0.9f`, agreeing with
`NavMesh::StraddleAt` and `entity_commands.cpp:85`, which have always done this. Behaviour was never
affected -- the repair only swaps paths when the portal path tests clear, and it never did, so the
funnel's (correct) output was used throughout. The damage was purely a log that lied.

**LESSON: a diagnostic is code, and it needs its own falsifier.** This one was written to catch
"routes through walls", was believed on sight because it confirmed the reported symptom, and was used
to justify a theory about portal geometry. The check that would have caught it was free: a 0.9 m leg
under the player's feet cannot be unwalkable. When a new instrument fires on everything, suspect the
instrument before the subject.

### Session 86 (cont.) — battle char switch: probe data received, implementation validated

`notes\probe_battle_char_switch_output.log` (run before the C++ went in; found late because the
launcher writes to `..\notes` and it was not checked) confirms the decompile derivation **exactly**:

```
[sw] #1 msg 0x23  dir=1   parent+0x2FE0  BEFORE=0x10000b  AFTER=0x10000c   <== CHANGED
[ros] roster 0 bcIdx=0x0 actor=0x2ce8a3c0 actorHandle(+0x08)=0x10000b name="Vaan"
[ros] roster 1 bcIdx=0x5 actor=0x2ce8b310 actorHandle(+0x08)=0x10000c name="Penelo"
```

`parent+0x2FE0` holds the value that `actor+0x08` holds -- i.e. the ACTOR HANDLE. So
`BattleState::ActorForHandle` (which scans the pool for `actor+0x08 == handle`) resolves it directly,
and `NameForActor` names it. Conf 1.00, now measured as well as derived. Also confirmed: msg `0x23`
fires once per left/right press, `dir` is +1/-1, and `DAT_022c8478` memberCount reads 2 for a
two-member party.

Note for anyone re-running that probe: `DAT_022c8064` printed as `131074` (`0x20002`) because it is a
**packed** field -- `FUN_0035bc50` writes it with `CONCAT62`/`CONCAT42`, so only the low 16 bits are
the record count. The probe read it as a plain u32. Nothing depends on it.

## Session 87 — 2026-07-29 — [ui+speech] Phrasebook, notice board, in-dialogue choices, battle menu

KEYWORDS: phrasebook Phrase::Get 12-locale notice board choice_reader in-dialogue choices 0x0E option
block ControlLength telop battle menu help bar FUN_0028fcb0 gambits ON OFF BtlChr bit 2 enemy ability
message 0x0E DefName Reloc pop-up prompt ownerChanged recycled window routing interaction cylinder

**RECONSTRUCTED FROM COMMIT `6f619e3` (2026-07-29 05:58).** This session shipped without a log entry;
the gap was found in Session 88 while checking the numbering. `CLAUDE.md` already referred to "Session
87" for the phrasebook, which is what fixes the number here. The commit message is the record — this
entry summarises it and adds nothing not stated there.

**PHRASEBOOK** (new `src/speech/phrasebook.{h,cpp}`). The 12-locale dictionary `CLAUDE.md` always
described now exists. ~100 mod-authored strings moved out of 21 files behind `Phrase::Get(Id)`:
directions, combat outcome words and verbs, entity categories, nav status, gauge labels, mod status
announces, the three baked-art title labels. English only; the other 11 columns are `nullptr` and fall
back — no invented translations. A `static_assert` keeps table and enum in step. Glue, format
specifiers, paths and game-supplied text deliberately stay put.

**NOTICE BOARD + IN-DIALOGUE CHOICES** (new `src/ui/choice_reader.{h,cpp}`). Both surfaces were
silent. Rows live in a `0x0E` option block inside the telop message, AFTER the question and column
headers — `GameText::ControlLength(0x0E)` is −1, so `Decode` halts there and the mod only ever saw the
question. Block layout is `FUN_003ffdf0`'s: count/flags header, then length-prefixed entries. A row is
multi-column and both separators decode to nothing, so columns are split and rejoined. The trailing
escape is the STATUS, a substitution indexed into an INLINE table at `window+0x1B8` — found via
`FUN_002b32d0`'s `param_7`, which Ghidra does not render at the call site. **Two detectors, one
speaker:** the 0x8000 dispatch drives the board's navigation, and a per-frame tick on `FUN_002a9980`
is the only thing that sees an in-dialogue choice (it polls input directly and sends no message). They
share a window, so the tick stands down while the dispatch is driving.

**BATTLE MENU.** `o` now reads descriptions on every list including submenus: the battle help bar is
the same window as the field one through a different wrapper (`FUN_0028fcb0` vs `FUN_00291d80`), and
only the field wrapper was hooked. Gambits (cmdId `0x0D`) announces ON/OFF on focus and on toggle,
read from `BtlChr+0x00` bit 2.

**COMBAT.** Enemy abilities are action category 7 → message `0x0E`, which sat in the log-only tier, so
enemy casts had never been announced. `AbilityName` re-pointed at `BattleState::DefName` instead of
the `Reloc()`-dependent chain that made every line say "attacks". `Reloc()` now branches on the read
succeeding rather than on the value, since 0 is a legitimate addend.

**NAVIGATION.** Routing to a wall-mounted or elevated target no longer fails: an object with no polygon
of its own (the notice board sits at y=2.0) bailed before the search ran. It now routes by the
interaction cylinder, reusing the existing reachable-but-unstandable fallback.

**POP-UPS.** The body prompt was gated on `ownerChanged`, an identity test — the game recycles pop-up
window addresses, so a second "return to the title screen?" and the quit prompt after it went silent.
Now armed by the game's own focus-change event.

## Session 88 — 2026-07-29 — [menus] The shop category was never missing — it was being cut off

KEYWORDS: shop category tab WEAPONS AMMUNITION LOOT interrupt queue Speech::Output g_queueNextItem
ConsumeCategoryAnnounce FUN_005655f0 FUN_0056e410 FUN_0056ded0 FUN_0056e5d0 inaudible race two speakers

**Symptom:** switching tabs in a shop said nothing about the category. A Frida probe
(`probe_shop_category.js`) was written and run to find where the tab label lives.

**The probe answered a question the mod had already solved.** Before reading a byte of it, the mod
log settled the matter — the category was already being resolved, logged AND spoken:

```
[19:26:39.421 +66078ms] [INV] category: owner=…2BED9000 "WEAPONS"
[19:26:39.421 +66078ms] [SPEAK-OUT] WEAPONS
[19:26:39.421 +66078ms] [SHOP] item: owner=…2BED9000 "Dagger, 195 gil, 1 in inventory"
[19:26:39.421 +66078ms] [SPEAK-OUT] Dagger, 195 gil, 1 in inventory
```

Six correct names captured (`WEAPONS ARMOR ACCESSORIES ITEMS LOOT AMMUNITION`), `Speech::Output
calls=2` in the same PERF window, same millisecond. The shop's row line ran `interrupt=true` ~0.2 ms
after the category and cancelled it before a syllable reached the user.

**Root cause — two speakers, two interrupt policies**, the exact failure CLAUDE.md warns about under
"one choke point per surface". The shop's tab change routes through the SAME shared refresh the party
item lists use — `FUN_0056ded0:82 -> FUN_0056e410:50 -> FUN_005655f0` (category), then
`FUN_0056ded0:83 -> FUN_0056e5d0` (row). `InventoryReader` had fixed this at birth with
`g_queueNextItem`, and `inventory_reader.h:32-33` even documents it verbatim — *"without it the
item's interrupt would cut the category off mid-word"* — but the flag was file-static, so
`shop_reader`, the other consumer of that same refresh, could not see it.

**Fix (2 files, strictly additive).** `OnCategoryRefresh` now also records WHICH surface it announced
for (`g_categoryOwner`), and exposes `InventoryReader::ConsumeCategoryAnnounce(owner)`.
`ShopReader::OnShopHighlight` consults it and, on a match, speaks its row QUEUED behind the category
and clears `g_lastItemId` so the new tab's row always speaks. **The party-menu path was not edited**
— `TryFocus` untouched, `g_queueNextItem` unchanged — because it is confirmed working and was
explicitly out of scope. Owner-scoping is what stops one surface consuming another's announcement;
the shop's two log lines carry the identical owner pointer, so the match is measured, not assumed.

**No second category reader was written.** Duplicating the `+0x180`/`+0xE8`/`+0xF0` chain inside
`shop_reader.cpp` would have re-created the very race it was meant to fix, one layer down.

**Also recorded in `GameArchitecture.md`:** the shop's previously-unnamed route into `FUN_005655f0`;
the trap that the per-tab record holds TWO source indices (`+6` label, `+7` category code) which
coincide only in a shop stocking every category — precisely how a probe "confirms" the wrong one; and
that `container + (pos+0x1E)*8` is not a pointer but the per-tab record's own address.

**Verified this session:** log rotation works exactly as designed — `x64\logs\` held exactly 20
archives with the cap holding, `…-Latest.log` beside `dinput8.dll`. An earlier "only one log exists"
claim was a bad search (globbed `x64\` only; archives are one level down), not a bug.

**LESSON: a feature can be fully implemented, logging correctly, and still be inaudible.** The log
said the feature worked. The user said it did not. Both were right — the utterance was emitted and
then cancelled. Read the log for the utterance before concluding a feature was never built, and when
two readers speak on one surface, check who interrupts whom before adding a third.

**Numbering note (RESOLVED — this entry was renumbered 87 → 88).** It originally took 87 by the
grep-the-file rule, because 86 was the highest header present. That rule assumes every session gets an
entry, and one had not: commit `6f619e3` (phrasebook, notice board, in-dialogue choices, battle menu,
combat, navigation, pop-ups) shipped with no log entry at all. It is the very next commit after
Session 86's `5bf0976`, and `CLAUDE.md` already called it "Session 87" — so 87 was its number, and
this entry took it by mistake. `6f619e3` now has a reconstructed Session 87 entry above, this one is
88, and the empty-category work is 89. **Lesson: grep the COMMITS as well as the log before taking
the next number — an unlogged session is invisible to a grep of the log.**

**Status: CONFIRMED IN PLAY** (tester, same day) — shop categories now speak on every tab switch,
followed by the row.

## Session 89 — 2026-07-29 — [menus] An EMPTY category spoke the previous one's row

KEYWORDS: empty category SHIELDS no shield owned stale paint previous category Leather Cap helms
IsEmptyCategory row array null +0xE0 FUN_0057cf20 FUN_005655f0 scroll count clamped generic painted
cell path TryFocus claim silent never speak filler STRUCK empty categories do not occur

**Symptom (tester):** in the equip list, SHIELDS — a category with nothing in it — announced
`"SHIELDS"` and then `"Leather Cap"`, a helm that was not highlighted and is not a shield. Every
populated category (HELMS, ARMOR, CHEST PIECES) behaved correctly.

**The log named the culprit reader immediately.** A populated category speaks through
`InventoryReader`; the empty one did not:

```
[INV] category: owner=…2BCF6400 "HELMS"          [INV] category: owner=…2BCF6400 "SHIELDS"
[INV] item:     owner=…2BCF6400 "Leather Cap"    [READER] focus owner=…2BCF6400 index=0
                                                 [READER]   item: "Leather Cap"
```

`[INV] item:` is `TryFocus`. `[READER] item:` is the **generic painted-cell path**. So `TryFocus`
declined the empty list and `menu_reader.cpp:365` fell through to the generic reader, which read the
cell's last-drawn text — the previous category's row.

**Why the cell looks populated.** `FUN_0057cf20:253-257` frees its row buffer and returns NULL when
it builds zero rows, so `FUN_005655f0:42` stores **null into `+0xE0`**. `FUN_005655f0:49-52` then
**clamps the count handed to the scroll widget from 0 to 1**, so `scroll+0xE8` reads 1 and the widget
reports a row at index 0 that does not exist. A null `+0xE0` with the scroll widget and tab table
still present is therefore the ONLY signal that a category is empty.

**Fix.** New `IsEmptyCategory(w)` — the same shape test `ReadList` uses, minus the row array whose
absence it is detecting. `TryFocus` now returns **true** (claimed) and speaks **nothing** for that
case, so the generic path never sees the cell. Populated categories are untouched: `ReadList`
succeeds and the first row is announced on the switch exactly as before. The pending category
handshake is consumed on the empty path too, so it cannot go stale into a later row.

**STRUCK: "Empty categories do not occur"** (`GameArchitecture.md`, S70). It was justified by "every
`[cat]` had n≥1" — a property of the SAMPLE, not the game. SHIELDS with no shield owned is built,
tabbed and reachable. **A negative claim founded on 'we never saw one' is not a finding.** Same shape
as S80's 48-id sample: a sample drawn from the range you already understand cannot falsify a claim
about the ones you do not.

**Measured in passing:** `OnCategoryRefresh`'s container and `TryFocus`'s `owner` are the SAME
pointer on the equip screen (`…2BCF6400` in both lines) — the party-side identity that S88 could not
confirm from any log then available.

**Status: CONFIRMED IN PLAY** (tester, same day) — SHIELDS speaks its name and nothing else;
populated categories still announce their first row on the switch. Log marker for the empty case:
`[INV] empty category -- claimed and SILENT`.

## Session 90 — 2026-07-29 — [combat] A charge verb on an execution line, and the mod's first settings menu

KEYWORDS: combat log, DamageLine, readies, uses, action category, row+0x1E, action_data.bin, mod menu,
F8, F4, combat verbosity, ShouldSpeakNow, 0x0D, 0x0E, FUN_00304850 repeat gate, FUN_00469af0,
text_capture ToggleInterception removed, mod_settings.txt, phrasebook

### The report

From play: enemy abilities are logged now, but the line is wrong. `"Urstrix A readies Slap."` is right
— that is the game's own sentence. `"Urstrix A readies Slap on Vaan. 14"` is not. Plus: make the
enemy charge announce speakable, put it behind a mod-menu toggle, and build the mod menu.

### 1. The verb — the announce vocabulary was mirrored into the execution line

`CombatFormat::DamageLine` runs on the damage **applier** `FUN_003112f0`, after the hit lands. Its
verb switch had been copied from `FUN_00469af0`, which is a **charge-phase** emitter — one caller,
`FUN_00304850` at action start, and its three ids (`0x0D` begins casting / `0x0E` readies / `0x0F`
uses) all describe an action about to happen. Categories 2/7/9 map to `0x0E`, so every landed technick
and enemy ability was narrated as still winding up. `combat_system.md` §9.1.3a said to mirror the
vocabulary in as many words; that sentence is now **STRUCK** in place.

Two vocabularies now, kept apart: the game's charge sentence read verbatim, and our execution line
`attacks` (cat 0 + unidentified) / `casts` (cat 1) / `uses` (cats 2, 3, 5, 6, 7, 9, 10).

**The user's gating question — does the game have execute-time narrative text? NO.** `FUN_00469af0`
has exactly one caller, fires once at action start, and picks its id purely by category. The only
target-bearing messages (`0x27`-`0x2C`, `0x35`, `0x44`-`0x49`) are specific boss/technick effect
lines. There is no general execute message, so synthesizing is correct — as it already was.

### 2. The category table, settled offline at 0.99 — no probe

`action_data.bin` is `32,612` bytes and `0x20 + 543*0x3C = 32612` **exactly**, which validates base,
stride and count before reading a field. The `row+0x1E` histogram gives **24 technicks, 13 Espers, 18
Quickenings** — three independent hard FFXII facts landing exactly. Full table in
`GameArchitecture.md`. Spot-checks: `0x096` Attack → 0, `0x0A9` Steal → 2, `0x1ED` Megaflare → 7.

This is the `feedback_extract_master_data` route: the shipped data answered it, so the archived
probe's 0.9 became 0.99 with nothing running.

### 3. The mod menu — F8 — and Combat verbosity — F4

First surface in this project that belongs to the mod rather than the game. `src/ui/mod_menu.{h,cpp}`.
Up/Down between settings, Left/Right to change, `o` for a description that changes with the value,
`F8` to close. `F4` toggles the setting from anywhere; both routes go through one `CycleSetting` that
owns the change, the write and the announce — two detectors, one emit point.

Persistence is `%LOCALAPPDATA%\FFXII-Screen-Reader\mod_settings.txt`, the directory
`entity_labels.cpp` already creates. **NOT `mod_config.ini`** — that belongs to the RVA byte-validator
and writing settings there would mask validator failures. The stale "should end up in mod_config.ini"
comment in `combat_format.cpp` is corrected.

Combat verbosity: **Normal (default)** / Verbose. Verbose adds `0x0D`/`0x0E` to `ShouldSpeakNow` and
**nothing else**. Damage lines stay log-only in both modes — they are appended `speakNow=false` and
never reach `ShouldSpeakNow`. Wording throughout is the user's own, which is what makes it admissible
under the phrasebook rule.

⚠ Default Normal reverses S72's decision that `0x0D` is realtime. That was the user's explicit call.

Arbitration rather than replacement: `StatusReader` and `MenuReader` already own the single
MenuNav/Describe slots, so `input_tracker` gained two first-refusal slots that the mod menu registers
and that decline while it is closed. Both existing paths are untouched.

### 4. F4's old owner, ripped out

`F4` was `TextCapture::ToggleInterception()` — a dev A/B diagnostic that disabled the painter callback
swap. Turning it off **stops row text being captured**, so one stray F4 silently killed menu reading
for a player who cannot see it happen. `InterceptionEnabled()` had zero callers; the flag had one read
site. Gone, with its two phrasebook strings.

### 5. Measured, then deliberately NOT acted on

`FUN_00304850` calls the announce only when the action **or** target differs from the previous one, so
an actor repeating one ability on one target announces **once**. Measured: Slap landed twice,
announced once; 4 Tier-1 messages in a session against 8 damage events; `HookedSprintf calls=1`. This
is a third suppressor beside the distance cull and the dedup ring, and the largest.

I proposed a new hook on `FUN_0030f760` to announce mod-side and make Verbose fire every time. **The
user rejected it, correctly:** the requested feature was "speak it as well as logging it", which is
the existing `speakNow` mechanism that loot, defeat+EXP and low-HP already use — no new hook, no
reconstruction, no probe. The measurement is recorded in `GameArchitecture.md` and `CLAUDE.md` as a
property of the game's pacing, and the README says so in plain words rather than the mod pretending
to a completeness it does not have. **Lesson: a real measurement is not automatically a work item.**

### Files

`combat_format.cpp` (verb switch + verbosity gate), `mod_menu.{h,cpp}` (new), `phrasebook.{h,cpp}`
(+8 ids, −2), `input_tracker.{h,cpp}` (F8, two first-refusal slots), `nav_commands.cpp` (F4/F8),
`text_capture.{h,cpp}` (rip-out), `dllmain.cpp`, `battle_state.h`, `CMakeLists.txt`.
Docs: `GameArchitecture.md` (category table + repeat gate), `combat_system.md` (strike),
`CLAUDE.md`, `Controls.md`, `README.md`, `debug.md`.

**Status: CONFIRMED IN PLAY** (2026-07-29, same day). Verb, mod menu, `F4`/`F8` and the toggle all
behave as intended.

**One open item, accepted by the tester and logged in `debug.md`:** with the menu open the arrow keys
also drive the camera, because the mod never swallows keys. That is the read-only rule working as
designed, not a broken intercept — the three options and their costs are written up there, and option
3 (mutating the DirectInput buffer) needs explicit permission before anyone reaches for it.

## Session 91 — 2026-07-29 — [dialogue] The page is a CURSOR, not a keypress

KEYWORDS: dialogue pagination, controller, gamepad, page 2 silent, SetConfirmCallback removed,
WM_CONFIRM, DIK_SPACE, DIK_RETURN, XInput, FUN_002a8c50, 0x188C50, widget+0x8A page cursor,
PTR_FUN_009164c8, 0x7F64C8, text dispatch table, slot 0, slot 2 null, FUN_002a9980, DAT_0215f200,
0x203F200, message window registry, FUN_003cb650 struck, telop FUN_002e16b0, NextPage removed,
dialogue_reader, choice_reader NotePage byte offset

### The report

*"Paginated dialogue only reads if Enter or Space is pressed on the keyboard. If the controller is
used to advance the dialogue, no subsequent lines are spoken at all. We do not rely on keyhooks, we
should be reading game events."* And, on the first plan: *"this is a complete restructuring of how
dialogue will be hooked and vocalized"* — one hook for both devices, not a second path beside the
first.

### The defect was one line, and it was the wrong KIND of fact

`message_reader.cpp` registered `InputTracker::SetConfirmCallback(&NextPage)`, and that edge came
from `dik[DIK_SPACE]` / `dik[DIK_RETURN]` in the DirectInput **keyboard** buffer. The proxy only
records `GUID_SysKeyboard` devices, and the game reads pads through **XInput** (one import,
`XINPUT9_1_0!XInputGetState`), which the mod does not touch at all. So the mod was never observing
*the box advanced* — only *a key that usually advances the box went down*.

A second bug rode along, invisible on a keyboard: the model counted **one page per Confirm**. The
first press on a page skips the typewriter reveal without turning it, so the count drifts.

### The dispatch table settled which function to hook — in one file already on disk

`FFXII-Decompile\output\text_dispatch_table.txt` dumps `PTR_FUN_009164c8` (RVA `0x7F64C8`), the
text-draw dispatch: stride 3 pointers, type byte `widget+0xA3`.

| type | slot 0 | slot 1 | slot 2 |
|---|---|---|---|
| 0 (choice-capable) | **`FUN_002a8c50`** | `FUN_002a9f00` | `FUN_002a9980` |
| 1 (plain) | **`FUN_002a8c50`** | `FUN_002aa800` | **null** |

That is the whole shape of the bug. `choice_reader`'s per-frame tick hooks `FUN_002a9980` — **slot
2, which is null on a plain dialogue box** — so the one dialogue hook the mod already had was
structurally incapable of paging a conversation without a cursor in it. **Slot 0 serves both.**

### The fix: read the cursor, do not catch the event

`FUN_002a8c50` (RVA **`0x188C50`**) starts its walk at `textBase + *(u16*)(widget+0x8A)` (`:77`) and
**`:199-200` is the instruction that advances `+0x8A` past a `0x03` page break** (guarded by
`+0x54 == 3 && mode == 0`). It is the cursor's writer, so hooking it is the event. It is
device-agnostic by construction: the release on a type-0 widget masks the engine's own unified button
globals, and we never have to read those.

`widget+0x8A` was **already in the mod, already play-confirmed** — `choice_reader.cpp`'s
`OFF_W_OFFSET`, used live since Session 87 to find option blocks. Meanwhile `FUN_003cb650` sat in
three documents as "the best unverified lead" for six sessions. It is **STRUCK**: a different window
singleton whose text is a pre-compiled glyph resource, so even a correct advance event there carries
no readable page.

### Built

**New `src/ui/dialogue_reader.{h,cpp}`** — one hook, and the single speech point for dialogue. Page 1
and page N take the identical route (page 1 is just the cursor's first value), so there is no second
speaker to race. Guarded by a change-check on `(base, cursor)`: the sanctioned per-frame exception,
naming `FUN_002a8c50` as the rule requires, and a transition detector rather than a dedup — the key
is dropped when `+0xC0` latches end-of-message, so re-entering a conversation speaks again. `+0xC0`
is read **PRE-call** because the next call consumes it (`:535-536` clears it).

Live-widget gate: `FUN_002e16b0` stores its window in `DAT_0215f200` (RVA `0x203F200`, 8 slots,
stride `0x68`) and the text widget is `window+0xD0`. Class identity alone would not do — the field
menu shares the `FUN_002a6190` window class. Rejects log once per widget and are hard-capped, so a
wrong gate is visible instead of silent without flooding a per-frame path.

**The page key is PER SLOT, and that is not cosmetic.** The first cut kept one global
`(widget, base, off)` triple. The game lays out several text widgets in a frame, so that single key
would flip between them every frame and re-emit the open dialogue page on each pass — a per-frame
repeat, exactly the failure the no-dedup rule warns is a redundant-call-path bug rather than
something to filter. Slot identity comes free from the registry walk, which now runs **before** the
change-check so a foreign widget can never disturb a live message's key.

**Deleted, not deprecated:**
- the whole keypress route — `SetConfirmCallback`, `WM_CONFIRM`, `g_confirmCb`, `g_confirmDown[2]`,
  `DIK_SPACE`/`DIK_RETURN`, both `DInputEdge` calls, the dispatch arm, the `Controls.md` row;
- `message_reader`'s telop hook, `OnTelop`, `g_pages`/`g_pageIdx`, `NextPage`, and the spent RAW BYTE
  DUMP diagnostic (its question — where the `0x0E` option block lives — was answered in S87).
  `message_reader` keeps the two surfaces that genuinely are its own and never paginate: the
  obtained-item toast and the menu system-message panel. It exposes `NoteSpoken()` so the `t`
  re-read has ONE store behind it rather than one per surface;
- `ChoiceReader::OnOtherMessage` and its `menu_reader.cpp` call site — the diagnostic that existed to
  ask this very question.

**Converged:** `ChoiceReader::NoteMessageText` + `NotePage(index)` became one
`NotePage(base, byteOffset)`, and `OptionCodec` seeks to the offset instead of counting `0x03`
bytes. The tick already used the byte offset — that representation was right all along, and now both
paths share it.

### Files

`src/ui/dialogue_reader.{h,cpp}` (new), `src/ui/message_reader.{h,cpp}`,
`src/ui/choice_reader.{h,cpp}`, `src/ui/menu_reader.cpp`, `src/input/input_tracker.{h,cpp}`,
`src/proxy/dllmain.cpp`, `CMakeLists.txt`. Docs: `GameArchitecture.md` (pagination block + two
strikes), `debug.md`, `Controls.md`, `plan.md`.

### Status — IN TESTING

Build clean, deployed. **Multi-page dialogue pagination CONFIRMED IN PLAY** by the tester the same
day, on the new mechanism.

**Still unexercised, and where a regression would land first:** the mid-dialogue Yes/No and the Hunt
notice board (`choice_reader` was rewired onto the byte offset — same speech, new feeder), and
tutorial/telop banners (the content-setter hook that used to speak them is deleted; they now arrive
through the page cursor). If dialogue ever goes silent, grep the log for `DIALOGUE text widget
outside the message-window registry`: that is the live-widget gate rejecting a page it should have
spoken, and it is a one-line fix.

---

## Session 92 — 2026-07-29 — [navigation+audio] The audio beacon, and SDL3 arrives

**KEYWORDS: audio beacon SDL3 SDL_OpenAudioDeviceStream SDL_LoadWAV_IO SDL_ClearAudioStream
SDL_SetAudioStreamFrequencyRatio RCDATA beacon_assets.rc audio_engine audio_clips audio_beacon
objective.wav Active_target.wav pan equal-power behind lowpass leg points outLegPoints endIdx
PathDirections::Describe RequestReplan CurrentEpoch seedBeacon silent replan PartyEngaged 0xEA4
0xEA9 FactionOf Category::Door Category::Shop hasNameSign sign repeats a doorway F9 mod menu
polled monitor**

**BUILT + DEPLOYED, NOT play-confirmed.** Not committed.

### What shipped

`\` still speaks the route; it now also drops an **audio beacon** on every corner where a spoken leg
runs out. The ping is panned toward the current corner and accelerates from 1.0 s to 0.2 s as the
player closes; reaching a corner advances **silently**; reaching the destination plays the sound
pitched up once and stops. Straying off the leg triggers a **silent re-plan**. In combat it switches
to `Active_target.wav` tracking the committed target live, goes quiet when there is no target, and
resumes the route on the leg it was holding. `F9` or the `F8` menu turns it off.

### THE ROUTE WAS BEING THROWN AWAY

`PathSearch::Run` fills a function-local `rawPoly`; `PathDirections::Describe` builds its legs in
another function-local. Both die on return, and a grep for `g_lastRoute` / `static std::vector<FVec3>`
found nothing persisting a route anywhere. **`Run::first/last` already tracked the corner indices** —
they were simply dropped at the `Run` → `Leg` boundary. So the fix is an out-param on the existing
`Describe`, not a second entry point: the corners MUST come from the same simplify/collapse/absorb
pipeline that produced the words, or the beacon aims at a corner the player was never told about.
`Leg` gains `endIdx`, carried through both the merge and the absorb passes. The DIAGONAL RULE is
untouched.

Last beacon point is forced to `poly.back()`: a trailing run that rounds to zero steps produces no
leg, so the final leg's own corner can stop short of the goal.

### SDL3 IS A THIRD KIND OF DEPENDENCY — it matches neither Tolk rule

Deploy does NOT copy it (the tester manages that file, as with Tolk). But the build **does link it** —
real headers, `add_subdirectory` of `D:/Games/Dev/SDL3-source` (release-3.4.4), `SDL3::SDL3` — so
unlike Tolk there is no silent-when-absent path: **without `SDL3.dll` the mod does not load at all**,
and the game will not start. The release zip therefore MUST ship it (now five files, not four).
Recorded in `release_procedure.md` as its own three-part note so nobody reasons about it by analogy
with Tolk. DQ7R's deploy script *does* copy SDL3 — deliberately not ported.

### The engine is a wrapper, not a mixer

First draft was a 4-voice sample mixer with interpolation and a callback-side scheduler. **The tester
struck it: "you shouldn't need the sampling since we're playing .wavs directly. just the SDL
integration."** They were right, and checking the 3.4.4 headers made it concrete — SDL already
provides pitch (`SDL_SetAudioStreamFrequencyRatio`), gain, retrigger (`SDL_ClearAudioStream`), format
conversion, and silence-when-empty. **The only thing SDL3 has no API for is PAN** (there is gain and a
channel map; neither is per-channel gain). So the one place samples are touched is interleaving mono
to stereo with L/R gains — applying volume, not mixing — and the behind-filter rides along in that
same loop. `audio_engine.cpp` came out ~170 lines with no callback, no voice array and no threading.

### The pan is a TRAVEL direction, not a turn instruction

Tester's correction, and it changed a justification as well as a test. The pan angle answers "which
way do I walk", not "how far do I rotate" — it is **the same angle as the spoken leg, from the same
`ReadCameraForwardStable` call `Describe` gets**, so "beacon pans right while the voice says West" is
structurally impossible rather than a bug to test for.

**The player does not drive the camera** — they treat FFXII as top-down movement, and the camera only
moves when the game moves it. That makes the behind-attenuation + lowpass **load-bearing, not
polish**: a pure `sin()` pan renders ahead and behind identically, and in a game where you turn to
look around that resolves itself the moment you turn. Here there is no such move, so front/back must
be carried by the sound or it is not conveyed at all.

### PartyEngaged: the probe was NOT needed

Planned a `probe_combat_state.js` to settle "am I in battle". **The tester pushed back — "we already
have it… it's basically just when being targeted" — and they were right.** Re-reading §7.1 rather
than its summary: `+0xEA4` stands at **0.97** and means literally "who is targeting me". What S49
struck was the *unfiltered* use of it (an ally's out-of-combat Cure sets the same bit, no hostility
gate). Filtering by `FactionOf` — already shipped — removes exactly that, using `+0xEA9` to map a set
bit back to its owner. The 0.90 `+4 & 0x100000` replacement the doc recommends is **not used**.
Escape mode needs no flag either: the state clears when foes stop targeting you. Known deviation — it
resumes when the escape *succeeds*, not when it is *toggled*.

### Doors and Shops: the discriminator was already there, and already discarded

Tester asked for doors as their own category, and suggested shops be told apart by "an interactable
that is text only, same label but with no map transition". **`TagDoorwaysAndDropSignTwins` already
finds exactly that pair** — `entity_postscan.cpp:238-240` even calls it "the shop SIGN and the shop
DOORWAY" — logs it as `sign repeats a doorway`, and **deletes the twin without recording that the
pairing happened.** One flag (`hasNameSign`) set before the erase, while the index is still valid, and
`Category::Shop` falls out. New order: `All, Exit, Door, Shop, SaveCrystal, …`.

**Not yet earned:** the evidence is East End's shops (doorway within ~2 m, twin 6-15 m away). That
shows shops HAVE the pairing; it does not show non-shop doors never do. The drop already logs
unconditionally — **grep `sign repeats a doorway` across several maps and check every pairing is a
shopfront before trusting the category.** Same shape as S89's mistake: a property of the sample read
as a property of the world.

**Deliberately NOT changed:** the `CatSign` fallback for an unnamed doorway. With a Door category the
generic path would say "Door", but "Sign" there is a word the tester authorised specifically for the
North End sign the game renders as "???" — swapping an authorised word for a generic one is not a
refactor. Left alone with a comment saying why.

### First play test: the beacon works, and found two bugs

Tester ran it and the beacon sounds, tracks, accelerates and advances. Two defects, both fixed below.

#### 1. The pan was MIRRORED — and the cause was computing one angle twice

Route to the south gate spoke "Northwest"; the beacon panned **hard right**. The section above
claims a beacon/voice disagreement is "structurally impossible" because both come from the same
`ReadCameraForwardStable` call. **That claim was wrong, and the reason is worth keeping.** Sharing
the *facing* input is not sharing the *angle*. `BearingToPan` took that shared facing and then
re-derived the bearing itself:

```
beacon:     atan2(dx,  dz) - facingRad
NavCommon:  atan2(dx, -dz) - CompassFaceDeg(facingRad)      // == 180 - yaw, not -yaw
```

Those are not two spellings of one expression — the second is the **exact negation** of the first
(reflecting Z flips the angle's sign, and `180 - yaw` flips it back the other way). Negation leaves
`cos` untouched and flips `sin`, so **front/back was correct and only left/right was inverted**,
which is precisely the failure that survives a code read: every term is present and plausible.
Northwest is relative octant 7 ≈ 315°, `sin` = −0.707 = left; the shipped code emitted +0.707.

The fix is not a sign flip. `Norm360(BearingDeg - CompassFaceDeg)` appeared **six times** in
`nav_common.cpp` as an inline expression with no name, which is what made a seventh, wrong copy the
path of least resistance. It is now **one shipped function, `NavCommon::RelativeBearingDeg`**, and
`CardinalBearingRelative`, `EgoBearing`, `RelativeOctant` and the beacon all call it. The plan for
this feature said the pan and the word are "two encodings of one value" that "must never be computed
twice" — the rule was right and the code did not honour it, because the value had no name to reuse.

**`Seed` now logs the first leg's octant beside the pan it will use** (`leg 1 octant=7 pan=-0.71`),
so the next sign error is a grep rather than a play session. Octants 1-3 must pan positive, 5-7
negative, 0 and 4 near zero.

#### 2. A gate crystal was filed under Doors

First gate crystal the tester has reached. `ClassifyByNameKey` correctly identifies it from the
game's own npcdic id (466 → `Category::GateCrystal`), and then the new Door/Shop promotion
overwrote it, because that loop re-categorised **any** `doorway` entity that was not an NPC.

`doorway` is TRUE and CORRECT on a gate crystal — it teleports, so the map script binds it a
`setfieldsignlocationjumpinfo` record like any other transition. So the flag is not the bug; the
promotion's reach was. **Door/Shop now refine `Category::Object` only** — the bucket the classifier
uses when it recognised nothing. Every other category is a positive identification off the game's
own name id or the character class, and a **proximity heuristic must never overwrite a name the game
supplied.** Clearing `doorway` instead would have been the wrong repair: the sign-twin dedup keys on
that flag, not on category, and would have started leaking duplicate shop signs.

Note the near-miss: the old guard was `!= NPC`, written to stop a person near a shop sign becoming a
door. It was the right instinct applied to one category instead of to the general rule.

### Second play test: pan confirmed fixed, two category defects left

Tester: *"works perfectly"* on the beacon — the pan now matches the spoken route. The remaining two
reports were both about categories, and neither was where I had looked.

#### 3. The gate crystal was in SAVE crystal — and the right answer was already written down

Narrowing the Door/Shop promotion (fix 2) did not put the crystal in GateCrystal; it revealed where it
had actually been classified all along. From the `'` dump: `nameIdx=435`, and `ClassifyByNameKey`
mapped `435-459 → SaveCrystal` under the comment *"area/life crystals"*.

The game's own npcdic name table settles it at conf **1.00** with no probe —
`FFXII-Decompile\notes\npcdic_names.csv`, extracted from `PS2Data\...\npcdic.bin`:

```
435 Rabanastre Crystal   436 Nalbina Crystal   ...   459 Ridorana Crystal
460 (Crystal 26) ... 465 (Crystal 31)          466 Gate Crystal
467 Life Crystal         468 Urn               469 Save Crystal
```

**435-459 are the 25 named per-area TELEPORT crystals**, 460-465 unused placeholders, 466 the generic
label. So `435-466 → GateCrystal`, and only `467`/`469` → SaveCrystal. Id 435 is literally
"Rabanastre Crystal" — a gate crystal, in Rabanastre.

**The lesson is where the answer was, not what it was.** `GameArchitecture.md` already said
*"435–465 = area gate crystals"* — **correctly** — under "Object name (master data)". And 130 lines
further down, the same file restated it as *"435–459/467 crystals"*. The code implemented the second
one. Grepping the canonical registry was not enough, because the registry contained a precise
statement of the fact **and a mushier paraphrase of it**, and the paraphrase is what got built. Struck
in place with the full table. **When you restate a fact you have already recorded, restate it exactly
or point at the original.**

Contributing cause worth its own note: the `rescan:` tally never got `Door=`/`Shop=` columns when
those categories were added this session, so it read `Save=1 Gate=0` and the two buckets an object
could have been promoted into were simply not printed. It now counts every category and sums to
`out.size()` — **a breakdown that does not add up hides the bug it exists to expose.**

#### 4. "South Gate" was a portal stuck in Interactables — the `+0x70` GROUPS are not one pool

Tester's rule: *"doors are portals that have map data, shops are doors that also have signs with the
same label within a close distance from them."* And a decisive extra observation — **Lowtown's own map
classified doors correctly**, so this looked map-specific. It was not; that map was lucky.

`doorway` had one writer: a **2.5 m radius test against ANY `+0x70` record**. Rather than guess a
threshold I added `LogSignTableOnce` and read it. Map 306, the whole table beside the objects:

```
g0[0] (119.95,-10,127.00) destIdx=20 -> "South Gate"          (124.00,-10,127.00) = 4.05m  MISSED
g0[1] (138.24,-10,140.53) destIdx=21 -> "Lowtown"             (137.82,-10,144.00) = 3.50m  MISSED
g2[0] (115.00,-10,151.00)            -> "Rabanastre Crystal"  (115.00,-10,151.00) = 0.00m  tagged
g1[0] (112.12,-10,198.00) areaId=14  -> walk-onto exit surface, bbox z[198..225]  = 47m
```

**My arrival-marker hypothesis was wrong.** The crystal was not tagged by a group-3 marker; `g2[0]`
sits *exactly* on it — a gate crystal's own teleport record. Two independent faults:

1. **Only group 0 holds press-Enter doorways.** Group 1 is the walk-onto map-jump surface — the one
   record whose `areaId` resolves (14), landing inside the exit surface bbox, and already the `Exit`
   category. Group 2 is the crystal's teleport record. Group 3 is arrival markers. Consulting every
   group is what tagged the crystal, and restricting to group 0 kills that by **data** rather than by
   the category-precedence guard from fix 2 (which stays as belt and braces).
2. **A group-0 record is never co-located with its door**, because it marks the "→ area" ARROW rather
   than the thing you press. So matching is **nearest-wins per record**, not everything-in-radius:
   each record claims its closest eligible non-NPC object, `kSignMatchDist = 8.0f` demoted to a sanity
   bound. The loop is inverted to records-outer — per record there is exactly one door; per object the
   question is ill-posed. Unused group-0 slots read exactly `(0,0,0)` (map 702: 4 live, 20 zeroed);
   filter on position, **not** on `shown`, which `map_exits.h` records as a live render gate and would
   make classification depend on where the camera points.

**The refuting constant was ten lines above the broken one in the same header.** `kSignMatchDist =
8.0f` already carried: *"a sign marks the '→ area' arrow and the slot is the volume you step into, so
they are never coincident: measured 3.6-6.4 m apart on every East End district door, against ~25 m to
the next-nearest door."* Same relationship, same scale, already measured — and `kSignObjectDist =
2.5f` sat under it doing the work. **Two constants for one geometric fact, and the wrong one was
load-bearing.** That is the same shape as defect 3's mushy-paraphrase and defect 1's duplicated angle:
three defects this session, one cause — *a fact stated twice gets built on in its weaker form.*

Also, the diagnostic's own first version latched on the first call of a new map — the one call where
`out` is still empty, because the handle table streams in over the next few rescans. It printed the
table and not a single object line. It now latches only once there is something to compare against.
Full group table with per-row confidence recorded in `GameArchitecture.md`; only the group-0 row is
load-bearing, and the 0.90 `n=1` group-2 identification is explanatory with nothing built on it.

### Verification status

Play-confirmed: the beacon sounds, tracks, accelerates and advances legs; **the pan agrees with the
spoken route**; the **active-target beacon works**; the gate crystal reads under Gate Crystal; "South
Gate" and "Lowtown" read under Doors. The category work was tested on **both** the Lowtown map and the
South Gate map, so the group-0 + nearest-wins rewrite did not regress the map that already passed under
the old radius — the one real risk in changing the matching rule underneath it.

#### 5. OPEN — the objective beacon does not stop when the PLAYER starts the fight

Found in the same test. `PartyEngaged()` is *"a living party actor has a `+0xEA4` bit set whose owner is
a `Faction::Foe`"*, and `+0xEA4` means **"who is targeting me"** — a **being-attacked** test and
nothing more. FFXII is seamless-battle, so combat starts two ways, and only one is covered: a foe
aggroing works, **the player attacking first does not**, so the route beacon keeps pinging toward a
shop mid-fight.

The definition came from the tester's own words (*"it's basically just when being targeted"*) — true of
the aggro case and silent about the other. **An accurate statement about one direction, implemented as
though it covered both.**

Engagement must become the OR: targeted by a foe **or** committed against a foe. And the second half
**already exists and is discarded** — `ActiveTargetPos()` resolves `CommittedTargetOf(LeaderActor())`
today, but the beacon only calls it *after* `PartyEngaged()` returns true. The discriminator is in the
file, behind the wrong gate. That is the third time this session a needed value turned out to be
computed and thrown away (the route corners, the shop sign pairing, now this) — **before adding a
source, check whether the value is already being calculated and dropped.**

Implementation constraints, in `debug.md` so they are not got wrong twice: filter the target's faction
to `Faction::Foe` (committing a *heal* is not combat — the same ally-heal false positive S49 struck on
the other side), scan party-wide rather than leader-only to match the existing side (gambits commit
non-leader members), and **measure how long a commitment lingers** — if it outlives the foe, the beacon
sticks in combat mode, which is this bug mirrored.

Still unexercised — not failures, simply never tested: the ally-heal false positive on `PartyEngaged`,
and whether `STALL_SCOPE("AudioBeacon::OnGameFrame")` ever shows up in a stall warning (it must not —
that would mean audio work reached the game thread).

Still not earned: **the Shop rule's premise.** Unchanged by any of this — grep `sign repeats a
doorway` across several maps and confirm every pairing is a shopfront. Shops *have* the pairing;
non-shop doors have never been shown to lack it. Cheaper to settle now, since `LogSignTableOnce`
prints the whole `+0x70` table per map beside it.

### ⚠ NOT COMMITTED

This entry is written but **the work is still in the work tree** — the shell permission classifier was
down at the end of the session and neither Claude nor the tester could run `git`, so the commit was
deferred to the start of the next session. Two things for whoever picks it up:

- **Session 92 is logged with no commit.** This is CLAUDE.md's `6f619e3` hazard inverted — that was a
  commit with no log; this is a log with no commit. **The next new session is 93.** Do not renumber
  this entry, and do not assume the highest logged `## Session N` has been committed.
- **Stage explicitly (`git add <paths>`), never `git add -A`** — the tree may also hold another
  track's files. The full file list is in the session memory
  (`project_audio_beacon_sdl3_session92.md`).

## Session 93 — 2026-07-30 — [navigation] The hard walkability check is a BODY vs BOUNDARY test

**KEYWORDS: hard walkability passability check routes through impassable terrain water cliff steep hill
fence no progress slide FUN_0022f9b0 border clearance elliptical footprint was-blocked bit FUN_00230c10
body sweep pure getter replicate not call Plan::Frontier never dead end ban portal re-search
kMaxSegChecks truncated corner inset SegmentHit flatten Ridorana failMask 0x0C areaManifest seam epoch
crossing oracle artifact PartyEngagement bidirectional stray re-plan storm party membership In party
field menu rename S93**

Six items came in for a ship-prep build. Five landed; one (party keys) was retracted by the tester and
one (gambit + teleport menus) is probe-gated and deferred to the next build.

### The tester was right, and three research passes had been asking the wrong question

The report was *"there are still bugs in the pathing... the pathfinder routed through impassible
terrain"*, with water on the Strand as an example. Two multi-agent passes (27 agents) went looking for a
terrain ATTRIBUTE that distinguishes water — and exhausted the walkmap record proving none exists. The
tester then corrected the framing outright:

> *"You hyperfocused on water. Water is just one example. We were looking for a HARD WALKABILITY CHECK...
> Focus on either collision physics or a strict function that stops the player from moving in a certain
> direction. It isn't a hard stop — the player can still walk against a cliff or steep hill, they just
> make no progress. The player can, to some extent, slide along angles."*

**That description IS the diagnosis.** "No progress head-on, slides when oblique" is the signature of a
sweep-plus-depenetration MOTION solver, not of an attribute lookup, and it was in the first sentence of
the original report. `FUN_0022f9b0` (RVA `0x10F9B0`) is the function: the character's elliptical footprint
may not overlap any triangle edge whose neighbour is absent **or fails `FUN_00230a40` for the movement
class** — `:85-88` demotes the second case to the first, which is why walls, cliffs, water, fences and
unwalkable ground all come out of ONE branch. On violation it pushes back to exact tangency along the edge
normal and sets `moveCtx+0x60 |= 0x10`. The push removes only the normal component, and
`FUN_002327d0:233-238` zeroes that normal's Y when ground-locked, so head-on cancels and oblique slides.

**Cliffs need no height test**, because `FUN_00380c40:24-28` pins the actor's Y to the poly plane — a cliff
is an edge with no neighbour. Which is independently why S68 and S75 were right to strike a step/slope
gate twice: there is no vertical term in the engine's refusal to gate on.

Full mechanism, RVAs, offsets and the purity audit are in `GameArchitecture.md` ("The hard passability
check"); the reasoning and the lesson are in `debug.md`.

### Replicated, not called — and the ruling that settled it

`FUN_0022f9b0` takes no footprint argument; the body reaches it only through globals `FUN_0022ef20`
writes. So **no footprint-aware engine predicate can be called without writing game memory.** Asked
directly, the tester ruled: *"DO NOT WRITE TO THE GAME, simply do what we're already doing by calling the
game's own NavMesh equivalent."* So the border test is a memory-only replica (`nav_footprint.cpp`), and
only the provably write-free half is called — `MapQuery::BodySweep` wraps `FUN_00230c10`, whose complete
29-function closure was verified free of game-memory writes by transitive closure plus an
assignment-target and out-param scan.

### The architectural defect: a validator that could only ever degrade

Validation sat AFTER the search, as a lambda over a corridor A* had already committed to. Its three
possible outputs were accept, substitute one pre-built alternative, or **accept the thing it had just
proved wrong** — and on the tester's own log it took the third option on **9 of 53 routes (17%)**,
shipping a path the mod had disproved with no change to the speech.

Now a failed validation **bans the offending portal and searches again** (capped by WORK, not retries), so
the detour is found by A* rather than approximated. And routes no longer dead-end: `Plan::Frontier` routes
to the reachable point closest to the goal, from a `bestNear` the A* loop had tracked since S74 and then
thrown away at `return Plan::NoPath`. It is a separate enum value so every consumer's switch is forced to
handle it — because the failure recorded at that exact line was a near-goal fallback SPOKEN AS A NORMAL
ROUTE, which walked the tester to a spot 3 m from an exit 7.8 m overhead. It speaks the legs, then
"Blocked", then how far short; both words already existed.

Four smaller defects, each of which had been masking the others: `SegmentHit` flattened its far endpoint
(a false BLOCK on rising portals, licensed by a 0.98 claim now struck); `kMaxSegChecks = 24` printed a
24-leg claim as a 140-leg one; the passed-waypoint drop was bypassed on exactly the routes that breached;
and every taut corner sat ON the boundary the footprint may not overlap. Details in `debug.md`.

### Ridorana: waiting for a resource the map does not have

*"Ridorana pathfinding fails completely."* On map 1101, `failMask=0x0C[areaId,areaColl]` on all ~3,530
field frames of the visit while the other six conditions passed. `FUN_003ea820` (RVA `0x2CA820`) writes
`DAT_02b5e0b8 = -1; DAT_02b5e0c0 = 0` when the area's streamed resource is absent, and nothing retries —
so `0x0C` is TERMINAL, not "not loaded yet", and no retry window could clear it. Navigation never read
that resource; the pair was a heuristic and `areaColl` was a misnomer. Both bits are now log-only.

Two companions from the same log, both cases of the mod accusing itself: the seam sweep was mis-keyed at a
transition's leading edge (now keyed on the teardown EPOCH — **strikes** the "nav-safe spans the whole
transition" justification), and both `CROSSING ORACLE ... MISMATCH` lines were ARTIFACTS of a gate-crystal
teleport 48-49 m from the nearest seam. **A diagnostic with no bound on its own confidence will eventually
indict correct code.**

### Beacon engagement, and a re-plan storm nobody had reported

`PartyEngaged` read only `+0xEA4` = "who is targeting me", so it was a being-ATTACKED test: start the
fight yourself and the route beacon kept pinging its way to a shop. `PartyEngagement` returns the OR plus
the target it resolved, so the beacon consumes one answer instead of re-deriving half of it one line too
late. The commitment side requires a LIVING `Faction::Foe`, which filters out an ally heal and closes the
one staleness channel the decompile leaves open. **The UNKNOWN S92 flagged is answered without a probe.**

Separately, the log showed `off route -> silent re-plan requested` firing every 3-7 seconds during
ordinary walking. The leg's reference line was seeded with the PLAYER's position instead of the route's
previous corner, so advancing a leg while standing a few metres to one side skewed the line, walking the
real route read as deviation, and the re-plan re-seeded the same skew. The corners were in `g_legs` all
along. The line now prints the measurement, because a bare event line sat in the log for a whole session
without being read as a defect.

### Party membership, and a rename with a trap in it

The field menu's first command `0x4b3` is a membership TOGGLE, not a stat screen — and the mod was already
speaking on that surface, wrongly: the live log has it saying *"Vaan, Level 99, HP 17026/8513, MP 648/648"*
one second after the row "Party" was spoken, which was the reported defect verbatim. `FUN_00284c90`
(RVA `0x164C90`) is the writer, and the writer is the event. Highlight now speaks "Vaan: In party" and a
toggle speaks the new state alone (the cursor does not move, so a press would otherwise be silent). Two
new phrasebook strings, authorized by the tester this session and no others.

The chooser moved out of `ingame_menu_reader.cpp` (739 lines) into `char_select_reader.cpp`, which owns the
shared grid for Party / Status / Equipment / Gambits — one controller, four commands. Its old header block
claimed `FUN_00285a10` does not fire on menu entry; the live log refutes that, and the S31 observation
behind it was made on the one-character prologue party — **a property of the sample, not of the handler.**

**The rename trap:** the game itself calls the outer `R` menu the "Party Menu" — that is the label on its
own Controls screen, which the mod reads back to the player, and its own banner says "Clan Primer has been
added to the Party Menu." Renaming the captured `Controls.md` row would fabricate a UI label. Ruled by the
tester: code, comments and README say **field menu**; the captured row keeps the game's words with a
footnote explaining the mapping.

### Deferred, and why

- **Party keys `4`/`5`/`6`** — retracted ("on second play, the party keys did work"). The logs never
  contained a nameless party line, and the one silent `6` was a genuinely empty slot during a two-member
  party. Written up in `debug.md` as NOT A BUG so it is not re-diagnosed. The real gap it exposed: keys
  reach 4 of the 9 roster slots `BtlChrForSlot` already supports, so reserve members are unreachable.
- **Gambit setup menu (`FUN_005691e0`) and the gate-crystal teleport list** — both fully reverse-engineered
  this session and both PROBE-GATED by the tester's own choice. The gambit screen needs one measurement
  (a row-0 crossing sends a SECOND `0x8000` whose first carries a stale column cursor) and the teleport
  rows rest on a 0.80 hypothesis about the option-entry bytes. Neither needs a new hook — every event
  already travels the `FUN_00247510` dispatch `menu_reader.cpp` owns. RVAs and offsets are recorded so the
  next session starts from the design, not the search.

### Files

New: `nav_footprint.{h,cpp}`, `path_funnel.{h,cpp}`, `path_validate.{h,cpp}`, `map_seams.{h,cpp}`,
`battle_state_diag.{h,cpp}`, `battle_state_names.cpp`, `battle_state_internal.h`,
`ui/char_select_reader.{h,cpp}`.
Splits (no logic change): `path_search.cpp` 667→499, `battle_state.cpp` 584→468, `map_query.cpp` 499→402,
`ingame_menu_reader.cpp` 739→641. `PerformanceIssues.md`'s claim that every `.cpp` was under 500 was false
and is corrected with measured numbers.


## Session 94 — 2026-07-30 — [menus] Probe results in, and a name resolved through the wrong table

**KEYWORDS: gambit setup screen FUN_005691e0 0x4491E0 display records column cursor double 0x8000
teleport destination substitution 0f2e window+0x1B8 arg table MAX_OPTIONS 32 real count party keys
4 5 6 no name CharacterName DAT_02ebf130 PoolString input thread DefName static record codec
operators 0xA6 = 0xB2 < 0xC4 >= word.bin listhelp_targetchip rescan enemies NPCs factionVerdict**

Both Session 93 probes came back with everything they were built for. They looked empty to the tester —
*"only values, no text output captured"* — because a probe is forbidden from calling the game's own text
decoder from the Frida thread, so every string is dumped as hex and decoded offline. That is working as
designed, but the run instructions should say so next time.

### The party keys, and a claim that was a property of its sample

`[PARTY] slot 1 charId=0 "Vaan, Regen, ... HP 17026/8513"` followed by
`slot 2 charId=3 "Regen, Libra, HP 8437/8437"` — vitals every time, the name only for slot 1.
`NameForBtlChr` resolves through `ActorForBtlChr`, which **scans the field actor pool** for an actor whose
def pointer is that BtlChr. Only the leader reliably has one. Vitals come straight off the BtlChr, which
is exactly why they never failed: *"only reads the status effects and the vitals"* is that split, spoken.

**A reader that resolves a ROSTER member through the actor pool is broken by construction.** Session 93
closed this as NOT A BUG on the sentence *"there is no party line in either log where the name is
absent"* — true of the two logs on hand, and a claim about the code it could not support. The leader is
always slot 1, so the first case anyone checks always passes. That entry is struck in `debug.md`; this is
the third time this file records a sample standing in for a population (S80, S89).

The fix could not be the obvious one. `DefName(0x02, charId)` answers the same question and the Party
screen already used it successfully, but it is a **game call** that stages its arguments in the *static*
record `DAT_022ca520` — and the party keys dispatch on the **input thread**. Two callers would race in
one buffer. So `BattleState::CharacterName` walks the character master table by hand
(`DAT_02ebf130` → `rec+0x30` → the shared pool, from `FUN_0031c5d0 case 1`), which is pure reads and
therefore thread-agnostic. `char_select_reader.cpp` moved onto it too, so there is one choke point.

### The teleport rows: everything needed was already written

The destination rows are literally `0F 2E <idx> 90` and nothing else — four bytes, no characters. So
`DecodeRow` returned empty and `BuildOptionLine` bailed on `text.empty()` **one line before** it would
have resolved the argument holding the place name. `RowArgIndex`, `ResolveArg` and the `window+0x1B8`
table were all already there and already correct; the bug was the order of two lines. Row 25 spoke only
because "Cancel" is a literal.

Two things fell out of the same measurement. The block header gives the real count (`0x9a & 0x7F` = 26),
so `MAX_OPTIONS`'s hardcoded 32 is gone from `OnFocus` — that is why the log read `choice[25/32]` and why
the hide-mask walk ran six slots past the terminator. And `g_dispatchCovers` sits *after* the empty-text
return, so the two-detector arbitration had never once run on this surface; resolving the row is what
reaches it.

### The gambit screen, and the picker I did not build

All four probe criteria passed: `owner` IS the panel, `val` IS the display-record index, `0xFFFF` ids
land on exactly the rows whose class byte is 2, and `popcount(panel+0x124)` matches the rows whose own
bit is set. Two hazards were **measured** rather than reasoned about: the panel resends `0x8000` for an
unchanged state (seven identical messages on entry, from the per-frame cat-`0xA` reconcile), and a row-0
crossing sends two `0x8000` for one keypress with the **stale** column in the first. Forcing column 0 on
the header row — which record 0's null action pointer independently justifies — collapses the pair, and
the change-check that absorbs the resends is the sanctioned per-frame exception, naming the function it
guards.

**The picker is not built.** The probe run never confirmed on a row, so it captured no picker messages at
all and `FUN_0056b4d0`'s row layout is unmeasured. Claiming that surface on a guess would silence
whatever covers it today, which is the regression `menu_reader.cpp` already carries a warning about.
The first play session settled that: the picker is **already read** by the generic painted-row path
(`[READER] ... item: "Self: MP < 60%"`), so claiming it would have broken a working surface.

### The column cursor: the game had already named all three

First play session, and the tester was precise — *"the left and right nav keys don't seem to be
announcing what is highlighted correctly... unsure if this is a category switch or more similar to a
page-up/page-down."* It is a three-way cycle, and I had one of the three wrong: `panel+0x33E` `0` is
the **per-slot ON/OFF checkbox**, not "the whole row". So arrowing onto it read the entire row out
instead of the one state highlighted. There is no whole-row cursor position at all.

**The game had a name for each column and I invented one instead of looking for it.** `case 0xc` picks
the description bar's text by this exact cursor, and the ids resolve in `help_menu.bin`: *"Toggle slot
ON/OFF."*, *"Change the conditions under which an action is performed."*, *"Change which action is
performed."* — the answer was one switch statement away in the function I had already read.

The fix is not "speak the column name": those are sentences for the description bar, and inventing
"Target"/"Action"/"Toggle" would be fabricated labels. **Which key moved decides how much to say** — a
ROW move speaks the whole row, a COLUMN move speaks only the field landed on. That also restores the
row wording the tester chose, which the first version had lost whenever the cursor sat in a column.

Second defect from the same log: **all three carousel panels take the entry `0x8000`**, so one keypress
produced three identical utterances at the same millisecond. Inaudible only because each interrupts the
last — which also meant the voice belonged to whichever panel dispatched last rather than the set on
screen. Now gated on `DAT_02ca9700`.

### Three comparison operators, pinned from the game's own words

`0xA6` `=`, `0xB2` `<`, `0xC4` `≥` were all being dropped, so every threshold gambit spoke without the
operator that carries its meaning — "Foe: HP  90%". The font atlas *is* the character map, so byte order
offers nothing to interpolate from and these can only ever be settled empirically. Surveying the 9,518
strings in `word.bin` put each byte in exactly one syntactic slot; then `listhelp_targetchip.bin` — the
help line for these very chips — said two of them outright: *"Target any ally with **less than** 10%
HP"* and *"Target any foe with HP **greater than or equal to** 1,000."* That is what separates `<` from
`≤` and `>` from `≥`, which structure alone could not do.

**I nearly shipped two operators where there are three.** The first pass found `0xA6` and `0xB2` from the
probe's own two witness strings and stopped, because those were the two bytes in front of me. The full
survey found `0xC4` sitting in the same slot across 39 more strings. Two witnesses agreed with a
two-operator story, and the population had three — the same shape of error as the struck claim above, in
the same session.

Also mapped: `0x81` = `ú`, whose only use anywhere is "Cúchulainn" (previously "Cchulainn"). `0xA3` and
`0xAD` stay unmapped on purpose — one appears only in dev strings marked `NOT USED`, the other has a
single witness.

### Enemies re-filed as NPCs on rescan

The tester's framing was *"rescan should use the exact same branch as the entity collection on map
transition"*. There is only one branch — `EntityScan::Build`, reached only from `RescanLocked`, with no
transition-time entity path anywhere — so the report is real but the diagnosis had to change: one branch
whose answer is not stable across samples.

On the field `Category::Enemy` has exactly one source. The classifier calls every character an NPC,
`ScanCombatants` runs after the handle-table loop and skips anything already listed (and must keep doing
so — its kind nibble files the player's own party as Enemy, struck in S86), so the actor-pool faction
override is the whole mechanism. One scan where the pool does not answer re-files every enemy on the map,
and every cycle keypress rebuilds the list. The grace window carries entities a scan **missed**, never a
category a scan got **wrong**.

`Entity::factionVerdict` now records whether the pool actually answered, which the category alone cannot
express, and the merge refuses to downgrade `Enemy`→`NPC` on a missing verdict — one direction only, so
an enemy that genuinely turns friendly can still stop being one. The cause is not yet proven from a log,
so `rescan:` gained `actorPool=` and `poolAnswered=`; they are on that line rather than the conditional
inclusion line because an empty pool can leave every counter the latter is gated on at zero.

### Files

New: `ui/gambit_reader.{h,cpp}`. Changed: `battle_state_names.cpp` + `battle_state.h`
(`CharacterName`), `party_status.cpp`, `char_select_reader.cpp`, `choice_reader.cpp`, `game_text.cpp`,
`entity_scan.{h,cpp}`, `entity_list.cpp`, `menu_reader.cpp` (two lines), `ingame_menu_reader.cpp`
(the `ROW_CHAIN` "gambits" mislabel — that class is cmd `0x4B8`).

**Not play-confirmed.** Nothing in this session has been heard yet.

## Session 95 — 2026-07-30 — [navigation] Half the routes shipped unvalidated, and a flag that only one code path honoured

**KEYWORDS:** pathfinder, frontier, unvalidated route, PathValidate, corner footprint, tight corner,
body sweep, depenetration, tangency, path_corridor, ProvenPrefix, banked prefix, audio beacon,
RequestReplan, beacon objective, seedBeacon, p key, locked target, routing through walls

Two tester reports:

> pathfinder is still bugged: clear evidence of routing through impassible terrain in the log

> also the beacon: when in active targeting state and then restored to beacon, the beacon only
> remembers the last leg it was on and considers that the destination, once that is reached the beacon
> stops. for example, I was pathing to a map exit and got into combat. when the targeting state ended,
> the beacon only routed me to the leg of the route I was on, it did not continue on to the next leg

Three causes. **All three were already printed in the mod's own log** — one had been sitting there for
a full session. What was missing was not instrumentation; it was counting what the instrumentation had
already written. Full evidence in `debug.md`, "three defects behind *routes through walls* and *the
beacon stops early*".

### The tally that diagnosed everything

```
plan=Frontier legs   18   |   validate BREACH  57   |   worstFrac >= 0.90 BREACH  25  (corner)
plan=Route    legs   17   |   validate OK      17   |   worstFrac <  0.90 BREACH  32  (sweep)
frontier: goal unreachable (validation never passed)  17
```

**More than half the session's routes took the failure path, and the failure path emitted an
unvalidated polyline.** One `grep | sort | uniq -c` says that; nothing else was needed to find the
first two defects.

### 1. `Plan::Frontier` shipped geometry nothing had checked

The frontier block funnelled `best.portals` — the corridor to the **goal** poly — toward a point on
`best.bestNear`, a **different** poly. A portal sequence and an endpoint that do not belong to each
other. In the genuinely-unreachable case `best.portals` is empty, so the "route" was a straight line
from the player to a point several polys away. And it ran **no validation at all**.

So the enum value that exists *specifically* so a shortfall can never be spoken as a plain route
announced its shortfall while walking the player through the wall it had failed to route around:

```
19:49:06  validate: attempt 4 ... worstFrac=0.85 BREACH
19:49:06  frontier: goal unreachable (validation never passed); ending at poly 325, 16.4m short ... corners=20
19:49:06  say="North 210, Northwest 7, West 64, South 7. 288 steps. Blocked, 22 steps"
```

Twenty corners lifted from attempt 4's corridor, which had just failed at leg 10 of 19.

New `src/navigation/path_corridor.{h,cpp}` (83 + 107 lines): rebuild the corridor **for the frontier
poly** from A*'s parent links, funnel it, **validate it**, cut it back to the part that passed, and
measure the shortfall from where it really ends. Two candidates compete — the furthest-reaching
**proven prefix banked across the attempts** and a fresh corridor to the nearest poly A* reached — and
the nearer one wins. Also: a `truncated` validation no longer ships as `Plan::Route`, which is the
exact claim `path_validate.h` forbids its caller to make and which the caller was making.

The prefix is banked **across** attempts on purpose. Attempts get *worse* as bans accumulate —
211.6 m → 230.9 m → 233.0 m → 233.5 m on one Garamsythe route — so falling back on the last attempt
means falling back on the worst one.

### 2. A tight corner is not impassable terrain

`NavFootprint::Clears` replicates `FUN_0022f9b0`, and S93 had already established what that function
does on a violation: **it pushes the body to tangency.** It is a depenetration rule. `path_validate.cpp`
treated a failure of it as a route breach.

That was **25 of 57 breaches, every one on a route whose legs into and out of the corner had both swept
clear.** Worse, the caller then banned the portal on a leg that had swept fine, so each retry detoured
around a good opening — which is why the routes above got longer every attempt and never converged.

Fixed: the body sweep decides `ok`; tight corners are counted and reported (`tight=N@i` on the
`validate:` line) and validation walks **past** one to ask the question that was never being asked —
does the next leg sweep? `GameArchitecture.md` now carries the two-instruments table so this cannot be
re-conflated.

`probes` vs `checked` separates the two breach kinds with no ambiguity, and it is a derivation from the
code rather than a reading of the numbers: `CheckLegs` spends one probe per sweep and one per interior
corner and returns on the first failure, so `probes == 2*checked - 1` is a sweep failure and
`probes == 2*checked` is a corner failure. Every `worstFrac >= 0.90` breach in the log is the even case.

### 3. `p` was silently redirecting the beacon

`PathPlanner` had ONE "last request" memory and `RequestReplan()` re-ran it — but `\` **and** `p` both
call `Request()`, and `p` routes to the locked battle target.

```
19:50:33.890  'p' pressed: target acquired at (66.23,6.85,108.37)      <- an enemy
19:50:34.078  [BEACON] party clear -> resuming objective
19:50:36.406  [BEACON] leg reached -> advancing to leg 3 of 10          <- objective route INTACT
19:50:44.875  replan: silent re-run of last target=(66.23,6.85,108.37)  <- the ENEMY
19:50:44.921  drain seq=29: target="Steeling A" ... plan=Route legs=2
19:50:48.578  [BEACON] arrived at destination -> final cue, stop
```

The resume itself is flawless — `advancing to leg 3 of 10` proves the route survived combat untouched,
exactly as designed. What killed it was the first off-route re-plan afterwards, which is near-certain to
fire: the stray test is skipped while engaged, so the instant the party is clear the player is standing
wherever the fight took them, well off the leg they were on.

**`p` already declared it has no business with the beacon** — it passes `seedBeacon=false`, and
`path_planner.h` explains why (a moving enemy makes static leg corners meaningless). That flag now also
gates whether a request may become the beacon's **objective**, which is its own snapshot. Only `\` can
write it; `RequestReplan()` restores from it.

**The shape of this one, and it is worth remembering:** a flag that correctly said *"this request is
not the beacon's"* was honoured on the outbound path and ignored on the recovery path. One fact, two
code paths, only one of them knew it. The same shape as S92's "sharing an INPUT is not sharing the
ANSWER".

### What this session did NOT change, and why

- **A `Frontier` beacon still fires the arrival cue at a point that is not the destination.** Measured:
  `19:48:52` seeds the beacon with a frontier point 9 steps short; `19:48:54` logs `arrived at
  destination`. The tester then pressed `\` from near that point and got a full `plan=Route` — so the
  goal was reachable from there all along. Fixing it means a new cue/word (permission) or continuing
  the beacon past a frontier endpoint, and either should be judged against the NEW frontier behaviour.
  Fixes 1 and 2 should make it much rarer; they also make a frontier route shorter when it happens.
- **`kMinFraction = 0.90` is a ratio, so it penalises short legs** — the fixed ~one-body-radius
  depenetration pull-back is 10% of a 2.7 m leg but more of a 1 m one. A distance test would be
  dimensionally correct. **Not changed: no measurement ties a specific false breach to leg length**, and
  the 0.85 cluster in the log could be real. Recorded as a hypothesis, not shipped as a tuning change.

### Files

`path_corridor.{h,cpp}` (new), `path_search.cpp` (495 lines, was 499 — the corridor rebuild and the
frontier moved out, the banked prefix and the two-candidate choice moved in), `path_validate.{h,cpp}`,
`path_planner.{h,cpp}`, `CMakeLists.txt`, `GameArchitecture.md`, `debug.md`.

**Not play-confirmed.** Built and deployed; nothing here has been walked yet.

### Session 95, second round — the sweep was being asked a question it cannot answer

The S95 fixes above deployed (hash-verified, and the new log carries `tight=`, `source=`,
`cutByValidation=` and `BEACON OBJECTIVE`), and the tester reported the **same failure modes**. The
new log is `2026-07-30 20:28`.

**Fix 2 worked and that is what made the real defect visible.** Every breach in the new log is a
SWEEP breach — zero corner breaches, `tight=1@8` counted and stepped over. With the corner noise gone,
the sweep failures stand alone:

```
 0.22 m  leg (174.0,110.0)->(174.2,109.9)   worstFrac 0.02   x14
16.24 m  leg (158.0,88.0)->(170.8,78.0)     worstFrac 0.19   x15
26.00 m  leg (179.1,109.5)->(179.1,83.5)    worstFrac 0.38   x13
26.82 m  leg (179.1,109.5)->(175.0,83.0)    worstFrac 0.42   x12
42.05 m  leg (26.0,110.0)->(67.8,114.6)                       x3
```

15 of 24 routes ended as Frontier, every one of them "validation never passed", every one ending at
the same poly 603 26.5 m short.

#### THE DIAGNOSIS: we were calling a per-frame check with a 26-metre argument

Two proofs, both arithmetic, neither needing another play session.

**1. A leg shorter than the body can never pass a ratio test.** Body radius is 0.27 m; the sweep's
depenetration pull-back at the far end is one radius. A 0.22 m leg is shorter than the pull-back, so
`achieved/requested` is ~0 no matter what is or is not in the way. Fourteen breaches were that one
0.22 m leg. This was recorded as an unmeasured hypothesis at the end of the first round; the log
turned it into a measurement, with coordinates.

**2. The `+/-30 degree` probes are a capsule approximation that only holds for a short step.**
Phase 3 of `FUN_00230c10` fires two probes rotated +/-30 degrees about the travel axis and keeps the
SHORTEST reach. At distance `d` those probes are `0.5*d` off the line. For the cone to stay inside a
0.27 m body, `d <= ~0.54 m` — which is exactly the per-frame displacement all three of the engine's
own call sites pass (`FUN_0032bcc0:55`, `FUN_0032beb0:38/:68`, `FUN_0032ca70:70`, already recorded in
`map_query.cpp`). Handed 26 m, it sweeps a cone 13 m wide and reports a wall in any corridor narrower
than that. 26.00 m -> 0.38 means it stopped at 9.9 m, where the cone is +/-5 m: a corridor width.

**This is the answer to the tester's "why aren't we just reading the game's check".** We are. It is
the game's own function, called purely, and it has been since S93. The bug was never the wheel — it
was the argument. The game asks "may I move 0.15 m?" sixty times a second; we asked "may I move
26 m?" once.

It also explains why the retry loop made things worse rather than better: a false breach banned a good
portal, so every attempt came back longer (211.6 -> 230.9 -> 233.0 -> 233.5 m) and none converged. **A
ban is only as good as the verdict behind it.**

#### The fix

`path_validate.cpp`, rewritten:

- **The one-shot sweep is kept as a FAST PATH.** A `clear` verdict from it is trustworthy — the probe
  cone only ever makes the test stricter, so nothing it passes can actually be blocked. Clean routes
  still cost one probe per leg.
- **A leg it calls blocked is RE-ASKED**, walked in 0.5 m steps with Y pinned to the walkmap under
  each step (`FindPolyAt` + `PolyHeightAt` — `FUN_00380c40` pins the actor's Y to the poly plane, so
  this is not an approximation of how the character moves, it is how it moves), carrying **the
  engine's own resolved position** forward so depenetration slides the body along a wall exactly as it
  does in play. Only that verdict is final. Bounded at 64 sub-steps per leg.
- **The test is a DISTANCE, not a ratio**: a leg passes when the body finishes within one body radius
  plus a little of where it was asked to go. Dimensionally right, and length-independent.
- **The proof is in the log**: `resweep=N rescued=M` on every `validate:` line, plus
  `bad=<leg> len=<m> reached=<m>` on a breach. A real wall (stops early on a long leg) and a
  measurement artefact (stops on a leg shorter than the body) now read differently at a glance. If
  this fix is wrong, the next log says so instead of needing another round of guessing.

#### Beacon work, requested the same round and shipped with it

- **The two beacons are separate features now.** The in-combat target ping lived behind the route
  beacon's `g_active` flag, so it only ever sounded if a route beacon happened to be running. It is
  its own setting, plays in battle whether or not a route exists, and survives the route beacon being
  switched off or the map changing. Everything else is unchanged: still combat-gated, still only on a
  COMMITTED target, and the route beacon still stands down for the fight and resumes on the leg it
  was holding. `OnGameFrame`'s O(1) idle check now consults both.
- **Behind pitches the ping down** — `PitchFor(front)`, linear, 1.0 ahead and abeam to 0.85 directly
  behind. A third cue alongside the existing gain drop and low-pass, because attenuation alone reads
  as "further away". One function, used by both beacons, so they cannot drift apart the way the pan
  and the spoken word did in S92. The arrival cue is unaffected (centred and ahead by construction).
- **Volume sliders for both**, 20%-100% in five steps, default 100%. **They render as NUMBERS, not
  words** — a digit string plus `%` needs no phrasebook row and translates itself, where five invented
  loudness adjectives across 12 locales would be the fabricated-label failure the phrasebook rules
  exist to stop. Deliberately does not reach 0: each beacon has its own Off, and a volume that can
  silence a switched-on feature is a support question waiting to happen.
- **The mod menu grew a second kind of setting** (`Named` vs `Percent`) and `Left`/`Right` became
  directional — they used to both advance, which was identical while every setting had two values.
  Volumes clamp at their ends rather than wrapping; the repeated spoken number is how the player hears
  the limit. `mod_menu.cpp` 231 -> 297.
- One phrasebook sentence was **corrected, not reworded**: "On plays a sound that ... and tracks your
  target in battle" became false when the battle half became its own setting.

#### Files

`path_validate.{h,cpp}` (rewritten), `path_search.cpp` (500, at cap), `audio_beacon.cpp`,
`mod_menu.{h,cpp}`, `phrasebook.{h,cpp}`, `Controls.md`, `README.md`.

**Deployed. Not play-confirmed.**

#### Behind, corrected the same round

The tester heard the design description and corrected it before play:

> behind should be a 180 degree sweep behind the player, not directly behind. pitch only needs to be
> a few degrees lower, not an octave. say pitched down by about 20%. the panning must work from
> behind as well, so behind left would be slightly lower pitched and to the left of the player, since
> we don't have spatial 3d sound. and all logic should apply to the active target beacon as well.

Three changes and one confirmation:

- **BEHIND IS THE WHOLE REAR HEMISPHERE.** The first version scaled by `-front`, so a target 100
  degrees round barely differed from one at 80 and only a target dead astern got the full cue. That is
  "directly behind", not "behind". `BehindAmount(front)` now returns 1 for anything in the rear half,
  with a 15-degree ramp past the abeam line -- present only so an enemy circling the player does not
  chatter between two timbres as it crosses, not as a gradient.
- **Pitch is 20% down** (playback rate x0.80), a few semitones.
- **The pan is untouched by any behind cue, and that is a REQUIREMENT.** There is no spatial audio
  here, so left/right is the only bearing information the player has and it has to survive all the way
  round: behind-left stays panned left and gains the behind cues on top. The attenuation and low-pass
  scale both channels equally and pitch is a playback rate, so none of the three can flatten it.
- **All three cues moved INTO `audio_engine.cpp`**, derived from one `behind` value beside the gain
  and the low-pass. The pitch had spent one build being computed in `audio_beacon.cpp` from its own
  reading of `front` -- one fact, two derivations, which is the exact shape of the S92 pan bug. Every
  ping goes through `PlayPing`, so "all logic applies to the active target beacon as well" is now
  structural rather than something each call site has to remember. The arrival cue multiplies rather
  than replaces, so it keeps its pitched-UP character.

## Session 96 — 2026-07-30 — [navigation] A terrain type is not a walkability check

KEYWORDS: bit 23, FUN_00230a40, FloorWalkable, movement class, shallow water, Garamsythe, map 311,
Central Spur Stairs, No. 10 Channel, string-pull, funnel corner, portal endpoint, Unpull, repair
ladder, retreat, full corridor, cost not cut, penalty, re-cost, ban, EdgeClearSpan, NavFootprint,
tightXing, volXing, STANDING-ON-REFUSED, stop cause, badStopAt frame

The tester's report was "routing works up to the waterway maps, then it routes through impassable
terrain." The fix for that shipped early in the session, took routing on map 311 from three working
exits to two, and took map 315 to **zero completed routes out of five**. Undoing it took the rest of
the session. Everything below is one lesson wearing four costumes.

### The change that caused it

`NavMesh::Walkable` was `(effectiveFlags & 7) == 0` and became
`MapQuery::FloorWalkable(poly, PartyMovementClass())` — the engine's own per-class floor test, whose
bit-23 branch was read as "water, lava, bog, out of bounds". On map 311 it refused **399 of 690 floor
prims**.

**The tester walks through that water.** Garamsythe's channels are ankle-deep, the game has no
swimming, and shallow water is ordinary floor with a puddle on it. A predicate that refuses ground the
player is demonstrably standing on is wrong however good the decompile behind it looks.

Two things went wrong at once and only the second was noticed at the time:

1. **The evidence had already been spent.** The justification written into the code was "three sessions
   of routes-through-impassable-terrain reports were this one omission". Session 95 had already found
   and fixed those: `Plan::Frontier` shipped an unvalidated straight line to a point several polys
   away. A second explanation was stacked on a solved problem, and only the second one broke anything.
2. **The class was derived from the wrong end.** `GameArchitecture.md` already recorded, at conf 0.97,
   that the movers pass class **4** — traced through the callers that actually pass it
   (`FUN_0032bcc0`, `FUN_0032ca70` -> `FUN_00230c10` arg5 -> `moveCtx+0x50`). The overriding claim
   traced what `FUN_002681d0` *writes* (0, to `holder+0x153`) and never showed that field is what
   reaches the callee. **Prefer the call site over the writer: only one of them says what the callee
   is handed.** Class 4 hits no per-class branch, which is exactly consistent with the party walking
   on bit-23 ground. Registry entry restored and annotated; the S96 override in `nav_rva.h` struck.

### The cascade — three global changes, one play test

With water newly a hard border, every sewer walkway gained one on both sides, and
`NavFootprint::Clears` started refusing crossings everywhere. Map 315: 618 reachable polys, zero
routes. The response was to **demote `Clears` in `BodyFitsAt` from a refusal to a counter** — which
removed the only thing keeping A* out of gaps the body cannot pass. A* then proposed corridors through
pinches, the string-pull's chord died in them, and the breach banned the only opening.

That is what cost the third exit, and the tester spotted it from behaviour alone: "pure A* got us to
all 3 exits. So whatever you added is genuinely flagging walkable terrain, or routing through an
obstacle it didn't before." Both, in fact — the first change did one, the second did the other.

**Three global changes, one build, one play test, and the interaction between them was the
regression.** `Clears` is fatal again.

### NOTHING SEVERS THE GRAPH; EVERYTHING DIFFICULT IS EXPENSIVE

The structural fix, and the reason this class of failure ends here. Every refusal that used to
`continue` past an edge now prices it, in metres:

| condition | was | now |
|---|---|---|
| no neighbour | cut | **still a cut** — nothing on the other side to price |
| terrain the class may not stand on | cut | +2000 (inert since the revert; kept for the shape) |
| body fits nowhere along the edge | cut | +500 |
| player physically failed here | cut | +2000 |
| a validated leg through here did not walk | **permanent ban** | +500, accumulating |

A* takes any detour up to the penalty rather than use a bad edge, reproducing the old refusal wherever
an alternative exists — and still returns a corridor when the bad edge is the only way, which the
deletion took away. The heuristic stays Euclidean and admissible: penalties only add.

The ban is gone with it. **A ban made a reachable goal unreachable**: the log has A* reaching the goal
on attempt 1, banning the breaching portal, and reporting "goal unreachable" on attempt 2.

Two holes this opened, closed in the same pass: water polys are now *expanded* rather than skipped, so
the frontier's `bestNear` and the interaction-cylinder fallback both had to ask for walkability
explicitly instead of getting it free from the search.

### The string-pull is an optimisation, and it is the thing that fails

With terrain out of the way the real defect was visible, and the log proved it both directions in one
request: the chord's leg 3 stopped the body at **6.23 m of 9.00 m on four consecutive attempts**, while
the frontier's less-taut polyline walked the same ground with `cutByValidation=0`.

The corridor A* returns is walkable **by construction** — every portal was measured with the body's own
footprint and sweep before the edge was expanded. The taut chord across it is not. So a breach is now
repaired **locally** instead of re-searching the graph: `PathFunnel::Unpull` splices that corridor's
own portal-span midpoints back into the one failing leg and leaves every other leg taut, so the spoken
route stays short. 52 successful repairs in the following session.

**And a target is allowed to be inside a volume.** `WallAcross` tested the destination point, so an
exit — an archway, a map-jump surface — vetoed every route to itself: `reached=0.00m why=wall` on the
exit's own coordinates, with `volXing=109` in that area. The final leg's endpoint is now exempt; its
midpoint is still tested.

### What is left, and why it is on the failure path

Nine failures survived, all identical: `bad=1`, `why=sweep`, and **`stop=(*, 0.90, 123.8)` — seven
different x values, one z**. A straight obstruction at z ~= 124.07 once the 0.27 m body radius is
added back. A breach on leg 1 had no recovery at all: re-costing is refused (the portal is the start
poly's own, and that guard is correct), `Unpull` only spliced into the *approach* and never replaced
the corner itself, and `ProvenPrefix` on `firstBad == 1` yields one point so the frontier had nothing.
"No path" from 3 m away from a reachable exit.

Fixed as a **ladder, entirely on the failure path**: taut chord -> un-pull the leg *and its corner* ->
**retreat to `badStopAt`**, the engine's own resolved position, which is reachable whatever is in the
way -> full corridor with no string-pull at all. A route that validates on the chord executes none of
it, which is the property that makes it safe to ship after a session like this one.

`badStopAt` had to be made trustworthy first: the sweep branch reported it in the **lifted** frame and
the wall branch on the **ground**, and the log printed both (`stop=(45.6,0.90,123.8)` against
`stop=(47.0,-0.00,124.0)`). Harmless as a diagnostic, a leg into the ceiling as a waypoint.

### Struck, and struck for a reason

- **`tight=0@0` on a breaching route means NOT TESTED, not "clear".** `CheckLegs` returns on a breach
  before reaching its own interior-corner check. This was read the wrong way once and nearly bought a
  global geometry change on the strength of it. `PathValidate::Diagnose` now asks the question properly
  — footprint clearance at the breaching corner, plus volume probes at the stop and 0.3 m beyond — so
  the next log says whether z ~= 124.07 is floor border or wall.
- **Insetting every portal span by a body radius: proposed, then dropped.** A taut corner is by
  construction a portal ENDPOINT, and the crossing test deliberately never samples endpoints
  (`SampleT = (i+0.5)/7`, never 0 or 1) — so the string-pull's preferred points are the two per portal
  that were never measured. That gap is real and is now recorded at `SampleT`. But the fix would change
  the geometry of every route on every map, its premise ("the corner is on the boundary") was never
  established, and it does nothing if the obstruction is a wall volume. After three global changes went
  wrong in one session, the repair belongs on the failure path where a mistake costs one route.
- **Orphaned counters.** `g_tightCrossings` / `g_volumeCrossings` were incremented by a comment that
  claimed they made regressions visible, and read by nothing. They print on the `costed:` line now —
  and `volXing` measuring 0 on the map where walls were the leading theory is what retired that theory.

### Still open, measured, deliberately unchanged

- **Whether class 4 or class 0 reaches `FUN_00230a40` for the player.** Play says the party walks
  bit-23 ground, which is consistent with 4. `NavMesh::TerrainRefused` still asks the per-class
  question and **decides nothing**; `NavTrace` checks it every metre against the poly the player is
  standing on (`STANDING-ON-REFUSED`) and stayed silent all session. It earns the right to be priced
  when that check has run for a long time without firing. The clean answer is to read the class the
  engine passes at its own call site.
- **The funnel is 2D.** `TriArea2` works in the ground plane and Y rides along on portal vertices. On a
  corridor that overlaps itself in XZ — a switchback stair, a walkway over the channel it later drops
  into — a taut chord can cut between levels. Not what failed here (the failing legs were flat), but it
  will bite on stacked maps.
- **`path_search.cpp` is 749 lines** against a 500 hard cap. See `PerformanceIssues.md`.

### For the Northern Sluiceway, when we come back to it — tester, end of session

**The Northern Sluiceway routes through to the North Spur Sluiceway.** A valid route exists across
that map, confirmed in play.

**This rules out the waterway control rooms.** The obvious hypothesis for map 315's failures was the
sluice-gate puzzle: the channels flood and drain under script control, `FUN_00232020`'s material bank
can flip a whole material's walkability at runtime without touching geometry, and that would make a
map genuinely unroutable at some gate states and not others. It is not that — the map routes with the
gates as they stand.

Consistent with what this session found: 315's zero-routes-out-of-five was recorded at 10:20–10:40,
which is the bit-23 build. **Those failures were ours, not the map.** Do not re-open the gate-state
theory without new evidence; start from the repair ladder and the `costed:` line instead.

**BUILT AND DEPLOYED. The repair ladder and the breach diagnostic are NOT play-confirmed.**

## Session 97 — 2026-07-30 — [navigation] Two over-refusals: a coarser duplicate, and a ladder that could not reach

KEYWORDS: FUN_00232490, FUN_0022f8b0, bit 31, FUN_0022de60, layer mask 7, FUN_0022d4b0, FUN_002315e0,
queryClass 4, WallAcross, WallSuspect, volHit, volWalked, PointInVolume, UnpullDeparture, departure
corner, final-leg breach, repair ladder, full-corridor gate, retreat insert, map 315, Northern
Sluiceway, map 321, No. 10 Channel, Central Spur Stairs, walls=0

Two tester reports: *"the pathfinder just abruptly cuts out and says no path"* on routes that had been
working, and the **Northern Sluiceway → North Spur Sluiceway could not be routed at all** — a hard
progress block. One log answered both, and the split in it was the whole diagnosis.

| | maps 311 / 321 | map 315 |
|---|---|---|
| `why=sweep` | **24** | 0 |
| `why=wall` | 0 | **16** |
| `repair[unpull]` | **17 run, 17 OK** | 16 run, 16 "still breaching" |
| `inset=` fired | 9 of 35 | 0 of 16 |
| spoken `No path` | **7** | 4 |

Two independent defects, one per map, and **both were over-refusals in code added in Session 96.**

### Map 315 — we asked a coarser instrument to overrule a finer one

Every breach on that map was `why=wall`, from `PathValidate::WallAcross`: **one `PointInVolume` sample
at the midpoint of a leg**, taken before the sweep ran and decisive. `MapQuery::BodySweep` — which that
file's own header calls the only thing that decides `ok` — objected to **not one leg on that map.**

The decompile struck both halves of the premise it was built on:

1. **The body sweep already sees volumes.** `FUN_00230c10`'s two ellipsoid push-out passes iterate with
   **layer mask 7 — layers 0, 1 AND 2** — through `FUN_0022de60`, over the same `0x4000`-tagged,
   `0x90`-stride array `FUN_00232490` reads. Conf 0.97. S96's "walls are not floor geometry ... so a
   wall standing inside a floor triangle passed every check the router had" was never checked against
   the decompile.
2. **`FUN_00232490` cannot tell a wall from a region.** Its callback `FUN_0022f8b0` filters on **exactly
   one bit — 31** — with no class test at all, where the engine's own movement collision reads
   `merged_flags & 7` against the mover's query class: 0 always solid, 1 conditional on bit 30, **4
   solid only when `queryClass != 4` — and the party's movers pass 4** — 2/3/5/6/7 never colliding, bit
   23 a hit recorded and not blocking. It also hard-excludes `>= 0x5000`, the doors and moving
   platforms. **Wrong in both directions.** Field map in `GameArchitecture.md`; no pseudocode anywhere
   names these categories, so they are not labelled.

And a point cannot answer a question about a line. The log caught it contradicting itself inside one
request (`seq=41`): leg 2 of attempt 1, `(15.7,114.1)->(20.2,114.6)`, midpoint `(17.95,114.35)` → WALL;
leg 2 of attempt 2, `(16.5,115.0)->(63.6,114.4)`, passing within 0.6 m of that same point, midpoint
`(40.05,114.7)` → clear, swept at 0.97. Which verdict a leg got depended on where its midpoint landed.
**When a new instrument fires on everything, suspect the instrument first.**

The veto is gone. The probe stays, ground-pinned (it used to lift `(a.y+b.y)/2`, an arbitrary altitude
on a leg between corners at 3.74 and 10.25), reported as `volHit`/`volWalked`.

**The deletion was measured safe BEFORE it was made**: `walls=0` on all 35 validation runs on maps 311
and 321, `walls=1` on all 16 on map 315. It could not change any outcome where routing works.

Two traps it left, both closed: the wall branch `return`ed **before `Diagnose`**, so every `why=wall`
line printed `corner: poly=-1 ... vol@stop=0 vol@+0.3m=0` — defaults read as measurements, the
`tight=0@0` trap again in a second branch of the same file; and it set `badReached = 0`, which disabled
the `retreat` rung, making a wall verdict **unrepairable by construction**.

### Maps 311/321 — the ladder repaired everything it was allowed to touch

**17 of 17.** And all 7 "No path" results were breaches all three rungs declined by their own guards:
`firstBad == total` every time. `Unpull`'s corner replacement is gated on `interior`, `retreat` on
`bad + 1 < poly.size()`, `full-corridor` on `bad == 1`; the re-cost is then correctly refused for a
final-approach breach and it falls to the suppressed frontier.

**It is one bad corner seen from either side.** Map 321, corner `(47.0,-0.00,156.0)`: from
`(44.60,157.01)` the breach lands on leg 1, `Unpull` replaces it, *"West 6, Southeast 10"*; from
`(43.17,159.42)` it lands on leg 2 with that corner as the DEPARTURE point, no rung, **"No path", 5.2 m
short of a 10 m route.** Pass and fail on identical geometry decided by which side of the corner the
player stands — which is exactly what "abruptly cuts out as I walk" looks like from outside.

`reached=0.27m` of 5.25 m with the engine resolving the body **+0.3 m away from the target** is
depenetration and nothing else. The corner is a raw portal endpoint, and `InsetCorners` is inert on
these maps because its improvement test is a mesh-boundary test where the walls are volumes — it
printed `margin=4.73m` and `margin=1e9` at points the body cannot move off.

Fixed by REACH, with no rung changed: new `PathFunnel::UnpullDeparture` (the departed-from corner →
that portal's measured span midpoint); `retreat` serves a final leg by **inserting** `badStopAt` before
the destination instead of replacing it (the destination never moves — S76); `full-corridor` loses its
`bad == 1` gate.

Be precise about when `Unpull` bows out — it is NOT "every final leg". Its splice still fires on a last
leg whenever portals sit strictly between the two corners (map 311 `seq=7` breached on leg 2 of 2 and
was repaired 3 → 4 points). What it cannot do is anything at all when the two corners come off
**adjacent** portals; then it returns 0 without even logging, which is why those 7 failures have no
`repair[...]` line.

### Deliberately not done, each for a measured reason

- **Ordering the two un-pull rungs by `badReached`.** Sound reasoning — a body that never left its
  corner cannot be helped by waypoints further down the leg — and it would have re-ordered two routes
  that **already repair** (`reached=0.17m`, map 311). Dropped: the ladder is now strictly additive, so
  every currently-succeeding repair takes the identical path.
- **`kMaxSubSteps` 64 → 256.** Past ~32 m that bound stops bounding the work and starts changing the
  step (a 47 m leg walks in 0.73 m steps, past the ~0.54 m limit S95 established). Real — and **the
  longest leg re-asked in the whole failing session was 12.14 m, 25 steps, so 64 was never once the
  binding constraint.** Raising it only moves the constraint onto `probeBudget`, where a starved
  re-ask returns `Budget` → `truncated` → "No path" on a route that used to pass. A change that fixes
  nothing observed and can only refuse more does not belong in a build repairing two over-refusals.
- **Capping the one-shot fast path by length.** S95 proved a long *blocked* verdict is meaningless; a
  long *clear* verdict rests on the segment march, a real class-aware cast over the whole displacement.
  Both live complaints are over-refusal; a stricter global test in this build is the S96 mistake.
### The split, taken on the one good seam

`path_search.cpp` 749 → 805 → **711**. The repair ladder came out whole as `path_repair.{h,cpp}`
(112 + 76). It is a real seam, not a line-count trick: it answers one question — *"the chord across this
corridor did not walk; is there another polyline through the SAME corridor that does?"* — and needs
**none** of `Run`'s search state to do it. No A*, no ban list, no centroid cache, no frontier. Its whole
input is the corridor, the polyline drawn across it, and the validator's verdict on that polyline.
Behaviour-identical by construction: same rungs, same order, same guards, same `InsetCorners`, same log
format, and the probe budget still decrements between rungs.

**It is still 711 and the next cut is not a good one yet** — see `PerformanceIssues.md`. What is left is
one ~600-line function whose bulk is the A* pass, and its lambdas close over a dozen of `Run`'s locals,
so lifting them means inventing a context struct: moving code for line count rather than on a seam. The
best remaining candidate is the refusal DIAGNOSTICS into a `Refusals` struct (`Note` + `Format`),
matching the project's `*_diag.cpp` pattern — worth ~45 lines, take it next time the file is open.

### The shape, and it is S96's own lesson pointing the other way

S96 ended "nothing severs the graph; everything difficult is expensive" and priced every refusal in
`path_search.cpp`. Both defects here are refusals that **were never brought under that rule**: a volume
veto in `path_validate.cpp` that cut a route dead, and three guards in the repair ladder that cut a
route dead. The doctrine was written and applied to one file. **A rule adopted in one file is not a
rule yet** — the same shape as S95's "a flag honoured on the outbound path and ignored on the recovery
path" and S92's "sharing an INPUT is not sharing the ANSWER".

**BUILT AND DEPLOYED (`b6bb9cb2`). NOT play-confirmed, NOT committed.**

## Session 98 — 2026-07-30 — [navigation] A transition's destination is a SURFACE, not a point

KEYWORDS: seam group, MapJumpSurface, surf.polys, NearestPointOnSurface, seam vertex, map-jump
surface, walk-onto transition, seamGroup, seam pass, ClosestPointOnPoly, map 315, North Spur
Sluiceway, straight-line nearest, walking nearest, S75 near edge, kArrivalTol, failure path

Session 97's two fixes were judged by this session's log. **One worked, one is untested, and neither
was the Northern Sluiceway's problem.**

- `why=wall` went from 16 to **zero**, and the counter that replaced it read `volHit=2 volWalked=2`
  — the volume probe flagged two legs and the body walked both. **The volume theory is retired with
  data.**
- All three new final-leg ladder rungs ran and all still breached; the tester confirms no loss of
  function on Central Spur Stairs. Untested, because the case they were built for did not recur.
- **Map 315 still said "No path" — for a completely different reason, and not a pathfinding one.**

### 21 of 22 legs validated

```
validate: attempt 1 legs checked=22/22 ... volHit=2 volWalked=2
  BREACH bad=22 len=19.99m reached=5.59m stop=(167.4,9.00,61.8) why=sweep
  stopPoly=324 walk=1 | corner: poly=324 clear=0 margin=-0.27m
frontier: ... ending at poly 324 (169.28,9.00,60.48), 16.4m short
```

`stopPoly=324` **is** `goalPoly=324`: the body ended on the goal polygon. `margin=-0.27m` means the
target's distance to a hard border is exactly **0.00 m**. And all three repair rungs failed
*including the full corridor* — 75 points, the least-taut polyline that exists. **When the corridor
itself cannot reach a point, the point is the problem.**

### The target was a seam triangle's VERTEX, chosen with a ruler

`exit_scan.cpp:234` is the only place an exit's `pos` is written, and it is
`MapQuery::NearestPointOnSurface` — a brute-force scan for the **nearest tagged VERTEX in XZ**. Two
independent defects in one line:

1. **A vertex is not a place to stand.** Every triangle vertex lies ON the walkable boundary by
   construction, which is exactly what `margin=-0.27m` reports. `kArrivalTol = 3.0` has been
   absorbing this everywhere, on every map, since S75.
2. **Straight-line nearest is not WALKING nearest.** On map 315's 27 m, 16-poly seam the
   crow-flies-nearest vertex was the corner the walkable approach reaches LAST.

The consequence, stated plainly: **the route drove 20 m ALONG the exit surface.** Leg 22 ran from
about `(173, 61.7)` to `(153, 62)` and the surface is `x[153..180] z[52..62]` — its *start* was
already inside. A player following it changes maps a third of the way through the last leg. And the
frontier the mod discarded ended at `(169.28, 60.48)`, **also on the surface**. *The route arrived;
the arithmetic said it had not.*

**This is systemic, not a map-315 quirk.** Map 321 has seven surface groups of 5-7 polys spanning
10-17 m, several with corner-to-centre distances of 9-11 m — all far outside the 3 m tolerance. On a
4-poly, 4x10 m seam the corner falls inside the tolerance and nothing shows.

### S75 was right about the near edge and wrong about "near"

Defect 2 is a *specialisation of a fix*, not an oversight. S75 deliberately moved exits off the seam
centroid onto the nearest vertex because *"Southern Plaza's seam is 28 polys spanning
z[132.0..140.0], so its centroid overstates the walk and the route drives through the transition
instead of to it"*. **Aim at the NEAR EDGE was correct. Implementing "near" as straight-line-nearest
was not** — and it reintroduced the very failure it was written to prevent, from the opposite
direction, the moment a seam was approached from its far side. Reverting to the centroid would
re-break Southern Plaza, so that was never the fix.

### The fix: only the search knows which part of a seam is reachable

`MapJumpSurface` has stored the poly list since S64, and its own comment already said what to do
with it: *"a route to this exit is a search whose goal set is exactly these"*. Until now exactly one
diagnostic read it.

`Entity::seamGroup` carries the group from `exit_scan` through `GetCurrentTarget` ->
`PathPlanner::Request` -> the drain, which resolves it to `surf.polys` via `CachedMapJumpSurfaces`.
`PathSearch::Run` takes the set as an optional parameter and consults it **only** where it would
otherwise fall through to the suppressed frontier: pick the seam member whose **closest point** to a
position the search PROVED reachable (the banked validated prefix's end, else the nearest poly A*
expanded) is nearest, and re-run the ordinary search at that point. Recursion is bounded at depth 1
— the re-run passes no seam set.

### Strictly additive, and that is checkable rather than hoped for

The tester's constraint was absolute: *"no plan can change how pathfinding that works in other areas
works in those areas. We want an increase in functionality, not a decrease."*

- The block sits **below** the `return Plan::Route` a validated route takes. If today's search
  validates, execution never reaches it.
- Every touched signature is additive with a default: `Entity::seamGroup = 0`,
  `GetCurrentTarget(..., int* = nullptr)`, `Request(..., int seamGroup = 0)`,
  `Run(..., const std::vector<PolyId>* = nullptr)`. `p` and the `'` probe take the old path
  unchanged.
- `CachedMapJumpSurfaces` returning **false means NOT SWEPT, not empty** — the set stays empty and
  the search behaves exactly as today. Never a fallback from a blind read.
- Only outcomes: `Plan::Route` where there was "No path", or fall through to the frontier untouched.
  A seam re-run that does not validate is not spoken.

### Two traps caught in review, before they shipped

- **`pick != goal` would have made the block a no-op on the case it exists for.** On map 315 the
  failed search's goal poly IS a seam member — the huge triangle the body ended up standing on — so
  `pick == goal`, while its centroid is 14 m from the vertex. **A poly is not a position**; the guard
  is now a distance between POINTS.
- **The member's CENTROID is not safe either.** A seam triangle here can be 16 m long, so its centre
  can sit on the far side of whatever stopped the route. The aim point is
  `ClosestPointOnPoly(member, ref)` — right next to ground the body has already walked.

### Deliberately not done

- **Not routing to the seam centroid** (the S75 regression).
- **Not changing the target selector for all exits.** `e.pos` is a fine PROXIMITY measure — it is
  recomputed per scan from the live player position, so `/`, `[`/`]` and the `kAtExitDist` check all
  keep working. It is only a bad ROUTE TARGET. The two uses had been conflated.
- **Not weakening the arrival test to "reached the goal poly"** — it would have fixed map 315 only by
  the coincidence that the frozen vertex sat on the triangle the body reached, and would have left
  the spoken final leg pointing at the wrong end of the seam.
- **Not reverse-deriving the group from the goal poly's flags.** The seam sweep reads RAW flags,
  while bits 3-6 are simultaneously the map-jump group AND the index into `FUN_00232020`'s group
  override bank — an override can rewrite the very bits the group would be read from. The group is
  plumbed explicitly and depends on no flag encoding.

### The shape

Three sessions have now blamed the pathfinder for this map. S96 blamed the terrain type, S97 blamed
a volume probe, and both times the instrument under suspicion was doing its job. **The route was
correct; the destination was wrong.** `e.pos` answered "how far away is this exit" and was asked
"where should the route end" — one value, two questions, and only one of them it can answer. The
same shape as S95's flag honoured on one path and ignored on the other, and S92's "sharing an INPUT
is not sharing the ANSWER".

**BUILT AND DEPLOYED (`b4533886`). NOT play-confirmed, NOT committed.**

## Session 99 — 2026-07-31 — [navigation] Full circle: the seam pass laundered a shortfall into a confident route

KEYWORDS: full circle, seam pass, circular validation, 0.0m from ref, proven-prefix, nearDist=0.0m,
one-shot sweep, 43.4m leg, fast path, resweep, worstFrac 0.19, x=45.5, map 315, North Spur Sluiceway,
NavBlocked, MovementHeld, stuck detector, gamepad, Plan::Frontier, S73/S74 failure

Tester, verbatim: *"we are back to where we started, routing north to a dead end and not being able
to properly route the character around that. we have officially come full circle."*

**They are right, and the S98 build made the mod's reporting worse rather than better.** This entry
records exactly what the log says. No fix is proposed here; the next session starts from the
`debug.md` "Tried & Failed" table this session wrote.

### What the S98 build did, mechanically

Build `b4533886`. Every counter says success:

```
44 plan=Route      0 plan=Frontier      0 plan=NoPath      18 seam re-runs, all VALIDATED
```

`why=wall` stayed at zero (S97's volume-veto removal continues to hold). The seam pass fired on
every North Spur Sluiceway request and every one of them reported:

```
seam: single-point goal failed; 16-poly surface, ref=proven-prefix (173.0,9.00,61.7)
      -> member poly 324 at (173.0,9.00,61.7), 0.0m from ref -- re-running
seam: re-run VALIDATED -- routing to the surface, not the vertex
drain seq=1: plan=Route pass=seam ... nearDist=0.0m
say="North 118, West 6, Northwest 26, North 35, Northwest 48, then 20 more. 253 steps"
```

### DEFECT 1 — the seam pass's validation is CIRCULAR, and `0.0m from ref` was the tell

The seam pass takes a reference point the search "proved reachable" — the **banked proven prefix's
end** — and then aims at the seam member nearest that reference. On map 315 the prefix already ends
on a seam poly, so the nearest point of the nearest member **is the reference itself**:
`0.0m from ref`, on all eighteen re-runs.

**So the re-run validates the prefix it was derived from.** `21/21 OK` is not evidence the route
reaches the exit; it is the previous attempt's own validated prefix handed back with its failing leg
removed. The number that says so was printed on every line and I did not read it.

The consequence is worse than the "No path" it replaced. `Plan::Frontier` exists precisely so a
shortfall can never be spoken as a plain route — S73/S74 walked the tester confidently to a spot 3 m
from an exit 7.8 m overhead, and the separate enum value was the fix. **This build re-created that
failure by a new road**: `nearDist=0.0m`, `plan=Route`, and 253 confident steps to a place that is
not the destination. A suppressed frontier at least said "No path". This says "arrived".

### DEFECT 2 — the route is still not walkable, and that is the ORIGINAL complaint

The player never got anywhere near the seam. Successive request positions, in order:

```
(13.75,112.14) (18.35,112.14) (35.15,112.14) (45.50,112.14)   -- walking east along the corridor
then, for ~40 s and ten more requests, oscillating around x=45.5:
(44.34,111.08) (45.48,105.08) (45.50,105.55) (45.21,108.34) (44.70,107.32)
(45.24,108.30) (45.50,109.04) (45.50,110.13) (45.50,111.87) (45.50,112.88) (45.50,113.50)
```

They walked to **x ≈ 45.5 and could not continue**, tried north (z down to 105, y climbing to 5.84),
and came back. That is "routing north to a dead end".

**x = 45.5 is 58% of the way along leg 3**, which runs `(20.2,3.83,114.6) → (63.6,3.90,114.4)` and is
**43.4 m long**. The validate line for that route reads:

```
legs checked=22/22 probes=96 worstFrac=0.19 resweep=2 rescued=1 swept=22 blind=0
```

`resweep=2` — only two legs in the whole 211 m route were ever re-asked in 0.5 m steps. **Leg 3 was
not one of them. A 43.4 m leg was certified walkable by a single one-shot body sweep.**

### The deferral that play has now falsified

Session 97's plan considered capping the one-shot fast path by length and **deliberately did not do
it**, on this reasoning, recorded at the time:

> *"S95 proved a long blocked verdict is meaningless; a long clear verdict rests on the segment
> march, which is a real class-aware cast over the whole displacement. The residual risk is an
> obstacle within 0.27 m of the line — and that is the pre-S96 behaviour under which the tester got
> 3/3 exits on map 311."*

The reasoning was that a *clear* verdict from a long sweep is trustworthy. **The player is stuck 25 m
into a 43.4 m leg that verdict passed.** Whatever the mechanism — a zero-width march threading past
an obstacle the 0.27 m body hits, the ±30° probes diverging to ±10 m at that range, or both — the
one-shot fast path is demonstrably certifying ground the player cannot cross. That deferral is
struck; see `debug.md`.

### DEFECT 3 — the stuck detector cannot fire for this player

`NavBlocked` — the whole S96 "remember where the player physically failed" mechanism, priced at 2000
in A\* — has **exactly one call site**: `audio_beacon.cpp:332`, gated on `InputTracker::MovementHeld()`.
The mod's own INIT line documents that tracker as *"Keyboard only; gamepad does not update the
timestamp."*

The player oscillated at x=45.5 for ~40 s across eleven route requests with the beacon running and
**not one `stuck -> blocked spot recorded` line appears in the log.** The one mechanism designed to
learn from exactly this situation never ran. Whether that is the keyboard gate or the 3 s timer, it
is untested code that has never once fired in a tester log.

### What this session did NOT establish

- **Why the body cannot pass x ≈ 45.5.** No probe was taken there. `vol@stop` / `corner:` are only
  filled on a breach, and leg 3 never breached — that is the whole problem.
- **Whether the seam route would work if the player could reach the seam.** Unknown and untestable
  until defect 2 is fixed; the player has never got past x = 45.5.
- **Whether `(173.0, 9.00, 61.7)` is on the transition trigger at all.** It is inside the surface
  bbox `x[153..180] z[52..62]`, and nothing further has been measured.

### The shape, stated plainly

Four sessions have now been spent on this map. S96 blamed the terrain type, S97 blamed a volume
probe, S98 blamed the target point. **Each of those was a real defect and each was fixed — and none
of them was what stops the player at x = 45.5**, because no session has ever measured that spot. The
one instrument that would have — a sub-step walk of the long legs — was proposed in S97 and deferred
on an argument that this log refutes.

**AND BE ACCURATE ABOUT WHOSE DEFECTS THOSE WERE, AND ABOUT WHAT WAS GAINED — WHICH IS NOTHING.**
This entry needed correcting twice. The tester's final accounting:

> *"when we started work 4 or 5 sessions ago, this is exactly where the pathfinder landed. We had 'no
> path' on the northern sluiceway map after some of your changes, but before it worked exactly as it
> does now. I'm being very serious, we have returned to exactly the functionality we had before. To
> the letter. No change at all — except that the path invalidation on final leg and in tight corners
> is still untested, so we may actually be in a worse state than when we started."*

**The mod has never routed map 315.** Confident dead-end route → "No path" → confident dead-end
route. A circle.

The terrain veto (S96) and the volume veto (S96, removed S97) were **both introduced by this line of
work**; removing them restored the starting point. And the "real gains" this entry claimed in its
second draft — pricing instead of cutting, the ban removal, the repair ladder — were **inferred from
code and internal counters, never demonstrated in play.** `debug.md` has carried the rule since S82:
*an abstract "yes" is not play-confirmation.* The ladder's "17 of 17" is the weakest of them: those
repairs land on maps that routed fine **before the ladder existed**, so the breaches it fixes are
most likely ones these sessions' own validation changes created.

**And the untested surface is one-directional.** In this very log the S97 final-leg rungs fired
**54 times and failed 54 times** (18 each of `unpull-departure`, `retreat`, `full-corridor`), all on
map 315 — they have never fired on a working map and have never succeeded anywhere. Tight corners
are detected on nearly every route (`tight=1@17..20`) and nothing acts on them. The S98 seam pass
took 18 of 22 routes. None of these can improve a working route; all of them can turn a failing route
into a confidently wrong one. Full accounting in `debug.md`'s ORIGIN column and "the asymmetry".

**And S98 made the diagnosis harder, not easier**, by turning the honest "No path" into a confident
253-step route. A mod that says "No path" is annoying; a mod that walks a blind player into a dead
end and reports `nearDist=0.0m` is worse. That is the regression to undo first.

**BUILT `b4533886`, PLAY-TESTED, FAILED. Committed as the record of the failure, not as a fix.**

## Session 100 — 2026-07-31 — [navigation] The sweep never read adjacency: the march, long-leg confirmation, auto-walk

KEYWORDS: adjacency march MarchLeg path_march sweep blind adjacency walls FUN_00230c10 anatomy zero
radius centre ray destination sphere FUN_0022f9b0 neighbour demotion S98 seam pass REVERTED circular
validation long leg confirmation kLongLegResweep 12m kMaxSubSteps 256 coverage not accuracy
position-based stuck detection motion accumulator gamepad auto-walk input injection GetDeviceState
DIK WASD camera-relative octant steering mod menu toggle combat disengage FUN_00231690 dynprobe
dynamic obstacles type-4 polarity party-only barriers map 315 x=45.5 measurement

**The research session that reframed four failed sessions.** An exhaustive decompile sweep (three
parallel agents over all 33,105 functions) found the structural fact S96-S99 were missing:
`FUN_00230c10` — the body sweep every leg is validated with — is ONE zero-radius centre ray plus a
0.27 m sphere at the DESTINATION only (±30° side rays fire only when that sphere hits), and it
NEVER reads walkmap adjacency. The engine's real refusal (`FUN_0022f9b0`) is PURELY adjacency-based
— edge is a wall iff neighbour < 0 or the neighbour fails the class-4 walkable test — with no
geometry prims involved. Cliff lips, mesh-boundary jogs and unwalkable neighbours stop the party
and are invisible to the sweep at ANY length and ANY step size. Only two functions in the whole
binary read the adjacency array: the mover and the boundary check. The engine has NO route planner
(one steering routine, straight at the target; NPC routes are scripted coordinate lists; planmap =
display strings). Full findings in `GameArchitecture.md` S100 section.

**Shipped, one build (each with its own log signature):**

1. **S98 seam pass REVERTED** (`path_search.cpp` seam block deleted; `seamPolys` stays plumbed but
   unread, with the S99 circularity rule in the header). `pass=seam` can never appear again; map
   315 transitions fall to the honest frontier ("No path") unless repair genuinely routes.
2. **The adjacency march** (`path_march.{h,cpp}`, `MarchLeg`): every leg is marched poly-to-poly
   across the mod's own navmesh applying the mover's accept rule at each edge crossing. Memory-only,
   FREE (no probe budget), fail-OPEN (every ambiguity = NoVerdict = behaviour byte-identical to
   before; vertex grazes never breach; a 1.0 m graze-scan rescues chords that kiss the boundary —
   `marchGraze` on the validate: line is the falsifier). A march breach is a breach REGARDLESS of
   the sweep (`why=march` + `| march: from=poly:edge nbr= nbrEff=` detail), enters the existing
   repair ladder / re-cost / frontier machinery unchanged; on a march breach the re-cost portal is
   picked nearest `badStopAt` (the measurement) instead of the leg midpoint. `WalkLeg` moved here
   too (shared helpers; one definition).
3. **Long-leg confirmation**: the missing length branch — a one-shot CLEAR on a leg longer than
   `kLongLegResweep=12.0 m` (largest data-proven-safe bound; 43.4 m disproven S99) now runs the
   0.5 m sub-step walk anyway (`long=` counter; `resweep/rescued` keep their exact S95 meaning).
   `kMaxSubSteps` 64→256 with a ROLE CHANGE: never a step-size divisor — a capped/starved walk
   covers what it can at full accuracy and reports Budget/truncated (the pre-S100 walk-nothing
   branch is gone). Cost: +0 probes on 311/321 (no legs > 12 m), ~+300 on a 211 m Giza route.
4. **Position-based stuck detection** (`audio_beacon.cpp`): the keyboard-only `MovementHeld` gate
   (which could never fire for the pad tester — S99 defect) replaced by evidence-of-trying:
   movement key held OR auto-walk engaged OR ≥1.0 m of accumulated jitter without closing (an idle
   player accumulates ~0 and can never read as stuck). Accepted blind spot recorded: a pad player
   pushing perfectly head-on shows neither.
5. **AUTO-WALK** (`auto_walk.{h,cpp}` + one injection call in `dinput8_proxy.cpp`) — the ONE
   user-authorized exception to the read-only-input rule (CLAUDE.md amended in this commit; default
   OFF, W/A/S/D bits only, one function, pre-injection observation preserved, real key wins the
   same poll, combat/menus/route-loss/focus/field-stall/15 s-no-progress all disengage, combat
   within one frame and re-engage is manual-only). `\` with the ModMenu toggle On walks the route
   by camera-relative octant steering rendered from the SAME bearing the spoken legs use.
   Per-engagement `AUTOWALK summary:` line + `AUTOWALK stuck:` ground-truth stop lines — the
   x≈45.5 measurement instrument the map-315 saga never had.
6. **`'` probe: `dynprobe`** — one safe-path SEH-wrapped call of `FUN_00231690` (the ONLY callable
   query that sees ≥0x5000 dynamic-obstacle prims; conditionally write-free at 0.97, BELOW the
   bar) with before/after snapshots of its conditional-write scratch. C++ diagnostic per user
   directive (not Frida). Nothing routes on it; the census waits for the ≥0.98 record.

**Canon corrections** (GameArchitecture.md): type-4 volume polarity was INVERTED — type 4 is solid
ONLY for class 4, the party-only invisible walls (verified 0.99 by direct read of both segment
callbacks); `FUN_00230c10` returns 0/1 (callers derive the fraction); `FUN_002315e0` is class-blind
(hard-coded −1) — the class-aware ray is `FUN_00230b60`; sweep anatomy as above. OPEN, deliberately
NOT in this build: the three-override-banks question (live code impact on `EffectiveFlags`).

**ACCOUNTING RULE, written before the tester round:** no gain is claimed until the tester reports a
behavioural change. The march's first run on 315 is the x≈45.5 measurement WHATEVER it shows:
(A) `why=march` at ≈(45,114) → repair routes past it or an honest frontier; (B) march clean and the
`long=` walk stops there `why=sweep` with the engine's own resolved stop; (C) both clean → dynamic
prims rise in priority. Auto-walk on 315 produces the stop line either way.

**BUILT AND DEPLOYED. NOT play-confirmed, NOT committed** (commit after the docs are complete).

### Session 100 addendum — the same-day tester round, log analysed

**"Autowalk works in most cases"** — the first play-confirmed navigation gain since S92. All five
S100 instruments functioned in play: 4 clean auto-walk engagements (`reason=Arrived`, walked≈route),
PlayerInput cancel worked, perf negligible (2.52 ms across 1,266 frames); the march produced its
first real catch (`why=march | march: from=94:1 nbr=-1` — a true mesh boundary on map 311); the
stuck detector fired under auto-walk with `motion=0.0m` (pure head-on cancel — exactly the case the
old keyboard gate could see and the new evidence-OR still sees); `pass=seam` is absent everywhere.

**Defect 1 — the transition loop (311 ↔ 321), reproduced twice, root-caused.** Arriving on Central
Spur Stairs from No. 10 Channel puts the player at (49,127), beside the No. 10 seam ((47..49,132)).
Routing to Lowtown North Sprawl: the player ends ~1.7 m off the route line (leg-advance radius 2.0),
west from there is a wall spur (replan sweep: 0.35 m of 5.74; march: poly 94 edge 1, no neighbour),
auto-walk pushes head-on → stuck → replan — ALL AS DESIGNED. But the replanned route's first corner
is **(49,132) — ON the No. 10 Channel seam** (the corridor detours down into the channel via the
seam's own doorstep), auto-walk walks it faithfully, the transition fires, map flips to 321. Twice,
identically. **ROOT DEFECT: the pathfinder treats FOREIGN map-jump surfaces as ordinary floor.** A
manual walker wobbles off lines and rarely triggers it; auto-walk walks the line exactly and
triggers it reliably. Fix direction (next build): PRICE polys whose `MapJumpGroup` != 0 and != the
request''s own `seamGroup` (a price, never a cut — doctrine) — the S98 `seamGroup` plumbing finally
gets its reader. Secondary: the teardown stop reached auto-walk as `RouteLost` (spoke "Auto-walk
stopped" during a map change — should be silent): the planner''s teardown `AudioBeacon::Stop()`
should pass `StopReason::MapChange`.

**Defect 2 — map 315, the x≈45.5 measurement is IN, and it is outcome C.**
`march=22 marchBlind=0 marchGraze=14 long=5` and NO breach anywhere mid-route: the march finds NO
adjacency break, and the engine''s own 0.5 m body-walk (leg 3 confirm-walked under `long=`)
TRAVERSES x≈45.5. The ONLY breach is the FINAL leg — 5.59 m of 19.99 along the exit surface toward
the vertex goal (153,62), goal corner ON the boundary (`margin=-0.27`), spoken honestly as "No path"
(frontier suppressed, 16.4 m short). **That is the S98 DIAGNOSIS — a transition''s goal is a
SURFACE, and the straight-line-nearest vertex is the corner the walking approach reaches LAST —
still real, still unfixed** (only its circular fix was reverted). The remaining 315 suspects for the
S99 physical stop at x=45.5 narrow to: dynamic obstacle (`dynprobe` never ran — tester did not press
`''` this round), footprint-vs-boundary on a narrow strip (ellipse ~0.5 vs sweep 0.27), or an
off-line stop against the channel side. **Fastest path to closure: implement the in-search seam
GOAL SET (endpoint = wherever the search first reaches the surface — no self-derived reference, so
the S99 circularity rule is satisfied by construction), get a full Route on 315, and let auto-walk
walk it — its stuck line then gives the exact ground truth.**

### Session 100 addendum 2 — the probe AT x≈45.5: THE 315 BLOCKER IS A TERRAIN-FLAG BOUNDARY

The tester pressed `'` standing at the blocked point (45.50,4.00,112.14). Everything converges on
one line in the mesh dump:

- The player stands on **poly 23, raw=eff=0x00200000** (clean). Immediately EAST — exactly where
  every session's walk has stopped — is **poly 224, raw=eff=0x17A00000: bits 23,24,25,26 set.**
  The walked trail (34 crumbs) ends at x=45.2; the flag boundary is the stop line.
- The terrain census: **2,545 polys on 315 carry 0x17A00000** — the flooded channels. The census
  has labelled that class "UNWALKABLE (bit23)" since S96.
- `dynprobe` AT the point: no dynamic obstacle (prim=-1, push=0, ret=0xFFFF), scratch deltas=0
  (write-freedom data points now 3/3 clean across 315+321).
- The march and the sweeps pass the east channel — correctly, per their own definitions: adjacency
  is connected and no volume exists. Neither instrument tests PER-CLASS terrain bits. The blocker
  is the one class of refusal no shipped instrument models.
- Decoded per the engine's floor test: 0x17A00000 refuses classes 0 (bit 23 — the LEADER per
  FUN_002681d0), 1 (bit 25 — followers), 2, 5; passes only 3 and 4. The real path the tester
  hand-walks is the SOUTH BANK (polys 26/27, y=6-8, clean 0x00200000) — exactly where the S99
  player's own detour went (45.48,5.84,105.08).
- **STANDING-ON-REFUSED has NEVER fired** (NavTrace is alive — trail dumps prove it): the player
  has never once stood on TerrainRefused ground. The S96 "the party wades that water" refutation
  condemned the predicate for polys the player never actually stood on; what S96 proved wrong was
  the LEVEL (a graph CUT) and possibly the class arg, not the predicate.

**VERIFICATION GATES before anything ships on this (0.98 rule):** (1) pin in the decompile which
class reaches FUN_00230a40 for the LEADER's per-frame mover (moveCtx+0x50 contents for the
player-controlled character — the S75 "movers pass 4" was traced at the SWEEP call-site; "one
name, two facts"); (2) one `'` press standing IN the water the tester wades on 311 — if that
ground is bit-23-clear in effective, the record reconciles completely.

**FIX SHAPE (proposed):** price TerrainRefused crossings in A* (kTerrainPenalty channel already
exists; price, never cut) + march/validation treat a leader-refused crossing as a BREACH (falls
back honestly through repair/re-cost/frontier — the S96-safe level). With the flood priced, the
clean south-bank route wins immediately and 315 routes around — globally, no per-map anything.
The S98 surface-vs-vertex goal fix stays on the list (EXIT-AIM still shows 17.4 m overshoot).

### Session 100 addendum 3 — BOTH VERIFICATION GATES CLOSED: the leader's floor class is 0, and bit 23 refuses it

**Gate 2 (play):** the tester pressed `'` standing IN the shallow water they wade (No. 10 Channel):
poly 524, raw=eff=0x00300000 — **bit 23 CLEAR**. The S96 "party wades that water" refutation and
the bit-23 predicate were never in conflict: the waded water is not bit-23 ground. (Also: dynprobe
clean point #4, and the 321 census caught the override bank LIVE — raw=0x00202000 -> eff=0x07A02000,
the flood state adding bit 23 at runtime, which effective-flag reads already track.)

**Gate 1 (decompile, every link read directly, conf 0.99):**
1. `FUN_002681d0` — leader class = **0** (followers 1, mounted 5), written to `walkObj+0x80` and
   `holder+0x153`.
2. `FUN_003db140(walkObj)` -> `FUN_00380c40(walkObj+0x30)` — **moveCtx = walkObj+0x30**, so
   `moveCtx+0x50` (the class every floor-test caller reads) IS `walkObj+0x80`. `FUN_00380b80`
   merely pre-inits it to 0xffff.
3. `FUN_002327d0:267` -> `FUN_00230a40(poly, *(moveCtx+0x50))` = class 0 for the leader.
4. `FUN_00230a40` (read whole): after the type test, **class 0 requires bit 23 CLEAR**; classes
   1/2/3/5 test bits 25/26/27/24; class 4 tests nothing extra.
5. Poly 224 (0x17A00000, bit 23 set) -> refused -> edge demoted to a wall -> the leader stops at
   the poly 23|224 boundary = x≈45.5, where the crumb trail ends.

**The "class 4" record reconciled, not overruled:** `FUN_0032bcc0` hard-codes 4 (0xffff unstick)
into the SWEEP's query class — a different question, correctly recorded, wrongly generalized to
walkability. ONE NAME, TWO FACTS: the SWEEP class is 4; the LEADER'S FLOOR class is 0.
**Leader walkability = `(eff & 7) == 0 && bit23 clear`** — S75's "walkability is one line" holds
only for the type half.

**FIX SET, now >= 0.98 and ready to implement:** (a) A* prices `TerrainRefused` neighbours
(kTerrainPenalty channel, PRICE never cut); (b) the march's accept rule gains the CLASS-AWARE
floor test (call `FloorWalkable(poly, PartyMovementClass())` — the S100 march replicated the
mover's rule with the sweep's class, the same conflation) so a refused crossing is a BREACH into
the existing ladder/re-cost/frontier; (c) `NavMesh::Walkable` itself STAYS the permissive type
test (S96 proved that lever over-refuses — it gates flood/goals/edges/GroundY). Then the S98
surface-goal fix, then auto-walk steer-to-line + foreign-seam pricing.

### Session 100 addendum 4 — the class-aware fix set BUILT AND DEPLOYED

Four changes, each with its own log signature, all on the priced-never-cut doctrine:

1. **A* prices leader-refused terrain** (`path_search.cpp`): a type-walkable neighbour failing
   `TerrainRefused` (the engine's own per-class floor test, leader class 0, bit 23) pays
   kTerrainPenalty. Counter `terrain=` on the `costed:` line; refused eff-flags named. The
   frontier's `bestNear` also excludes refused polys -- a shortfall route may never end the
   player in the flooded channel.
2. **A* prices FOREIGN transition surfaces**: any poly whose map-jump group != the goal's own
   group pays kForeignSeamPenalty=2000 (the 311<->321 auto-walk bounce). Counter `foreignSeam=`.
   The goal's group is derived from the goal poly -- no new plumbing.
3. **The march's accept rule gains the mover's CLASS** (`path_march.cpp`): step-in now requires
   `Walkable && !TerrainRefused`, and a graze rescue must land on class-standable ground. The
   S100 march had replicated the accept rule with the sweep's class 4 -- the same conflation the
   verification unwound -- which is why it passed 315's leg 3. A refused crossing now breaches
   (`why=march`, nbrEff shows bit 23) into the existing ladder/re-cost/frontier.
4. **Auto-walk steers to the route LINE, not at the corner** (`auto_walk.cpp` + `LegSnapshot.
   legStart`): off the validated line by > 1.0 m, aim at the line's nearest point + 2.0 m lead.
   Kills the off-line beeline that hit the 311 wall spur head-on (motion=0.0m) and triggered the
   pathological replan. `TerrainRefused` promoted from hypothesis to consumer-bearing predicate
   (nav_mesh docs updated); `NavMesh::Walkable` itself deliberately untouched.

**Expected on 315:** A* pays 2000/crossing through ~2,545 flooded polys, so the clean south-bank
route wins; the march would breach any residual chord into the flood. Expected on 311: the
Lowtown replan can no longer corner on the No. 10 seam. **NOT play-confirmed. No gain is claimed
until the tester reports one.**

### Session 100 addendum 5 — the terrain price WORKED; the pinned-corner acceptance + unstick

Tester round on the fix set: 315 STILL "No path", auto-walk no longer crosses transitions
(foreign-seam price CONFIRMED by absence) but wedged against the 311 spur and needed manual help.
The log shows how close 315 came:

**The terrain price did its job.** The route now climbs the SOUTH BANK -- corners
(18,110) -> (26,94) y=10 -> (46,82) y=13.2 -> east along the ridge -- `corridor pays terrain=0`,
`terrain=`/`foreignSeam=` counters live. What killed it: the funnel PINS corner (18.0,110.0)
against a wall (`corner: clear=0 margin=-0.27`), depenetration forbids the body standing closer
than one radius to it, the walk stops **0.54 m short = radius 0.27 + overlap 0.27 EXACTLY** against
a 0.42 m tolerance, every repair rung re-aims at the same unstandable corner, four attempts, "No
path". The S95 corner lesson re-manifested in the ARRIVAL test.

**Shipped:**
1. **Pinned-corner acceptance** (`path_validate.cpp`, validation-only, `pinned=` counter): a
   sweep-stop short of an INTERIOR corner whose own footprint test FAILS is accepted when
   `shortfall <= radius + min(|margin|, radius) + 0.25` -- the measured tangency bound. A genuine
   mid-leg wall stops far shorter and still breaches; the beacon advances legs at 2.0 m anyway.
   Also counted as a tight corner (it is one).
2. **Auto-walk unstick**: after each stuck fire, sidestep 90 degrees off the held heading for
   700 ms (alternating sides per fire) so a concave wedge releases instead of requiring the
   player to take over. `kRejoinDist` 1.0 -> 0.35 (the second 311 pin was 0.75 m off-line, inside
   the old threshold, so the walker beelined into the spur's wrong side).

**Expected on 315:** attempt 1 validates with `pinned=1..2`, plan=Route up the bank. NOT
play-confirmed; no gain claimed until the tester reports one.

### Session 100 addendum 6 — InsetCorners was inert by geometry; measured-direction inset

Round 3 on 315: `pinned=2` (the acceptance fired, two corners passed) and validation died deeper
up the bank on the same physics with worse geometry -- leg 3 stopped 0.98 m short of corner
(42,90) (`clear=0 margin=-0.14`; an OBLIQUE wall projects the tangency stop further along the leg
than the perpendicular bound), and an alternate attempt missed its bound by 7 cm.

**The root mechanism was already in the tree and inert: `InsetCorners` tries exactly ONE
direction -- the interior-angle bisector -- and moves only when measured clearance improves.**
Right for a corner pinched between its own legs; wrong for the pinned class (portal-endpoint
corner whose wall runs PARALLEL to a leg -- the clearance gradient is the wall NORMAL, the
bisector slides along the wall, `after > before` never passes). `inset=0` on every funnel line of
the entire saga is that condition never passing.

**Shipped:** (1) InsetCorners now tries the bisector AND both perpendiculars of each leg, keeps
the candidate the footprint MEASURES best (still a measurement, never a nudge); candidates must
also pass `TerrainRefused` (never inset onto flooded ground -- closes the class half of the S96
`inset=3` water-corner failure). (2) Pinned-acceptance slack 0.25 -> 0.35 (obliquity backstop).
(3) Auto-walk announces "Auto-walk stopped." on ARRIVAL (tester request, this conversation): a
route to a transition deliberately stops short for the manual step-through, and the hand-off was
invisible without it.

Expected on 315: `inset > 0` for the first time, the pinned corners move off their walls, attempt
1 validates -> plan=Route up the bank. NOT play-confirmed.

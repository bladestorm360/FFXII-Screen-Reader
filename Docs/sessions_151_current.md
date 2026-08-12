# FFXII-Screen-Reader — Session Log (Sessions 151–current)

Continues `sessions_101_150.md`, which is closed at **Session 150** (the tester-report session:
the dialogue-path audit, collected treasure, and the equipment-category instrument).

Entry format is mandatory: `## Session N — YYYY-MM-DD — [track] <title>`, where `N` is a single,
global, monotonically increasing integer shared by all parallel tracks. Never a date-only header,
never a letter sub-session. Before appending, grep this file for the highest `## Session N` AND
check `git log` for an unlogged session after it; the next session takes `N+1`. Split again after
Session 200 (`sessions_151_200.md` + `sessions_201_current.md`). Every entry carries a KEYWORDS
line for grep.

**Play-confirmed at the close of Session 150:** the enemy pruner (98 drops, all `+0x14=0xB0 kind=1`
on named enemies, nothing collateral) and the collected-treasure drop.

**Open at the close of Session 150** — carried forward so it is not lost with the file split:

- ~~**Mariam's Tomb pathfinding** (tester, 2026-08-11)~~ — **NOT A DEFECT. Report withdrawn
  2026-08-11**, verified in play by the user AND the tester. The statue "would not route" because
  **the doors leading to it had not been opened yet** — there was no route to plan, and the planner
  said so correctly. No session was spent on it and none is needed.
  **The lesson is about triage, not pathing:** a routing refusal on a map with a closed door is the
  planner working. Before opening an investigation into "X will not route", establish that a route
  EXISTS — the `oracle:` line already answers this
  (see the S115 note: *read the `oracle:` line before proposing a cause for any "No path"*).
- ~~**Offhand shields**~~ — **root-caused and CLOSED in Session 151 below.** The Session 150 note
  here claimed it was "root-caused and FIXED in §5b… empty-vs-equipped, not shields"; **that claim
  was wrong and had already been refuted in play** when this file was written. The real cause is the
  off-hand's cursor HOST object; see Session 151.
- **The gamepad hotkey gap.** `t` re-read, `o` describe, and the Status / Clan Primer line-by-line
  walks are keyboard-only, so a pad player hears each surface's entry line and cannot step through
  the rest (`input_tracker.h:9`).

## Session 151 — 2026-08-11 — [menus] The off-hand's cursor lives on a different object

KEYWORDS: offhand, off-hand, shield, shields, ammunition, equipment candidate list, FUN_003fdfe0,
FUN_003fd860, FUN_003fd6b0, FUN_003fd1d0, FUN_002d47c0, cursor host, container+0xC0, widget+0xC8,
0x8000, IsFocusedPane, DAT_0208ebc0, IsCursorHost, IsCandidateList, navigation silent, S150 followup

**The last open defect before release, and it closed on a measurement S150 had already shipped.**
**PLAY-CONFIRMED 2026-08-11 by the user — the off-hand list now speaks every row as the cursor
moves.** Navigating the off-hand (shield) candidate list was silent: the pane announced its category and its
first row on entry, then said nothing for any cursor move. Every other equipment slot — including an
unequipped helm — read correctly, on the SAME window instance.

### The answer was the S150 diagnostic's own output

`FFXII-Screen-Reader-2026-08-11_15-41-40.log`, cursor sitting on the shield list:

```
[READER] focus msg on a NON-cursor pane: owner=…CB5BBA0 val=1 -> cursor pane=…BE9CDC0 class RVA=0x2DDFE0
[READER] focus msg on a NON-cursor pane: owner=…CB5BBA0 val=0 -> cursor pane=…BE9CDC0 class RVA=0x2DDFE0
```

`val` tracks the player up and down the list. The same instance `…BE9CDC0` addresses its own 0x8000
directly for WEAPONS and HELMS, minutes apart in the same log:

```
[READER] pane owner=…BE9CDC0 focus=…BE9CDC0 focused=1 rowOff=0x0
[INV] item: owner=…BE9CDC0 "Magoroku"
```

So the message was never missing and the reader was never wrong about the rows. Only the ADDRESSEE
differed. **The instrument that answered this was shipped in the session that could not answer it —
because it was built to record which branch declined, not what was seen.**

### Root cause — slot 1 is the only slot whose cursor widget is not its own

`FUN_003fdfe0`'s init (`:31-36`) splits on the slot: `== 1` → `FUN_003fd860`, everything else →
`FUN_003fd6b0`. Slot 1 is the OFF-HAND (`FUN_003fd360`: `category = slot + 0x40`, so `0x41`).

- `FUN_003fd6b0:38-40` — every other slot takes its cursor widget straight from the container's own
  scene subtree. The widget's notify target is the container, so the pane that holds the cursor is
  the pane that receives the focus message.
- `FUN_003fd860:33-38` — the off-hand first creates an intermediate object,
  `FUN_00244f50(200, FUN_003fd1d0, 0)`, parks it at **container+0xC0**, attaches it as a child, and
  takes the cursor widget from THAT object's subtree. `FUN_002d47c0:15-16` sends 0x8000 to
  `widget+0xC8` — the widget's host — so the message arrives on the host, not the list.

`FUN_003fd1d0:41-47` then forwards every category-0xC message to its parent's handler **as a direct
call, not another `FUN_00247510`** — which is why exactly one focus message is ever observed, why it
carries the host, and why `val` is unchanged when `FUN_003fdfe0` indexes `val * 0x20 +
container[+0xE0]` (the same array, stride and `+0x08` id field this reader already walks).

**One mechanism explains BOTH halves of the defect.** The entry silence S150 patched had the same
cause: the stash arms on the message's owner (the host) while `HookedFocusSet` replays on `newWin`
(the list), so the two could never match and the replay could not fire.

### The fix

`InventoryReader::IsCursorHost(cursorPane, host)` — two gates, both required: obj[0] class
`0x2DDFE0`, **and** `cursorPane[+0xC0] == host`, an identity no shape test could establish.
`+0xC0` is written only by the off-hand's build path, so nothing else in the family can match.
`HookedDispatch` then speaks `TryFocus(focusWin, index)` and claims the row either way — an empty
category must stay deliberately silent rather than fall to the generic painted-cell path (S89).

The `IsFocusedPane` gate is untouched; it is still what stops the inventory reading several panes at
once. The S150 entry announce is kept exactly as it shipped (play-confirmed) and now shares the
class predicate instead of repeating the base arithmetic.

### Struck

- **STRUCK: "it is empty-vs-equipped, not shields — any bare slot is affected"** (S150's own carry
  forward, above). It was refuted in play before it was written down: an unequipped HELM reads fine,
  and the off-hand fails WITH a shield equipped.
- **STRUCK as the cause: `FUN_0057cf20` case `0x41`'s two-pool shields+ammunition merge.** Real, but
  innocent — it decides which ROWS the list holds, never who is told about the cursor. It was the
  live hypothesis for two sessions on the strength of being the only bespoke branch anyone had found.

### Committed in passing — an S150 change that was never committed or logged

`5a388c8` staged `equip_compare.h` but **not `equip_compare.cpp`**, so the AutoDetail gate on the
per-highlight stat preview — the tester's *"the delta comparison is vocalizing automatically in the
unequip menu with autodetail off"* — had been sitting in the working tree unversioned ever since,
and the Session 150 entry never mentioned it. Found by `git status` while staging this session.
Committed on its own so the history stays one-commit-per-session; the code is unchanged from how
S150 wrote it. **`git status` before staging is what catches this** — an uncommitted file is
invisible to a grep of the session log, exactly like the unlogged session `6f619e3`.

### Lessons

- **A surface that will not speak has two candidate faults, and they are not the same question:
  "is the message wrong?" and "is the ADDRESSEE wrong?"** Three sessions searched the row build —
  what the list CONTAINS — because that is where a list's differences are expected to live. The
  difference was in who owns its cursor, which is settled at construction and never appears in the
  data the list holds.
- **When one member of a family misbehaves, diff its CONSTRUCTOR, not its contents.** The split was
  one branch in the init handler, on the slot index, in plain sight.
- **The line that answers a defect is the one naming which branch declined.** S150 shipped that
  instrument, and it was enough on the first pass through the surface. Contrast the three throttled
  diagnostics that same session mistook for measurements.

## Session 152 — 2026-08-12 — [per-frame] NEVER COUNT FRAMES: four budgets converted, and the audit's own severity number struck

**KEYWORDS: per-frame frame counting kWaitFrames 90 kStrayFrames 45 kMaxPaintRetries 8 kIdleReportAt
wall-clock GetTickCount64 deadline tier C PerFrameAudit game speed multiplier 1x 2x 4x speed index
0x1EB4A98 speedTable 0x7E8BA8 ac4 ac8 sim accumulator DAT_02064AC0 FUN_0022a770 field frame
FUN_00343ee0 zero callers frame_probe StallProbe FrameTick threshold IsFieldNavSafe FUN_0026e960
walkmap writer event-driven path_planner nav_probe audio_beacon menu_reader TEXT NEVER PAINTED S152**

**Asked for:** diagnose and plan fixes for the documented per-frame checks — event-driven if
possible, otherwise correct at any FPS and any game-speed multiplier.

**Shipped:** all four frame-counted budgets converted to wall-clock deadlines, plus the instrument
that settles the question the audit could not. Builds clean, deployed. **NOT yet play-confirmed.**

### The three things the audit had wrong

1. **"Both already have `now` in scope" — false.** `path_planner.cpp` pulls no `<windows.h>`,
   directly or transitively; the only header in `src/` that does is `core/mem_read.h`. It needed a
   new include, so the "mechanical, identical in all three" edit was not.
2. **`nav_probe`'s expiry is not log-only — it SPEAKS** (`Phrase::Id::DiagnosticUnavailable`).
3. **The prescribed measurement cannot work.** `StallProbe::FrameTick` discards every gap below its
   100 ms warn threshold, so an ordinary session emits nothing. Verified against the corpus first:
   five `input-poll` GAP lines across twenty logs, all boot stalls. Full entry in `debug.md`.

### GAME SPEED CANNOT MOVE A FRAME COUNTER — the audit's open question, answered from the decompile

`FUN_0022a770` (RVA `0x10A770`, what the mod hooks as `FIELD_FRAME`) **contains** the sim loop:
`:250 acc += ac8*ac4` once per call, `:139 while (1.0 <= acc)`, `:185 acc -= 1.0`. The mod detours
the OUTER function, so `HookedFieldFrame` and its five callees fire **once per call at every speed**
— 2x/4x runs the *inner* loop more times. Recorded in `GameArchitecture.md`.

**Three months of "measure the game-speed mechanism before tuning anything" was gating on a fact the
decompile already held.** The lesson is not that measuring is wrong; it is that the audit never
asked whether the question was decompile-answerable before declaring it play-session-blocked.

### ...AND THE SEVERITY FIGURE IT WAS ALL BUILT ON IS UNVERIFIED

`ac8` — the per-frame delta, in sim TICKS not seconds — is a hard `1.0f` at every reachable writer
(`0022a0b0:44`, `002628b0:165`, `003601d0:13`); its only variable writer `FUN_00343ee0` has **zero
callers**. So one call is one sim tick. **If that call followed display refresh the entire game
would run 2.4x fast at 144 Hz, which nobody reports** — so "at 144 fps the budget is 0.63 s" is
probably wrong, and the rate actually reachable in-game is 30, where every budget got *longer*.

**Not established either way, and it is runtime-only** — hence the instrument. The conversions
shipped anyway because a millisecond deadline is correct at every rate and behaviour-identical at
60 fps: zero regression risk, so there was nothing to gain by waiting.

**A number quoted in a table with three decimal places is not a measurement.** Every cell in the
audit's `at 144 fps` column was arithmetic on an unexamined assumption.

### A fifth site the audit missed, found in our own log

`menu_reader.cpp:104 kMaxPaintRetries = 8`, driven by the frame-paced paint callback: consecutive
retries **15–16 ms apart** in `logs/…2026-08-10_11-05-54.log:699-705`, and the budget **exhausted
four times** in that one 60 fps session, each logging `TEXT NEVER PAINTED … this surface is MUTE`.
Those four may be genuinely mute surfaces — what is proven is that the budget is *reachable* in
ordinary play, so a higher rate gives the same surface less wall-clock, and giving up costs a blind
player the announcement outright.

Fixed by **ORing** a time floor onto the count, never ANDing — the budget must never be smaller than
before at any rate. Its one-shot give-up notice also had to move from `g_retryCount ==
kMaxPaintRetries` to an explicit latch: **an equality test on a counter that can now overshoot logs
nothing at all**, which would have silently deleted the diagnostic while looking like a no-op.

### "Without polling" is not available, and the wait was never the polling the rule is about

`IsFieldNavSafe()` is a conjunction of six predicates across four subsystems. The planner already
re-evaluates it on the game's own field-tick event and fires on the first frame it is true — level
-driven, not a poll loop. The only frame-derived quantity was *how long before apologising to the
player*, and no game event exists for "this will never become ready" (a request in a cutscene, or on
a Ridorana-style terminal map, must still answer the keypress).

Writers were traced anyway: **`FUN_0026e960` (RVA `0x14E960`)** is the sole writer of the walkmap
globals `MapQuery::HasWorld()` reads. Hooking it still leaves the conjunction to evaluate, it is
below the 0.98 bar without a Frida probe, and it removes no deadline. Recorded in
`GameArchitecture.md` as a candidate for the separate walkmap-*identity* question, not as an event.

Corrected while there: `HookedWorldStep` caches the **Bullet** context, a different world from the
SQEX one `CondWorld` tests. That cache says nothing about nav readiness.

### The trap in the audit's own fix sketch

Its `g_straySinceMs` snippet shows the arm and the compare but **not the disarm**. The counter it
replaces is zeroed the instant the player is back inside `kStrayDist`, so it measures one
CONTINUOUS stray run; a deadline without the matching clear in the `else` branch silently means
"has been stray at some point in the last 750 ms" — looser than the code it replaces, and it fires
on a player oscillating across the boundary. **A conversion that only moves the arm and the compare
changes the predicate.** The reset now lives in `ResetPhase()` beside `g_stuckSinceMs`, which buys
all three re-arm paths.

### Left alone, deliberately

`dialogue_reader.cpp kIdleReportAt = 64` — **the count IS the quantity** (`:243-245`: "this many
inert repeats is this many re-speaks the old code produced"). Rendering it in milliseconds would
destroy what it measures. Also verified and left: `kPolysPerFrame`, `kMaxAttempts`, `kProbeBudget`,
`kMaxAttemptLogs`, `kScriptFireLogBudget`, `kPaneSeenMax`, `kLatchCount`, `kPolyReadBudget` — all
bounded *work* or *log-volume* budgets, correct as counts. **Not every counter is a clock.**

### Files

`src/navigation/path_planner.cpp` (+`<windows.h>`, `kWaitMs = 1500`) · `src/navigation/nav_probe.cpp`
(`kWaitMs`, lazy arm preserved — stamping at the keypress would have changed behaviour) ·
`src/navigation/audio_beacon.cpp` (`kStrayMs = 750` + the disarm) · `src/ui/menu_reader.cpp`
(`kPaintWaitMs = 150` + `g_retryGaveUp`) · **new** `src/core/frame_probe.{h,cpp}` ·
`CMakeLists.txt`, `nav_hooks.cpp`, `dinput8_proxy.cpp`, `dllmain.cpp` (three-line wiring, no new hook).

### Outstanding

**Play-confirm, then a tester at high FPS** — nothing here is confirmed. Read the `[PERF]`
`frame pacing:` line first: it answers whether the field tick is display-paced (finding 2), and
toggling Speed Mode with it running confirms finding 1 live.

### ⚠ LATE IN THE SESSION — the tester says GAME SPEED, and that reopens the biggest question

The tester who prompted the audit clarified that their symptoms occur **specifically at raised game
speed**, not at raised frame rate: **dialogue read incorrectly, and an "aaaaaaaaa" runaway loop.**
No other tester reports it; not reproduced here.

**This forced a scope correction to a claim written earlier THIS session.** "Game speed is not a
factor anywhere in this file" was an overreach and has been struck in `PerFrameAudit.md`. What is
actually established is narrower: game speed cannot move a counter driven by the **field tick**,
because the mod hooks the outer function. It says nothing about hooks reached from **inside** the
sim loop — and `FUN_0022a770:139-184` runs a dozen subsystem updates in there, `ac4` times per
rendered frame, **including `FUN_00314020`, the mod's own former drain point**.

So the two facts fit together exactly one way: **the defect is on a hook inside the sim loop.** The
text walk is the prime suspect and matches the symptom's sound — S149's `widget+0xC0` end latch is a
LEVEL that oscillates and produced speech at FPS/2; inside the sim loop at 4x that is 4x the rate,
which is what "aaaaaaaaa" is.

**It could not be settled offline:** `FUN_002a8c50` and `FUN_002a9980` have **zero static callers**
in the 33k-function decompile — dispatch-table slots, reached indirectly. Runtime fact.

**So the instrument grew one counter** (`FrameProbe::OnTextWalk`, ticked from `HookedTextWalk`
before the widget filter, because the question is how often the GAME calls it). The report line now
carries `textWalk N/s (M, X/frame)`. Open a box, hold it, change Speed Mode: **~1.00/frame refutes
the hypothesis; ~4.00/frame confirms it** — and then the tier-A table's "output is O(changes)"
argument has to be re-examined for all twelve hooks, because it silently assumed one call per
rendered frame.

**Three lessons worth more than the fix:**

- **I closed a question the same session I opened it, and was wrong to.** "Speed cannot move a frame
  counter" was true of what I had checked and false as written. **State the scope you actually
  measured, not the scope you were thinking about.**
- **"Once per frame" has to say WHICH frame.** Rendered frame and sim tick are the same thing only
  at 1x. Every "per frame" comment in the mod was written assuming they are one.
- **A tester-only, condition-specific defect is evidence about a CALL PATH, not their machine.** The
  reproduction condition was in the report the whole time; "cannot reproduce" was never the finding.

### Session end state — NOTHING HERE IS PLAY-CONFIRMED

Built clean and deployed twice, no warnings. **No play data was obtained this session.** Required
before any of it is called fixed:

1. **Read the `[PERF] frame pacing:` line** on any session. It answers whether the field tick is
   display-paced — the unverified premise under the audit's whole severity story.
2. **Dialogue box up, change Speed Mode, watch `textWalk … /frame`.** This is the one that matters;
   it either confirms or kills the sim-loop hypothesis for the tester's defect.
3. **Nav regression:** `\` immediately on a map transition → a route, not `Route unavailable`, and
   the retry line now reads in ms.
4. **Beacon:** >6 m off route → exactly one silent re-plan; oscillating across the boundary → none
   (that second test is what proves the disarm).
5. **Menus:** heavy lists (save slots) → no new `TEXT NEVER PAINTED`.
6. **Then the tester**, at their speed setting, on this build.

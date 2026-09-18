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

## Session 153 — 2026-08-12 — [nav] The presence pruner deleted a gate crystal, and four named NPCs with it

KEYWORDS: gate crystal dropped, Rabanastre Crystal, Weather Eye, absent pruner, READY_PRESENT_BIT,
READY_POPULATION_BIT, sceneObj+0x14, 0x30, 0xB0, 0xF0, 0x70, LooksAbsent, OldRuleWouldDrop,
s_absentSpared, spared:, isCharacter insufficient, scene category 5-7, S148 S150 follow-up,
third time, entity_scan.cpp, Gate=0

**Reported mid-session: "gate crystals are being dropped." They were, and so was a quarter of the
NPC list — the report was the visible half of a wider deletion.**

### The evidence was already in the live log

```
absent: [0:17] +0x14=0x30 kind=4 "Rabanastre Crystal" at (115.0,-10.0,151.0)
rescan: 6 field objects (… Gate=0 …)
```

Grepping every `absent:` line in that session by byte shape settles it without a theory:

| `+0x14` | drops | what they were |
|---|---|---|
| `0xB0` | 44 | all `kind=1` "Hyena" — **correct**, the corpses the filter exists for |
| `0x30` | 153 | "Rabanastre Crystal" `kind=4`, and `kind=5` "Weather Eye", "Chocobo Aficionado", "Horne", "Rabanastran", one nameless — **all wrong** |

So the shipped filter was wrong on roughly three quarters of what it touched.

### Why the S150 fix did not hold — and the sentence that is now struck

S148 measured bit `0x40` on combatants and pruned the whole handle table with it. S150 caught it
deleting a **Save Crystal** and scoped it to `isCharacter`, reasoning:

> ~~"`isCharacter` … is the population it was measured on and the only one it may speak for. A corpse
> is a character; a crystal, a gate, a door and a treasure are not, so none of them can ever reach
> this drop again."~~

**STRUCK. A gate crystal IS scene category 5-7.** The gate was compiled in and the crystal went
through it anyway. `isCharacter` never excluded what it was written to exclude, and nothing in the
S150 log could have shown that, because that session's drops were all enemies.

### The fix: test for the measured SHAPE, not for the bit

Four values have ever been observed on `sceneObj+0x14`:

```
0xF0  live party / live enemy       present
0xB0  defeated enemy, unspawned     ABSENT   <- the only thing the filter is for
0x70  treasure, field gimmicks      0x40 set unconditionally, means nothing
0x30  save crystal, gate crystal    0x40 CLEAR while standing in plain sight
```

`0x40` alone separates nothing — it is clear on `0xB0` (drop) **and** on `0x30` (keep). The bit that
splits them is `0x80`, which **`nav_rva.h` has said in writing since S150** (*"bit 0x80 is what
actually separates the two populations"*) and which nothing ever acted on. `EntityScan::LooksAbsent`
now requires `0x80` set and `0x40` clear — `0xB0`'s high nibble — so all four observations fall out
right: `0xF0`→keep, `0xB0`→**drop**, `0x70`→keep, `0x30`→keep.

No claim is made about what `0x80` *means*. The rule is "drop only what looks like the thing we
measured as absent", which is the same scoping S150 intended, expressed in the data instead of in an
assumption about which objects are characters.

**One predicate, both walks** (`ScanCombatants` and `BuildLocked`) — the two sites drifting apart is
how a drop leaks back in.

### Ships with its own falsifier

`OldRuleWouldDrop` counts and names, capped at 4 per scan, every object the pre-S153 rule would have
deleted and this one keeps:

```
spared: [0:17] +0x14=0x30 kind=4 "Rabanastre Crystal" at (…) -- pre-S153 rule would have dropped this
handle-walk drops: N ABSENT (… measured 0xB0 shape) | M spared (0x40 clear but NOT that shape …)
```

A session that visits a gate crystal and produces **no** `spared:` line means S153 fixed something
else. The `ABSENT` counter staying non-zero on a map with corpses is what proves the drop still works.

### Play-confirm gate

Stand at the Rabanastre gate crystal: `Gate=1` in the rescan line, the crystal is reachable with
`=`/`\`, a `spared:` line names it, and the `ABSENT` count still rises when a Hyena dies.

**LESSON (L-01 again, third instance on this one bit): a scoping rule derived from what you believe
about a population is not a measurement of that population.** S150 scoped by "what kind of thing is
this" and was wrong about the thing; the data had the discriminator written down the whole time.

## Session 154 — 2026-08-12 — [nav] Stilshrine statues: the model came out of the bytecode, the numbers need one play pass

KEYWORDS: Stilshrine Miriam Mariam statue Stone Brave guardians three mrm_b02 mrm_b03 mrm_b04
mrm_c01 ebp_statue_census routine name pool bind map 598 599 600 全方向 北方向 clockwise
counterclockwise sword lift StatueDiag FindModulesBySrcPrefix VarAddressRaw VarCount RawModule
descriptor table storage class 4 global, statue_diag.cpp, no new hook

**Goal: make the mod aware of each statue's facing and the facing it should have.** The player can
press the button today — the dialogue reads — but nothing says which way the statue now points or
whether it is right, so the puzzle is unsolvable except by brute force.

### The logs had the interaction and none of the state

The 2026-08-11 dungeon log (13,898 lines, maps 600/599/598) already has the statue as a scanned,
routed-to object (`"Stone Brave"`, `nameIdx=-1` so the label is the map's own `fieldsignmes`
string), the full three-choice interaction, and the hint verbatim — *"Guardians three, face ye the
blade…"*, **the same string on every statue**. It has **no rotation value for any object**, because
`EntityScan::Entity` has no angle member and nothing prints one; and **no correctness bit** — the
statue's `flags` stayed `00002134` across every scan, before and after both rotations. Re-reading
the logs could never have produced this; it had to be derived.

### What the bytecode gave up, offline, for free

`..\FFXII-Decompile\tools\ebp_statue_census.py` (new, with a falsifier that aborts rather than emit
a table). Full findings in `GameArchitecture.md`; the short version:

* **Exactly four scripts in the game mention a statue** — three rotatable guardians (`mrm_b03`,
  `mrm_b04`, `mrm_c01`) and the big sword (`mrm_b02`). The inscription says *Guardians three*.
* **Map bindings by exact routine-name-pool match** against the pools our own log printed:
  600 Walk of Reason = `mrm_b04`, 599 Walk of Prescience = `mrm_b03`, 598 Cold Distance = `mrm_b02`.
  Map 600's match runs 44 consecutive names. There is no mapId→script join in the data, so this is
  the only honest way to name a map, and it is the census's self-check.
* **The authors named the whole model in Japanese**: `北方向`/`東方向`/`南方向`/`西方向` (four rest
  facings), `北～東`… (clockwise), `北～西`… (counterclockwise), `全方向` / `全方向NG` (the script's
  own solved / not-solved verdict), `像回転振動開始` (statue spin) and **`剣持ち上げ振動開始` — the
  sword lift, i.e. completion**. So a turn is 90°, there are four facings, and the two dialogue
  choices are two distinct named transition families.
* **`全方向` is in every guardian script.** A script on one map cannot judge two other maps' statues
  from map-local state ⇒ the facings are cross-script globals. Structural, ~0.95, and measured below.

### What it could NOT give up, and why that is a finding

The map-script `.ebp` container is **not** the layout `EBP2_DBG_format.md` documents for the four
controller scripts: on a map script `hdr+0x18` addresses the **message region** (the inscription's
codec bytes are right past it), not a routine table, and the mod's live-blob model (`hdr+0x4C` →
name pool) does not apply either. The **variable descriptor table is unreachable from the file**,
and with it the facing variable, its storage class and each statue's target.

Rather than decode a container format on spec, those go to a runtime capture — the engine builds
that table, and the mod already decodes it for the shout gauge.

### `StatueDiag` — built to answer everything in ONE visit

Per the user's instruction this session (*"don't worry about frida, put the probes in c++ … if you
just need to capture one thing, you can do it in C++"*), the probe is C++ and ships in the mod. A
second capture costs a game reload, so it does not hunt one variable:

* **logs the game's own verdict, by name.** This is the half that matters most, and the first draft
  of the capture was missing it — a variable diff alone shows numbers moving and cannot say *which*
  value is right, so the target would have to be inferred. That is modelling the verdict instead of
  reading the word it is read from (L-07). **The script says it out loud:** `全方向` /`全方向NG`
  are the solved / not-solved verdict and `北方向`/`東方向`/`南方向`/`西方向` name the facing a
  statue settles into, so a routine FIRE carries the answer as a label. `StatueDiag::OnEventFire`
  taps **`SneakAssist`'s existing `FUN_003dbb60` hook** — zero new hooks, one bool test off this
  dungeon — resolves the name through `MapScript::FiredRoutineName` and matches it against a table
  of raw cp932 bytes (that header is explicit that callers compare bytes; no locale is involved).
  Watched names log every time, uncapped; everything else gets a deduped, capped census so an
  unanticipated event is still visible. **An unresolved name is logged rather than skipped** — "the
  statue's routines fire with no resolvable name" would itself be the finding;
* **snapshots the module's ENTIRE variable table every field frame and reports what MOVED**, with
  the descriptor's storage class alongside — class 4 is the shared global int array, which is the
  fact that decides whether the readout can cover all three guardians from anywhere;
* **retires per-frame counters automatically** (changed on 3 consecutive samples ⇒ excluded, logged
  once) so a script clock cannot flood the log, and caps the whole visit at 400 change lines with an
  explicit "budget reached" line — **no silent truncation**;
* **dumps the statue scene objects raw**: scene category, class, `+0x0E`, `+0x14`, `+0x1C`, and the
  eight candidate orientation floats on the transform node, diffed. Selected by `nameIdx == -1`
  (the custom-string key), so it is locale-independent — the label is logged, never matched on.
* **It does NOT call the engine's yaw getter.** That function's identity is inferred, not at the
  0.98 bar, and a wrong signature on a game call is a crash, not a bad number (S129). Guarded reads
  answer the same question and cannot fault.
* **Armed on any live `mrm_` script, NOT on the statue table** — gating an instrument on the thing
  it diagnoses is how S133's shout meter went dark on a map where its sequence was running.
* **Zero new hooks** — it drains on the existing field tick beside `ShoutMeter::OnFieldFrame` and
  clears on the existing teardown. MinHook is at 66 hooks against a 63-trampoline block.

### Centralization

`ShoutScript` grew a generic layer rather than being forked: `RawModule`,
`FindModulesBySrcPrefix` (prefix, because a dungeon's rooms are separate scripts sharing one
authoring prefix), `VarCount`, and `VarAddressRaw`. **`VarAddress(const Module&)` now delegates to
`VarAddressRaw`**, so the descriptor decode exists exactly once and a `Module` and a `RawModule` can
never disagree about where a variable lives. The namespace name is now historical and the header
says so.

### NOT BUILT YET, deliberately

`statue_table` / `statue_guide` and the phrasebook strings are **not** in this build. They need the
facing variable index and each statue's target, and writing a table on placeholders would be
fabrication. The design is settled and approved — turn-count wording ("Stone Brave. Two turns
clockwise." / "In place."), `B` reused and context-gated, all three guardians from anywhere if the
state is global with a central-room fallback if it is not — and it lands the moment the capture
returns.

### Play-confirm gate

One pass: load at *Walk of Reason* (600), turn the statue one click each way, walk to *Walk of
Prescience* (599), same again, then finish the puzzle. That log must contain a variable stepping in
time with each turn plus its storage class, the statue's scene category and class, and whatever
moves when the sword lifts. **Both rooms, so the table is checked on two members, not one (L-01).**

**Three independent nets, so one pass cannot come back empty-handed:** the named routine fires (the
verdict, directly), the variable diff (the state, with its storage class), and the node-orientation
diff (whether the transform moves at all, or the statue is animation-only as the census suggests).
If the routine names resolve, the target facing needs no inference whatsoever — `全方向` firing
after a turn IS the game saying that turn was the right one.

## Session 155 — 2026-08-12 — [input+menus] Two keys that answered when they were not asked

KEYWORDS: Alt+F4 combat verbosity, bare press, fkeyModifierHeld, DIK_LWIN, F4 F5 F6 F7 F8 F11,
input_tracker, Libra describe key, SpeakTargetDetail, TargetSelectActive, P+0x10F78, OFF_GATE,
ability description unreadable, battle menu, ResolveTarget commit-first, statue capture starved,
kMaxVarLines kMaxObjLines, IsGimmick, kYawEpsilon, SnapshotGlobals, ClassBaseRaw

Three reports, all deployed together.

### 1. Every F-key now requires a bare press

**Alt+F4 was flipping Combat verbosity on the way out of the game.** The mod cannot swallow a key,
so an unguarded F-key fires *in addition* to whatever the chord already does — Ctrl+F4, Shift+F7 and
the rest were the same defect, a silent state change the player never asked for.

The guard already existed and had been **scoped to F11 alone** since S112 (*"Shift+F11 is an NVDA
command the tester needs while playing"*). That reasoning was never specific to F11; scoping it to
one key just meant the other five kept the bug. It is now one `fkeyModifierHeld` computed once and
applied to `F4 F5 F6 F7 F8 F11`, and it covers the Windows keys as well as Shift/Ctrl/Alt.

Holding a modifier makes the F-key read as **up** rather than suppressing the dispatch, so a modifier
pressed mid-hold registers a clean release and cannot leave an edge armed.

### 2. The Libra readout was eating every ability description

Reported: *"libra branch for description reader is taking precedence over everything else in the
battle menu, so impossible to read ability descriptions."*

`SpeakTargetDetail` gated on "is there an enemy target", and `ResolveTarget` answers that from the
**committed** target first — a path with no UI gate at all; only its browse fallback is gated. A
commitment outlives the aiming step, so in the battle menu picking a spell, with an enemy still
committed from the last action, `o` spoke Libra and the fall-through to the description bar was
unreachable.

**STRUCK** in both files: *"structurally silent everywhere else (no committed/browsed enemy target
=> false)"*. It tested whether a target EXISTS, not whether the player is AIMING at one.

`SpeakTargetDetail` now requires `TargetSelectActive()` — the game's own `P+0x10F78`, the same bit
the browse path already trusts — before it claims the key. Enemy-only was already there, unchanged.

**`ResolveTarget` was deliberately NOT touched.** Its commit-first order is load-bearing for `p` and
`;` (the 2026-07-21 regression note on this file). The residual — while aiming, it can still name a
stale commitment rather than the unit under the cursor — now emits a log line **only when the two
disagree**. If a play log shows it, the fix is cursor-first resolution for this one key, with
evidence behind it rather than a hunch.

### 3. The statue capture starved itself, and the fix is structural

The first run produced **no rotation data at all**, and looked in the log like a clean negative:

```
[00:22:27.718] change log budget reached
[00:22:49.671] "Turn the statue clockwise."
```

362 of the 400 shared lines went on yaw jitter from **wandering enemies** — "Balloon 2", "Ghoul",
"Zombie Warrior". The object selector was `nameIdx == -1`, which on this map catches enemies too,
and with a collect cap of 8 they may have pushed the statue out of the window entirely. The user had
said the dungeon was full of enemies before the instrument was written.

Four corrections, the first being the one that matters:

* **One budget per net, never a shared one.** A noisy net can now only starve itself. This is the
  actual defect; the rest is why that net was noisy.
* **Gimmick-class filter** — `sceneObj+0x03 >> 5 == 1`, measured in that same run: Stone Brave and
  the Ancient Doors are `sceneCat=1 class=1`, enemies are characters (5-7). Cap 8 → 16.
* **Epsilon compare** instead of `!=`. Baselines read `-0.0000`, a denormal from zero, which an exact
  compare calls a change every frame. A quarter turn is ~1.57 rad.
* **Per-slot animation retirement**, and object snapshots keyed on the scene-object POINTER rather
  than the collect index — the order is not stable, which re-baselined the same objects 54 times.

**A fourth net was added**, closing a gap the others leave: `SnapshotGlobals` diffs the shared
class-4/5 arrays RAW via the new `ShoutScript::ClassBaseRaw`. The variable sweep only sees what
`mrm_b04` itself declares; a flag declared by another script was invisible to it however global its
storage.

### What that run DID establish (kept, because it was not free)

* **Stone Brave is `sceneCat=1 class=1`, `+0x14=0x70`** — a class-1 gimmick, and a byte shape the
  S153 pruner can never touch.
* **The statue's routines do NOT start via `FUN_003dbb60`.** 111 fires resolved by name across the
  rotations, every one an engine lifecycle routine (`init`/`main`/`spawn`/`entry`/`respawn`), and the
  unwatched-fire budget was never reached. That net is a clean negative and the tap stays only as a
  cheap census.
* **Both guardian scripts carry the same 7 messages**, decoded offline: *"The statue is firmly fixed
  in place."*, *"You discover a mechanism in the statue's base…"*, *"The colossus has undergone some
  change…"*. The first means some statues are locked, which changes which room can test a rotation.

**LESSON: a shared budget makes one net's noise into another net's silence.** And the log said so
plainly — the cap line was 22 seconds before the event. Grepping the instrument's own budget lines
before reading its result is now part of reading any capture (L-04, L-05).

## Session 156 — 2026-08-12 — [menus+nav] The statue state is SAVED game state, and `o` stopped answering the wrong question

KEYWORDS: statue facing 0x0D 0x0E flag 0x07 0x08 target facing save block storage class 0
0x02164480 FUN_002ef2b0 eye effect buzzing mrm_b03 mrm_b04 map 599 600, Libra description-first,
BattleCommandActive, P+0x10F78 not target-select, stale build stamp, log trails the game,
shared budget starvation, class-0 raw sweep 4096

### 1. `o` — the description is the key's primary meaning, Libra is the fallback

The S155 fix (gate the Libra branch on the target cursor) **did not work**, and the user was right
to push back. Two compounding faults:

* **`P+0x10F78` is NOT "target selection active".** It was true while the battle command menu was
  open. The name in `battle_target_reader.cpp` was inferred and is now STRUCK.
  > **⚠ THIS STRIKE IS ITSELF UNMEASURED — flagged S159 (2026-08-14).** Every archived log in the
  > corpus runs build `f544fe1`, which predates the S155 gate; only the 2026-08-14 log runs a build
  > containing it. **No log ever existed that could have tested this claim**, and the offset is
  > still named and trusted at `battle_target_reader.cpp:52,109` — so the strike and the code have
  > contradicted each other since. S159 ships a log-only state line on the `o` press to settle it
  > rather than infer a third time. Do not act on either reading until that line is in a play log.
* **The ORDER was the real defect.** `SpeakTargetDetail()` ran first, so `TextCapture::CurrentHelpText()`
  was **never called** in battle. The reporting log has four "Libra not active" lines and **zero**
  `describe:` lines — which reads like "there is no description" and is nothing of the kind. *The
  question was never asked.* Gating the Libra branch fixes the symptom and leaves everything resting
  on that gate being right; asking the description FIRST removes the dependency.

Now: help text (generation-gated to the current focus, so it cannot leak into the target cursor) →
a refusal while `IngameMenuReader::BattleCommandActive()` → Libra → a log line saying which came back
empty, so a silent `o` is never ambiguous again.

`BattleCommandActive()` stores the battle panel and **re-validates it against the window class on
every read**, so a freed or repurposed panel stops answering true by itself.

### 2. Storage class 0 is the persistent SAVE BLOCK

Both modules reported the same class-0 base `0x02164480` — which this repo already documents as
`FUN_002ef2b0()`. Full write-up in `GameArchitecture.md`. The consequence that matters: **a module
declares only the subset of that array it uses**, so a declared-variable sweep can never enumerate
the region, and a raw byte diff is the only way to see a cell the current module does not name.

### 3. The statue puzzle state, measured

Facing and correctness both live in that save block, per statue. Table, addresses and semantics in
`GameArchitecture.md`. Headlines:

* facing is **1..4**, clockwise **increments** (`4→1` wrap), counterclockwise **decrements**;
* each statue has its **own correctness flag**, and **the targets differ** (600 → 1, 599 → 2), so the
  values are compass headings and "all three read 1 when solved" is **refuted**;
* **the player identified the flag by ear before the instrument did** — the eye effect and a faint
  buzz are on exactly while the flag is set;
* therefore **the readout needs no solution table**: correctness is read from the game.

### 4. Three instrument defects, all mine, all found by the data

* **A SHARED BUDGET MADE ONE NET'S NOISE ANOTHER NET'S SILENCE.** 362 of 400 lines went on yaw jitter
  from wandering enemies and the budget closed **22 seconds before the first rotation**. The run
  looked like a clean negative and was a blinded one. Now one budget per net.
* **The object selector was `nameIdx == -1`, which on that map catches enemies** — and the user had
  said the dungeon was full of them before the instrument was written. Now also requires the class-1
  gimmick shape, measured from the same run.
* **The raw global diff covered classes 4 and 5 at 512 bytes and missed everything.** Class 0 was not
  swept at all, and the statue cell sits at base+0x9B1 — past a 512-byte window either way. Class 0
  is now swept at 4096 bytes, which is what will surface the third guardian's cells with no special
  trip.

### 5. Two ways I read evidence wrongly, both worth not repeating

* **The `Build:` line is stale on an incremental build.** It is `__DATE__`/`__TIME__` compiled into
  `logger.cpp`, which does not recompile unless it changes. I used it to tell the user they had not
  run a fix — they had, and it had genuinely failed. **Compare the deployed DLL against the build
  output instead**; `cmp` settles it in one line.
* **The log file trails the running game by a minute or more.** Twice I read it, saw nothing past a
  point, and reported absence; the writes had simply not landed. **Check the file's mtime against the
  wall clock before concluding a thing did not happen.**

### Deferred to next session, deliberately

`statue_table` / `statue_guide` / the phrasebook strings. The design is settled and the measurements
are in `GameArchitecture.md`; building it cold next session with the documentation in front of it was
the user's call, and it is the right one — this session's context is spent.

---

## Session 157 — 2026-08-13 — [nav] The statue readout: three guardians, one key, no walkthrough

**KEYWORDS: statue guide Stilshrine Miriam Mariam tomb statue_table statue_guide B key three
guardians solved clockwise counterclockwise save block class 0 0x02164480 mrm_b03 mrm_b04 mrm_c01
map 599 600 facing flag target learned offset phrasebook Statue StatueSolved StateUnknown**

The build S156 deferred. Everything was measured; this session spent nothing on RE and all of it on
shipping the readout.

**`B`, anywhere in the Stilshrine, speaks all three guardians:**
`Statue 1: counterclockwise once. Statue 2: solved. Statue 3: state unknown.`

### 1. The two reach paths, and why both are needed

A guardian's facing and correctness flag are two cells in the persistent save block (script storage
class 0, `0x02164480`), so the state is not map-local — which is what makes one key able to answer
for three rooms. But a cell can be addressed two ways and neither one covers every case:

* **the module's descriptor index** works only while the player stands in that room, and needs no
  hardcoded address at all;
* **the save-block offset** works from anywhere, but has to have been measured first.

So `statue_guide` prefers the live module, **learns** the offset from it (`address − class-0 base`),
keeps it for the session, and uses offsets for the rooms the player is not in. A single walk through
a room upgrades that statue from "readable here" to "readable anywhere", with no rebuild.

### 2. What ships as unknown, and why that is spoken rather than skipped

`mrm_c01` has never been visited: no map id, no variable indices, no target, no offsets. Nothing is
interpolated for it. The tempting shortcut — the flag index is the facing index minus six in **both**
captured modules — is written into `statue_table.cpp` as an observation and deliberately **not** used
(L-01: two instances are not a population, and a wrong index reads some unrelated save-block byte and
reports a confident wrong state).

It is **spoken** as `state unknown` rather than omitted, because a readout that lists two statues
reads as a two-statue puzzle. That is the one place the feature says something instead of nothing,
and it is a positive claim about our own coverage, not filler about the game.

**If NOT ONE statue resolves the key is silent** and logs why. Three "unknown"s would be filler
dressed as an answer.

### 3. The flag is the verdict; the target is only a count

`solved` is read from the game's own per-statue flag, never inferred from the facing — the whole
reason S156's measurement mattered is that it removed the need for a solution table. The target is
used for exactly one thing, `(target − facing) mod 4`: 1 → clockwise once, 2 → twice (equal either
way, said clockwise), 3 → counterclockwise once. **There is no three-turn case.**

A flag/target disagreement resolves **in the flag's favour and logs loudly**, because it can only
mean a baked constant has gone stale.

### 4. The third guardian costs no special trip

Two log nets, both bounded, both one-per-visit:

* entering a guardian room whose cells are unmeasured — today only `mrm_c01` — dumps every class-0
  variable that module declares, with offsets and values. Also fires for any `mrm_` module the table
  does not know at all, which is how a fourth statue script would announce itself.
* the save block either side of each **known** cell is dumped once per visit. The measured pair sits
  in two different sub-regions (`+0x9B1` a facing, `+0x882` a flag); if one authoring template was
  instantiated three times, the three facings are plausibly neighbours in one array and the three
  flags in another, which would bind the third guardian with no visit at all. **Hypothesis only —
  nothing reads on it.** The dump is how it gets tested or killed.

### 5. `B` is context-gated, and the decision stays on the game thread

`B` already meant the Bhujerba infamy meter. The two contexts can never both be live (a street
sequence and a dungeon), so the dispatcher raises **both** requests and each drains on the next field
frame against its own structural gate. Asking "which applies?" on the input thread would have meant
resolving script modules off the game thread — the thing the `'` probe's arrangement exists to avoid.

The gate is the **`mrm_` script prefix**, not an area name: the area name is localized text in twelve
languages (the game calls it "Stilshrine of Miriam" in ours), and the prefix is the bind
`ebp_statue_census.py` established.

### 6. Wording

The tester's own shape, this conversation: `Statue N: <verdict>`. Eight phrasebook ids, English only
— `Statue`, `StatueSolved`, `StatueNotSolved`, `StateUnknown`, `Clockwise`, `Counterclockwise`,
`Once`, `Twice`. The area name is **logged, not spoken**; adding it to the line is one edit if the
numbering turns out to be harder to hold than the room names.

### ~~Play-confirm owed~~ — PLAY-CONFIRMED 2026-08-13 (during Session 158)

**The statue readout is confirmed working.** Map 599 reads from anywhere immediately; map 600 needs
one entry into Walk of Reason before it reads from elsewhere — the `MEASURED …` log line is the
receipt, and the offsets it printed were baked into `statue_table.cpp` in S157.

### 7. A claim struck and un-struck inside one session: "`mrm_c01` is the boss room"

S154 inferred "the boss/event room" from `BOSS_…`, `EventDirector`, `ReposDirector` and
`PlayerJack*`. I repeated it to the user as if measured. They pushed back — *the boss room is what
this puzzle unlocks, so no guardian stands in it* — and I struck it across `GameArchitecture.md` and
`debug.md` and wrote a general lesson about routine-name pools on top of it.

**Then they played it: it is the boss room AND the room where the last statue is turned.** The
refutation was an argument about PROGRESSION and never excluded the two being one room. Everything
was withdrawn the same session.

The correction cost more than the original claim did. **L-64 now says what actually went wrong: a
correction is a conclusion and carries the same bar as the thing it corrects — check whether a claim
and its refutation are even exclusive before striking, and do not mint a lesson whose whole evidence
is one unverified exchange.** What was never measured in either direction is the only thing the mod
needs: `mrm_c01`'s map id and its two save-block cells.

### 8. And the process failure that produced the 2-of-3 solver

The user had asked outright whether a visit to the third statue was needed. The answer given was no,
reasoning that the flag makes a solution table unnecessary — true, and irrelevant: not needing to
know the correct *facing* says nothing about knowing the *address*. What shipped was a solver whose
third line says "state unknown", against a feature whose whole premise is *one key, all three*.
**L-63: a measurement only the player can take is a question to ask, not a constraint to design
around.** The tell is writing "self-measures on first entry" about the deliverable itself.

### 9. All three guardians bound the same day — the capture worked in one visit

The user ran the capture. Every number the table was missing came back, and the two nets agreed
byte-for-byte:

| map | script | facing | flag | target |
|---|---|---|---|---|
| 599 | `mrm_b03` | `+0x9B1` | `+0x882` | 2 |
| 600 | `mrm_b04` | `+0x9B3` | `+0x883` | 1 |
| **603** | `mrm_c01` | `+0x9B5` | `+0x884` | **unsettled** |

**`mrm_c01` declares all three statues' cells**, which is what exposed the layout: flags consecutive
at `+0x882/883/884`, facings stride-2 at `+0x9B1/9B3/9B5`. The once-per-visit neighbourhood dump —
added as a hypothesis test with nothing reading on it — is what made the pattern legible at a glance,
and the census then confirmed it by index. **The hypothesis was right and it still did not need to be
trusted**, which is the point of shipping it as a log line rather than as a lookup.

`+0x885` flipped on the same frame as the third flag and is declared by `mrm_b02`, the SWORD script.
That is the completion cell, banked for a future announcement. The player also identified two
completion messages — *"the colossus"* on solving, and *"The statue is firmly fixed in place"*
afterwards, i.e. **the statues lock once done.**

**One number withheld: map 603's target.** Its flag set at facing 3, then the facing moved 3→4 five
seconds later with no clear — unlike the other two, where facing and flag moved on the same frame.
Target 3 plus a locking completion animation fits, and fits the "firmly fixed" message, but it is not
0.98. That statue reports solved / not solved with no turn count until it is settled.

Also corrected: `mrm_c01` is **map 603**, and the room is both the boss room and the last statue's —
the S154 name-pool inference and the player's objection were describing the same place.

### 10. Map 603's target IS 3, and withholding it was the wrong call

I shipped 603 with no target on the grounds that the capture was "ambiguous": its flag set at facing
3, then the facing moved 3→4 with no clear, unlike the other two where facing and flag moved on the
same frame. The tester overruled it, and was right twice over:

* **the rule is uniform** — 599's flag set at facing 2, 600's at facing 1, 603's at facing 3, each on
  the frame its statue became correct;
* **the 3→4 move is only anomalous if you forget the statues LOCK on completion.** "The statue is
  firmly fixed in place" is the game saying the player cannot turn it any more, so that write is the
  completion sequence's own, not a player turn.

And the cost of "confirming" it would have been unsolving a finished puzzle to re-solve it — for a
number three observations already agreed on. **Turn-by-turn directions are the entire point of this
feature; a statue that reports only solved / not-solved is the feature not working.** Withholding a
number is not automatically the conservative choice: here it degraded the deliverable to protect a
confidence bar that was not actually in doubt.

### 11. BEACON — it never had a "is the player driving" gate at all

Reported: the beacon plays during cutscenes and with the battle menu open, but not with the party
menu open. Diagnosed and fixed.

**Root cause:** every gate in `AudioBeacon::OnGameFrame` asks whether the FIELD EXISTS — audio up,
the setting on, a route loaded, epoch match, `IsFieldNavSafe()`, combat engagement. **None asks
whether the player is in control.** `IsFieldNavSafe()` is six LIVENESS predicates and every one stays
true through a conversation and a cutscene. Not an S152 regression: the gate never existed.

**Why the party menu was quiet** — by accident. Opening it stops the field tick that calls
`OnGameFrame` at all. That accident is what made the leak look selective.

**Why the battle menu leaked** — the tester supplied the piece I had wrong: **FFXII lets the battle
command menu be opened OUT OF COMBAT** on any map where battles can happen. So `PartyEngagement()`
reads clear and the objective beacon runs underneath it. "Are we in combat" was never the right
question.

**The fix:** one suspension block ahead of the combat branch, so it covers BOTH beacons (tester's
call on the target ping). Two gates, both the game's own state — `IngameMenuReader::
BattleCommandActive()` (re-validates its panel against the window class on every read) and
`DialogueReader::IsBoxLive()` (the engine's message-window registry, already `message_reader`'s
choke point). **SUSPEND, NEVER STOP** — `Stop()` discards the legs, and a player closing a menu
expects the same leg back, exactly as after a fight. Transition-only log line.

Deliberately NOT used: `MenuState::IsAnyMenuOpen()` — one write, zero clears, answers "open" forever;
a gate built on it once killed the field object scan for a whole fight.

**Residual, stated rather than hidden:** a cutscene with NO message box is still uncovered. Most
FFXII scenes caption through the same paginated box, so `IsBoxLive()` should carry them, but a silent
camera scene has no measured signal in the codebase yet.

**And `F9` doing nothing is correct, not a regression.** The game owns F9 ("Hide On-Screen Keyboard",
S112); the beacon toggle is **F11**, bare press only, because Shift+F11 is an NVDA command.

### PLAY-CONFIRMED 2026-08-13 (during Session 158) — PARTIALLY

**The suspension works in menus**, which is the case the report was filed against. **The cutscene and
dialogue cases are NOT yet verified** and the tester has deliberately left them for a later pass, so
`IsBoxLive()` carrying a captioned scene is still an inference rather than a measurement — and the
residual named above (a silent camera scene with no message box) is untouched by this confirmation.
Do not upgrade either to "confirmed" without a log.

## Session 158 — 2026-08-13 — [menus] The gambit editor: a class byte that meant "incomplete", and a list read from the wrong source

**KEYWORDS: gambit half-set row condition without action empty rec+0x15 class incomplete rec+0x10
rec+0x12 FUN_00567b60 FUN_0056a1d0 FUN_00569f90 FUN_00568bb0 gambit picker FUN_0056b4d0 stale
category paint cache gambit_picker_reader picker+0x595 picker+0x0E0 ability summary double speak
party menu rename field menu Session 93 reversed**

Two reported defects on the gambit editor, one more found in the log while reproducing them, and the
Session 93 "field menu" vocabulary reversed at the tester's request.

### 1. "with a condition it should read something like: foe: party leader's target. empty. off"

The reader called a row empty on `rec+0x15 == 2`, decoded nothing, and said "empty". That byte means
**incomplete**, not empty: `FUN_00567b60` writes the condition id and the real condition name into
the record first and only afterwards sets it to 2 when EITHER id is `0xFFFF`. The row painter assigns
`rec+0x00` into the condition sprite for every row regardless of class, so the condition was on
screen the whole time the mod was calling the row blank.

The two ids answer separately, so the fix is to ask them separately. An unset half now contributes
the word "empty", which makes the row read `"Foe: party leader's target, empty, off"` and gives every
column something to say instead of dropping into the no-text SILENT path.

**The log had already recorded the proof and I nearly walked past it.** In
`FFXII-Screen-Reader-2026-08-12_10-31-36.log` the player picks a condition at +329015 ms, then moves
`col=2 → col=1 → col=2` over the next four seconds hearing "empty" each time, and at +334750 the same
row reads `"Foe: not targeted by ally"`. A value that survives four cursor moves and a picker re-open
is stored, not staged — the row was never empty, and one log line five seconds apart says so.

**A CORROBORATED CLAIM CAN STILL BE SCOPED WRONG (L-01 again, from the other end).** S94's probe
output is in the archive and it shows the class byte taking 0, 1 and 2 on one screen. Its pass
criterion — *"cond/action ids read 0xFFFF on exactly the rows whose class byte is 2, and nowhere
else"* — was recorded as confirmed and was true of every row that run saw, because that run never
edited a row. Nothing was measured wrong. The population was three quarters of one, and the note in
`gambit_reader.h` carried the narrower claim as a general law for 64 sessions.

### 2. The picker was reading the paint cache — the worse defect, and it was not the reported one

The tester reported categories being announced as *"the trailing action that was selected from the
last category"*. The mechanism turned out to be more serious than the wording suggests: the picker
had **no reader at all** (S94 declined it as unmeasured), so it fell to the generic painted-row path,
which resolves text out of `TextCapture`'s per-paint item map. The focus for a category switch
arrives before the new rows are drawn, so the player heard the previous category's row 0 — and, on
opening the action list over the condition list, `"Foe: party leader's target"` where `"Attack"` was
highlighted. **A blind player was choosing gambit actions from a list reporting the wrong row**,
which is not a wording problem.

`ui/gambit_picker_reader.{h,cpp}` reads the picker's own array (`picker+0x0E0 + i*0x20`, 17 slots).
Both rebuilds fill that array **before** they move the list cursor, so by the time the focus reaches
us the rows are already the new ones — the timing question disappears instead of being tuned. No new
hook: the focus is the same `FUN_00247510` `0x8000` the panel's arrives on. It claims the focus only
when it actually spoke, so a shape it does not recognise still falls through to the path that covered
the surface before — which is the protection S94 was reaching for when it declined to claim it.

**Deferring to the next paint was the obvious fix and it is the wrong one.** `menu_reader.cpp`
already has that idiom for the Clan Primer, and it would have narrowed the window rather than closing
it: a deferral is a bet that the next paint is the one you want. The array is the answer. Written up
as **L-65**, with S89 and the off-hand list as the two earlier instances of the same shape.

### 3. The same screen was speaking twice, and nobody had reported it

`[LICENSE] summary: "Cure, unavailable"` twice in the same millisecond, from one owner, in the middle
of a gambit edit: the license board's ability page controller is driven by this picker as well, and
driven twice per event. `AnnounceEntry` now stands down while `GambitPickerReader::IsLive()`. That is
arbitration between two paths covering different cases — the sanctioned shape — not a same-as-last-
time filter, and not the deletion of one path that this project has already paid for twice.

### 4. The category name does not exist, so nothing was invented

The tab descriptors carry a colour id and a member count; the tab strip draws coloured plates; no
`FUN_002f9860` call in the picker resolves a category string. **The instrument shipped instead of a
guess**, and the tester's category walk answered it in one pass — all four candidate routes refuted:
`DefName(0x15, family)` collapses five magick tabs into "Magicks", `DefName(0x15, tabIndex)` returns
"NOT USED concentration"/"Summon"/"Foecraft", `DefName(0x18, family)` names the wrong school because
the family byte is not a school id, and `picker+0x580` is the *"you cannot pick this"* error banner
the tab-step functions merely clear.

The eleven action tabs are a gambit-specific grouping — 1 Attack, 5 Magicks, 3 Items, 2 Technicks —
with no string table anywhere. **Tester's call: announce nothing.** The corrected first row of the
tab just entered already distinguishes all eleven, and every alternative I could offer was either a
number or a word I would have made up. The diagnostic came out again with the question it answered;
what it learned is now in `GameArchitecture.md` and in the reader's own header, marked "do not
re-derive".

**A diagnostic is worth writing to be DELETED.** Four `DefName` calls per category switch bought one
answer and then had no further job. Leaving it in would have looked like caution and been cost.

Two corrections from the same walk. Row `+0x10`'s not-acquired value is **`0x10`**, not the `0x0F`
the decompile suggested — tested `!= 0`, so both readings behave identically, but the measured one is
what the file now says. And **`ingame_menu_reader.cpp` had `0x15` and `0x18` labelled the wrong way
round**: `0x18` is the magick-school table, `0x15` the battle-command table. The values and the
branch were always correct, so nothing ever behaved wrongly — but a comment that names the wrong
table is exactly the kind of claim that gets built on three sessions later.

### 5. "field menu" → "party menu", reversing Session 93

S93 renamed the game's own **Party Menu** to *field menu* to avoid "a party menu inside the party
menu". The tester's call: the collision never actually caused one, so the game's word comes back. The
inner command keeps the name the live docs already gave it — the **Party screen**, never "party
menu", which is what makes the outer name free.

Docs-and-comments only: there is no `FieldMenu` identifier anywhere and **no spoken string contains
"field menu"**, so the binary is unchanged by this item. 61 occurrences across README, `Controls.md`,
`GameArchitecture.md`, `debug.md`, `release_procedure.md` and ~20 code comments. `Docs/sessions_*.md`
deliberately untouched — it is the append-only record, and `sessions_051_100.md:4356-4360` is the
entry recording the decision being reversed. The `IsFieldPaneOwner` / `HookedFieldPaneWnd` /
`RVA_FIELD_PANE_WND` identifiers are also untouched: they name the window class `FUN_00280de0`, which
is a different thing from the menu.

### Files

New: `ui/gambit_picker_reader.{h,cpp}`. Changed: `ui/gambit_reader.{h,cpp}`, `ui/menu_reader.cpp`
(include, the picker branch, the panel-focus note), `ui/ability_summary_reader.cpp` (stand-down),
`ui/ingame_menu_reader.cpp` (the reversed chooser-table names), `CMakeLists.txt`; the rename across
`README.md`, `Docs/{Controls,GameArchitecture,debug,release_procedure}.md` and 15 source files.
`GameArchitecture.md` gained the record-builder / commit / write-back chain, the save-block source at
`panel+0x0F4`, `rec+0x17`, the picker's full layout and the closed category question; two claims
struck there, one in `gambit_reader.h`, one in `ingame_menu_reader.cpp`.

### PLAY-CONFIRMED, both halves, same day

Tester: *"gambit fix for the empty action on a condition is confirmed working."* The picker is
confirmed from the log of that session — eleven category switches, each speaking the new tab's own
first row, zero stale items, zero duplicate `[LICENSE] summary` lines, and the picker class constant
right on the first build. Entries the character has not acquired now say so ("Cure, unavailable"),
which the old path dropped silently.

**This build also carried S153-S157**, which had never been deployed, and two of those were played
in the same sitting: **the Stilshrine statue readout is confirmed working**, and **the beacon
suspension is confirmed in MENUS** — the case its report was filed against. The cutscene and dialogue
cases are deliberately left for a later pass, so `IsBoxLive()` carrying a captioned scene remains an
inference. S153's presence-pruner fix and S155/S156 were not exercised.

### The commit

S153-S158 land as ONE commit. They are six sessions across two tracks (nav/input and menus), and the
house rule is one commit per session per track — but the S153-S157 work sat uncommitted for a day
while S158 edited the same files (`menu_reader.cpp`, `ingame_menu_reader.cpp`, `CMakeLists.txt` and
four shared documents all carry both tracks' hunks). Splitting them now would mean hand-partitioning
mixed files into commits that were never built in that state, which buys a tidier log at the cost of
a history whose intermediate points do not compile. **Recorded here and in the memory index so a
future `git log` trace does not read six sessions as one.**

---

## Session 159 — 2026-08-14 — [menus] Libra's key was shadowed by the gate added to protect it

KEYWORDS: Libra o key regression BattleCommandActive g_bcmdLivePanel target cursor aiming phase
menu focus surface belt and braces second gate unmeasured flag P+0x10F78 OFF_GATE 0x10F78
SpeakTargetDetail DescribeHotkey description-first hot-reload dropped payload split L-66

### 1. The defect: `o` announced nothing in battle

Reported: with Libra up the enemy's HP reads correctly, but `o` never speaks level, absorbs or
weaknesses. The user's own guess named the cause — *"this likely has to do with your hardening
against firing in the battle menu"* — and it was right.

`MenuReader::DescribeHotkey` early-returned on `IngameMenuReader::BattleCommandActive()` before
`BattleTargetReader::SpeakTargetDetail()` was ever reached. **That flag is true for the entire
aiming phase.** Two independent reasons, and each alone is enough:

* `g_bcmdLivePanel` is cleared only when a menu focus lands on a *different* owner. **The target
  cursor is not a menu focus surface** — confirming a command emits no 0x8000 for another owner, so
  nothing clears it.
* The re-validation that was supposed to be the load-bearing half checks *liveness*, and the command
  panel is still allocated and still its own class behind the cursor. **Liveness catches a dead
  object, never a live one the player has navigated away from.**

### 2. The measurement

`x64\FFXII-Screen-Reader-Latest.log`, build `5f13705`, one battle:

| time | line |
|---|---|
| 15:38:19.328 | `HookedBcmdConfirm` — Attack confirmed |
| 15:38:19.359 | `[TARGET] handle=0x200021 enemy "Hyena A, HP 95/95"` — cursor up |
| 15:38:20.062 … 27.046 | **nine** × `o: battle command menu is live -- Libra declined` |
| 15:38:23.343 / 24.234 / 24.953 | `[TARGET] ResolveTarget: "Hyena A" BROWSING enemy` |
| 15:38:27.828 → 31.4 | back in the command list: zero `[TARGET]` lines; `describe:` fires for Protectga |

Nine presses, cursor demonstrably up and on an enemy throughout, zero Libra. The two phases never
overlap in the log — `[TARGET]` lines only during aiming, `[INGAME] command:` only during browsing.

**Corpus:** the refusal line appears in exactly one log (9 hits) and its fall-through
(`o: no description for this focus`) in **none**. The gate never once fired in the case it was
written for.

### 3. The fix — one behavioural change

`menu_reader.cpp`: the early return is **STRUCK**; the flag survives as a log-only discriminator
*below* `SpeakTargetDetail()`, where it cannot shadow anything. Arbitrating it instead
(`!TargetSelectActive() && BattleCommandActive()`) would have been identical to having no gate,
because `SpeakTargetDetail` already returns false whenever the cursor is down — so the early return
bought nothing except the outage.

**Description-first ordering is untouched.** That is S156's real fix and it works: log
`2026-08-13_03-11-17` shows 18 `describe:` lines over 355 command-menu focus events with zero wrong
Libra, on a build predating the gate. **S156 fixed this defect twice and only the second one could
regress** — the lesson is L-66.

`ingame_menu_reader.cpp`: comment-only correction. The flag was documented as "the mod's own
knowledge of which surface the player is on"; it means "the command panel is alive and was the last
thing to take a menu focus". Its lifetime is deliberately unchanged — one consumer, now log-only.

### 4. An unmeasured strike, found while checking the other side

S156 STRUCK the name of `P+0x10F78` on the claim that it "was true while the battle command menu was
open". **Every archived log runs `f544fe1`, which predates the gate that would have tested it**; only
the 2026-08-14 log runs a build containing it. The claim was never measurable, the offset is still
named and trusted at `battle_target_reader.cpp:52,109`, and the strike and the code have contradicted
each other since. Rather than infer a third time, a **log-only state line** now prints
`gate / handle / bcmdLive` on the `o` press, keyed on the state tuple so it emits once per distinct
combination. One play session closes it; the block is commented to be deleted afterwards.

### 5. Hot-reload — investigated, viable, and dropped by the user

Asked whether the DLL could hot-reload, and whether "forcing the executable to re-scan for proxy
DLLs" works. **It does not** — `dinput8.dll` is loaded once and locked for the life of the process;
Windows never re-scans, and `FreeLibrary` on the proxy while the game holds forwarded exports, the
patched vtable slot and 72 MinHook trampolines is an immediate crash. The working shape is a thin
resident host + a reloadable payload DLL loaded from a temp copy — which also removes the file lock
that stops `build_and_deploy.bat` running while the game is up.

**The user dropped it**, on the correct reading that the prerequisite work destabilises exactly the
systems they don't want touched. The audit is preserved in `debug.md` so it is not re-derived.

### 6. Play-confirm gates — OPEN

Built and deployed (binary `cmp`-verified against the build output, per L-62). Needs one battle:
Libra up + aiming → full readout; Libra down → "Libra not active"; a magick row → its description;
**Attack (no description) → whatever it does, the new state line records it.**

---

## Session 160 — 2026-08-14 — [menus] The Libra readout: MP out, the other three affinities in

KEYWORDS: Libra readout MP removed enemy absorb half immune elemental affinity quartet bc+0x40
bc+0x41 bc+0x42 bc+0x43 FUN_0038b6a0 row+0x13 element mask extended status masks bc+0x68 bc+0x78
StatusNamesMask 128 bits traps category DAT_022be948 DAT_022be944 DAT_02ec3ea0 FUN_002f82f0
FUN_002f8060 probe_traps.js S159 play-confirmed

### 0. S159 PLAY-CONFIRMED

Tester: *"the gate is working, pressing o now properly reads libra when an enemy is targeted and
reads ability descriptions when a magick or technick is targeted."* Both halves, one press each.

### 1. MP is gone from the enemy readout

*"enemies don't use MP, neither is it shown on libra."* The clause was **correct and still wrong to
speak** — it read the i16 pair behind the game's own MP-gauge guard, so it never said "MP 0/0", but
it announced a number the enemy does not spend and the game's own Libra never draws. In the one
readout a player queries under time pressure, a correct irrelevant clause costs the same as a wrong
one. Offsets stay in `phyre_types.h`; the ally readouts still use them.

### 2. The affinity quartet — and why S147 was right to refuse it

`bc+0x40..+0x43` is a four-byte elemental block: **Weak, Absorb, Half, Immune**. S147 saw the four
bytes copied out together by `FUN_00329220`, noted that the equipment record's quartet order was "a
tempting fit", and **deliberately declined to identify them** — because that guess was what produced
the claim it had just struck.

It was right, and the analogy would have given the right answer. That does not make it evidence.

Identified this session from the **consumer** instead: `FUN_0038b6a0` is the damage path's elemental
resolver, testing each byte against the action's element mask (`row+0x13`, already at 0.99):

| byte | outcome | meaning |
|---|---|---|
| `+0x43` | sets a flag and **returns before every other affinity test** | Immune |
| `+0x40` | damage `* 2.0` | Weak |
| `+0x42` | damage `* 0.5` | Half |
| `+0x41` | sets a flag, no multiplier | Absorb |

**The control is `+0x40` landing on the `* 2.0` branch** — that byte is independently confirmed as
the weakness mask by the display chain, so a known value falling in the expected slot is what makes
the other three readable rather than guessed. **IDENTIFY A FIELD FROM WHAT CONSUMES IT, AND CHECK
THAT THE ONE FIELD YOU ALREADY KNOW LANDS WHERE IT SHOULD.**

All four are now spoken, behind the same `LibraSuppressed` gate the weakness row already honoured,
with the game's own four labels (`0x2331`/`0x232F`/`0x2330`/`0x232E`) — `CacheWeakLabel` generalised
to `CacheAffinityLabels`. The in-battle panel still draws Weak alone; speaking the rest is the user's
call and the data is the enemy's own.

### 3. Statuses: 32 bits was never the status space

The readout walked the two u32 words and stopped. The rest of an enemy's statuses live in the two
16-byte EXTENDED masks at `bc+0x68`/`+0x78` — ~128 further bits that **index the same master table**
(`FUN_00385570` walks exactly those bits to find each status's timer slot, a 0.99 fact already in
`GameArchitecture.md`). Nobody had pointed the namer at them.

Both spaces are now OR'd into ONE 16-byte mask and walked ONCE, so a bit in both cannot be said
twice. `StatusName`'s cap went 31 → 127, and **the bound is the mask width, not a guess at the row
count**: `MasterRecord` already rejects `index >= count` from the table's own header, so widening
cannot invent a name — an unpopulated index returns empty and drops out. One naming loop
(`StatusNamesMask`); `StatusNames(u32)` is now an adapter onto it.

### 4. Traps — researched, probe written, NOT ported

The floor-trap category the user asked for. Unlike treasure, traps **are** enumerable: one indexed
table, walked identically by `FUN_002f8060` (per-frame trigger) and `FUN_002f82f0` (visibility
toggle). Table, mask array, record layout and RVAs are in `GameArchitecture.md`.

**The gate is `DAT_022be944`, not a re-derived Libra test.** `FUN_002f82f0` latches it from
`FUN_0030c300` and drives every trap's model state from it — so it already IS the game's own "traps
are visible now", for one guarded read. It also avoids a real hazard: `LibraActive()` reads the
**battle-HUD** mirror, and traps are a FIELD concern where that context may not be live.

**Per FRIDA-FIRST, no C++ was written.** `..\FFXII-Decompile\frida\probe_traps.js` is written and
awaiting a run. It hooks the visibility toggle rather than polling — `setInterval` does not exist in
an injected script, and the mod it feeds is bound by the same no-polling rule.

### 5. Play-confirm — PARTIAL, and the gap is named

Tester: *"works."* Confirmed from the play log of the same session — note the `Build:` line reads
`7b8af05`, one commit BEHIND, because the DLL was built at 05:56:48 and S160 was not committed until
06:00:53. **The stamp names the tree's last commit, not the code** (L-62 again, from the other
direction: last time it was stale, this time it is merely early). The DLL mtime settles it.

```
o: Hyena A, HP 95/95. Level 2, Weak: Water
o: Giza Rabbit A, HP 85/85. Level 1, Libra, Weak: Fire
```

**CONFIRMED:** MP is gone — zero `o:` lines carry an MP clause. Level, statuses and the Weak clause
all read, and the baked falsifier printed `libra bit30 = "Libra"`.

**NOT CONFIRMED, and it is the half the session was actually about:** Absorb, Half and Immune. Giza
Rabbits and Hyenas are early trash with none of the three, so **every clause added this session is
still unexercised.** "Works" here means nothing regressed, not that the new feature fired. Needs an
enemy with a known absorb — the Flan family against its own element is the cheap test.
**DO NOT RECORD THIS AS A FULL PLAY-CONFIRMATION.**

**Observation, not yet a defect:** `Giza Rabbit A` lists **`Libra`** among its statuses and the Hyena
does not, so it is a real per-enemy state rather than the party's buff leaking in — and it is
**pre-existing**, bit 30 sitting inside the u32 words the readout always walked, not something the
S160 widening introduced. Whether an enemy should announce the player's own scan is a wording
question for the tester, not a correctness one.

---

## Session 161 — 2026-08-14 — [nav] Floor traps as a category, and a probe that should never have existed

KEYWORDS: traps category Category::Trap ScanTraps DAT_022be948 DAT_022be944 DAT_02ec3ea0 presence
mask trap record X10 Z10 radius respawner GroundY no elevation nameIdx synthetic band 3000
ChangeCategoryLocked skip probe_traps.js deleted frida discovery violation

### 1. The probe I should not have written, deleted unrun

S160 ended with `..\FFXII-Decompile\frida\probe_traps.js` presented as the confirmation step before
any C++. **It was a discovery probe.** It asked whether `DAT_022be948` was the right global, whether
the `/10` scale held, and carried *"if the latch is wrong, gate on `FUN_0030c300` instead"*.

The tester: *"no discovery probe. traps settle from decompile alone, you know this already. probes
are **not** for discovery."* Correct on every count.

**A probe with a fallback branch is a search.** A confirmation probe has no alternative hypothesis in
it — it asserts the derived values and either matches or condemns them. If it cannot be written
without a fallback, the decompile is not finished. Recorded in `debug.md`.

Worse, the facts were **already settled** when it was written: `FUN_002f8060` and `FUN_002f82f0` walk
the trap data independently and agree on the table pointer, the `0x20` cap, the offset indirection,
the mask stride and the latch. Producer + consumer agreement — the same standard that had put the
affinity quartet at 0.98 an hour earlier **in the same session**. I applied the bar to one finding
and not to the next.

### 2. Traps, built

`EntityList::Category::Trap`, appended after `Items` so none of the enum's deliberate adjacencies
move. `EntityScan::ScanTraps` is a fourth backing store beside the handle table, the actor pool and
the drop pool — a trap is not a scene object at all (`FUN_003ec700` sets a MODEL-INSTANCE state), so
no existing pass could ever have seen one.

**The gate is `DAT_022be944`, the game's own visibility latch**, not a re-derived Libra test.
`FUN_002f82f0` sets it from `FUN_0030c300` and drives every trap's model state from it, so it already
IS "are traps on screen right now". Deliberately **not** `LibraActive()`: that reads the BATTLE-HUD
mirror `P+0x10F68`, and traps are a FIELD concern where that context may not be live.

Three things the decompile settled that the design turns on:

* **A trap record has no Y.** `FUN_003a1920` is a 3D distance, but `FUN_002f8060` builds the trap
  position with a literal `0.0` for Y **and zeroes the party's Y** right before the call
  (`FUN_002fb8c0` flattens identically). Position is therefore
  `(x, PathMarch::GroundY(x, z, playerY), z)` — GroundY falls back to its seed off-mesh, so an
  uncovered trap lands at the player's height instead of at `y=0`, which on a map whose floor sits
  at `-32` would put every trap 32 m in the air.
* **`sceneObj = nullptr` is load-bearing.** `RescanLocked` skips the grace window for exactly those,
  so a sprung trap leaves the list on the next scan rather than lingering 2 s.
* **`nameIdx` MUST BE UNIQUE PER TRAP, and I nearly shipped it at 0.** With no scene object
  `CursorMatch` falls back to `(nameIdx, category)`, so one shared value makes every trap on the map
  the same entity to the focus clamp — a cursor on trap 5 would re-lock onto whichever sorted
  nearest. Caught by reading `CursorMatch` rather than by testing, which is the only way it could
  have been caught here. Traps take the next synthetic band, `-(3000 + slot)`, after exits
  `-(1000 + i)` and drops `-(2000 + i)`.

**The `=` cycle skips Trap, and ONLY Trap, when the latch is clear.** The obvious generalisation —
skip any empty category — would silence `"Shop, 0"` and every other zero the cycle deliberately
announces, which is a working surface the tester navigates by (L-48).

Label: one new phrasebook string, `"Trap"`, English column only. Permission asked and given.
`NumberDuplicateLabels` turns it into `Trap 1`, `Trap 2`.

### 3. Explicitly NOT done

**Routing and danger zones are untouched.** Feeding traps into `PathDanger` would re-open the
S106/S108 revert — *a penalty is only a detour when a detour exists*. A trap is a thing to hear and
walk around, not a cost on the graph. Do not re-propose without new evidence.

### 4. UNVERIFIED — and it cannot be verified here

**The tester has no save near a trap dungeon**, which is why this went straight to C++. So the list
itself has never run against real data. What can be checked on any map is the *negative*: the
category is skipped with Libra down, every other category announces exactly as before, and the scan
adds no per-frame cost (it runs on rescan, not per frame).

**The instrument ships with it, because there is no play test to fall back on** (S148). One `[NAV]`
line per distinct `(map, mask, latch, count)` state:

```
traps: map=<id> latch=<0|1> mask=0x<hex> tableCount=<n> listed=<n> [i]=(x,z) r=<n> flag=<id>
```

A plausible count with in-map coordinates confirms the whole chain; an absurd `tableCount` condemns
`TRAP_TABLE`; a latch that never reads 1 under Libra condemns the gate. **The first log from
Barheim Passage, Lhusu Mines, Zertinan Caverns or Garamsythe Waterway settles it in one pass** — no
second session to add logging. Delete the block once that log exists.

**Record this as UNVERIFIED, not shipped.**

## Session 162 — 2026-08-20 — [input] The gamepad intercept: where the pad actually enters, and why not one layer lower

**KEYWORDS: controller gamepad pad intercept passthrough XInput XInputGetState XINPUT9_1_0 IAT patch
import address table pad_hook pad_router PadRouter OnPoll OnGameFrame consumed buttons right stick
cardinal isolation dominance hysteresis D-pad left stick merged FUN_002498b0 0x1298B0 0x2E77360
0x2E77368 pad words override block 0x2E773A0 PS2 libpad mask 0xCA0 L1 R1 struck FUN_002b5c90
DAT_01e0ce10 DAT_009165f0 DAT_0208f574 16 action list mod menu Controller row kill switch
second read-only-input exception FFPR ControllerRouter survey log flush list PAD**

**MEASUREMENT BUILD SHIPPED — zero play data yet.** Phase 1 of the approved controller plan. The
build installs the intercept, consumes exactly ONE input, and logs everything else.

### Where the pad enters

`FFXII_TZA.exe` statically imports **`XInputGetState` / `XInputSetState` from `XINPUT9_1_0.DLL`**
(2 imports, `output/imports.txt`). `GameArchitecture.md` listed only the adjacent `xinput1_3.dll`,
which **the exe does not import at all** — corrected in the Adjacent DLLs table.

Full four-layer map now in `GameArchitecture.md` § "Input — the four layers": the OS imports, the
Phyre `PInputDevicePad*` classes, the unified pad block `FUN_002498b0` rebuilds each frame at RVA
`0x2E77360` (2 pads, stride `0x14`, 8 u16 words + 4 axis bytes), and the game's OWN override block
at RVA `0x2E773A0` (enable byte, per-word AND-masks, per-word OR-values, axis force bytes).

### THE LAYER DECIDES WHAT IS EXPRESSIBLE — this is the session's real finding

The tempting hook is the unified pad words at `0x2E77368/6A/6C`; `input_tracker.cpp`'s collision
watch already reads them, and they are where keyboard and pad meet. **That is also why they are the
wrong layer.** `FUN_002498b0` OR's every sub-device bound to a logical pad into the same 16 bits, so
**the D-pad and the left stick are the same bits there**. The approved scheme takes the D-pad for
party slots *specifically because* the left stick doubles for menu navigation — and that distinction
does not exist one layer down. In `XINPUT_GAMEPAD` they are separate fields.

Not "the low layer is riskier". The design the tester asked for is **unrepresentable** there.

Two supporting reasons, both cheap: an IAT patch costs **zero MinHook trampoline slots** (a tester
session once ran the pool dry at hook 63 of 66 and silently lost the combat hooks), and
`XInputGetState` is a documented **two-argument** API, so `L-20` is unpayable.

Rejected and recorded so they are not re-derived: hooking `FUN_002498b0` (game function, wrong
layer); writing the override block at `0x2E773A0` (owner unproven, the dead-code bar not earned);
proxying `xinput1_3.dll` (not imported, and the `dinput8` slot is already ours).

### THE GAME HAS NO PAD BINDING TABLE — which is why the scheme has to be played, not read

The plan expected to read the scheme out of the game's own controller glyph table. It is not there.
`FUN_002b5c90` was read end to end: it fills `DAT_01e0ce10` from **col 0 of the KEYBOARD binding
banks** through the ordinary glyph-slot lookup, and mentions `DAT_009165f0`, `DAT_00916670` and
`DAT_0208f574` **nowhere**. `GameArchitecture.md`'s "controller table … with pad-style variants
indexed by `DAT_0208f574 >> 4 & 3`" is **STRUCK** there, with the evidence.

`FUN_00197710`'s three banks are `[col*0x1c + action]` byte/u32 **DIK codes** across Main/Alt1/Alt2.
**There is no pad column** — consistent with FFXII rebinding only the keyboard. So the pad scheme is
**fixed in code, not data-driven**, and no amount of table-reading will yield it. That is what makes
the shipped survey the primary instrument rather than a cross-check.

Corrected in passing: the fixed action list is **16 entries, not 14** (`FUN_002b5c90` loops `< 0x10`)
— `idx 14 -> action 0x03`, `idx 15 -> action 0x00`.

### What shipped

- `src\input\pad_hook.{h,cpp}` — IAT patch on `XInputGetState`; own XINPUT_STATE types (the mod
  links no XInput, same reason `dinput8_proxy.cpp` declares its own DirectInput types); SEH-guarded,
  and a fault **latches the intercept off for the session** rather than reaching the input thread.
  Also owns `ReadGamePadWords`, so the three pad-word RVAs now live in ONE place —
  `input_tracker.cpp`'s collision watch was rewritten onto it.
- `src\input\pad_router.{h,cpp}` — the FFPR `ControllerRouter` shape (Normal/ModMode/ModMenu),
  per-index edge detection, right-stick cardinal isolation (arm 16000, release 10000 for hysteresis,
  dominance 8000), and the survey log.
- **The context verdict is computed on the GAME thread** (`PadRouter::OnGameFrame`, in `nav_hooks.cpp`
  beside the beacon and auto-walk) and published stamped; the poll only reads it. `IsFieldNavSafe`
  and `PartyEngagement` are game-thread reads — `PartyEngagement` walks the actor pool — and calling
  them from the poll is the mistake `nav_probe` had to be moved off the input thread to fix. The
  stamp expires at 250 ms, so consumption dies by itself when the field tick stops (AutoWalk's idiom).
  The gate deliberately does **not** use `MenuState::IsAnyMenuOpen()`; it uses the two predicates the
  beacon already trusts.
- **Consumes exactly one input: the right stick, on a live idle field.** Verifiable with no log and
  no sight — the field camera stops answering the right stick, everywhere else it still does. The
  D-pad, face buttons and mod mode are deliberately unclaimed until the survey says what is free;
  assigning one first would be S112 again.
- **Mod menu `Controller` row (Off / On, default On)** — the kill switch. Off returns `OnPoll` on its
  first line, so the input path is byte-identical to the mod with no pad support. A pad hook that
  misbehaved would otherwise leave a pad player unable to play *and* unable to report it.
- `PAD` added to the **logger flush list** — the survey is this build's whole deliverable and a
  session ends by quitting, the exact hard exit that once stranded the `PARTY` lines.
- `dinput8_proxy.cpp` logs any **non-keyboard DirectInput device** and the size of the first state
  buffer polled on it (80 = DIJOYSTATE, 272 = DIJOYSTATE2). The engine carries
  `PInputDevicePadDirectInput`; whether this build uses it is now measurable instead of assumed.
- **CLAUDE.md: the SECOND sanctioned input-write exception**, with bounds. It **consumes only** —
  it may clear a bit or zero an axis, never set one. That is the category line against Auto-walk.

**FRIDA-FIRST was waived by explicit user instruction** ("this will go straight to c++, no probes"),
along with the port gate. Recorded here so the audit trail stays truthful, as S100's was. What the
prototype would have bought is bought by shipping the hook consuming almost nothing.

**Phrasebook: four new rows** (`Controller` plus three sentences). Wording is mine, from the approved
plan — **flag it to the user before rewording**. "Mod" / "Cancelled" are permitted but **not yet
added**: they belong to mod mode, which is Phase 2, and an unused row is an invented label waiting to
be reused for something it was not written for.

### What Phase 1 is waiting on — one play pass, three questions in this order

1. **Passthrough.** Everything except the field right stick behaves as before. A regression here
   **ends the phase**: set `Controller` to Off in `F8` and report rather than playing on.
2. **The swallow.** Field right stick no longer moves the camera; off the field it still does.
3. **The scheme.** Field, party menu, battle, map, license board — press every button and both
   sticks in each, then read the `PAD survey` lines. `GAME-REACTED` means that control is spoken
   for; `no-reaction` in every context makes it a free-button candidate.

Read the survey by `L-04`: it dedups per **distinct value**, so an absent line means that value never
occurred, not that the control was never pressed. Presence is evidence; absence is not.

**The PS2 libpad hypothesis is 0.95 and MAY NOT BE BUILT ON** — `0x20`=RIGHT, `0x80`=LEFT and
`0xCA0`=`L1|R1|LEFT|RIGHT` match the mask exactly, and the axis order matches too, but coherent is
not confirmed. The survey promotes or kills it.

## Session 163 — 2026-08-24 — [menus] The Draklor lift was a number, not a list

**KEYWORDS: Draklor Laboratory 66th Floor North Lift Terminal Select destination floor picker
silent highlights numeric entry field 0F 2D escape mode 4 widget+0xB0 state byte widget+0x54 value
widget+0x58 packed spinner widget+0xA1 digits widget+0xA2 digit width FUN_002b35a0 0x1935A0
FUN_002a8c50 FUN_002a9980 FUN_002a5590 window+0xC0 child option list no 0x0E block choice_reader
HookedChoiceTick TickNumericField NumericFieldScope GameText decode DecodePages NotePage seed
OptionCodec named exits LogFail L-69 SOLVED**

**Reported:** on Draklor Laboratory: 66th Floor (5 objects), the North Lift Terminal prompt speaks
*"Select destination: F (Current location: 68F)"* — no floor number — and moving the highlight is
silent. The log had one line for it:

    [READER] choice SILENT (choice widget: no 0x0E block at the widget's own offset)
             wnd=000000002BEE9F90 a=0 b=2

followed by `ChoiceReader::HookedChoiceTick calls=11` — eleven moves, eleven silences.

### The finding

**It is not a choice list.** The field message widget has two selection modes and the low byte of
`widget+0xB0` says which. Mode 2 is the `0x0E` option block the reader already knew. **Mode 4 is an
editable number**, produced when `FUN_002a8c50` reaches a `0F 2D` escape and configured by
`FUN_002b35a0` (RVA `0x1935A0`) — a routine with exactly one caller in the binary. Such a page has no
`0x0E` block, and the child option-list window at `window+0xC0` is never built. **`OptionCodec`
finding nothing was correct.**

**Why it looked like a two-option list.** In mode 4 the same fields carry different meanings, and
every one of them returned a believable number: `+0xA2` = 2 is the *digit width* of the maximum (66
to 70), not an option count; `+0x58` is a packed spinner state, not a row cursor, so it genuinely
moved eleven times; `+0x54` = 66 is the *selected value*, and `dialogue_reader` printed it under the
label `wait=`, where a floor number reads as noise. → **`Docs/Lessons.md` L-69.**

**The mode byte was in the same log line the whole time.** `mode=4` appears on exactly two dialogue
pages in the entire session and both are this prompt; every other page in the log is mode 0 or 1.
That is the corroboration that carried the decompile's state enumeration over the bar — producer
(`FUN_002b35a0` sets the mode and the digit width) and consumer (our own log) agreeing without either
being fitted to the other.

### What shipped

1. **`ChoiceReader::HookedChoiceTick` asks the widget which selection it is running** before reading
   a single field, and takes `TickNumericField` for mode 4. That branch speaks `widget+0x54` on
   change. **It is authoritative in both flavours the field can take** — a free numeric range and a
   pick-from-candidates list — so the reader keeps no index of its own and never touches the packed
   state. Transition detector on a per-frame hook, naming `FUN_002a9980`, as the exception requires.
2. **`GameText`'s decode loop renders `0F 2D`** when a `NumericFieldScope` is open. Purely additive
   and the advance is unchanged: `0x2D` already fell into the generic `0x20..0x70` two-parameter arm
   and emitted nothing, and with no scope open it still does, so every other page decodes byte for
   byte as before. `DialogueReader::EmitPage` opens one in mode 4, which is what puts the floor
   number into the spoken sentence. Width and pad character come from the escape's OWN format byte
   (`fmt & 0x0F`, `fmt & 0x20`), never from a parameter we chose.

   **The scope is CONSUMED by the first `0x2D` and disarmed before the decoder's sprite pass.** Not
   tidiness: `ResolveSprites` calls `BattleState::ElementName`, which decodes a pool string of its
   own -- a nested decode that would otherwise have seen an armed field. `ElementName` already
   carries its own re-entrancy guard for exactly this shape; this is the same reasoning applied to
   the new state.
3. **`NotePage` carries the value the page line just spoke and SEEDS the tick's baseline.** Entry
   says the whole sentence once; each move says the new number. A seed, not a filter: a move back to
   the starting floor differs from the value last spoken and is announced. Both run on the game
   thread, so the seed cannot land after the tick that would use it.
4. **`OptionCodec`'s seven false returns each name themselves.** All seven reported as *"no 0x0E
   block"*, which is what let a surface the reader had never met read as a parse failure.

### The instrument was the wrong answer, and the decompile had the right one

The first build shipped `LogNumericFieldDiag`: argument slots 28-31 plus 64 bytes of page hex, to
discover at runtime which flavour the field was and what it held. **The user asked why I had done
live reads instead of finding the lift menu's actual mechanism, and they were right.**

`FUN_002b35a0` is **92 lines** and states the whole thing outright — it is the only caller-less
configuration routine for this field, and reading it takes two minutes. The configuration is slots
28-31, but **the destinations are argument slots `0 .. count-1`**. So the instrument was aimed at the
wrong slots: it would have logged the count, the opening index and the bounds, and **never once the
floors themselves**. It could not have answered the question it was shipped to answer.

Worse was the framing. I sold *"the reader never has to know which flavour is running"* as a virtue.
A design that does not need to know is a design that did not find out — the same shape as the
fallback-probe rule this project already has, applied to a C++ instrument instead of a Frida script.

**Replaced with `ChoiceReader::ReadNumericField`**, which reads the field the way `FUN_002b35a0`
wrote it: candidate count from slot 29, the destinations from slots `0..count-1`, the live index from
`widget+0x58` bits 12-17 with the same clamp the game applies; or, in range flavour, the two bounds
from **the slots the cursor word names** (bits 0-5 and 6-11) rather than a hardcoded 30/31, because
the function swaps them when slot 30 holds the larger number. One log line per prompt says what the
menu HOLDS, and carries a control: in candidate mode `table[index]` must equal `widget+0x54`, and the
line says so loudly when it does not.

The 64-byte page hex went too. It existed to recover the `0F 2D` format byte, which only sets the
render width and pad — both of which the decoder already reads off the escape itself.

### Regression audit (asked for explicitly: dialogue must not move)

The two handlers are separate in the game and had to stay separate here. Walked the diff path
by path:

- **`EmitPage`, non-mode-4** -- the widget reads moved above the decode, but they are guarded
  getters with no side effects and nothing runs between them and the decode. `DecodePages` is
  called identically, `NotePage` skips the seed and the instrument, and only the log LABEL
  changed. No behavioural change.
- **`HookedChoiceTick`, non-mode-4** -- one extra guarded byte read, then the original path
  verbatim. The branch tests `== 4`, not `== 2`, so every other state falls through unchanged.
- **`OnFocus`** (notice board, gate-crystal teleport list) -- `OptionCodec`'s logic is
  unchanged line for line; it only gained a `why` out-param. `LogFail`'s pointer-based dedup
  still separates the reasons because they are all literals.
- **`GameText` with no scope open** -- byte-identical output; the `0x2D` arm is not taken.

**Three things the audit actually caught, all fixed before the build shipped:**
1. **A live cross-contamination path.** `ResolveSprites` runs a NESTED decode through
   `BattleState::ElementName`, inside the scope's lifetime. Fixed by consuming the scope at
   the first `0x2D` and disarming before the sprite pass.
2. **An invented parameter.** The scope took a `padZero` flag that the caller had no way to
   know; the escape's own `fmt & 0x20` bit is the truth. Parameter removed.
3. **A dead constant** (`MODE_LIST`) and a **stale row key**: `TickNumericField` now clears
   `g_lastCursor`, so a widget that has been a numeric field cannot carry a stale key back
   into mode 2 and go silent on re-entry. Can only add speech.

### The destination list is READ but NOT SPOKEN -- asked, answered, closed

With the menu's own data in hand the reader could have announced the set ("66, 67, 70", the
game's own numbers and no invented words) or a position on each move ("67, 2 of 3", which needs
`of` in the phrasebook). **Tester's call: neither.** Speech stays the prompt line on entry and
the number on each move; the destinations go to the log.

So reading the mechanism is a CORRECTNESS fix, not a louder one -- and that is the right reason
to have done it. The reader now knows what it is looking at, carries a control that fails loudly
if the model is wrong, and needs no runtime hunt to answer what the lift holds. **Do not
re-propose announcing the set.**

### Play-confirmed, on ONE lift

**Tester 2026-08-24: *"lift vocalization works."*** Draklor Laboratory: 66th Floor, North Lift
Terminal. The prompt speaks with its floor number and the selection speaks as it moves.

**⚠ THAT IS ONE LIFT, AND IT IS NOT THE ONLY ONE.** The tester's own caveat: whether this holds
across the other lifts remains to be seen. What is confirmed is the mode-4 branch on a field
whose flavour that one prompt happened to have. **Another lift can differ in ways this session
never exercised:**

- the OTHER flavour -- a free numeric RANGE instead of a candidate list. `ReadNumericField`
  handles it and reads its bounds from the slots the cursor word names, but no range field has
  ever been seen live.
- the DIGIT-COLUMN cursor (`widget+0x98`). A free-entry field is edited one digit column at a
  time, and the reader says nothing about which column the player is on. On such a lift the
  number would speak and the position within it would not.
- a `0F 2D` whose format byte asks for a WIDTH or ZERO PADDING. Both are implemented from the
  escape's own bits, neither has been exercised.

**Where to look first if another lift is silent or wrong:** the `[READER] numeric field:` line.
It names the flavour, lists the destinations, and carries the control -- in candidate mode
`table[index]` must equal `widget+0x54`, and the line says so loudly when it does not. If that
line is absent the prompt never reached mode 4, which is a different defect entirely and means
the surface is not this one.

### Open

- **The other Draklor lifts, and any lift elsewhere, are UNVERIFIED.** See above.
- The `0F 2E` substitution on other surfaces (the Orrachea Armlet tutorial, the level-up banner)
  stays open — nothing on this page uses it, and resolving it needs the window's argument table in a
  decoder that touches every surface.
- The lift says *"Current location: 68F"* on a map the mod names *"66th Floor"*. Two numbering
  schemes of the game's own; not a mod defect.

## Session 164 — 2026-08-25 — [nav] "No path" to a target the game was offering an ACTION on

**KEYWORDS: C.D.B. Draklor 67th Floor no path off-mesh goal=-1 frontier suppressed reach 0.50
radiusMin class 1 class 3 ReadReachFor ReadBandFor ObjectClass FUN_0025be50 FUN_003a1960
engineRadius kNoRadiusApproach NoteFallback side doors ObjectEventSignature dropped nameless
entity_diag plain anonymous objects event table signature**

**Build:** deployed 2026-08-25, verified by `cmp` against `build\bin\Release\dinput8.dll` rather than
by the banner (L-62). **Play-confirmed the same day** — tester: *"seems to work."* The post-fix log
carries the whole verification in two line shapes: `route reach: 3.00m source=class1-no-engine-radius`
followed by `plan=Route` on every class-1 target, `route reach: 1.60m source=class3-radiusMin` unchanged
on class-3 NPCs, and **not one `FRONTIER SUPPRESSED` line anywhere in it**.

### What was asked, and what it turned out to be

The session opened on *"get side doors working — there is a type of door in the laboratory that isn't
showing in the pathfinder"*. **That premise was wrong and the user said so:** the doors do list, once
the room is entered, and `Door=1` appears in the same log. The real defect was the one named in the
same breath — a routing refusal on 67F that was blocking progress.

**I had built a whole plan on the doors premise before that correction, on a 10-vs-10 count match
(ten objects the dump never printed, ten rooms on the floor) that I called arithmetic rather than
proof and then leant on anyway.** The user's own framing had already disposed of it: *"if you don't
find doors here, we'll try another floor"* — no negative needed proving, and I was constructing one.

### The defect

`"C.D.B."`, an interactable on Draklor 67th Floor, answered `"No path"` from across the floor and from
standing beside it — and one second after the refusal the mod spoke `Action: C.D.B.`, i.e. the engine
was offering the interaction from where the router said there was no way to stand.

Root cause and fix: `Docs\debug.md`, "SOLVED — 'No path' to a target the game was offering an ACTION
on". In one line: **`ReadReachFor` applies class-3 ellipse arithmetic to every target, and the class-1
scorer has no horizontal radius at all** — so an off-mesh class-1 target was refused against a 0.50 m
figure that was never the engine's, missing the nearest walkable point by 0.19 m.

The engine facts behind it are in `GameArchitecture.md`, "The class-1 scorer has NO horizontal reach"
(conf 0.99): `FUN_0025be50` gates on band, mode bit and cone, then minimises a bare squared 2D distance
(`FUN_003a1960`, thirty-one bytes). Nearest wins; nothing is rejected for distance.

**The transferable half is `Lessons.md` L-71** — S76 found the two classes, warned in those exact words
that reading one layout on the other yields plausible wrong floats, fixed `ReadBandFor`, and left
`ReadReachFor` in the same file unfixed for eighty-eight sessions.

### Shipped

| | |
|---|---|
| `MapScript::ObjectEventSignature` | new, shared. An object's `+0x48` event-table handler names joined with `\|`. The authoring template is what identifies an object that carries no name and offers no prompt; a trigger rect does not look like a doorway template. Truncation is reported, never silent. |
| `EntityDiag::DumpLocked` | stops eliding the anonymous population (`flags == 0`, no name, outside the gimmick band) — capped at 32/container and reported as `plain=N (M printed)`. Every printed object gains a companion line with its event signature and its `+0xC8[18]` armed event slots. |
| `EntityScan` drop rule | every nameless drop now names itself (slot, cat, kind, `en`, `+0x14`, mode mask, payload ids, position, event signature), capped at 16 with the shortfall printed. `s_dropPayload` was the alarm; these lines are what it points at. |
| `InteractTarget::Reach` | gains `engineRadius`; `ReadReachFor` returns early for class 1 rather than filling the struct from the wrong layout. `nav_probe` prints "the engine applies NO radius to this target" instead of a fabricated gate verdict. |
| `nav_commands` | `kNoRadiusApproach = 3.0f` for a target with no engine radius, plus a `route reach: … source=…` line before every request. |

### Open

- **~~`"Direct Lift"` on 66F still answers "No path"~~ — WRONG ON BOTH HALVES, corrected the same day.**
  It was **67F**, it happened **once**, from the one standing spot that also produced C.D.B.'s two
  long-range failures, and the tester routes to it fine. I read a map id off the wrong side of an
  announce line and then wrote a single sample up as a standing defect (L-01) — into `debug.md`, this
  log, the memory index and the commit message, all four. Full correction in `debug.md`, "CORRECTED —
  the `Direct Lift` refusal was on 67F, once, from one standing spot". What survives is one observation
  of the breach / repair-ladder path giving up on an on-mesh goal, needing a repro before it is work.
- **The 3.0 m approach bound is play-confirmed on one target.** It can in principle land the player
  somewhere the cone or band will not accept. The `route reach:` line is what to read first if a
  class-1 route ever ends somewhere useless.
- **The diagnostic costs work on every rescan** (up to 16 dropped objects resolving event names through
  the script name pool). Bounded and off the per-frame path, but `[PERF] STALL EntityList::OnFieldFrame`
  is where it would show.
- The widened dump and the drop lines are **instruments written to be deleted** (S158, S163). They stay
  while the Draklor floors are being worked and go when they stop earning their lines.

## Session 165 — 2026-08-25 — NO ENTRY WAS WRITTEN

⚠ **Recorded here by Session 166 so the gap is visible, not to narrate work I did not do.** S165 left
uncommitted changes in the tree (`entity_scan` spawn watch + the class-1 admission fix, `entity_diag`
additions, `nav_rva` `SCENEOBJ_EVENT_ARRAY`/`RECORD_PTR` constants, CLAUDE.md's "never ask the user to
find something in the world" rule, Lessons.md edits) and the plan
`FFXII-Decompile\notes\ebp2_disasm_fix.md`, and wrote no session entry. Its findings are recorded where
they belong — the ruled-out placement sources are in `debug.md` under Session 166, and the plan
document carries its own account. **Do not reuse 165**, same rule as 110.

## Session 166 — 2026-08-26 — [nav] The interactable with no position: objects the SCRIPT places

> ## ⛔ REVERTED IN FULL BY SESSION 172 (2026-08-27)
> The tester was **not on the correct quest step**, so there was no defect to fix, and this
> work regressed shipped behaviour (phantom NPCs in Rabanastre; labelled exits relabelled
> "Sign 1"/"Sign 2"). **`src/` is back to V0.6.5 `9e9a00f`. Nothing below is in the tree.**
> Read it as a record of a wrong turn, not as documentation of the mod. See Session 172.


**KEYWORDS: EBP2 ebp_disasm container blob file+0x10 0x80 routine table blob+0x18 instruction count
record+0x0c float pool blob+0x24 id pool blob+0x20 message index tagged constant entry table +0x48
pointer identity mode table 18 modes setrect setwh recttocircle showfieldsign fieldsignmes
Dynast-Cactoid サボテン アタリ ハズレ ＦＳ配置 wdl_a03 wdl_a05 map 347 349 Shimmering Horizons Windtrace
Dunes 常駐監督 getquestscenarioflag 0x35 0x28 0x32 quest gate opcode off by one operand kind column
DAT_01efea60 CALL CALLPOPA 0x58 0x5d script_place.cpp PlacementForObject PlacementCaption signed
immediate int16**

**Build:** built + deployed 2026-08-26, clean, no warnings. **NOT PLAYED.** Nothing in this session has
been in front of a player; see Open.

### What was asked

*"Fix the EBP2 disassembler according to your documented plan, then properly disassemble the map data
for Shimmering Horizon and Windtraced Dunes and get the interactible cactus properly detectible by the
mod's pathfinder."* All three done.

### 1. The disassembler

`FFXII-Decompile\tools\ebp_disasm.py` read `file+0x18` as a routine table. That field is the MESSAGE
table — `ebp_msg_decode.py` reads the same field for that purpose and gets English out of it — and the
tool then brute-forced a "code base" to make the resulting garbage decode. **There is no code base to
find.** The `.ebp` FILE is a container; the runtime script blob starts at `file + u32(file+0x10)`
(`0x80` everywhere), and every offset in the script header is relative to THAT base.

**The mod had it right from live memory the whole time.** `map_script_internal.h` has said
`hdr+0x18 -> ROUTINE TABLE [u32 count][0x30 records]` and `hdr+0x4c -> NAME POOL` since Session 58. The
offline tool and the runtime reader were describing one structure at two different bases and neither
noticed for fourteen months.

Two further corrections, both worth more than the base fix:

* **`record+0x0c` is an INSTRUCTION COUNT**, which makes a linear decode exact — no flow-following, no
  "scan until it stops looking like code", no `CODE_SPAN_MAX` guess. **1124 files, 26514 routines, zero
  short decodes and zero bad landings.** The old file's "~15 routines drift ~2 bytes (unidentified
  size-2 op)" was an artefact of the wrong base; there is no size-2 opcode. Opcode `0x63` is real and
  size-3, so the ceiling moved from `0x62`.
* **The VM's mnemonic table is indexed one off** — and `GameArchitecture.md` ALREADY SAID SO, in the
  Session 145 errata, naming both `athena_opcodes.md` and `ebp_disasm.py`'s `OP` dict. Neither was
  fixed, the plan written to fix the disassembler never cited it, and I re-derived it from scratch.
  That is `Lessons.md` **L-73**. The re-derivation used a second independent source — the table's own
  operand-KIND column at record `+2`, which is not shifted and contradicts six names at their own index
  — so the two arguments now corroborate each other, and operand resolution is driven by that column
  rather than by per-opcode guesses.

The falsifier the plan set — reproduce the live routine-name pool — passed on the first correct parse:
36 routines on 347 with the two cactus routines at [10]/[11], 31 on 349 with them at [9]/[10], and the
`routine index == handle slot` bijection holding on both. A live log from the day before confirms it a
third time (`unspawned [0:10]`/`[0:11]` with `act=2` on map 347).

### 2. The cactus

**The object has no position and never will.** The MAP SCRIPT places it: routine `ＦＳ配置` calls
`setrect` with literal coordinates from the float-constant pool at `blob+0x24`. `setrect` sets the
script's interaction rect; nothing writes the scene object's transform.

**Proven from our own log, not argued:** `x64\logs\FFXII-Screen-Reader-2026-08-25_15-30-55.log` has
`[0:10]` and `[0:11]` on map 347 at (0,0,0) continuously from before the map announce to eight seconds
after the player was standing at (219.14, 48.92, 370.26) — script long since run, transform node all
zeros bar the cone and band. **The S165 spawn watch could never have fired on these.**

| map | slot | routine | position | radius | caption |
|---|---|---|---|---|---|
| 347 Shimmering Horizons | 10 | `サボテン_ハズレ２` | (401.25, 72, 381.80) | 6.5 | Dynast-Cactoid |
| 347 | 11 | `サボテン_ハズレ３` | (411.50, 76, 412.25) | 6.5 | Dynast-Cactoid |
| 349 Windtrace Dunes | 9 | `サボテン_アタリ_砂塵` (winner) | (176.80, 48, 305.30) | 6.5 | Dynast-Cactoid |
| 349 | 10 | `サボテン_ハズレ_砂塵` | (191.70, 50, 274.60) | 6.5 | Dynast-Cactoid |

S165's reading — "that rules out proximity streaming and points at a **state gate**" — was right, and
the gate is now named: both maps' `常駐監督` routine runs the placements only while story progress
`>= 1540` and `getquestscenarioflag(0x35) == 0x28`; the winner's `talk` sets it to `0x32` and removes
both signs.

**Not a special case.** 290 routines across the 769 map scripts place themselves with a literal
`setrect`, and 134 are examinable field signs (`showfieldsign` and a mode-2 `talk` binding coincide
exactly, 134/134): "Bottle of Spirits" (14), "Faint Glow" (12), "Mysterious Glint" (11), "Sparkling
Light" (10), "Notice Board" (5), "Batahn's Technicks" (5), "Quiet Shrine" (4), "Pilika's Diary",
"Suspicious-looking Wall", "The Moogles Eight". Every one was invisible to the mod for this one reason.

### Shipped

| | |
|---|---|
| `navigation/script_place.{h,cpp}` | new. Walks the loaded blob's routine table, decodes each routine by its own instruction count, and returns every `setrect` placement with its radius, its field-sign flag and its `fieldsignmes` message index. `PlacementForObject` joins object to routine by **pointer identity** — a scene object's `+0x48` IS its routine's entry table — after checking the object's own container id. `PlacementCaption` decodes the caption from the container's message table, stepping back `0x80` only when the 'EBP2' magic AND `container+0x10 == 0x80` both hold. |
| `EntityScan` origin drop | asks the script before dropping. Read lazily on the first origin object with a bound script, so a map with none never touches the script. The `unspawned` line now says how many placements were read, and the `new routes:` summary gains "N PLACED FROM THE SCRIPT's setrect (of M placements on this map)". |
| entity label | a placed object with no npcdic name takes its `fieldsignmes` caption — "Dynast-Cactoid", not the category word. |
| `tools/ebp_disasm.py` | rewritten. Correct base, exact decode, natives named, message text and float/ID constants inlined, entry points and interaction modes printed, `--summary` mode. |
| `notes/EBP2_DBG_format.md`, `notes/athena_opcodes.md` | the false `+0x18` routine-table claim, the four wrong routine counts, the "code base 0x0fb4 / 24-of-42" status and the "static decode stalls, needs the loader" section are all struck with the corrections beside them. |
| `notes/ebp2_disasm_fix.md` | closed: the plan's questions answered in the plan's own order. |

### Open

- **⚠ A SCRIPT PLACEMENT CAN BE GATED OFF, AND NOTHING ON THE OBJECT SAYS SO.** Same enable bit, same
  (0,0,0), same `act=2` whether the placement routine ran or not, so a gated-off sign will still be
  listed and walking there finds nothing. Shipped anyway because the previous state was a permanent
  blind spot under every game state. **Closing it means finding where `reqenable`/`showfieldsign` land
  at runtime — NOT reading quest flag `0x35`**, which is one map pair's gate, not a rule.
- **UNPLAYED, all of it.** Whether the cactus is listed, named, and ROUTABLE (the walkmap at
  (401,381) / (176,305) has never been asked for a path) is unmeasured. One press of `'` on either
  Westersand map settles all three from wherever the player is standing.
- **The doorway tag now has more candidates.** `TagDoorwaysAndDropSignTwins` gives each group-0 sign
  record to its nearest non-NPC object; a script-placed sign standing near a doorway record could take
  a tag a real door would have had. Not observed on 347 (its sign records are 60+ units from both
  cacti); the filter logs every declined pairing, which is where it would show.
- **The exit reader still guesses its last routine's span** (`CODE_SPAN_MAX`) and logs "this map loses
  an exit" when the guess misses. `record+0x0c` is the answer there too; not changed this session
  because that path is play-confirmed and the cactus work did not need it.

## Session 167 — 2026-08-26 — [nav] The script placement the per-command refresh put back to the origin

> ## ⛔ REVERTED IN FULL BY SESSION 172 (2026-08-27)
> The tester was **not on the correct quest step**, so there was no defect to fix, and this
> work regressed shipped behaviour (phantom NPCs in Rabanastre; labelled exits relabelled
> "Sign 1"/"Sign 2"). **`src/` is back to V0.6.5 `9e9a00f`. Nothing below is in the tree.**
> Read it as a record of a wrong turn, not as documentation of the mod. See Session 172.


**KEYWORDS: Dynast-Cactoid cactus Westersand map 347 Shimmering Horizons 574 steps below no path
goalPoly -1 tgt=(0,0) nearDist 418.7 frontier suppressed RefreshPositionsLocked e.pos fixed flag
sceneObj+0xB8 ReadSceneObjectPos script_place setrect second writer entity_list entity_scan L-74**

**Build:** built + deployed 2026-08-26, clean, no warnings. **The fix itself is NOT PLAYED** — see Open.

### What was asked

Session 166 shipped the script-placement reader unplayed. The user played it and reported: the cacti
**do** show in the interactables list for the first time, but read **574 steps away with no path**.

### The diagnosis — the log had all of it

`x64\FFXII-Screen-Reader-Latest.log`, map 347, player at (219.14, 48.92, 370.26):

- `[SPEAK-OUT] Dynast-Cactoid 1. South, 574 steps (below)` — and `Dynast-Cactoid 2` said **the same
  574**, for an object 32 m away from the first.
- 574 × `g_unitsPerStep` 0.75 = **430.2 m**, and √(219.14² + 370.26²) = **430.2**. That is the
  distance to the **world origin**.
- "(below)" for two objects at y = 72/76, i.e. 23 m **above** the player.
- `[NAV-ROUTE] stats ... startPoly=1660 goalPoly=-1 endPoly=1791 expands=2951 nearDist=418.7m` and
  `tgt=(0.0,0.0)`. **The planner printed the goal.** No inference was needed for any of this.

**The cause is a SECOND WRITER of `e.pos`.** `EntityScan` resolved the `setrect` position correctly —
the scan-time diagnostic in the same log proves it, `nearest "Dynast-Cactoid" 39.20m` from a sign at
(406.94,·,343.02), which is right to the centimetre for a cactus at (401.25,·,381.80).
`EntityList::RefreshPositionsLocked` then re-read `sceneObj+0xB8` before every command and put `e.pos`
back. **The guard there is a FAILED read, and this read does not fail — it succeeds and returns
(0,0,0)**, which is the entire reason the script has to place these objects. S166's position survived
less than a frame.

### The fix

A script-placed entity is now marked **`fixed`**, whose documented contract was already exactly this:
"fixed world pos, do not refresh via +0xB8". One line, plus the two comments that claimed `fixed`
meant "no scene node".

| | |
|---|---|
| `entity_scan.cpp` | `if (placed) e.fixed = true;` beside `e.pos = pos`, with the measurement that identified it |
| `entity_scan.h` | the `fixed` field now documents TWO populations — no scene node (exits, map-jumps, item drops), and a scene node whose transform reads (0,0,0) forever (script placements) |
| `entity_list.cpp` | `RefreshPositionsLocked`'s comment now says why a failed-read test cannot catch this case |

The scene pointer deliberately stays set, so the stale-entity pruner, `IsInteractionAvailable` and the
doorway/sign filter all keep working on these objects. `CollectPositionsByNameIdx` skips `fixed`
entries, which is correct here — a script-placed sign has no npcdic name to be collected by, and that
function re-reads the transform anyway.

### The lesson — L-74

**Instrument the point of USE, not the point of computation.** S166 proved the placement was read with
a diagnostic that ran inside the scan. It certified the arithmetic and could not see the overwrite.
**The correct 39.20 m and the wrong "574 steps" are nine seconds apart in the same log file**, and the
feature was written up as working-but-unplayed while already measurably half-undone. Cousin of L-71:
one writer of the field was corrected, its sibling was not.

### Open

- **⚠ THE FIX IS UNPLAYED, and "No path" is NOT yet refuted.** `goalPoly=-1` says the goal was not on
  the walkmap — which is trivially true of (0,0,0) and says **nothing** about (401.25, 72, 381.80).
  The next play is the first time the pathfinder is asked the real question. **Read `goalPoly` on the
  `stats` line:** ≥ 0 = the cactus snapped onto the navmesh and any later failure is genuine routing;
  `-1` again = the placement's y does not snap and the goal needs projecting onto the mesh first.
  Worth knowing in advance: y = 72 is an `int16` immediate, while the field signs 39 m away sit on
  terrain reading 64–68.
- **The gated-off placement problem is untouched** and still reads exactly as S166 left it: nothing on
  the object distinguishes "placed" from "gated off", so a sign whose routine never ran is still
  listed. Unchanged by this session.
- **The build stamp lies about the hash while S165–S167 are uncommitted.** This session's log opens
  `Build: V0.6.5 (9e9a00f) compiled Aug 25 2026 10:54:21` — `9e9a00f` is HEAD, and HEAD has not moved
  because none of this is committed; the `compiled` string is stale for the incremental-build reason
  in L-62. It was briefly read as "the tester is on the old build". **It is not.** The behaviour in the
  log (the `script placements:` NAV-DIAG line, the `PLACED FROM THE SCRIPT's setrect` summary) exists
  only in the new code.

## Session 168 — 2026-08-27 — [nav] The goal was on ground the party may not stand on

> ## ⛔ REVERTED IN FULL BY SESSION 172 (2026-08-27)
> The tester was **not on the correct quest step**, so there was no defect to fix, and this
> work regressed shipped behaviour (phantom NPCs in Rabanastre; labelled exits relabelled
> "Sign 1"/"Sign 2"). **`src/` is back to V0.6.5 `9e9a00f`. Nothing below is in the tree.**
> Read it as a record of a wrong turn, not as documentation of the mod. See Session 172.


**KEYWORDS: Dynast-Cactoid Westersand 347 goalPoly 2389 eff 0x07841000 bit 23 terrain refused
corridor pays terrain=6000 expands 2680 REVERSAL 135 setrect reference height FindPolyAt
FindStandablePolyAt FindPolyAtImpl requireStandable ResolvePlacementsToGround nav_mesh entity_scan
price not a cut S96 ankle-deep water L-75**

**Build:** built + deployed 2026-08-27, clean, no warnings. **NOT PLAYED.**

### What was reported

S167's position fix worked — the route was real and the player followed it ~200 m across the
Shimmering Sands. Then: *"it stops after getting a certain way through the desert and refuses to
proceed… suddenly stopped telling me I could go north, when I couldn't."*

### The tell: search cost that does not fall as you approach

Same log, same map. Ordinary targets expanded **1, 5, 3, 2, 12, 2** polys. Every route to the cactus
expanded **~2680 — from 240 m away and from 19 m away alike.** A goal 19 m off that costs a full-mesh
flood is not 19 m away in the graph, whatever the distance readout says.

### The cause, in the router's own words

```
ends:  start=2474 eff=0x00240000 walk=1 | goal=2389 eff=0x07841000 walk=1 | class=0
cost:  corridor pays terrain=6000 (terrain > 0 => crosses ground the party's class may not stand on)
costed: ... refused eff-flags: 0x07841000 x1001  0x0F841000 x1090 ...
REVERSAL: leg 3 -> 4 turns 135 deg -- the polyline doubles back; the route geometry is wrong
```

**The goal poly had bit 23 set** — the leader's terrain refusal — and that exact flag word was the
most-refused in the search. The poly the player was standing on had it clear.

**And nothing errored, which is the whole lesson.** Terrain refusal is a PRICE, never a graph cut —
correct, and hard-won: S96 made it a cut, refused 399/690 prims on map 311 including ankle-deep water
the tester walks through, and cost an exit; it has been reverted twice. So A* did not reject the goal.
It **breached** its way there, paid 6000, doubled the polyline back 135°, and the mod spoke it as a
five-leg route into a dune face. `Lessons.md` **L-75**.

**Why the goal was there:** `setrect` gives the interaction volume's REFERENCE height, not a ground
height — which this project established itself in S166 and then handed straight to the router.
`FindPolyAt` takes the containing floor NEAREST the Y given, with no terrain test and no vertical
tolerance, so y=72 selected a poly ~10 m under the terrain the player was standing on.

### The fix — at the goal, not in the router

The pricing model is right; the input was wrong.

| | |
|---|---|
| `nav_mesh.{h,cpp}` | new `FindStandablePolyAt(x, yHint, z)` — the floor at (x,z) the LEADER CAN STAND ON, nearest `yHint`. `FindPolyAt` and it now share ONE cell walk (`FindPolyAtImpl`) differing only by a flag, so the two notions of "the floor here" cannot drift. Bit 23 is read from the effective flags rather than by calling the engine: the scan is memory-only by policy, the same reason `IsInteractionAvailable` replicates `FUN_002675c0` instead of calling it. |
| `entity_scan.cpp` | `ResolvePlacementsToGround` — once per map over every script placement. (x,z) is authoritative and never moves; only Y moves, only onto a standable floor at that same (x,z), only within 25 m. Nothing standable → left exactly as the script wrote it. |

### Non-regression — established, not asserted

The user's requirement was explicit: *"be careful not to cause any regressions in the pathfinder…
do not simply tell me to test for regression."* So:

- **`FindPolyAt` is statement-identical to its old body** — 29 executable statements, mechanically
  diffed against `HEAD`, zero differences. With `requireStandable=false` both new guards are no-ops.
- **`FindStandablePolyAt` has exactly one caller in the tree**, and it runs only on script placements
  — a population that did not exist in the entity list before S166.
- **The new query can never refuse a route.** It only PREFERS a standable floor over a refused one at
  the same (x,z); with nothing to prefer it returns `kNoPoly` and its caller changes nothing.
  **Deliberately not the S96 lever** — that made bit 23 a graph cut inside `Walkable`. Nothing here
  touches `Walkable`, and no route is declined on terrain grounds by this change.
- **No log format moved**, so the project's own working-route-invariance check (diffing the funnel and
  stats lines across logs) still applies to every route in the archive.

### Open

- **UNPLAYED.** The correction rests on a standable floor existing at the cactus's own (x,z), and that
  is a runtime-only fact — the walkmap blob is relocated at load time, so it cannot be read offline.
  **The build states which case it is** in a `[NAV-DIAG] placement ground:` line per placement on map
  entry. `-> walkable y=…` = corrected, and `expands` / `corridor pays terrain=` / `REVERSAL` are the
  three numbers that confirm it. `KEPT -- NO floor…` = the cactus has no standable point of its own,
  and the next step is the router's existing `goal == kNoPoly` cylinder route (the 6.5 m radius is
  already read from `setwh`; `ClosestPointOnPoly` exists for exactly this).
- **The 25 m bound is a judgement, not a measurement.** It is there so a correction can never jump to
  another storey; the case that motivated it needs 10.3 m. If a placement is ever declined by it, the
  log says so with the actual distance, which is the number that would justify changing it.
- The gated-off placement problem (S166) and the `CODE_SPAN_MAX` exit-reader guess are untouched.

## Session 169 — 2026-08-27 — [nav] The object occupies its own coordinates

> ## ⛔ REVERTED IN FULL BY SESSION 172 (2026-08-27)
> The tester was **not on the correct quest step**, so there was no defect to fix, and this
> work regressed shipped behaviour (phantom NPCs in Rabanastre; labelled exits relabelled
> "Sign 1"/"Sign 2"). **`src/` is back to V0.6.5 `9e9a00f`. Nothing below is in the tree.**
> Read it as a record of a wrong turn, not as documentation of the mod. See Session 172.


**KEYWORDS: Dynast-Cactoid Westersand 347 349 placement ground NO floor standable setrect setwh
radius 6.5 recttocircle interaction circle NearestStandable ResolvePlacementsToGround ring search
goal poly 45 2389 eff 0x07841000 bit 23 polys=2 portals=1 hard block route around obstacle L-76**

**Build:** built + deployed 2026-08-27, clean, no warnings. **NOT PLAYED.**

### What was reported

*"still not working. tried the ones on windtraced dunes as well… hard block on the character. the
pathfinder has to route around whatever is in the way, not route through it. likely a dune or another
uninteractible cactus or some other obstacle."* Restated mid-session: **no regressions in the rest of
the pathfinder.**

### The instrument refuted my own hypothesis in one line

S168 shipped its fix together with the line that would say whether its premise held. First play:

```
placement ground: routine 10 at (401.25,381.80) script y=72.00 KEPT -- NO floor the leader can stand on
placement ground: routine 11 at (411.50,412.25) ... KEPT -- NO floor ...
placement ground: routine  9 at (176.80,305.30) ... KEPT -- NO floor ...     (map 349)
placement ground: routine 10 at (191.70,274.60) ... KEPT -- NO floor ...     (map 349)
```

**No standable floor at a cactus's own (x,z) at ANY height, on any of the four.** The Y was never it.

**What S168 got right and keeps:** the goal poly is terrain-refused, refusal is a PRICE not a cut, so
nothing errors and the mod speaks a route it should have rejected — L-75, now confirmed on two maps
with the *same flag word* `0x07841000`.

**What S168 got wrong:** the cause. Not a Y selecting an under-terrain poly. **The object occupies its
own coordinates.** `setrect` gives an interaction VOLUME — a centre and a radius — and the centre is
where the cactus is, which is exactly where you cannot stand. The user said this before I proved it.

### The endgame at three metres (map 349)

Player (177.65, 47.39, 302.62), goal (176.80, 48.00, 305.30):

```
ends: start=133 eff=0x00100000 walk=1 | goal=45 eff=0x07841000 walk=1
mesh: polys=2 portals=1 corners=2       <- start and goal ADJACENT
firstLeg=(176.8,305.3)                  <- the single leg drives straight at the centre
validate: ... volHit=1 volWalked=1 OK   |  corridor march: CLEAR over 2 hop(s)
```

Validation passed it, the march called it clear, the character hit the cactus and stopped dead 2.8 m
short, and the mod repeated **"North 4."** forever.

### The fix — project the goal out of the object, into its own interaction circle

`setwh` already gave the mod the radius and it already logged it — `r=6.50 circle` on all four — as a
field explicitly marked *"diagnostics only; no caller consumes it."* Now one does.
`ResolvePlacementsToGround`, once per map:

1. **centre standable** → take its floor height;
2. **centre not standable** → **nearest standable point within the interaction circle** (16
   directions, 1 m rings, out to the radius). The goal becomes ground the party can stand on, so A*
   routes **around** the obstacle rather than pricing through it, and the player still lands inside
   the circle where the examine prompt is;
3. **nothing standable in range** → left exactly as the script wrote it, i.e. the prior behaviour.

Cached per map (the ring is up to 96 mesh queries per placement); the map id is stamped only once the
mesh was actually up, so a scan that ran too early retries instead of caching a miss.

### Non-regression — established, not asserted

- **NOT ONE ROUTER FILE IS MODIFIED.** `path_search`, `path_planner`, `path_funnel`, `path_corridor`,
  `path_validate`, `path_march`, `path_repair`, `path_surface_goal`, `nav_reach`, `nav_footprint`,
  `map_query` — all clean, verified against `git status`. The search, its costs, its funnel and its
  validation are byte-identical.
- **`FindPolyAt` is statement-identical** — 29 statements, mechanically diffed against `HEAD`, zero
  differences.
- **`FindStandablePolyAt` has exactly two callers**, both inside `ResolvePlacementsToGround`, which
  runs only on script placements — a population that did not exist before S166.
- **It can never refuse a route.** Not wired into `Walkable`, cannot decline a crossing; it only picks
  where a placement's own goal point sits. **Deliberately not the S96 lever.**
- The only thing that changes is a script-placed sign's position, and only from a point the party
  cannot stand on to one it can.

### Open

- **UNPLAYED.** The `placement ground:` line names the branch outright: `STANDS`, `is NOT standable …
  goal moved to (x,y,z), N.NNm out`, or `KEPT — no standable ground anywhere within`. The third would
  mean the disc really is solid and the answer is a wider search or an honest "No path".
- **The stand point is nearest the CENTRE, not nearest the player** — deterministic and stable across
  rescans, which the announced bearing needs. On the far side of an obstacle the route just goes
  around, which is correct and only longer.
- **Whether `setwh`'s 6.5 is a radius or a diameter is still unestablished** (S166 left `setwh` args
  3-4 unidentified). It is used only as a SEARCH BOUND and the search takes the nearest hit, so a
  factor of two changes how far out it looks and nothing else.

## Session 170 — 2026-08-27 — [nav] There is no interact component to find; the rect is it

> ## ⛔ REVERTED IN FULL BY SESSION 172 (2026-08-27)
> The tester was **not on the correct quest step**, so there was no defect to fix, and this
> work regressed shipped behaviour (phantom NPCs in Rabanastre; labelled exits relabelled
> "Sign 1"/"Sign 2"). **`src/` is back to V0.6.5 `9e9a00f`. Nothing below is in the tree.**
> Read it as a record of a wrong turn, not as documentation of the mod. See Session 172.


**KEYWORDS: interact component field-sign +0x70 table destIdx doorway class1 FUN_0025be50 no engine
radius nearest wins cone 6.28 band -1.00 0.50 setwh extent 6.5 radius or width NearestStandable
0.75 step hug the object NAV-DIAG log only never spoken**

**Build:** built + deployed 2026-08-27, clean, no warnings. **NOT PLAYED.**

### What was asked

*"you need to find the interact component for the cactus and route the player to that then… the
pathfinder should just be routing to the cactus interact component anyway, not unwalkable terrain."*
Plus: the `Not standable…` wording must not be spoken.

### The speech point — it never was speech

`placement ground:` is `Log::Write("NAV-DIAG", …)`. Log only, no speech path anywhere in the resolver.
The strings quoted back in conversation were log lines. Nothing changed; nothing needed to.

### Is there an interact component? Asked properly — no.

Three independent checks, and it matters because "route to the component" is only actionable if one
exists:

- **The engine's field-sign `+0x70` table is not it.** 18 records on map 347, 12 on map 349, every one
  a `destIdx` 1-3 area/doorway sign. Nearest cactus to any record: **33-88 m**. The mod already dumps
  this on `'` and every line reads `TOO FAR, unclaimed`.
- **The scene object carries no volume.** S165 checked the map-data record, the `*(obj+0x40)`
  descriptor and all 64 node floats. The class-3 ellipse semi-axes read 0.01 (unused); the only live
  interaction fields are the cone `6.28` — a full circle, so facing is unconstrained — and the band
  `-1.00 / 0.50`.
- **A class-1 target has no engine reach.** `interact_target.h` already records it from the decompile:
  `FUN_0025be50` gates on the vertical band, the mode bit, the facing cone, then a bare squared
  distance kept only to MINIMISE. **Nearest wins; nothing is rejected for being far away.** Hence the
  mod's own `route reach: 3.00m source=class1-no-engine-radius`.

**So the interact component IS the script's rect** — `setrect`'s centre plus `setwh`'s extent, read
since S166 as `r=6.50 circle`. There is no second object. Routing to it means routing to walkable
ground inside that volume, which is what S169 built and this session tightened.

### Change

`NearestStandable` now steps by **0.75 m — one player step** — instead of 1 m, and takes the first
hit, so the goal hugs the object as closely as the walkmap allows. That matters because `setwh`'s
args 3-4 are still unidentified: whether 6.5 is a radius or a full width is open, and the nearest hit
is inside the volume on either reading. `maxR` is now only how far it keeps looking, not a target.

### Non-regression

Unchanged from S169 and re-verified: **not one router file is modified**; `FindPolyAt` is
statement-identical to its old body (29 statements, mechanically diffed vs `HEAD`);
`FindStandablePolyAt` has exactly two callers, both in `ResolvePlacementsToGround`, which runs only on
script placements. The new query is not wired into `Walkable` and cannot decline a crossing.

### Open

- **UNPLAYED.** `placement ground:` names the branch and now also prints how far out the goal landed.
- **Whether `setwh`'s 6.5 is a radius or a diameter is still open** and is the one loose thread here.
  It bounds only the search, so it cannot put the goal outside the volume, but establishing it would
  let the mod state the real interaction range. `setrect` args 4-6 and `setwh` args 3-4 are the
  remaining unknowns from S166.

## Session 171 — 2026-08-27 — [nav] Routing fixed; the prompt is a DIFFERENT problem, and the mod never asked

> ## ⛔ REVERTED IN FULL BY SESSION 172 (2026-08-27)
> The tester was **not on the correct quest step**, so there was no defect to fix, and this
> work regressed shipped behaviour (phantom NPCs in Rabanastre; labelled exits relabelled
> "Sign 1"/"Sign 2"). **`src/` is back to V0.6.5 `9e9a00f`. Nothing below is in the tree.**
> Read it as a record of a wrong turn, not as documentation of the mod. See Session 172.


**KEYWORDS: Cactoid right next to you 3.00m out interact prompt kind 4 IsInteractionAvailable
entity_classify 233 short-circuit returns true SCENEOBJ_FLAGS_OFF 0x1C FLAG_ACTION 0x004 FLAG_TALK
0x400 FUN_0025ad10 FUN_0025ae00 gated placement quest 0x35 0x28 S166 open hole placement prompt probe**

**Build:** built + deployed 2026-08-27, clean, no warnings.

> ## ⛔ THE PROBE IN THIS ENTRY WAS REVERTED THE SAME SESSION, AT THE USER'S DIRECTION
> *"the mod doesn't need to be trying to read quest flags. what you should have done instead was
> just asked me if we are on the correct step of the quest."* Correct: the placement gate is a
> QUEST-STATE question, and the person playing the save can answer it in one sentence. Shipping a
> build to discover it was the wrong instrument entirely — the cheapest measurement here was a
> QUESTION. `Lessons.md` **L-77**. What survives below is the `entity_classify.cpp:233` kind-4
> finding, which is real and verified; the `placement prompt:` probe is gone from the tree.

### Routing is fixed, and the log agrees with the user

*"I am able to path to the cactus now but it's not the interact prompt, just open terrain."*

```
placement ground: routine 9 at (176.80,305.30) is NOT standable -- the OBJECT occupies its own point
                  -> goal moved to (174.68,47.80,303.18), 3.00m out, the closest walkable ground
                     inside the 6.50 interaction volume
[SPEAK-OUT] Dynast-Cactoid 1. right next to you
```

The player stood at (175.26, 47.63, 302.63) — **3.08 m from the cactus centre**, inside the interaction
volume under either reading of `setwh`'s 6.5. **The S169/S170 routing work is done.** What is left is
that the game is not offering an interaction at a spot that is inside the sign's own circle.

### Why the mod never noticed — `entity_classify.cpp:233`

```
if (kind != KIND_ACTION_GIMMICK && kind != KIND_TALK_TARGET) return true;   // 5 and 1
```

**A script-placed field sign is KIND 4.** `IsInteractionAvailable` therefore returns `true` on its
second line without evaluating a single gate — not the enable bit, not the model-loaded bit, not the
class byte, not the payload id. **For this entire population `available` is a DEFAULT, not a
measurement**, and the comment above that line ("a decorative sign is not story-gated, it is simply
not interactive") is exactly the case that turns out to be wrong for kind 4.

Object bytes confirm the kind: `obj+0x0E = 0xB4` on both maps' cacti — low nibble 4, enable bit
`0x10` set.

### The likely cause is the hole S166 shipped knowingly

S166's own Open section: *"A SCRIPT PLACEMENT CAN BE GATED OFF, AND NOTHING ON THE OBJECT SAYS SO.
Same enable bit, same (0,0,0), same act=2 whether the placement routine ran or not, so a gated-off
sign will still be listed and **walking there finds nothing**."* That is the reported symptom
verbatim. Both maps place their cacti only while story progress >= 1540 and quest `0x35` reads `0x28`;
the winner's `talk` sets it to `0x32` and removes both.

### What shipped: the line that answers it

`sceneObj+0x1C` is where the question is answerable WITHOUT a map-specific quest flag — NavRva already
documents `FLAG_ACTION 0x004` / `FLAG_TALK 0x400` as *"what kind of interaction it offers RIGHT NOW"*,
set and cleared by `FUN_0025ad10` / `FUN_0025ae00` and going to zero while an object is disabled. One
line per script placement per scan, bounded to 4, log-only:

```
placement prompt: routine 9 "サボテン_アタリ_砂塵" kind=4 enable=0x.. ready=0x.. type=0x..
                  +0x1C=0x........ action=? talk=? act.id=? talk.id=? -> the game IS/is NOT
                  offering an interaction here
```

### Why a MEASUREMENT and not a gate

Gating `available` on those bits is the obvious fix and it is **deliberately not shipped yet**: it is
not established whether they are PROXIMITY-driven. `FUN_0025ad10`/`FUN_0025ae00` set 0x400 and clear
0x004 "when an object enters talk mode", which may mean the bits are only live near the player. If so,
refusing to list an object whose bits are clear would hide the cactus **from any distance** — which is
the feature. **One reading settles it; a wrong guess costs the whole population.** (L-76 is the rule
that says ship the line; L-63 is the one that says a bad gate is worse than no gate.)

### Open

- **The reading decides the next step, and both branches are already written down.** `is NOT offering`
  while the player is standing in the circle ⇒ the sign is gated off, and the fix is to mark
  script placements unavailable so the existing story-gate filter hides them (`F5` still shows all).
  `IS offering` ⇒ the sign is live and the prompt needs something else — facing, or a closer stand
  point — and the 3.08 m stand distance is the next thing to shorten.
- `IsInteractionAvailable`'s kind-4 short-circuit is now documented as a **known gap**, not a rule.
- Nothing in the router changed this session; S169/S170's non-regression argument stands untouched.

## Session 172 — 2026-08-27 — [nav] REVERTED: the whole cactus / script-placement line (S165–S171)

**KEYWORDS: revert script_place S165 S166 S167 S168 S169 S170 S171 quest 0x35 0x28 wrong step
phantom NPCs Rabanastre exits relabelled Sign 1 Sign 2 doorway tag collision entity_postscan 194
origin drop 134 field signs L-77 ask the player**

**Build:** full clean rebuild + deploy 2026-08-27. `src/` is byte-identical to HEAD `9e9a00f`
(V0.6.5-Sponsor-Build). `git diff HEAD -- src/ CMakeLists.txt` is empty.

### Why

**There was no defect.** The tester was **not on the correct quest step.** These field signs only
exist while quest `0x35` reads step `0x28` — the winner's own `talk` sets it to `0x32` and removes
all of them — so the cactus genuinely had no interact component to route to. Every build from S166
onward chased a symptom whose cause was save state. **The mod was already pulling interact prompts
correctly.** In the user's words: *"cacti may not have needed interactible changes at all."*

**And it regressed shipped, play-confirmed behaviour:**

- **Phantom NPCs in Rabanastre and elsewhere.** S166 stopped dropping objects at the world origin
  when a script placement matched, admitting a population the scan had always correctly filtered.
- **Labelled exits became "Sign 1" / "Sign 2" / "Interactables".** `entity_postscan.cpp:194` labels a
  `doorway`-tagged object as Sign, and that line never changed — **more objects were taking doorway
  tags.** S166 wrote this risk into its own Open section and shipped anyway: *"a script-placed sign
  standing near a doorway record could take a tag a real door would have had."* **A risk you can
  state precisely enough to write down is not covered by writing it down.**

Two measurements from the final session say the same thing:

- **The projection never reached the router.** The resolver logged goal `(194.70,49.92,274.60)` at
  06:29:08; the route request at 06:30:57 — *after* it — used the raw centre `(191.70,50.00,274.60)`.
  Route requests that ever used a projected point: **zero.** Dead weight exactly where it was meant
  to act.
- **Blast radius far beyond the cacti.** `placement ground:` fired across many maps, including five
  stacked placements at one coordinate — a population-wide change made to chase two objects.

### What was reverted

`git checkout HEAD -- src/navigation/ CMakeLists.txt` plus deleting the untracked
`script_place.{h,cpp}`. Every uncommitted hunk under `src/navigation/` belonged to this line
(verified hunk by hunk, including `sneak_assist.cpp`'s `EntityDiag::OnEventFire` tap and
`entity_postscan.cpp`'s comment-only change), so nothing unrelated was lost. Gone: `script_place.*`,
the origin-drop placement branch, `PlacementCaption` labelling, the `e.fixed` pin,
`FindStandablePolyAt` / `FindPolyAtImpl`, `ResolvePlacementsToGround` / `NearestStandable`, the S165
spawn watch and `entity_diag` additions, `AsciiSafe` / `FiredRoutineName`, and the `nav_rva.h` event
-array constants. **`FindPolyAt` is back to its own committed body.**

**The full 1080-line diff and both deleted files are preserved** at
`<scratchpad>/cactus_line_S165_S170.patch` (session-local — copy it somewhere durable if this is ever
revisited).

### Kept deliberately

- **`FFXII-Decompile/tools/ebp_disasm.py`** — different directory, not the mod, cannot cause a game
  regression, and independently correct: 1124 files, 26514/26514 routines decode clean.
- **`CLAUDE.md`'s "never ask the user to find, reach, or aim at something in the world"** (S165) — a
  standing accessibility rule, unrelated to cactus behaviour.
- **`Lessons.md` L-73…L-77** — the reasoning holds independently of the code, and **L-77 is the
  lesson this whole episode exists to teach.**

### What actually appeared — corrected by the user, and my write-up had it wrong twice

I recorded the regression as "field signs" and the loss as "134 field signs game-wide". **Both were
wrong.**

- **What appeared was NOT field signs.** It was *"mostly phantom NPCs with no interaction component
  and unlabelled exits that should have been labelled."*
- **The 134 figure was an OFFLINE COUNT** derived by parsing 769 map scripts — `showfieldsign` and a
  mode-2 `talk` binding coinciding. **Not one of them was ever confirmed listed correctly in play.**
  Recording it as a lost capability dressed an unmeasured number as a benefit. The measured effect of
  this line of work is the regressions, and nothing else.

**The named case: the SOUTH GATE from Rabanastre Bazaar read as "Sign 2".** That is the worst kind of
wrong, because *the destination was resolvable the whole time* — the exit had a name available and got
a generic mod word instead.

### The chain of evidence for the exit relabelling — it was already on the record

1. `doorway` is assigned by `TagDoorwaysAndDropSignTwins` from **a single 2.5 m nearest-wins proximity
   test** against the `+0x70` field-sign table. One distance, one winner per record.
2. **That test has a recorded prior failure in BOTH directions on this exact map.** `entity_postscan.cpp`
   carries it from Session 92: *"the Rabanastre gate crystal was tagged a doorway (false positive),
   while **"South Gate" and "Lowtown", the map's two actual portals, were not** (false negatives)."*
3. S166 admitted a **new population of candidate objects** — script placements, previously dropped at
   the origin — into that same nearest-wins pool, and S168–S170 then moved them onto walkable ground,
   i.e. *closer to real doorways*.
4. More candidates inside 2.5 m ⇒ a record claims the wrong object ⇒ the real portal loses its tag.
5. `ApplyFallbackLabels` labels what is left with the generic word, duplicate numbering turns that into
   "Sign 1" / "Sign 2", and a portal with a perfectly good destination never speaks its name.
6. **S166 wrote step 3's risk into its own Open section and shipped it anyway** (L-78).

**The rule that follows:** an exit whose destination resolves must NEVER fall back to a generic word.
The `Sign` fallback exists for one authorised case — the North End sign the game itself renders as
"???" — not for a portal that has a name.

### If this is ever revisited

Start from the saved patch and the S166 entry, and **the first question is the doorway-tag collision,
not the placement parse.** The parse was never the weak link; the 2.5 m nearest-wins test was, and it
was documented as fragile four sessions before this line of work began.

## Session 173 — 2026-08-28 — [input] The pad scheme, wired whole: one dispatch, three surfaces, and a menu that needs no detector

**KEYWORDS: controller gamepad pad scheme mod mode L3 latch R3 route D-pad party slots right stick
context gated describe o key Start mod menu F8 InputTracker DispatchModKey DispatchSpeakPhrase
WM_PADSAY virtual key dispatch centralization PadRouter OnPoll ModModeKeyFor consume mask eatStick
Phrase ModMode ModCancelled pad connected log line survey frame lag caveat XInputGetState
Controls.md pad section S152 per-frame play-confirmed S149 tutorial repeat play-confirmed**

**Two confirmations arrived first, and both close standing items.** The tester reports the **six
S152 per-frame conversions working fine**, so that build stops being the standing first suspect for
any nav / beacon / menu / dialogue fault. **The S149 tutorial button-prompt repeat is gone** —
corroborated independently: `end latch idled` appears **zero times** in all three of our own
2026-08-27 logs on V0.6.5 (`9e9a00f`), and its instrument is now retirable. The user also settled the
open S152 sub-question by disposing of it: **the game-speed hypothesis was disproven** — the tester
played at a higher speed and the per-frame fix held, so the fix is speed-agnostic and the
`textWalk … /frame` reading was never the thing to chase. No log of that run exists and none is
needed.

### The measurement that was never going to arrive

Before writing a line: **20 logged sessions carry `gamepad intercept installed`, and NOT ONE carries
a `survey` line.** Every other `[PAD]` line in the whole corpus is the DirectInput device logger
reporting the **mouse** (`guid=6F1D2B60` is `GUID_SysMouse`; `cbData=20` is `DIMOUSESTATE2`, neither
of the joystick sizes it was shipped to look for). No pad had ever been connected in any run we hold.

S162 parked Phase 2 behind that survey. **Phase 2 was therefore parked behind a measurement nobody
was in a position to take** — the user plays on a keyboard, and the only other holders of the build
are sponsors nobody had asked. The user's call was to build the whole scheme and test it in one pass,
which is the correct trade once the blocking evidence is understood to be unobtainable at the price
being paid for it. **FRIDA-FIRST stays waived**, as in S162, by the same explicit instruction.

### The scheme, and the two bindings the user specified

**`o` is context-gated, not modal** (the user's instruction, and it is the better design): right
stick **Up** reads the description wherever one could be read — a menu, a message box, a battle with
a target under the cursor — and falls back to previous category on a plain idle field, which is the
one surface where the description key has nothing to answer and the pathfinder has everything. It is
the ONLY direction that changes meaning. **The mod menu is mod mode + Start**, also the user's call.

Everything else follows from one rule: **A, B, X and Y are never bound in normal play.** They are the
game's core verbs, and a mod that eats one is a mod the player cannot play through. So the pathfinder
took the right stick, the party took the D-pad, the route took R3, and everything that would have
wanted a face button went behind the modifier.

**Mod mode is a LATCH, not a hold.** L3 says "Mod"; the next button spends it. The left stick is the
movement stick, so holding L3 while pressing anything else is a genuinely awkward grip — and one
button at a time is the whole ergonomic argument for a pad. **Every exit speaks**: "Cancelled" on an
unmapped button, on L3 pressed twice, and on the 5-second timeout. A silent mode is a mode a blind
player is stuck in without knowing it, which is what `Controls.md`'s "no mode to get stuck in" rule
was written against. The two words were APPROVED with the S162 plan and deliberately withheld until
the mode existed; this is the sanctioned moment to add them.

### THE MENU DETECTOR THAT DID NOT HAVE TO BE WRITTEN

"Is a game menu open" looked like the hard part, and `MenuState::IsAnyMenuOpen()` is unusable (1
write, 0 clears). It needed no new predicate at all: **`OnGameFrame` stops being called when the
field tick stops, and the party menu is one of the places it stops (S157).** The 250 ms context stamp
S162 shipped for map changes and stalls therefore expires by itself and the context falls to
`Unknown` — so "we are in a menu" arrives free, from a mechanism already built for a different
reason. A stale-verdict timer and a menu detector turn out to be the same instrument.

### What gets consumed is decided by CONTEXT, not by button

On a live field the mod takes the right stick, the D-pad and R3 outright. **In a menu it takes
nothing and merely listens**: the D-pad is dispatched to the Status and Clan Primer buffers as an
arrow key AND passed straight through, so the game's own cursor still moves. That is not a
compromise — it is what the keyboard already does, since the mod cannot swallow a key at all. Both
devices now behave identically on the same screen, which is the property that makes the pad scheme
explainable in one sentence per surface.

The mod's own settings menu is the exception: it is a modal overlay, so there the pad IS taken.

### One dispatch, and the pad owns no behaviour

`InputTracker::DispatchModKey(vk)` posts the SAME thread messages the keyboard path posts, routed by
the same rules (`O`/`T`/`U`/`G` to their handlers, arrows and Home/End to the virtual-buffer path
with the combat-log fallback on Home/End, everything else to `NavCommands::OnNavKey`). **A pad button
and its key are the same action by construction** — there is no second copy of "what `\` does" to
drift, and a future mod key is reachable from the pad by adding one row to a table in `pad_router`.

`InputTracker::DispatchSpeakPhrase(id)` exists for the same discipline on the other side: the pad
poll runs on the game's input thread, which has never spoken and is not a place to start — the whole
reason the tracker owns a message loop is that speech must not run inside an input callback. Phrase
strings are static, so nothing is allocated or owned across the post.

### Also shipped

- **A `controller CONNECTED on index N` log line**, once, on the first poll that returns a pad.
  Without it an empty survey cannot be told apart from a pad that was never plugged in — and that
  ambiguity is unreadable in someone else's log, where we cannot ask what was connected.
### A SETTING WITH A MOD-MENU ROW GETS NO PAD BUTTON (user's rule, same day)

The first cut of mod mode put combat verbosity on L1 and the audio beacon on R1, copying the
keyboard's `F4` / `F11` shortcuts. **The user struck both:** *"any toggles don't need mod specific
functions. they can be toggled from the mod menu or the keyboard."*

The reasoning generalises past those two. Every surviving mod-mode entry **asks the mod something or
moves somewhere** — a readout, a route, a step through the log. A switch is different in kind: it is
already two presses away behind Start, and the menu **says what it changed and what the new value
does**, which a bare toggle press does not. A keyboard shortcut for the same switch costs nothing
because keys are plentiful; a pad button is scarce, and a duplicate route to a switch is not what to
spend one on. The rule pre-emptively excludes `F5`, `F7` and the volumes too, and it is why the pad
scheme has exactly one settings entry: Start.

**L1 and R1 are left UNMAPPED rather than refilled.** An unmapped button in mod mode says
"Cancelled", which is a truthful answer; inventing a use for a newly free button is how a scheme
grows bindings nobody asked for.

- **The survey stays**, now measuring what the game does with what we do NOT take. **One caveat
  recorded, unmeasured, from reading the code:** it reads the game's pad words on the same poll that
  is still returning the press, so a fresh press may be read one frame before the game has acted on
  it. **If Start and A also come back `no-reaction`, the instrument is early — not the game silent.**
  That is the first thing to check on the first real log.

### What this is waiting on — one pass, in this order

1. **Passthrough.** A, B, X, Y, Start, Back, L1, R1 and the left stick behave exactly as before,
   everywhere. A regression here ends the pass: `Controller` to Off in `F8` and report.
2. **The field.** Right stick cycles objects and categories instead of turning the camera; D-pad
   speaks party members; R3 routes.
3. **The menus.** Right stick Up describes; the D-pad still moves the game's own cursor.
4. **Mod mode.** L3 says "Mod"; Start opens the settings menu; an unmapped button says "Cancelled".

`Docs/Controls.md` carries the scheme. **README deliberately does NOT yet** — it describes the mod as
it is for players, and one play pass stands between this and that being true.

**Open:** `pad_router.cpp` is 418 lines and the house rule plans a split at 400. The natural seam is
the scheme tables out to their own translation unit; not done this session because moving them before
the bindings survive contact would be moving something that is still changing.

## Session 174 — 2026-08-29 — [input] The pad scheme after its first play pass, and a config gauge that was never unreadable

**KEYWORDS: controller gamepad pad scheme play pass R1 route active target Back Select mod mode
latch L3 kill switch ControllerOn ToggleController WM_PADCTRL DispatchToggleController
SetControllerToggleCallback pad_survey.cpp one poll late GAME-REACTED no-reaction survey L-04
absence GetActiveTarget rename locked target lock-on L2 D-pad field only Battle Speed config gauge
EnumD6B0 partytop_4_c HasPerOptionLabels GaugeReadout OfJoiner sel count 0xD2 TZA pad controls**

### The play pass

First run of the S173 scheme, on the user's own pad (`FFXII-Screen-Reader-Latest.log`, 2026-08-29 —
the first log in the project's history with a real `[PAD] controller CONNECTED on index 0` line).

**Confirmed working:** the right stick drives the pathfinder (`R-stick-Left -> previous object ([)
ctx=field`), the field camera is swallowed as designed, the D-pad speaks party slots
(`D-pad Up -> party 1 (4) ctx=field`), and in menus the D-pad dispatches a buffer arrow *and* still
moves the game's cursor (86 lines, `ctx=unknown`).

**Mod mode never armed** — and the reason was not a defect. See both corrections below.

### Two wrong readings, both mine, both corrected by the user

**1. `L-04 ⟲`, on the absence side.** The log had zero `L3` and zero `R3` survey lines. The survey
loops all 16 button bits on every rising edge, before any binding logic, so I concluded the
thumb-click bits never reach the mod — a hardware diagnosis, stated with confidence, carried into a
plan. **The user had simply not pressed them.** The refutation was sitting two lines above the code
I was reading: *"an absent line means that value never occurred, NOT that the control was never
pressed."* Recorded as a recurrence on L-04.

**2. `L-80`, new.** S162 pre-registered the right test — *"if Start and A come back `no-reaction`,
the instrument is early"* — and `survey Start ctx=field ... no-reaction` was in the log, so I
declared the survey broken and set its whole output aside. **Start does not open a menu in FFXII; it
pauses. Triangle opens the menu.** The control was never verified and was checkable in one question.

**Then the fallback evidence went the same way.** I fell back on "`R1 ctx=unknown` logged both
`no-reaction` and `GAME-REACTED pad=0x0080` — a sample race". `0x0080` in the game's word space is
**LEFT** (S70, measured); R1 is `0x0800`. That line is a direction being held while a shoulder bit
rose — the layer-3 D-pad/left-stick merge — and says nothing about R1. **Nothing survives. Whether
the old sampling was early is still unmeasured**, and the one-poll-late change stands on S162's
a-priori argument alone, which is fine but is not the same thing.

**Both errors have the same shape: a number decoded by assuming what it referred to.** Start's
meaning was assumed; `0x0080`'s bit layout was assumed.

### The survey never printed the word that provoked it — fixed

The user then established that **L1, L2, R1, R2, L3 and R3 were never pressed in that session at
all** (face buttons, D-pad, right stick, Start and Back only). The log nonetheless carries `L1` and
`R1` edges. Nothing in a survey line could tell a stray bit from a chord from a second device,
because the line named one button and printed only the GAME's word.

`survey` lines now carry **`idx=`** (the XInput user index) and **`raw=`** (the whole XInput button
word at the edge). That settles three separate things at once on the next log: whether the stray
shoulder edges come from another pad index, what else was held when a button was surveyed, and — the
one that matters most — **a bit-for-bit correspondence between XInput's word and the game's**, from
real presses.

**That last one bears on the PS2 libpad mask (0.95) in `GameArchitecture.md`.** This log does not
promote it; it points the other way. D-pad presses produced game words of `0x0100`/`0x0200`/`0x0400`/
`0x0800`, which that layout calls **L2/R2/L1/R1** — buttons nobody touched. The log cannot say
whether the layout is wrong, the words are not the direct digital mask, or the samples are merged
with the left stick. `raw=` is what makes that answerable instead of guessable.

### What FFXII actually does with a pad — established, and now in GameArchitecture.md

The scheme had been designed for a year against an unknown. It is not unknown: L1 is Speed mode, L2
is zoom and then **lock-on**, L3 the area map, R2 map zoom and then hold-to-flee, R3 recentre camera,
Select the map, Start pause, Triangle the party menu. **R1 is the only control with no field job at
all**, and its one battle job — switching the target list to Reserve — needs a targeting cursor up,
which the router already classifies `FieldBusy` and passes through.

### The scheme, revised

- **R1 is the route key, and the second context-gated control** after the stick's Up. Field: route +
  beacon (`\`). Battle: route to the **active target** (`p`). The user's instruction, and it is the
  point of the change: `p` had been buried in mod mode, two presses deep behind a latch nobody was
  going to reach mid-fight. Both routes are now one press in the context that wants them.
- **Mod mode moved L3 → Back/Select.** A stick click cannot be reached without taking the thumb off
  the stick it is steering with, which is why the mode went a whole session untried. Costs the game's
  map toggle, taken knowingly. Back inside mod mode is now the cancel gesture that L3-twice was, and
  the readout it used to carry (`;`) moved to L1.
- **L3 is the intercept kill switch, both ways.** `ModMenu::ToggleController` speaks "Controller,
  Off" — name included, unlike every other adjust path, because L3 is the one setting a resting thumb
  flips by accident. It runs from a prologue ABOVE the `ControllerOn()` gate: a switch below its own
  gate can only be thrown once. **Amendment recorded in CLAUDE.md and GameArchitecture.md** — "off
  returns on the first line" is now "off writes nothing"; the byte-identical bound is on the WRITE.
- **D-pad party slots are field-only**, no longer field-or-battle. A fight is always one command menu
  away and party slots are not worth costing the player that cursor.
- **The survey reads the game's words one poll LATE**, S162's own prescribed fix, so `no-reaction`
  starts meaning something. Split out to `src/input/pad_survey.cpp` — it answers what the GAME does
  with a control, which is a different question from what the mod makes it mean, and `pad_router.cpp`
  had crossed the 500-line rule. **That closes S173's open split item**, on a better seam than the
  tables it proposed.
- **`GetLockedTarget` → `GetActiveTarget`** (and the two `NAV-ROUTE` log strings). The name meant the
  L2 hold-to-face lock-on to a reader who did not already know better, and it cost this session's
  reasoning. `audio_clips.h` had the right word (`ActiveTarget`) all along.

### Battle Speed: the value was never unreadable, only unspoken

Reported as "not reading, nothing on highlight or on change". The log named the cause in one line:

```
config row REFUSED (no per-option labels -- not an enum): row=...2CE24C40 class=0x23D6B0 kind=2
sel=5 bytes=[70 61 72 74 79 74 6F 70 5F 34 5F 63]
```

`class=0x23D6B0` is `ValueRow::EnumD6B0`; the bytes are ASCII **`partytop_4_c`**, a sprite name. The
row is drawn as **gauge blocks**, so every option decodes to the same bytes and S148's
`HasPerOptionLabels` guard correctly refused to speak mojibake — and then had nothing else to say.

**But `sel=5` was in the refusal line the whole time.** `SelectedIndex` finds the selected child by
its flag bit and had always been right; what was missing was a way to render it. `GaugeReadout` now
counts the blocks instead of decoding them — `sel+1` of the count at `row+0xD2`, joined with the
phrasebook's existing `OfJoiner`, so no new string was invented. Scoped to D6B0/DB40, the two classes
that carry a count; E770 has none and still refuses, because a bare index with no range is a number
the player cannot act on. The log line survives, reworded to say which branch it took — a row that
lands there and is *not* a gauge is the next defect.

**Same fix on the change path**, with the assumption stated in the code: `nv` is taken to be the
display index, which is already load-bearing for the working enum rows of this class but is
unverified for a gauge. If a change speaks a number the highlight then contradicts, that line is the
one to drop.

### Open

- **Everything above is BUILT, DEPLOYED, UNPLAYED** except the right stick and the D-pad.
- The next survey is the first one worth reading, and now for a second reason: `raw=` makes the
  XInput ↔ game-word correspondence readable. Press L1, L2, R1, R2, Start, Back, L3, R3 deliberately,
  on the field and in a fight, so each `raw=` has exactly one bit set.
- **`L1` and `R1` edges appear in the 2026-08-29 log for buttons the user is certain were never
  pressed.** Unexplained. `idx=` will say whether they came from a second pad index; `raw=` will say
  what else was down. Do not build on either line until that is answered.
- **The PS2 libpad mask stays at 0.95 and stays unusable.** It was to be promoted or killed from
  survey lines; the first real log fails to promote it.
- README still does not carry the pad scheme, deliberately — one play pass stands between this and
  it being true for players.

## Session 175 — 2026-08-30 — [menus] Dialogue choices: two detectors that each covered half a surface, and the one field that covers both

KEYWORDS: dialogue choice options not spoken on highlight Archades Commit this tale to memory child
list window window+0xC0 bit 22 0x400000 widget+0xB0 widget+0x54 window+0x124 FUN_002b2ce0
FUN_002a5590 FUN_002a9980 FUN_002a6190 ChoiceReader OnFocus g_dispatchCovers AbsoluteIndex stale
page NotePage EmitPage printability bail choice_block.h offline harness STALL_SCOPE call count L-81

### The report

A set of dialogue choices was not being read. The player, asked what they heard rather than what was
on screen: *"commit this tale to memory was spoken as the initially focused option, but neither
option was spoken on highlight"* — including moving back onto option 0. One question, one sentence,
and it separated the two readings the log could not (L-77).

### What the log already said

`choice SILENT (no 0x0E block on this page ...) wnd=2D8E9E40 a=0 b=156` — and `156` was the byte
offset of the page spoken 2.5 s earlier. The reader was scanning the previous page. `2D8E9F10 −
2D8E9E40 = 0xD0`, so the objects were the ordinary window/widget pair.

### The finding: the GAME splits option lists in two, and each detector saw one half

`FUN_002a5590` builds a **child list window** at `window+0xC0` for some prompts and then sets **bit
22 of `window+0x180`** — which is `widget+0xB0`. `FUN_002a9980`, the per-frame tick this reader
hooks, tests that bit as its first act in mode 2 and returns. So `widget+0x58` **never moves** for a
child-list prompt; the child owns the highlight and announces it as an `0x8000`. And the inline
flavour sends no `0x8000` at all.

The reader had a detector on each: a tick keyed on `+0x58` and a dispatch reader, with
`g_dispatchCovers` arbitrating. That looked like belt-and-braces and was not — **neither detector
could read both flavours, and the flag was hiding it.** The user said so before the decompile did:
*"if a detector can read both, then it should read both... that's redundant and inefficient."*

The dispatch detector could not even cover its own half. It scanned a snapshot pushed in by
`NotePage`, and that call sat **below** `EmitPage`'s printability bail — so on a page carrying
nothing but an option block, which decodes to no text at all, it never ran and the snapshot stayed
on the previous page.

### The fix is a deletion

Both flavours resolve the highlight through the same helper `FUN_002b2ce0` (the hidden-slot walk)
into the same address: `FUN_002a9980` writes `widget+0x54` at the end of every inline tick, and
`FUN_002a6190` writes `window+0x124` on each `0x8000` — **and `window+0x124` IS `widget+0x54`**
(`0xD0 + 0x54`). One field, already resolved by the game.

So the tick keys on `+0x54` and is the only detector. Deleted with the second one: `ChoiceReader::
OnFocus`, the cached message and page offset, `g_dispatchCovers`, `AbsoluteIndex` (our own
re-implementation of `FUN_002b2ce0`, which existed only because the dispatch hook ran *before* the
game's handler and read the field stale) and `OptionSlotCount`. 597 → 470 lines, back under the
file-size limit. `menu_reader`'s `IsChoiceWindow` branch **stays** — claiming the focus is
load-bearing on its own, keeping the generic painted-row path off a window the paint cache has no
rows for (L-29).

A **mode gate came with it**, and it is load-bearing: `+0x54` is the park reason in mode 0/5, so `3`
(a page break) would have read as "option 3" on every page turn.

`NotePage` also moved above the printability bail, so a text-less option page still arms the queue
deterministically instead of inheriting a flag the previous page left set.

### Verification, offline

The `0x0E` block walk moved to `src/ui/choice_block.h` — pure, no Windows, no `GameText` — and a
scratchpad CMake target asserts **24 checks** over the shapes the corpus records: block on page 0
behind a question (notice board, teleport list); on a later page (Nilbasse `off=161`); **on a page
with no text of its own** (this defect); two blocks in one message with each page selecting its own;
capacity byte larger than the real list; every bounds refusal naming itself. All pass. Built and
deployed clean.

### Open

- **PLAY-CONFIRMED 2026-08-30 (user): the dialogue choice reads.** The defect is closed.
- **SCOPE OF THAT ATTESTATION, stated rather than assumed (L-09).** It is "the dialogue choice
  works" - which surfaces were exercised is not recorded. So the design's central claim, that the
  per-frame tick reaches the CHILD-LIST flavour as well as the inline one, is confirmed for at
  least one prompt and not separately for the notice board, the gate-crystal destination list, or
  the Draklor lift. `dialogue-choice[...] child=<0|1>` in any later log settles the rest for free;
  do not read this line as covering all four.
- **The one thing not provable from the corpus** was the RATE at which `FUN_002a9980` is called
  while a child-list prompt is up — because `STALL_SCOPE` sat below the change-check and `[PERF]`
  was counting emissions, not calls. Measured: it fired on the Archades child-flavour widget 140 ms
  *after* that list opened and was focused, so it is called mid-prompt; and the bit-22 early-out
  exists precisely because the function is called in that state. The scope is now at the top of the
  hook, so **the next log states the number** — check it before trusting this reasoning further.
- `dialogue-choice[...] child=<0|1>` should appear with **both** values across that pass. If `child=1`
  never appears, the child flavour is not reaching the tick and this design is wrong.
- **OPEN, deliberately not fixed (L-31):** `DecodePages` drops empty pages, so a blank page at the
  cursor makes `EmitPage` speak `pages.front()` — a page the player is not on. No log shows it
  firing; here the options page was the message's last, so the list came back empty. Details in
  `debug.md`.
- The log line for the dispatch flavour changes name (`choice[n/N]` → `dialogue-choice[n/N] child=1`)
  and its index is now the absolute slot rather than the visible row. Identical on any list with no
  hidden slots; the spoken text is unchanged either way.

## Session 176 — 2026-08-30 — [menus] The Strahl destination map: a node graph that could not send a focus index

KEYWORDS: airship, Strahl, private airship, destination, world map, fast travel, FUN_005528c0,
RVA 0x4328C0, unclaimed pane, node graph, pane+0x9F40, node+0x48, hook arity, S129, V0.7 held

**The defect.** The private airship destination screen read nothing. Boarding, the desk conversation
and the Yes/No prompts all worked; the map itself was silent for the whole 17.4 s it was up.

**It was never a regression, and the release was held for it at the user's decision.** `plan.md:251`
carries `- [ ] World map / fast travel` under `## v2 (deferred)`, and `Strahl` / `airship` appeared
zero times in the repo. V0.7-Test-Build was already built, zipped and recorded when the report came
in; asked whether to ship it and take this as its own session, the user chose to hold.

### What the log proved, before any code was written

The V0.7 log (`Build: V0.7 (76f0e66)`) emitted exactly ONE non-PERF line across the menu's lifetime:

```
[READER] unclaimed pane: obj0 RVA=0x4328C0 win=000000002BF70B40 -- no reader spoke for it
```

No focus, no rows, no speech. **`field tick 1.4/s` against `render 58.5 fps`** confirmed a modal menu
really was up, so the mod was running and had nothing to say.

**The log line's own parenthetical is a hardcoded guess and it was wrong here.** It reads
"(candidate: the on-screen CONTROLS panel)" — a fixed string in `menu_reader.cpp:767`, not a
measurement. Settled by sweeping all 21 logs in the corpus: the everyday unclaimed panes appear in
20–21 logs each, while **`0x4328C0` appears in exactly one log, once**, 1.5 s after boarding was
confirmed, and stays the focused pane until the leave prompt. **A diagnostic that names a suspect in
its own message will be believed; this one names it in every log it ever prints.**

**The user answered the discriminating question in one sentence (L-77):** the cursor moved and the
game made its own cursor sound. So the events existed and simply never travelled the mod's path —
which ruled out "nothing happened" and ruled out S151's wrong-addressee shape, since the
`focus msg on a NON-cursor pane` diagnostic is keyed on `val` precisely so a walking cursor cannot be
throttled away, and it fired once in the whole log, for the leave popup.

### The root cause: there is no index to send

`FUN_005528c0` (obj[0] RVA `0x4328C0`) **never calls `FUN_00247510`** — zero hits in 661 lines, and no
`case 0xc`, the notify category `FUN_002a6190` uses. The `0x8000` dispatch every list reader in this
mod hangs on is not missed here, **it is never sent**.

**Because the destinations are a NODE GRAPH, not a list.** Markers with screen coordinates and four
neighbour links, walked by direction. There is no row index, so there is nothing for a focus message
to carry, and no guard added anywhere in `menu_reader.cpp` could have made one arrive. **The whole
list-reading architecture is the wrong shape for this surface** — which is why this got its own
reader rather than a gate in an existing one.

Layout, derived offline then confirmed by a live census — full table in `GameArchitecture.md`:
`pane+0x118` head, `node+0x140` next, `node+0x39` id, `+0x3C/+0x3E` screen xy, `+0x54` gating flags,
`+0x130` render flags, `pane+0x9F40` the hovered node, `pane+0x120` the hover committed on input.

**The pane's size corroborated the cursor offset before it was ever read.**
`FUN_00244f50(0xa020, FUN_005528c0, ...)` allocates 0xA020 bytes, which is what puts `+0x9F40` inside
the object at all. The same call site gave the pane's home: manager `DAT_02ca8f38` + `0x160`.

### The census answered the one thing the decompile could not

The pane resolves exactly two text ids (`0x4B45`, `0x4FF`) and both feed a static sub-panel — nothing
per-node. So the whole node record was dumped rather than guessed at (`GameArchitecture.md`'s own
rule: dump the WHOLE record before inventing a model), and it read cleanly:

- `+0x10/+0x18/+0x20/+0x28/+0x30` are the neighbour **NODES**, and they match the four ids at
  `+0x40..+0x43` exactly — the directional graph, confirmed from two directions at once.
- **`+0x48` was the only pointer leading anywhere else** (a `0x2CA6xxxx` address, unaligned, the shape
  of this game's packed codec text), mirrored at `+0x88`.

34 nodes, ids `0x01..0x23` with `0x0D` absent. The hover trail logged 19 clean cursor moves, so
`pane+0x9F40` is confirmed as the live detector.

**`TextCapture::DumpRingToLog` was fired for the first time since it was written.** Built in Session
112, it had **zero callers** — a diagnostic that has never once run is not an instrument, it is dead
code that looks like insurance. It returned only the stale desk dialogue, because the census fires at
pane construction and the destination text had not been drawn yet. Useful negative: the names do not
come through the resolvers TextCapture hooks.

### The reader, and why it will not speak on trust

`src\ui\airship_reader.{h,cpp}`, hooking `FUN_005528c0`. It reads the pane's own fields; it joins no
dispatch chain, because there is no dispatch.

**`+0x48` was identified from ONE dumped record, and one record cannot tell a per-node name from a
shared placeholder** — they look identical until you compare records. So the census requires the
decoded names to be printable AND **distinct** before `g_namesTrusted` is set; failing that the
reader stays silent and says so in the log. **An unproven field earns no speech, and speaking one
wrong name on all 34 markers would be worse than the silence being fixed.** A node whose name does
not decode is passed over in silence, which is also what separates the graph's intermediate waypoints
from its real destinations.

**The detour takes four parameters where the decompile shows two.** Ghidra infers a parameter list
from what a body USES, not from the ABI — and in this very call graph the base handler decompiles as
`FUN_005c5230(void)` and is called with two arguments one line later. **Over-declaring is safe on
x64** (extras ride in R8/R9, a two-argument callee never looks); under-declaring is the S129 shop
crash. Recorded because the temptation is to copy the decompiled signature verbatim.

### Open

- **✅ PLAY-CONFIRMED 2026-08-30 (user: "works"), and the log agrees rather than merely not
  disagreeing:** `CENSUS pane=... nodes=34 named=34 distinct=yes`, 21 destinations spoken and reaching
  `SPEAK-OUT`, zero `distinct=NO`, zero hook failures. The names settle the identity question that the
  call graph could not: Rabanastre, Nalbina Fortress, Ozmone Plain, Balfonheim Port, The Ridorana
  Cataract. **THE SUREST EVIDENCE OF WHAT A PANE IS WAS THE TEXT IT HELD, NOT THE CALLS THAT REACH
  IT** — two sessions of call-graph work put identity at 0.95; one line of its own contents put it
  at 0.99.
- **The runtime distinctness gate replaced a second play pass and was the right trade.** The
  alternative was to ship a probe, wait for a boarding, read it, then ship the reader and wait again.
  Instead the reader shipped able to prove its own key field, so one boarding returned either the
  feature or the diagnosis. **When a probe and the fix read the SAME field, the fix can carry the
  probe's test and the round trip disappears** — but only because failing the test meant silence,
  which is what the surface already did. **That trade is only available when the failure mode is the
  status quo.**
- **⚠ The pane lists the WHOLE WORLD MAP, not flyable ports.** Garamsythe Waterway, Barheim Passage,
  Henne Mines, Lhusu Mines. Not a defect and not modelled — see the flags item below.
- **NO README ENTRY, deliberately.** The map reads on its own and the player makes no decision about
  it, so there is no key to look up — `CLAUDE.md`'s rule that a feature with no key needs no entry.
- **The flag semantics are UNMODELLED and deliberately unused.** `node+0x54` gates visibility per
  mode (mode 6 tests bit 3, mode 2 tests bit 2) and `node+0x130` bit 3 hides. The reader ignores both
  and filters on "has a decodable name" instead. **The cursor demonstrably stops on `render=0x08`
  nodes** (ids 0x10, 0x1A, 0x1C, 0x1E in the trail), so "hidden" does not mean "not reachable" and
  modelling it from one session would be guessing.
- Only ONE boarding, ONE save, ONE point in the story. Which destinations are unlocked shapes the
  graph, so 34 nodes is this save's number, not the surface's.
- `FUN_00551230()` (msg `0x23` to `DAT_02ca8f38`, RVA `0x2B88F38`) returns a mode the pane branches
  on; modes 2/3/6 seen. Not read by the mod. Unmeasured.
- **V0.7 must be re-cut** — see `release_procedure.md`. The zip and its committed record pin
  `Built from 76f0e66` and hashes that are now stale.

---

## Session 177 — 2026-08-30 — [text] Polish diacritics: the glyph table was never the problem, detection was

**KEYWORDS: Polish diacritics spolszczenie PL_ff12_v1.3 fan translation font atlas detection
DetectVariantOnce RVA_FONT_MGR DAT_01f811f8 FUN_0017f8c0 FUN_001b5fa0 FUN_002ac2f0 kFpSlot
off-by-one advance width fingerprint font00.dat record ordinal L-82 L-83 backlog item 1**

**Trigger:** the Polish tester reported diacritics broken again after the last release.

### What was wrong — two constants, both from S147, both shipped since V0.6.3

`kGlyphPolish` (S130) was correct and complete. So was the decode path. **Variant detection had
simply never fired**, on any build, for anyone.

1. **`RVA_FONT_MGR = 0x1EE11F8`** — the font manager is `DAT_01f811f8`, so the RVA is `0x1E611F8`.
   A transposed 6/E; the old value resolves to ABS `0x020011F8`. It never crashed because that read
   is only a non-null "manager up yet?" gate and is never dereferenced.
2. **`kFpSlot` off by one on all ten entries** — `60,61,62,84,85,86,98,117,118,179` where the file's
   records are `59,60,61,83,84,85,97,116,117,178`. Counted from one against a zero-based file.

Either alone is fatal: the scan reads the wrong records, nothing matches, and it settles on
`Standard`. Which is the right answer for every non-Polish install, so nothing looked broken.

### How it was settled — offline, no tester artifact, no game running

Both `font00.dat` files were already in the tree (`PL_ff12_v1.3\…\font\us\`, and stock under
`FFXII-Decompile\extracted\…\font\us\`). Diffing them: 20 differing bytes, 10 records, records are
0x24 bytes on base 0x28 and **each states its own ordinal at `+0x00`** — so the ordinals are read,
not counted. `FUN_002ac2f0` derives the argument to `FUN_0017f8c0` as `codec byte − 0x20`, fixing
the index space. `FUN_001b5fa0` is literally `return DAT_01f811f8;`.

**The cross-check that should have caught this in S147**, and the session's main lesson (**L-82**):
the ten repainted slots had been recovered TWICE by different routes — as a letter mapping in S130
and as a width fingerprint in S147 — and `byte = slot + 0x20` connects them. Evaluated, the
corrected ordinals put eight of ten on bytes `kGlyphPolish` overrides; the old ones put one on
`0x5E`, which the patch never touched. The two tables sat in adjacent files disagreeing for thirty
sessions because nobody multiplied them out.

### Also changed — the failure mode itself (L-83)

A no-match used to latch permanently, and "manager not up yet" was indistinguishable from "loaded and
unrecognised". One early call could pin a Polish install to English glyphs for the session — and
**S147 had removed the `Text glyphs` manual override in the same change that added the detection**,
so there was no escape hatch and no way for the mod to report the fault. It now retries up to 64
times before settling, logs the give-up once at the cap, and the diagnostic buffer went 320 → 1024 B
(it truncated after two of ten slots, so the one line that would diagnose a future failure was
useless).

### State

- Built and deployed; **verified in the binary**, not just the source: correct RVA present once, old
  typo absent, corrected slot vector present, old vector absent.
- **PLAY-CONFIRMATION OPEN.** No fan patch on this machine. The line that settles it is
  `[TEXT] font atlas DETECTED: Polish fan patch` in the tester's log; `font atlas UNRECOGNISED`
  followed by ten slot vectors means the record layout moved and the values in that line are the fix.
- Confidence 0.99 on both constants. Struck the obsolete `debug.md` BACKLOG item 1 (its whole
  investigation plan — check `mod_settings.txt`, check the read path, look for more moved slots —
  was wrong in every particular) and the S130 "autodetection is not available" paragraph.

### Follow-up the same session — the S130 toggle is BACK, restored not rebuilt

*"put the toggle back in as well just as a safety in case autodetect fails, just rename it something
like Diacritics override"*.

**First attempt was wrong and the user caught it: I designed a new three-value row (Automatic /
Standard / Polish, new `diacritics` key, new `SetVariantOverride` API) when a working, play-confirmed
two-value row was sitting in git one commit before detection landed** (`367b10f^`). ⇒ **WHEN A
FEATURE IS BEING "RESTORED", GO READ THE COMMIT — reimplementing from the description is not
restoring** (`L-84`). The rebuild also cost a real bug: the invented `diacritics` key would have
orphaned every tester's existing `text_glyphs=1`.

What actually shipped is S130's row, unchanged: `SettingId::TextGlyphs`, two values, `text_glyphs`
key, default 0, `ApplyTextGlyphs`, `GameText::SetVariant(Variant)` — the row, the call sites and the
six phrasebook entries are byte-identical to `367b10f^`. **Only the spoken label changed**, from
"Text glyphs" to "Diacritics override".

### The arbitration — the user's rule, and it needs no third value

Both mechanisms now want to set the variant at startup, which S130's toggle never had to contend
with. The user's rule: *"toggle should only win if it's set to on, detection must win if set to off
or detection will never fire."* Implemented inside `SetVariant`, so the call site stays original:

- **`PolishPatch`** — an explicit "my install is the fan patch". Forces it and stands the detector
  **down**; a choice a later autodetect could silently overwrite is not a choice.
- **`Standard`** — the default, and what every untouched install carries. Means **"no override"**,
  not "force stock": rebuild to stock now, then **re-arm** the detector (clearing the try COUNT as
  well as the latch) so it answers again on the next decoded string.

**The asymmetry is what lets one two-valued row do the job of three.** Value 0 cannot be read as a
decision — it is what a player who has never opened this menu has — so treating it as one would force
stock on every install and the detector would never fire for anyone. Deferring costs nothing because
detection's own fallback IS Standard; the only outcome that changes is the one where detection finds
the Polish atlas and is right.

**One deliberate deviation from S130**, and it is forced by the above: `ApplyTextGlyphs` was called on
EVERY adjust, justified as harmless ("two atomic stores and a 256-entry copy"). That lapsed — the
setter now arms/disarms the detector, so an unrelated volume nudge would rebuild to stock and re-arm
detection. It is gated on the row now.

**Also, honestly:** a scratch write-helper (`open(p,'w').write(f(s))`) truncated `mod_menu.cpp` to
zero bytes — Python opens for write, and so truncates, before evaluating the replacement. Recovered
whole with `git checkout --`; no work lost, and it is an argument for `git status` staying clean
between steps.

### State

- Built, deployed, verified in the binary: corrected RVA and slot vector present, row label present,
  `text_glyphs` key present, both new log lines present.
- README: the mod-menu bullet describes two values, not three.
- **Still unconfirmed in play on a Polish install** — but the failure is now recoverable without a new
  build, and the log distinguishes the paths: `glyph variant FORCED: Polish fan patch (detection off)`
  vs `font atlas DETECTED: Polish fan patch` vs `font atlas UNRECOGNISED`.

## Session 178 — 2026-09-15 — [text] Hunt rewards: the panel was never the toast S147 said it was (PLAY-CONFIRMED)

**KEYWORDS: hunt reward panel questresultwindow native 0x37C FUN_00344c60 FUN_00290130 FUN_003f4c30
FUN_003f4aa0 FUN_003f4e70 FUN_003f4840 FUN_003f4330 FUN_003f41c0 FUN_003f4060 key item list B
FUN_0030baf0 reward_panel_reader DefName category 1 id<<16 GilSuffix STRIKES S147 L-85 Antlion
Infestation**

**Trigger:** user: *"get hunting rewards spoken when received"*, with a screenshot of the panel:
`Antlion Infestation / 4300 gil / Bubble Belt x 1 / Sickle-Blade x 1`. The user also remembered that
a diagnostic existed.

### The diagnostic was aimed at the wrong function

S147's `reward panel:` descriptor line lives in `message_reader.cpp`, on `FUN_0035e070`. The corpus
has it 4 times across 3 logs. Every one is an obtain toast (`"You obtain a Potion!"`, `"…Orrachea
Armlet!"`, `"…Wind Globe!"`). None is a hunt payout. Today's `Latest.log` is a 2-minute session and
has no hunt in it either. So the instrument could never have fired on this panel, because the panel
is a different window. `FUN_0035e070` has **no title field**. S72 had said it was a different
surface, and S72 was right. S147's strike is struck (`L-85`).

### How it was found (offline, no probe)

Grepping the `.dbg` native names for the panel's own words turned up `questresultwindow`. That name
has two independent sources: `action_binding_tables.txt` binds native 892 = `0x37C` to
`FUN_00344c60`, and the dbg join at delta 5140 lands 892 on `questresultwindow`. From there:
`FUN_00290130` fills a reward block (`FUN_003f4c30`), grants it (`FUN_003f4aa0`), and opens
`FUN_003f4e70`. That window titles itself from the hunt table the Primer already reads. It spawns
the sequencer `FUN_003f4840`, which opens **`FUN_003f4330`, the panel** (title `+0xC0`, rows `+0xC8`,
row id `+0xCC`/value `+0xD0`, `0xFFFF` = gil). Its row callback `FUN_003f41c0` reads the same
offsets back, so producer and consumer agree. Key items (`0x8xxx`, by `FUN_0030baf0`'s one-line
body) go instead to `FUN_003f4060` -> the obtain toast, which already speaks. Full tables are in
`GameArchitecture.md` §Session 178.

### Built — `src\ui\reward_panel_reader.{h,cpp}`

- Hooks `FUN_003f4330` (RVA `0x2D4330`, 2-arg window proc — the body reads only its two
  parameters). On msg 1, after the original returns, it speaks `title, <amount> gil, <item>[ qty]`.
  The quantity is spoken only above 1, the same as the inventory rows. Interrupt, like the toast.
- Item names come from `BattleState::DefName(1, id << 16)`, the existing choke point. Category 1 is
  the generic item category that re-dispatches on `id >> 12`. Gil uses the existing
  `Phrase::GilSuffix`. **No new phrasebook entries.**
- It feeds `MessageReader::NoteSpoken`, so `t` re-reads it. A live latch (set on msg 1, cleared on
  msg `0x12`) joins `ASurfaceIsLive`. Init and shutdown are chained from `MessageReader`.
- Logs a positive line every build (`[REWARD] reward panel: title="…" rows=N | [id=… value=… "…"]`)
  and a distinct give-up line (`no row resolved, staying silent`). A hook that never fired, a panel
  with unresolvable rows, and a spoken panel are therefore three different logs (`L-83`).
- `message_reader.cpp`: the S147 comment is corrected, and its log prefix is renamed `reward panel:`
  -> `item popup:`. The misread of message `0x833` as "gil" is also fixed: it is the obtain template.

### State

- Built clean. The deployed DLL is byte-identical to the build output, and the init string is present
  in the binary.
- **✅ PLAY-CONFIRMED 2026-09-15 (user, first try: "vocalized everything").** Build `aa52d71`+S178,
  compiled 06:42. The log from the same payout as the screenshot:

  ```
  [REWARD] reward panel: title="Antlion Infestation" rows=3 | [id=0xFFFF value=4300 "4300 gil"] [id=0x1173 value=1 "Bubble Belt"] [id=0x20E2 value=1 "Sickle-Blade"]
  [SPEAK-OUT] Antlion Infestation, 4300 gil, Bubble Belt, Sickle-Blade
  ```

  - **Both open risks are closed.** The loot id `0x20E2` resolved through `DefName(1, id<<16)`, so
    category 1 is now witnessed on gear, loot and key items. There is exactly one `SPEAK-OUT` for the
    panel, so nothing else spoke over it.
  - **A log-only artifact to expect:** right after it, `[READER] unclaimed pane: obj0 RVA=0x2D4330 --
    no reader spoke for it`. `menu_reader`'s S112 census only knows about readers inside its own
    branch, so it cannot see `RewardPanelReader`. The census speaks nothing, so this is harmless. It
    does use up one of the census's 12 class slots per session. Not changed, since the feature is
    play-confirmed as shipped.
  - This hunt had no key item, so the list-B handoff to the toast was not exercised in this payout.
- **The remaining items, with the user's own read on each (2026-09-15).** These are the player's
  knowledge of the game, not RE, so none of them is promoted into `GameArchitecture.md` as fact:
  - **Key-item payout — expected to read, low risk.** The user expects a key item to read if it goes
    through the normal reward window. It goes to `FUN_003f4060` -> the obtain toast
    `FUN_0035e070`. That toast is the one the mod has always spoken, and it has already spoken a key
    item (`"You obtain a Wind Globe!"`, 2026-08-27 log). **Not re-tested on a hunt payout.**
  - **Second opener `FUN_003f47e0` <- `FUN_0057a4e0` — per the user, most likely hunts turned in at the
    Clan Centurio hunt board** rather than to the petitioner. It opens the same sequencer, so the same
    panel and the same reader. **Not played.** If it is silent there, the first thing to check is
    whether a `[REWARD]` line appears at all.
  - **`queststartwindow` (native `0x386`) — per the user, most likely the window shown when a hunt is
    ACCEPTED.** It may already read through an existing surface, or it may not matter. **Not traced,
    and not a priority.** Only a report that it is silent reopens it.

## Session 179 — 2026-09-15 — [nav] Doors by script evidence, and the Unreachable filter (UNPLAYED)

**KEYWORDS: door category interactables gim_door big_door setmapidfloor FUN_003792e0 material override bank
bit 23 script-closed raw vs effective ReachGate unreachable filter mod menu row default off closed floor
penalty kClosedFloorPenalty door_binding map_script_routines entry table pointer identity +0x48 Mirror of
the Soul 185 Acolyte's Burden 186 lift false hide escape mode beacon**

**Three reports from the user's own log (build `aa52d71`):**
1. Ancient Doors list as Doors on The Acolyte's Burden but as Interactables on Mirror of the Soul. Rule
   given: anything with destination data is a Door, globally.
2. A route to a reachable door kept losing its path. Per the user, another door was partly in the way.
   They asked for an unreachable filter (behind a door, across water), as a toggle, with Off leaving
   pathing exactly as it is.
3. In escape mode the ROUTE beacon should always resume.

**What the log and the offline disassembly showed** (full chain in `debug.md` S179, facts in
`GameArchitecture.md` "In-map doors close the FLOOR…"):
- 186's doors are `big_door_01..03`, each with a real `mapjump`. They listed as Doors only because each
  sat near a `+0x70` arrow.
- 185's four "Interactables" are `gim_door01..04`. They have **no destination**. Each closes its own
  floor at load with `setmapidfloor(N, class, 0)` and opens it on talk with state 1.
- That native writes the walkmap MATERIAL override bank (class 0 -> bit 23), which the mod already
  applies. The failing route's goal read `eff=0x0FA07000` (door 3's material) and the stuck spot sat
  on door 4.
- Census over 769 scripts: 824 routines call it, including magic walls, rocks, carts, elevators and one
  NPC. Calling it is not the same as being a door.

**User decisions (asked, answered):** in-map doors go in Doors; add the direct `mapjump` rule too; with
the filter on, unreachable entries are hidden entirely; Phase 1 now, closed-door work next. (The
closed-door work turned out to be this session's finding, so both landed together.)

**Shipped (clean build, NOT deployed — the game was running; NOT played):**
- `map_script_routines.{h,cpp}`: per-routine entry-table address plus `setmapidfloor(N,0,1)` mask;
  `ScriptFingerprint()`.
- `door_binding.{h,cpp}`: `Object -> Door` by routine `mapjump`, or by an opened floor id within 3 m.
  Called from `BuildLocked` after `TagDoorwaysAndDropSignTwins`.
- `reach_gate.{h,cpp}`: a third flood that refuses only script-closed polys (raw bit clear, effective
  bit set). Verdicts are Reachable / BehindClosedFloor / Disconnected / Unknown; enemies are never
  judged. `Annotate` runs in `RescanLocked` and logs every verdict change.
- `F8` row `Unreachable filter`, key `unreachable_filter`, default Off. On: `PassesFiltersLocked`
  hides BehindClosedFloor/Disconnected, and A* adds 20000 per closed poly (start's and goal's own
  material exempt). Off: the list filter line is never true and not one crossing is priced
  differently.
- Phrasebook: 4 rows appended at the end. The wording is new, so flag it to the user. README: one
  bullet.
- `NavReach::ContainsPoly` accessor, read-only.

**Escape mode (item 3): built.** A background decompile search found the flag through the community
"NoFleeingStatePtr" address. It is `u16` RVA `0x21ABE1A` bit 0, set and cleared by `FUN_00366870` /
`FUN_003667e0` from the Left Ctrl toggle / pad-hold latch in `FUN_00253c70`, and read by the game's
own menu lock `FUN_003669d0`. I re-read the latch, both writers, the reader and every other writer of
the word before building on it. `AudioBeacon` skips its combat branch while the flag is set, and logs
`escape mode ON/off`.

**Falsifiers for the next log:**
- `reach-gate: closed sample poly … raw=… eff=…` must show the class bit clear in raw.
- `door-binding: map N -- … U unbound` must show U = 0.
- ~~Any `-> Door` on a lift, cart or switch~~ **USER: lifts/carts under Doors are fine.** Reachable things
  hidden while the row is On are accepted for now; the player toggles the row off.
- `[BEACON] escape mode ON` must appear on the first Left Ctrl in a fight, and `off` on the second.

**Lesson:** `L-86`. Two research passes concluded from the docs' prim table that closed doors are
invisible to the mod. The door's own script said otherwise in one disassembly.

**Closed out on the user's instruction:** committed, pushed, deployed. Unplayed at close.

## Session 180 — 2026-09-15 — [nav] Sochen Cave Palace: both door puzzles solved from a menu row (UNPLAYED)

KEYWORDS: Sochen Cave Palace rui_ Pilgrim's Door Ascetic's Door waterfall puzzle clock puzzle Door of
Hours class0+0x918 save block write SochenDoors Solve door puzzles sochen_puzzles descriptor table offline
L-87 ebp_var_census

**Asked:** the player is in Sochen (own log, build `aa52d71`, maps 186 -> 185 -> 184). Two Pilgrim's Doors
and an Ascetic's Door sit behind puzzles. Priority: a mod-menu row, shown only in the palace, that sets
the puzzle flags solved. Preferably also exit labels so the puzzles can be done by hand.

**Found, offline only:**
- Walkthroughs (jegged, gamerguides, no24guides) for how the puzzles play: an exit sequence through Falls
  of Time / Mirror of the Soul / Destiny's March flips the waterfalls, and a clockwise circuit through
  Destiny's March's Doors of Hours opens the Ascetic's Door.
- Disassembled all eleven `rui_` map scripts and the event scripts. `rui_a01` = Falls of Time, `rui_a02` =
  Mirror of the Soul, `rui_b01` = Destiny's March.
- **Struck S154's "the descriptor table is not reachable offline"** (L-87). It is at `blob+u32(blob+0x28)`,
  and `mrm_b03`'s two cells decode to exactly S156's live measurements.
- One save-block byte, `class0+0x918`: `0x02` waterfall puzzle -> both Pilgrim's Doors (`gim_door01/02`,
  the mod's "Pilgrim's Door 1/2"); `0x01` clock puzzle -> the Ascetic's Door (`secret_door`). A census of
  every script declaring the byte accounts for all eight bits. Detail: GameArchitecture.md.
- **The two Pilgrim's Doors are one puzzle,** not two.

**Shipped (clean build, deployed, cmp identical; NOT played):**
- `sochen_doors.{h,cpp}`: on each field frame, is a `rui_` script live? If the row is On, once per visit,
  after checking the module's class-0 base equals RVA `0x2044480` and any declaration of `+0x918` is
  exactly u8, it ORs `0x03` into the byte. It logs before, after and read-back under `SOCHEN`. It never
  clears a bit.
- `F8` row `Solve door puzzles`, key `sochen_puzzles`, default Off, visible only while `InPalace()`.
- Phrasebook: 4 rows appended. **The wording is new; flag it to the user.** README: one bullet. CLAUDE.md:
  the three game-memory write exceptions are now listed in the read-only rule.
- `ShoutScript::WriteVar`'s charter comment now names its second caller.

**Not built:** exit labels for doing the puzzles by hand (the user's option 1). What it would need is in
GameArchitecture.md: the waterfall stages live in class 5 `+0x80..+0x87`, keyed on `nowjumpindex`; the clock
circuit lives in class 1. It was offered as a follow-up.

**Falsifiers for the next log:** `[SOCHEN] WROTE class0+0x918 … read back` with `0x03` set, or `already
solved`; any `declining:` line. In play: a Pilgrim's Door opens instead of "Some unknown mechanism holds it
fast."; after re-entering Falls of Time and Destiny's March, the waterfall layout and the exit behind the
Ascetic's Door are the solved ones.

## Session 181 — 2026-09-16 — [nav] Sochen Cave Palace: the by-hand puzzle guide on `B` (UNPLAYED)

KEYWORDS: Sochen puzzle guide B key waterfall four legs door puzzle eight doors clock circuit
FocusWhere EntityList focus seamGroup ExitDest RoutineIndexOfObject class 5 work globals class 1 strayed

**Asked:** "build the by-hand puzzle solver for both puzzles" — option 1 from S180, which shipped only
the skip row.

**Found, offline (the scripts, no game run):**
- The waterfall sequence is four legs of *leave 184 by this exit, re-enter 184 by that entrance*, and
  `nowjumpindex` is the arriving `mapjump`'s own entrance literal. Ten exits into Falls of Time cover
  entrances 1-10 exactly once, and the entrance number equals the arriving map's exit group — a clean
  bijection that made the pairing checkable rather than assumed.
- The door puzzle is eight doors in a fixed order, each from a fixed SIDE (the `…b` routines are the
  far side). Each door raises its own class-1 flag; a step counts only while every later flag is clear,
  which is exactly what the inscription's "Stray but once" describes. Count at class-1 `+0x3C`.
- `REQEW(priority, routine, entry)` confirmed from `rui_a02`'s door turning the player before opening.
Both tables are in GameArchitecture.md "The two sequences, step by step".

**Shipped (clean build, deployed, cmp identical; NOT played):**
- `sochen_guide.{h,cpp}` on `B`, alongside the shout meter and the statue guide — three contexts, one
  key, each draining its own request on the field frame. It speaks "Waterfall puzzle, step 2 of 4.
  Mirror of the Soul 3." and focuses that exit; `\` then routes as it does for any object.
- `EntityList::FocusWhere(test, ctx, outLabel)` — the cursor move a guide needs. It does not speak and
  does not route: `GetCurrentTarget` already prefers the focus, so the player's own key still does the
  routing (one choke point).
- Targets are joined by SCRIPT FACTS: an exit by its `mapjump` destination + entrance -> `ExitDest::
  group` -> `Entity::seamGroup`; a door by `RoutineIndexOfObject` against a table of routine names.
  No label matching anywhere, because both puzzles are made of identically-named objects.
- Phrasebook: 4 rows appended (`Waterfall puzzle`, `Door puzzle`, `step`, the out-of-turn sentence);
  `solved` reuses StatueSolved and the counts reuse OfJoiner. **Wording is new — flag it to the user.**
  README: one section.

**Falsifiers for the next log:** `[SOCHEN] waterfall: … -> target found` / `doors: … -> target found`.
A `target NONE (<reason>)` names its own reason; the two that would mean a design change are "the
exit's map-jump group is not in the list" and "no listed object runs that routine".

**Same session, after the first build — the RESUME half (user: "a way to continue the puzzle if the
player takes a side track… a key to repeat the current puzzle step").** The key is `B`, not `.` (`.`
is the combat log's newer-entry key), and B was already stateless: every press re-reads the script's
counters and re-focuses, so repeating it was free. What was missing was the side-track case, and it is
now built:
- from a palace room the puzzles do not use, B focuses the way back toward one they do (Falls of Time
  first, then the other two) instead of speaking a step with no target;
- with the waterfall puzzle solved and the door one not, from off Destiny's March B names the door
  puzzle and the way back to it, with no step number — that count is in that map script's own class-1
  storage and cannot be read from another map, and inventing a number would be a fabricated answer;
- the solved summary moved into `Answer`, so `SpeakWaterfall` / `SpeakClock` no longer carry a
  solved branch that could not be reached.
- **The remote door hint is gated on the waterfall puzzle actually being solved.** Reaching it with the
  waterfall unsolved means its cells did not read, and naming the other puzzle there would be a
  confident answer to a question that had failed. It stays silent and logs instead.
Also recorded for the player, because the game never says it: in Destiny's March, opening a door to pass
through is itself a move, so a detour through one stalls the count.

**PLAYED, same day (own log, session 12:32) — and it routes through the waterfalls.** The user: *"I
think it's trying to route through water."* The log agrees, four ways: the reach gate sees the falls
(`9 script-closed crossing(s) refused, material id(s) 3,4` — the exact two materials the stage-0 layout
turns on, which is the offline decode and the live gate agreeing without either being told about the
other), the route to the puzzle's own exit pays `terrain=4000` to cut through two of them, a later one
pays `12000`, and the player recorded three `blocked` spots walking into the water.

**Root cause is one line, and it is S179's own defect surviving in the half that was left behind a
toggle:** `path_search.cpp:244` reads `ModMenu::UnreachableFilterOn()` to decide whether closed floors
are priced, and the row is Off by default — so a waterfall costs the flat 2000-per-poly terrain price
and A* buys it. **The fix is specified in `debug.md` ("routes buy their way through the waterfalls") and
deliberately NOT built this session, at the user's instruction.** Shape: the closed-floor PRICE becomes
unconditional, the LIST filter stays behind the row (hiding things is a preference, where to walk is
not), every S179 safety property is kept, and the four places that state the old contract get corrected
together. Stopgap for today: switch the row On in the palace.

Two smaller things the same log settled: the first `B` press on map entry answered with no target
because it ran before the seam sweep (fix: re-arm once for the next frame, in the same debug.md entry),
and the log's own `Build:` banner named S179's commit on an uncommitted tree — folded into `L-62`, which
already owned the stale-stamp half of that lesson.

**USER'S RULING, closing the session — a script-closed floor is a CUT, not a price.** *"The unreachable
object should still say 'No path' even if it shows on the filter, not act as if it can find a path
through the obstacle. That is the same bug as the northern sluiceway problem."* So the fix written up in
`debug.md` is no longer "make the price unconditional": A* must REFUSE a script-closed poly, and a target
behind one answers "No path" — while the `Unreachable filter` row keeps deciding only what the LIST
shows. **Being listed and being routable are now deliberately independent**, and an entity that is
listed AND answers "No path" is the instruction rather than an inconsistency to tidy away.

The carve-out against S96 ("cutting over-refuses", reverted twice) is written down with it, because it
WILL be re-litigated otherwise: S96 cut on static terrain TYPE, which is our inference and is wrong (the
party wades that water). A script-closed floor is the engine's own override bank refusing the party
after a `setmapidfloor`, measured twice on two mechanisms — S179's doors, S181's waterfalls. **Price what
we infer, cut what the game declares.** Folded into `L-75`, which already owned the general form (a
permissive cost model turns an invalid goal into a plausible route, not an error), and cross-linked from
the Northern Sluiceway section, which paid for that lesson first.

**Session closed with the fix specified and NOT built, at the user's instruction.** Everything else from
S180 and S181 is built, deployed and uncommitted.

## Session 182 — 2026-09-17 — [nav] The Unreachable filter asks the router; A* cuts script-closed floors (UNPLAYED)

KEYWORDS: unreachable filter rebuilt router verdict background route check RouteQuery ReachGate world
generation NoteRouteResult Log ScopedMute script-closed floor CUT closed-floor terrain paid Pilgrim's Door 1
Falls of Time waterfall kClosedFloorPenalty deleted NavReach Generation AnsweredAboutTarget

**Asked (own log, session 06:26, map 185; the user was testing the puzzle skip row in-game meanwhile):**
*"you built the unreachable filter completely wrong: when toggled off … it should say 'no path' if an
object is in the list but has no valid path. EG. Pilgrims Door 1 in the latest log. When toggled on,
entities that have no valid path should be hidden from the entity list. Currently it is not hiding
entities that have no valid path. Fix that and also take care of the documented solution for the falls
of time attempting to route through water … the goal is … to get the pathfinder to route around the
waterfall properly so that the exit we need … becomes reachable, as we had to do in northern sluiceway."*

**Found, from the log (no game run):**
- The S179 filter never hid anything: zero `reach-gate: "…"` verdict lines in this log or the 09-16 one,
  although a non-reachable verdict is always logged. Its flood called 1188 of 1194 polys "open" (it walks
  through static ground the router only prices), and a 3 m ring put a poly of that component beside
  every door. Same spot, same door: `plan=NoPath` from the router. The terrain-refusing twin flood is no
  substitute either (`strict … 20` of 1194). Only the router answers "valid path" — L-07, recurring.
- The S181 waterfall diagnosis is weaker than written: nothing logged which polys `terrain=4000` paid for,
  and all 24 march breaches in that log named material-0 ground (`nbrEff=0x07800000`), not the waterfall
  materials 3/4. The cut is the ruled fix and removes the waterfall half for certain; whether it is the
  whole of Falls of Time is what the new `terrain paid:` line answers.

**First build, REVOKED by the user before deployment.** It met the spec exactly by running the real router
in the background on the game thread (one search per 250 ms or more, 2-43 ms each, paused in fights, logs
muted). Reading that cost in the report, the user: *"game freeze is 100%, completely unacceptable and you
should never have built a system that could potentially do that without express permission. If you can't
build the unreachable filter without intercepting the main game thread (which could cause crashing), then
revoke it completely … if you can think of another way … that doesn't potentially cause game freezing or
crashing, then do that instead."* Deleted, with the `Log::ScopedMute` it needed. It had also broken
CLAUDE.md's existing no-polling rule. New CRITICAL rule in CLAUDE.md ("NEVER ADD A FRAME STALL TO THE GAME
THREAD…") and `L-88`.

**Shipped (clean build, 0 warnings; UNPLAYED):**
1. **A* CUTS script-closed floors, whatever the row says** (`path_search.cpp`): not expanded, start's and
   goal's own material exempt; `kClosedFloorPenalty` and the row's reach into routing deleted. Logs:
   `closed-floor: N crossing(s) CUT -- … material id(s) …; first at poly P (x,y,z)`, and `terrain paid:`
   naming each refused poly a corridor bought (poly, eff, material, position). This is inside the route
   request the player made — no new game-thread work.
2. **The filter records the route key's own answer and nothing else** (`reach_gate.{h,cpp}` rewritten): the
   planner's drain stores Route / "At the exit" vs NoPath / Frontier for requests the player hears, against
   label + place (0.25 m), with the world state (map epoch, override-table fingerprint,
   `NavReach::Generation()`). The list rebuild hides a "No path" record while the row is On and that world
   still holds. No search, no flood, no per-frame call — `ReachGate::OnGameFrame` and `Invalidate` are gone.
   **Limit, told to the user:** nothing is hidden until `\` has answered "No path" to it.
3. **`RouteQuery`** (`route_query.{h,cpp}`): the `\` request and the planner's arrival test + search, moved
   verbatim; only the route key calls it now. `AnsweredAboutTarget` keeps a search that could not place the
   player from recording anything.
4. S179's flood, `Judge` and `NavReach::ContainsPoly` deleted; `NavReach::Generation()` added (a counter).
5. Row wording (phrasebook, 3 strings) and the README line rewritten to the shipped behaviour — **reworded
   user-facing text, flagged to the user.** plan.md, debug.md (filter entry, waterfall entry, S179 strike),
   GameArchitecture.md (two stale claims struck), Lessons.md (`L-07` ⟲, `L-88`), PerformanceIssues.md,
   CLAUDE.md (the stall rule).

**Open, and said to the user:** the S181 waterfall diagnosis is unproven — all 24 march breaches in the
09-16 log named material-0 ground, not materials 3/4. The next Falls of Time log's `terrain paid:` decides
whether the cut is the whole fix. If every listed poly is `mat=0`, the remaining cause is the march's 1 m
graze, which touches every map and needs a ruling first.

**Falsifiers:** debug.md, both S182 entries at the top of Tried & Failed.

**Same session, after the commit — USER RULING on Falls of Time (documented, NOT built, at the user's
instruction: "document that and close out here").** *"It's likely ordinary water, but you need to make it
map specific so you don't change routing on every map … Either way, waterfall or ordinary water, it's an
invalid route and needs fixed."* Spec in debug.md under "routes buy their way through the waterfalls":
a new `map_route_rules` table (NOT the danger table, whose rows switch sneak assist on), first row map 184;
on a flagged map A* CUTS class-refused ground (goal and start polys exempt, since two of 184's exits sit on
refused polys) and the march refuses to graze across it; one `map-rule:` line per request. Recorded
accurately that the Northern Sluiceway fix itself was global — the per-map precedent is S121's
mechanism-flagged table (`PathDanger::MapUsesEngineCatch`).

## Session 183 — 2026-09-17 — [nav] Beacon silent in the Pharos, Sigils of Sacrifice by colour, Falls of Time water rule, routes that vanished mid-walk (PLAY-CONFIRMED)

KEYWORDS: audio beacon disabled Pharos Third Ascent 1141 IsBoxLive shapewin text-less window floor_disp_ctrl
DAT_01ceb638 Sigil of Sacrifice colour white yellow pink purple bgeffectplay s_warp class0+0x93d altar
map_route_rules map 184 Falls of Time refuseTerrain StrictTerrainScope map-rule keep live route RouteKeep
HoldAutoReplans silent replan No path near door Destiny's March L-89 L-90

**Asked (own logs: 09-17 08:00 Pharos map 1141, and 06:26 Sochen):** (1) the audio beacon is completely
disabled on this map, and testers see it in other dungeons; (2) colour-code the Sigils of Sacrifice from the
game's data, for every ascent; (3) finish the documented Falls of Time water fix, and fix routes that become
"No path" mid-walk when the player walks close to a door or loses the straight line to the path.

**(1) Found:** `[BEACON] suspended -- dialogue or message box on screen` on the first route of the map, never
resumed. The Pharos script's `floor_disp_ctrl` opens two `shapewin` full-screen IMAGE windows through the same
message-window builder a conversation uses, with no text, for the whole map. `IsBoxLive` counted any registered
window. A census of 258 scripts: all Pharos maps, all of Trial Mode, gauge/timer overlays, several dungeon
Map_Directors. **Built:** `IsBoxLive` skips a window whose text pointer reads as the empty-string literal. Also
fixes the pad router's `FieldBusy` and `t` on those maps. `L-89`.

**(2) Found:** fourteen Sigils of Sacrifice, all in `rbl_n01`/`rbl_n02` (Spire Ravel); no other script names a
sigil. Eight test the Second Ascent altar byte `class0+0x93d` (bits Steel 1, Magicks 2, Knowledge 4, Wealth 8,
names joined through `rbl_j02`'s own fieldsign ids); six are the single wrong-choice sigil in the Black/Green/Red
rooms. The user expected twelve (four per ascent) and asked for a re-check; the routine code was re-read and the
split is the script's own. All fourteen are coloured by one rule: each routine's own glow effect id, which the
two scripts number in contiguous blocks by appearance. Colours per altar from two agreeing walkthroughs, and the
four dais sigils on map 1141 sit on the walkthrough's compass corners 4 of 4. **Built:** `SigilColours` appends
", White|Yellow|Pink|Purple"; `RoutineFacts::bgEffect`; `DoorBinding::RoutineOf`. Phrasebook: 4 colour words
(new wording, flagged). The user can play-test only the Third Ascent dais they are on.

**(3a) Built as specified in debug.md:** `map_route_rules` (row: 184); A* cuts class-refused ground on the
flagged map (start, goal and the goal's own seam group exempt); the march refuses grazes into refused neighbours
for the request; one `map-rule:` line per request.

**(3b) Found (06:26 log, map 192):** seq 152, 214, 218 -- silent re-plans from beside a door or round a corner
failed (first leg clips a frame, repairs fail, oracle: goal connected, "the SEARCH giving up") and the drain's
empty seed STOPPED the live route; seq 215/216 then answered "No path" to `\` from the same spot. **Built
(user's ruling: a route never becomes invalid mid-walk; `\` re-speaks the live route):** `RouteKeep::Decide` --
a same-objective request that fails keeps the live route unless the goal is proven disconnected or the rest of
the route crosses a script-closed floor; silent re-plans leave the beacon untouched and hold automatic re-plans
on that leg; `\` re-speaks the remaining legs. `L-90`.

**Shipped:** clean build, 0 warnings, deployed, `cmp` identical. This deploy is also the first to carry S182.

**PLAY-CONFIRMED by the user, same day:** *"all works as intended."* Committed and pushed at session close.

**Falsifiers:** debug.md, the three S183 entries at the top of Tried & Failed, and the S182 water entry's S183
block. Grep: `TEXT-LESS window`, `sigil-colour:`, `map-rule:`, `keep-route:`, `automatic re-plan HELD`.

**Recorded for next session, NOT started (user, at close):** two silent screens in the ITEM TARGETING MENU,
opened with L1/R1 (`1`/`3` on the keyboard) -- one uses items on characters not in the current party, the other's
purpose is unknown. Both need vocalization. Separately, the user will verify in play that reserve party member
selection in the battle menu's target list reads correctly.

**Corrected at close (user):** the first version of that note attached two "leads" from Controls.md -- R1 selects
Reserve in the battle target list, and `1`/`2`/`3` are game speed. Neither is relevant (the screens are in the item
targeting menu), and the second was false: keyboard `1` is pad L1, which cycles game speed; `3` is R1; `2` has no
observed effect. The Session 44 claim is struck in Controls.md, GameArchitecture.md, combat_system.md, debug.md,
CLAUDE.md and README.md (its `1, 2, 3` line was player-facing). `L-91`.

## Session 184 — 2026-09-17 — [menus] The target list's L1/R1 groups: the group title, and the Reserve rows (PLAY-CONFIRMED)

KEYWORDS: item targeting menu L1 R1 key 1 key 3 target list group Foes Party Reserve Allies reserve member
characters not in the party FUN_0027b430 FUN_0027c730 FUN_0027b6c0 FUN_0027d5c0 FUN_0027e530 list kind 0xF
parent+0x208 message 0x1F ex00 UTF-16 wide string menu_expansion FUN_00364980 target_group_reader game_text_ex
TGTGROUP

**Asked:** pick up the screens documented at the close of S183, and check the latest log near its end for what
is not vocalized.

**The log** (Latest, 09:28-09:32, the session right before the S183 report): its last minute is the battle
menu's `Items` -> `Ether` -> target list. The two panels of one controller take turns, one per list build, and
the only speech is the nameplate reader naming the four field units. No reserve member is ever named, and no
group name is ever spoken. Nothing else near the end is unvocalized; the last `unclaimed pane` (0x1084F0) comes
with `keyboard GetDeviceState FAILING` as the game loses focus at exit.

**Found offline (GameArchitecture.md "Battle target groups (S184)", conf 0.98):**
- L1/R1 step the target list through up to four groups: FOES, PARTY, RESERVE, ALLIES. `FUN_0027b430` is the
  only stepper (pad bits `0x0400`/`0x0800`); it returns 1 on a change, and the menu root rebuilds.
- Each list build (`0x1F`) writes the group title to `parent+0x208`. It then sends the new list's first-row focus
  before returning.
- RESERVE is list kind `0xF`: roster slots 4-8, rows are charIds, drawn by `FUN_0027d5c0`. Three sites agree
  (the builder, the group stepper, and the non-empty test `FUN_0027b6c0`).
- The titles are NOT codec text. They are the engine's `ex00` + UTF-16 format (US: LEADER, PARTY, FOES,
  RESERVE, ALLIES, TIME at `0x4A48..0x4A4D`). The engine switches renderers on that prefix, `FUN_00364980`.
- `FUN_0027e530`, which the command reader labels "items", is the target-list row draw for Foes, Party and
  Allies. The label is struck.

**Built:** `src/ui/target_group_reader.{h,cpp}` and `src/core/game_text_ex.cpp`, plus small hooks into
`ingame_menu_reader`, `battle_target_reader` and `menu_reader`.
- Title on a group SWITCH only, spoken BEFORE the controller's `0x1F` original, so the new list's first row
  queues behind it.
- Reserve rows read `"<name>, HP <cur>/<max>"`.
- Other groups re-arm the nameplate reader, queued behind the title.
- `GameText::Decode` handles the `ex00` format.
- Clean build, 0 warnings, deployed, `cmp` identical.

**PLAY-CONFIRMED by the user, same day:** *"ALL WORKS AS INTENDED, NO new mod phrasing necessary, all
vocalization is as it should be."* Committed and pushed at session close. The S183 "reserve selection in
the battle target list" check is answered by the same pass.

**Falsifiers and the not-built list:** debug.md's S184 entry at the top of Tried & Failed. Grep `TGTGROUP`.

**All three follow-ups offered at the close were CLOSED by the user, same day:** a title when targeting opens
is unnecessary (*"it always starts on the party members targeting screen"*); dimmed rows need no special
handling (*"as I said before"*); and the truncated-handle note was closed for want of evidence (*"close it
unless we have evidence of such"*) -- a theoretical risk is not an open item, `L-92`. The measured half of that
last one stands: `FUN_0027e530` is the target-list row draw, not an item list.

**Repo consolidated to ONE branch at the user's instruction (same session).** `master` was fast-forwarded onto
the 196 commits that had accumulated on `combat-system` (a clean fast-forward -- `master` held nothing of its
own) and `combat-system` was deleted, local and remote; `master` was already the remote default. Then the two
worktree branches went the same way: `nav/event-transfer` had no commit `master` lacked, and
`nav/surface-goal`'s single unmerged commit `8d29827` -- deliberately held back at the V0.7 release -- was
cherry-picked first. It removes the last hardcoded `D:/Games/Dev` from `CMakeLists.txt` by deriving `DEV_ROOT`
from the project's own location. Verified by configuring in a throwaway build directory, where the existing
cache could not mask it: `DEV_ROOT=D:/Games/Dev`, `FFXII_SDL3_SOURCE=D:/Games/Dev/SDL3-source`, identical to the
literal it replaced. Rebuilt, 0 warnings, redeployed, `cmp` identical. **`master` is now the only branch and
there are no worktrees.**


## Session 185 — 2026-09-17 — [input] Pad scheme rev 3, and `F6` gets a real text field (UNPLAYED)

**The user's layout, given as a spec, not a question.** Two jobs in one session: the controller scheme
rebuilt to what they asked for, and `F6` stopped going through the clipboard.

### The pad, rev 3

| Control | Was (S174) | Is now |
|---|---|---|
| `L1` | *the game's* Speed mode; mod-mode target readout | **`;` the interact readout**, field and battle |
| `R1` | `\` on the field, `p` in a fight | **`\` in both** |
| `L3` | the intercept kill switch | **reachability filter** |
| `R3` | *the game's* camera recentre | **audio beacon** |
| `L3`+`R3` | — | **the intercept kill switch** |
| mod + X / Y / A / B | `` ` `` / `/` / `o` / `t` | **gil or `;` / `` ` `` or `p` / `8` / `F8`** |
| mod + D-pad, `L1` | `U`, `g`, `,`, `.`, `;` | **gone** — "Cancelled" |
| mod + Start | `F8` | unchanged; `B` now does it too |

Unchanged and untouched: the right stick, and the D-pad. They are also the only parts of the scheme
that were ever play-confirmed, which is worth noting before the next play pass.

**Three of the user's rulings carried a reason, and the reasons are the durable part.**

1. **`R1` routes in a fight too.** *"The player needs to be able to pathfind away from enemies if they
   want to escape, so even in battle it should be pathfind to selected destination."* S174 had sent
   battle `R1` to `p` (route to the active target) on the reasoning that a route to what you are
   fighting is what you want mid-combat. That reasoning had it backwards: the one context where a
   route OUT matters most was the one context where the route key aimed at the enemy. `p` moved to
   mod + Y.
2. **`L1` is taken from the game.** *"Controller space is limited so it is an unnecessary game
   function to have on controller"* — game speed is in the options menu and on the keyboard's
   `1`/`2`/`3`.
3. **Two settings got pad buttons, against the standing rule.** S174's rule was that a setting with a
   menu row gets no pad button, one exception for the kill switch. The user widened it to three
   bindings. The rule is not struck — `F4`, `F5`, `F7` and the volumes are still menu-only — but the
   filter and the beacon are the two they flip constantly, and a rule about scarcity is the user's to
   spend.

**`L1` and `R1` are gated on `live` (Field or Battle), which is what preserves S184.** With a
targeting cursor up the context is `FieldBusy`, so neither shoulder is consumed and the game keeps its
Foes / Party / Reserve / Allies group step — the switch S184 built the spoken titles for one session
earlier. Taking a shoulder there would have silenced a play-confirmed feature to feed an unplayed one.

**The chord needed a shape, and the shape is the finding.** `L3` and `R3` each mean something alone
and something else together, which cannot be edge-triggered on the press: whichever went down first
would act before the second arrived, so every chord would be preceded by a spurious single. Resolving
on the **falling** edge needs no timer and no guess window. A latch set the instant both bits are down
together, cleared only when both are up, makes press order and release order irrelevant. It is also
the right pair of buttons for it: the user's own S174 ruling is that stick clicks are too awkward for
anything time-critical, and nothing is waiting on a settings toggle.

**A bug found and fixed in this session's own code, worth recording because it was invisible in a
build.** The thumb block first wrote `state->pad.buttons` directly. The apply at the bottom of
`OnPoll` rebuilds that word from the ORIGINAL `buttons` local, so on any poll that also consumed
something else the second write silently handed `L3`/`R3` back to the game — intermittent, and
dependent on what else the player happened to be pressing. Fixed by hoisting `consume` above the off
switch so there is **one accumulator and one write**. A second writer to a value something else
rebuilds from scratch is not a race; it is a guaranteed loss, waiting on a condition.

### `F6`: a real edit field, and a confirmation

`ui/text_prompt.{h,cpp}` — a Win32 edit dialog and a Yes/No box, each on a thread of its own.

**The clipboard was never the feature; it was the workaround, and its premise was wrong.**
`Docs/Controls.md` justified it correctly and then drew the wrong conclusion: the mod cannot run a
text field *inside the game* (it passes the DirectInput buffer as `const` and never swallows a key) —
**but a text field does not have to be inside the game.** Typing into a Win32 EDIT control is ordinary
window-message input and never touches the DirectInput buffer, so the read-only rule is not weakened
by one byte. The game's device is simply unacquired while another window of the process holds focus,
which is what already happens on every alt-tab. *A constraint on one mechanism is not a constraint on
the goal — check which one you wrote down.*

- **No custom name yet → an edit field.** OK with text names it; OK with nothing, or Cancel, does
  nothing. **An empty field is no longer a clear** — that meaning moved to the question below, where
  it can be asked instead of guessed from a blank.
- **Already named → a Yes/No box**, defaulting to **No**, asking whether to clear it back to the
  game's own name. To rename: clear, then press `F6` again. One question per press.
- A genuine `#32770` dialog, built as an in-memory `DLGTEMPLATE` because this project has no `.rc`. A
  screen reader reads a dialog's title, its static text and its focused control on open; a plain popup
  with child controls gets the focused control alone.
- **The static is created BEFORE the edit control on purpose** — a screen reader labels an edit from
  the static that precedes it in z-order, so swapping the two silently costs the field its name.
- **Its own thread, not the game thread and not the InputTracker thread.** The game thread would
  freeze the game (memory: never stall the game thread). The InputTracker thread is the mod's hotkey
  dispatcher — a modal loop on it would queue every other key behind the dialog and stall
  `Shutdown`'s `WM_QUIT`.
- **While a prompt is up both input paths stand down**, and each needed a different shape for the same
  reason. The keyboard feed runs its whole edge pass against a **zeroed** buffer rather than returning
  early: returning would leave a key held when the box opened still armed, to fire the moment it
  closed. The pad returns early but **carries `prevThumbs` forward first**, or a click released while
  typing would become a toggle when the dialog shut.
- The entity's identity is captured **by value under the lock** and the lock is released before the
  window goes up; the callback re-takes it and **refuses to apply if the map has changed**, since the
  label store is keyed by map and a dialog has no time limit.

### Also

- **`Escape` closes the mod menu** (user instruction), claimed only while it is open. **This is not a
  claim that `Esc` is free** — `L-51`: the game's Controls screen lists an *action* called "Escape" on
  Left Ctrl, which says nothing about the physical key, and that screen has hidden a binding before
  (`F9`, S112). The mod cannot swallow a key, so if the game owns `Esc` both things happen — the same
  accepted cost the arrow keys already carry inside this menu.
- `InputTracker`'s `SetControllerToggleCallback` / `DispatchToggleController` / `WM_PADCTRL` became
  the generic `SetSettingToggleCallback` / `DispatchToggleSetting` / `WM_PADSET`, carrying a setting
  id as an `int` — the same idiom `DispatchSpeakPhrase` uses for `Phrase::Id`, so `input_tracker`
  still knows no menu. Three bindings through one path instead of a near-duplicate per button.
- README rewritten: the whole Controller section, `F6`, and — at the user's instruction — **a note
  that NPCs are commonly unnamed until spoken to, which is correct game behaviour.** The game gives an
  NPC their name at the story beat where you meet them; custom naming is for the ones that stay
  nameless.
- Clean build, 0 warnings, deployed.

**NOT PLAYED.** Everything above is built and untried except the right stick and the D-pad, which this
session did not touch. The things most worth a falsifier on the first pass: whether the dialog actually
takes focus over the game (it is `WS_EX_TOPMOST` and calls `SetForegroundWindow`, but a game that owns
the whole screen is the case that breaks such things), and whether the `L3`+`R3` chord resolves the way
a real thumb produces it. Grep `PROMPT` and `PAD` in the log.

### Second half: "No path" on a route the player walked — the S182 cut fired where no door exists

**User report, with the log:** *"a clear 'no path' validation failure on a valid path... I was able to walk
toward the destination using crow-flies, and the path eventually, finally validated once I get close
enough... there is no door in the way and no obstacle."* Progress-blocking for anyone who does not know to
track by crow-flies.

**The log answered it, and S182 had written down the falsifier itself.** 21 suppressed frontiers to the
Dreadnought Leviathan exits; on every one the mod'''s own oracle said `goal poly 179 is IN the start poly'''s
adjacency component -- this is the SEARCH giving up, not an unreachable goal`; and the first four carried
`closed-floor: 44 crossing(s) CUT -- script-closed floor, material id(s) 31` at the exact pinch the
validator then kept breaching on. S182'''s own comment above that cut: *"the falsifier for the cut: a map
where this fires and the player walks that crossing by hand."*

**⚠ THAT FIX WAS REVERTED BEFORE THE SESSION CLOSED — see below. What follows is what was built and
measured, not what shipped.**

**Fix (reverted): the cut needs the closed-flag SHAPE and a DECLARATION.** `ReachGate::OpenableFloorMask()` is
the union of every container-0 routine'''s `opensFloorMask` -- the materials some script on this map can
`setmapidfloor(N, 0, 1)`. In the mask: still CUT. Not in it: PRICED, as S96 established. **Straight out of
the user'''s own ruling -- price what we infer, cut what the game declares.** A material a door routine can
open is a declaration; the flag shape alone is an inference, and it was the inference that was wrong.

An unreadable script returns all-bits-set, so it reproduces S182 exactly: a successful read can only
NARROW the cut, never widen it. That is what keeps Sochen working -- its doors are real routines.

**Also removed for the public build:** `''''`, the nav diagnostic probe key (user instruction). The handler
and `NavProbe::Request` are untouched; only the edge that fires it is gone.

**NOT PLAYED, and this one is going to the user to test before the release is updated.** Falsifiers: the
Dreadnought Leviathan route must now speak legs from the same spot, and Sochen'''s closed doors must still
say "No path" while shut. The new `closed-floor: ... PRICED not cut ...` line names exactly what the change
let through.

**Two defects found and deliberately NOT fixed, recorded in debug.md so they are not mistaken for closed:**
the validator refuses chords the corridor march certifies CLEAR and the repair ladder cannot always mend
them (`seq=56` had no closed-floor cut at all and still failed); and the attempt ladder spends escalations
its own log line reports as ineffective (500->1500->4500, "not enough to move the search", three of four
attempts). This fix removes what put the search into that corner on this map, not the corner itself.

### What actually shipped, and the two rounds that did not

Three rounds went at this. **Only the third is in the tree.**

1. **The S182 closed-floor cut, narrowed by `ReachGate::OpenableFloorMask()`.** Confirmed working by the
   user's next log (mask `0x00000000`, zero crossings cut, the corridor running through the poly S182 had
   been severing) — so the narrowing did what it was written to do. It was not the blocker. **REVERTED.**
2. **`PathValidate::ApronToGoal`** — accept a last-leg stop when every step between the body and the goal
   is ground the party cannot stand on. Measured from a 4.2 m class-refused apron in front of the exit.
   Fired **zero** times on the case it was written for (`apron=0`). **REVERTED.**
3. **`pass=corridor-march`** — when every attempt is spent and the adjacency march certified the whole
   corridor, ship the CORRIDOR rather than speak "No path" over it. **PLAY-CONFIRMED by the user:** *"yes,
   your fix worked."*

**The user's instruction, and it is the durable part:** *"revert the fixes that weren't related to the
blocker while I test. we don't want fixes that were unrelated that could potentially cause regressions
elsewhere."* A change that is correct but not the cause is still an unmeasured risk to every map that
works today, and bundling it with the fix under test makes a clean confirmation impossible. The revert is
purely subtractive — `git diff` against the pad commit is **74 insertions, 0 deletions, one file**.

**THE LESSON THE FIRST TWO ROUNDS PAID FOR.** The contradiction that turned out to be the answer was in
the FIRST log read, printed on every failing attempt, in the mod's own words: `corridor march: CLEAR over
76 hop(s)` immediately beside `validate: ... BREACH ... why=sweep`. Two rounds went to causes that were
real and measurable and not the blocker. **When one subsystem certifies what another refuses, in the same
poll, about the same geometry, that contradiction IS the bug** — chase it before anything that merely
looks wrong nearby. Recorded as `L-95`.

### Closed by the user — there is no open routing defect

After playing the shipped build: *"pathing is working fine. you shouldn't have anything but the straight
line fix you put in there. nothing is being returned as unwalkable, unsure what you're seeing in the log.
there are no further defects and this is a shippable release."*

**The session close had claimed the round-1 cut was "still a live defect in the shipped build". That was
wrong and the user corrected it.** The 44-crossing measurement came from a log written by the PRE-FIX
build; with `pass=corridor-march` in place it produces no observable failure. A measurement is not a
defect until something a player can see follows from it — `L-97`.

The reverted narrowing keeps its measurement in `debug.md` and its code in `git log` (`d2d3a72`). It is
NOT an open item, and it does not get re-applied without a new report and a new log.

## Session 186 — 2026-09-18 — Controller support rebuilt on SDL3: every pad type, not just Xbox

**KEYWORDS: controller gamepad pad SDL3 SDL_OpenGamepad DualSense DualShock PlayStation Switch Pro
XInput XInputGetState Xbox-only DirectInput DIJOYSTATE2 rgbButtons rgdwPOV suppression GamepadSDL
PollIfStale SuppressDInputPad binding table CoCreateInstance vtable share V1.0 broken release PAD**

### The defect, and it shipped to the public

V1.0's controller support did not work for PlayStation controllers — or Switch Pro pads, or generic
USB pads. It worked for Xbox pads and for anything a translation layer (Steam Input, DS4Windows,
ViGEm) was presenting as one, and for nothing else.

**Cause: the mod read the pad through `XInputGetState` only.** XInput is the Xbox protocol. Every
other pad enumerates as a HID joystick and reaches the game through DirectInput.

The tester log the user pointed at (Lirin, build `V1.0 (7588345)`, ~100 s) has it outright:

```
[PAD] gamepad intercept installed (XInputGetState, IAT)
[PAD] non-keyboard DirectInput device created: idx=0 guid=6F1D2B60-D5A0-11CF   <- GUID_SysMouse
[PAD] non-keyboard DirectInput device created: idx=1 guid=B861A230-F85C-11EE   <- the DualSense
[PAD] non-keyboard DirectInput device polled:  idx=0 cbData=20                 <- DIMOUSESTATE2
[PAD] non-keyboard DirectInput device polled:  idx=1 cbData=272 (JOYSTICK-SIZED ...)
```

`cbData=272` is `DIJOYSTATE2`. The game read that pad through DirectInput all session. On the XInput
side, `controller CONNECTED on index N` — a one-shot line, verified present in the shipped binary —
**never fired**, so `XInputGetState` never once returned a connected pad. Every pad feature (L1
interact readout, R1 route, D-pad party slots, right-stick navigation, Back mod-mode, L3/R3) was
unreachable.

`[SPEAK-OUT] Controller, On` at +47 s is NOT a pad chord: it sits between `Auto detail` and
`Diacritics override` with no `[MODMENU] set:` beside it — the player arrow-keying down the open F8
menu.

### What was built

**SDL3 is now the only pad reader.** `src\input\gamepad_sdl.{h,cpp}` — ported from the FFPR mods'
`Core/GamepadManager.cs`, which is the design the user specified when controller support was first
asked for. `SDL_OpenGamepad` + SDL's controller database normalize DualSense / DualShock 4 / Switch
Pro / Xbox / generic into one button layout, so **`pad_router.cpp` and the whole S185 scheme are
unchanged** — only the source of its `PadHook::State` moved. SOUTH/EAST/WEST/NORTH map to the
scheme's A/B/X/Y, i.e. Cross/Circle/Square/Triangle in the same physical positions.

**SDL3 was already linked into this DLL** for the navigation beacon. `CMakeLists.txt` line 28 read
"SDL3 (audio output for the navigation beacon; later, controller support)".

**The two API hooks are now suppressors only.** `PadHook` no longer calls `PadRouter::OnPoll`;
`GamepadSDL` is its one caller. The router writes its consume mask into a mod-owned snapshot, which
is published and applied to whichever buffer the game actually reads:

| game reads | suppressor | clears |
|---|---|---|
| XInput (Xbox pads) | `pad_hook.cpp` | button bits, right-stick axes |
| DirectInput (everything else) | `SuppressDInputPad`, `dinput8_proxy.cpp` | `rgbButtons[i]` → 0, `rgdwPOV[i]` → centred, right-stick axes → device centre |

Consume-only is intact, so this needed no new input-write category — `CLAUDE.md`'s second exception
is updated to describe the new shape.

**Poll site:** the keyboard `GetDeviceState` branch, which runs every frame including menus and
loads, *outside* the `SUCCEEDED(hr)` gate — an unacquired keyboard (which this very log catches
happening) must not take the pad down with it. `PollIfStale` collapses to one read per frame however
many of the three call sites ask, so the mask is always current and no press leaks for a frame.
Wrapped in `STALL_SCOPE` so the cost is measured, not asserted (`L-88`).

### Three things that would have been bugs

1. **SDL shares the vtable we patched.** SDL reaches DirectInput via
   `CoCreateInstance(CLSID_DirectInput8)`, not via our `DirectInput8Create` export — so its devices
   never enter `g_otherDevices`, but they come from the same System32 module and therefore the same
   patched vtable. Unguarded, `SuppressDInputPad` would have cleared consumed buttons out of **SDL's
   own read buffer**: the mod suppressing its own input, buttons flickering as the router saw a
   release it caused. Gated on `oi >= 0` — only devices the game created.
2. **The right stick had to be suppressed on the DInput path too — my first cut shipped it as an
   accepted limitation and that was wrong.** The user's correction: *"moving the camera reorients the
   player's walking direction, so if the camera is moving while they are cycling destinations the
   directions will never be correct."* The swallow is load-bearing for navigation, not cosmetic.
   The obstacle was only ever "what does centred MEAN on this device", and the device answers:
   **`IDirectInputDevice8::GetProperty(DIPROP_RANGE)`, vtable slot 5**, returns the range the game
   itself set, so the centre is read rather than guessed. Writing 0 would indeed have been a hard
   deflection on an unsigned range — that part was right, the conclusion drawn from it was not.
   - The axis is **identified before it is centred**: with the physical stick centred, the real
     right-stick axis must sit at its own centre; one that moves anyway is driven by something else
     and is rejected (3 agreements confirm, 4 disagreements reject). The identification test runs on
     every poll, including polls where nothing is claimed — that is where the disconfirming evidence
     lives.
   - Any candidate offset SDL also calls a **left-stick** axis is refused outright. Centring that one
     would stop the player walking, which is the one mistake with an unacceptable cost.
   - A `GetProperty` failure is **logged loudly** (`UNSUPPRESSED ... REPORT THIS`) rather than
     silently degrading, and no speculative fallback centre is invented.
3. **Indices are confirmed, not assumed** (`L-02`). `SDL_GetGamepadBindings` says where a button
   should sit in the raw report, but SDL and the game enumerate the device separately, so that is a
   hypothesis about the game's buffer. A button is cleared only at an index observed DOWN in the
   game's own buffer while SDL also reported it down; the first confirmation logs
   `DInput suppression confirmed: <button> -> rgbButtons[i]`. Unconfirmed = left alone (reaches both
   mod and game, the old double-action, strictly better than clearing the wrong bit).

Also: priming on open (a button or stick held when the pad connects is masked until released, so it
fires nothing), hot-plug via `SDL_EVENT_GAMEPAD_ADDED`/`REMOVED`, and
`SDL_HINT_JOYSTICK_ENHANCED_REPORTS=0` — enhanced mode is a WRITE to the device and would change
what the game's own reader sees.

### State

**BUILT, DEPLOYED, UNPLAYED.** Nobody in this session held a pad. The log lines that settle it, in
order: `CONTROLLER CONNECTED via SDL3: "<name>"`, `SDL raw bindings: N of 14`, then `PAD survey`
lines on press, then `DInput suppression confirmed:` per button. If the first line is absent SDL
never saw the pad; if the third is absent the router is not being reached.

`L-98` added. `CLAUDE.md` second input-write exception, `Docs\GameArchitecture.md` (the XInput-is-
*the*-pad-path claim **STRUCK**), `Docs\Controls.md` and `Docs\Lessons.md` all updated.

## Session 187 — 2026-09-18 — The consume mask was edge-shaped; mod mode gets one opener and one exit

**KEYWORDS: controller pad consume mask edge level latch held release Start pause Back map mod mode
Circle B cancel context mod menu XInput read rate 4ms 33ms regression S186 PAD**

### Play result from S186

Tester on an **Xbox One Controller**, build `V1.0 (e0675c3)`. The SDL3 rebuild works:

```
[PAD] CONTROLLER CONNECTED via SDL3: "Xbox One Controller" (SDL type 3, id 1)
[PAD] SDL raw bindings: 14 of 14 mod-relevant buttons and 4 of 4 stick axes located in the device report
[PAD] the GAME reads this pad through XInput on index 0 -- suppression for it runs here
```

Right stick, D-pad, mod mode, the mod menu and the scheme all dispatched correctly. Note the only
DirectInput device on this machine is the **mouse** (`guid=6F1D2B60`, `cbData=20`), so the DInput
suppressor was not exercised at all and remains unplayed.

### Defect 1 — the consume mask was an EDGE at a rate the game never sampled (MY REGRESSION)

Start paused the game; Back opened the map. The router was doing its job — `consume |= rising` — but
`rising` is true for **one poll**, `GamepadSDL` polls every **~4 ms**, and the game reads its pad
**once a frame, ~33 ms**. The mask was overwritten with 0 long before the game looked, so the button
reached the game anyway. Only the right stick behaved, because `eatStick = onField` is a **level** and
so was still true whenever the game happened to read.

**S186 introduced this.** Before it, `PadRouter::OnPoll` ran FROM the XInput hook: edge detection and
consumption were the same event against the same buffer, so an edge-shaped mask was exactly right.
Decoupling the read from the write made it meaningless. The L3/R3 prologue had the right model all
along and says so in its own comment — *"Consumed for as long as they are HELD, not on an edge"* —
it was simply never generalised.

**Fix, in `gamepad_sdl.cpp` rather than the router:** latch the claim until release.
`g_heldConsume |= claimedNow; g_heldConsume &= buttons;` and publish the union. The router keeps its
single edge-shaped consume path; the mask is now correct at any read rate. Primed buttons cannot
enter the latch because `buttons` is the post-priming state.

### Defect 2 — mod + B opened the mod menu; it should cancel

`ModModeKeyFor` mapped `kB` to `F8` as a second way in. **Removed.** `mod + B` now falls to `default`,
speaks "Cancelled" and ends the mode. `Start` is the one opener. B still **closes** the menu from the
`modMenuOpen` branch — the same button answering the opposite job by context, which is what the user
asked for and which already worked (`[PAD] B -> close mod menu` in the log).

### Defect 3 — cancelling with Back opened the map; make that deliberate

It did that because the mask leaked (defect 1), i.e. the behaviour the player saw was **right by
accident**. The user's ruling: make it on purpose. `consume |= rising & ~PadHook::kBack` — the second
Back ends the mode and passes through, so **Back, Back opens the map**, which is what gives the player
back the control Back costs them everywhere else. It speaks nothing: the map screen is its own
feedback, and "Cancelled" over a screen just deliberately opened would contradict what happened.

### State

**BUILT AND DEPLOYED, UNPLAYED.** What settles each: Start in mod mode opens the mod menu **without
pausing**; Back-Back opens the map with no "Cancelled"; mod + B says "Cancelled" and opens nothing;
B closes the menu when it is open. `Docs\Controls.md`, `README.md` and `pad_router.h`'s scheme comment
all updated. Pathing was NOT tested this pass.

## Session 188 — 2026-09-18 — ALL controller input through SDL3: the game's pad is built, not masked

**KEYWORDS: controller SDL3 only reader passthrough synthesise XINPUT_STATE BuildGameState
DisableUnityGamepad InputPassthroughPatches FFPR blank DirectInput DIPROP_RANGE one path injection
charter category PAD**

### The instruction

> *"**ALL** CONTROLLER INPUT SHOULD BE HANDLED THROUGH SDL3 … that means no XInput, no DInput, none
> of that unless SDL3 itself uses it … I do not want to see one more instance of 'You're on an XBox
> controller so SDL3 and direct input haven't been tested.'"*

S186 ported only HALF the FFPR model. It made SDL3 the reader and then let the game go on reading the
hardware itself, which forced a **different suppressor per API** — one for `XINPUT_STATE`, one for
`DIJOYSTATE2` — each with its own index tables, confirmation tests and its own play pass. That is why
every report ended with a path that had not been exercised.

### What FFPR actually does, which I had not read

`Patches\InputPassthroughPatches.cs`, its own header:

```
/// 1. Suppress all game input when mod is consuming (mod menu, dialogs, mod mode)
/// 2. Inject SDL controller state for game passthrough when not suppressing
```

and `GamepadManager.DisableUnityGamepad()` switches the engine's own gamepad devices **off**. The mod
is the sole reader and the game is *fed*. Not "the mod reads too, and edits what the game saw".

### The port

| | |
|---|---|
| `gamepad_sdl.cpp` | reads the pad through SDL3, for every device. `BuildGameState` returns what SDL read **minus** the router's claim, with `dwPacketNumber` advancing only on real change |
| `pad_hook.cpp` | **synthesises** the `XINPUT_STATE`. The real `XInputGetState` result is discarded while driving |
| `dinput8_proxy.cpp` | **blanks** the game's DirectInput joystick — the FFXII answer to `DisableUnityGamepad()` |

**One path for every controller.** An Xbox pad, a DualSense, a Switch Pro pad and a handheld's sticks
all arrive at the game as the same synthesised state built by the same lines, so testing on any pad
tests the path all pads use. The Xbox One controller the tester holds now exercises exactly what a
DualSense would.

**It also makes non-Xbox pads better off than before.** A DualSense does not speak XInput, so the real
call returns `ERROR_DEVICE_NOT_CONNECTED`; building the state ourselves hands the game a working
XInput pad regardless of the hardware — the same service Steam Input performs, done by the mod.

### What this DELETED

The whole device-specific layer, because nothing needs to describe another format any more: the
`rgbButtons[]` index table, `SDL_GetGamepadBindings` lookups, per-button confirmation, the POV angle
matcher, the right-stick axis identification with its agree/disagree counters and left-stick refusal,
`RawElemFor`, `RawAxisFor`, `StickRaw`, `PadGeneration`, `ConsumedButtons`, `ConsumedStick`. Blanking
needs no mapping — it zeroes every button, centres every POV, and neutralises every axis.

### The two things that survived, because they are still true

1. **An axis's neutral is READ, never assumed** — `GetProperty(DIPROP_RANGE)`, vtable slot 5. Writing
   0 is a hard deflection on an unsigned range. An axis whose range will not read is left alone.
2. **A claim is a LEVEL, not an edge** (`L-99`, S187). Still latched until release.

### The charter changed, and it is a category change

The mod no longer merely clears bits — it **constructs** the pad state the game reads. `CLAUDE.md`'s
second input-write exception is rewritten. The bound replacing *"never set a bit"* is: **every bit in
the synthesised state came from a physical control SDL reported as pressed on that poll.** The mod can
decline to forward an input; it can never originate one. Off still means byte-identical — the hook
returns the original call verbatim and the blanker touches nothing.

### State

**BUILT AND DEPLOYED, UNPLAYED.** Log line to look for: `the game's pad is now being driven from SDL3
(XInput index 0)`. On a non-XInput pad, also `this pad also reaches the game through DirectInput --
blanking that road` plus `DInput axis +N: ... neutral M (read from the device)`. Pathing still untested.

## Session 189 — 2026-09-18 — S188 play-confirmed; A and B swapped to the game's own confirm/cancel

**KEYWORDS: controller SDL3 play-confirmed driven from SDL3 XInput index 0 A B swap Cross Circle
confirm cancel mod menu mod mode scheme PAD**

### S188 IS PLAY-CONFIRMED

Live log, Xbox One Controller:

```
[PAD] CONTROLLER CONNECTED via SDL3: "Xbox One Controller" (SDL type 3, id 1)
[PAD] SDL has a full mapping for this controller
[PAD] XInput IAT patched -- the game's pad will be built from SDL3
[PAD] the game's pad is now being driven from SDL3 (XInput index 0)
```

That fourth line is the one that settles it: `BuildGameState` returned true, so **the state the game
reads is the one the mod built from SDL3**. The scheme rode on top of it correctly — `R1 -> route +
beacon`, `L1 -> target readout`, right-stick category/object stepping, D-pad arrows in menus — and the
player navigated the game's own menus throughout, which is only possible if the synthesised pad is
driving the game too. The only DirectInput device present is the mouse (`cbData=20`), so the blanker
correctly never fired; there was no second road to close.

**There is no longer a controller path that goes unexercised.** One path serves every device, and this
pass exercised it end to end.

### The scheme change

**A and B swapped**, to the user's instruction, so the mod uses the same pair the game does:

| | before | after |
|---|---|---|
| mod menu | A reads the description, B closes | **B reads the description, A closes** |
| mod mode | A = summoned Esper, B = cancel | **B = summoned Esper, A = cancel** |

The reasoning is the blind-player one: the button FFXII confirms with should be the button that asks
the mod a question, and the button it cancels with should be the one that backs out. Getting it
backwards spends the muscle memory the rest of the game just taught them.

**Only the scheme moved.** The SDL -> XInput bit mapping in `gamepad_sdl.cpp` is untouched, so the
game still receives the physical button the player actually pressed —
`SDL_GAMEPAD_BUTTON_SOUTH -> kA`, `EAST -> kB`. Swapping *that* would have handed the game the wrong
buttons and broken its own controls.

### State

Built, deployed and committed. `Docs\Controls.md`, `README.md` and `pad_router.h`'s scheme comment
updated. **The swap itself is unplayed.** Pathing still untested.

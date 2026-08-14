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

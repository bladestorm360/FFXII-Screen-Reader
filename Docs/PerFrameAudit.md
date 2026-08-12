# Per-frame work audit — what runs every frame, and what is FRAME-COUPLED

> **SESSION 152, 2026-08-12 — the four tier-C sites are FIXED, and two of this file's own claims
> are STRUCK.** All four now hold wall-clock deadlines. Read the Session 152 block at the bottom
> before acting on anything in the original text: the game-speed question this file left open is
> ANSWERED **for the field-tick callees** (speed cannot move those), the "0.63 s at 144 fps"
> severity figure is **unverified and doubtful**, and the measurement this file prescribed **cannot
> work as written**.
>
> **AND THE BIGGEST ITEM IN THIS FILE IS NOW OPEN, NOT CLOSED.** The tester reports the symptoms
> occur **at raised GAME SPEED, not raised frame rate** — dialogue misread, an "aaaaaaaaa" runaway.
> That cannot come from the field tick, which points at a hook driven from **inside the sim loop**,
> where the call rate *does* multiply by 2x/4x. **Tier A is not automatically safe.** See the final
> section.

**Written Session 149, 2026-08-10**, after a tester reported a dialogue box repeating its line
forever (fixed — see `debug.md` S149) and then asked the right follow-up: *"please tell me we are not
reading dialogue per-frame and reconstructing the dialogue, because that could cause problems if FPS
in the display settings is changed, or at game speed."* Testers are reporting issues at higher game
speeds and frame rates, so this file is the inventory.

## The distinction that matters

`CLAUDE.md` says "NO polling, timers, or per-frame checks — event-driven hooks only". In practice the
mod has a dozen hooks on functions the game calls every frame, and that is not automatically a
violation: **hooking a per-frame function is fine; deriving behaviour from the frame COUNT is not.**

Three tiers, and only the third is a bug:

| tier | shape | frame-rate safe? |
|---|---|---|
| **A. Notification** | a per-frame hook whose only job is to notice that a *game-owned* value changed (a cursor, a handle, an index). Output is O(changes), not O(frames). | **Yes.** More frames = more no-op calls. |
| **B. Wall-clock** | logic keyed to `GetTickCount64()` deltas. | **Yes for FPS.** See the game-speed caveat below. |
| **C. Frame-counted** | a counter incremented once per call, compared against a constant. The constant silently means "N/60 seconds". | **NO. This is the defect class.** |

The bug fixed in S149 was a fourth thing and worth naming separately: **a tier-A guard defeated by a
game field that oscillates**. `widget+0xC0` is set by one call and cleared by the next, so re-arming
on it produced speech at FPS/2 — genuinely frame-rate-coupled output out of a correctly-shaped hook.
The lesson generalises: **before re-arming on a game flag, establish whether it is an edge or a
level.**

## Tier C — the frame-counted logic (~~FIX THESE~~ **FIXED, Session 152**)

Four sites listed. Every one silently assumed 60 fps. **A fifth was missed and is recorded in the
Session 152 block below.**

| site | constant | at 30 fps | at 60 fps | at 144 fps | status |
|---|---|---|---|---|---|
| `navigation/path_planner.cpp:83` | `kWaitFrames = 90` | 3.0 s | 1.5 s | **0.63 s** | **FIXED** → `kWaitMs = 1500` |
| `navigation/nav_probe.cpp:36` | `kWaitFrames = 90` (comment says "~1.5 s") | 3.0 s | 1.5 s | **0.63 s** | **FIXED** → `kWaitMs = 1500` |
| `navigation/audio_beacon.cpp:46` | `kStrayFrames = 45` (comment says "~0.75 s at 60 fps") | 1.5 s | 0.75 s | **0.31 s** | **FIXED** → `kStrayMs = 750` |
| `ui/dialogue_reader.cpp` | `kIdleReportAt = 64` | — | — | — (log-only, harmless) | **LEFT ALONE ON PURPOSE** — see below |
| `ui/menu_reader.cpp:104` | `kMaxPaintRetries = 8` | — | — | — | **MISSED BY THIS FILE. FIXED** → count OR `kPaintWaitMs = 150` |

**`dialogue_reader`'s was deliberately not converted.** The count *is* the quantity it measures —
`:243-245` says it reports "this many inert repeats is this many re-speaks the old code produced".
Rendering that in milliseconds would destroy the thing being counted. It is log-only and correct.

**`path_planner`'s is the one most likely behind a tester report.** `g_framesLeft = kWaitFrames` is
the budget a pending route request gets to wait for `IsFieldNavSafe()` after a map change or a
transition. At 144 fps that budget is 0.63 s of real time — less than half what it was tuned for — so
a route asked for during a fade-in can expire before the map is ready and come back as a refusal. A
player at high frame rate would see intermittent "No path" that a player at 60 fps does not.
`nav_probe` is the same shape for the `'` probe (log-only impact).

**`audio_beacon`'s inverts a deliberate design decision.** The comment above it says the test is
"deliberately slack — a beacon that re-aims every time the player rounds a pillar would be worse than
one that is briefly stale." At 144 fps the slack is a third of a second, which is roughly *"every
time the player rounds a pillar"*. The intent survives only at exactly 60 fps.

**The fix is mechanical and identical in all three:** replace the frame counter with a `GetTickCount64()`
deadline, which is what the comments already say they mean.

```cpp
// before
constexpr int kStrayFrames = 45;          // ~0.75 s at 60 fps
if (++g_strayCount >= kStrayFrames && ...)

// after
constexpr uint64_t kStrayMs = 750;        // says what it means, at any frame rate
if (g_straySinceMs == 0) g_straySinceMs = now;
if (now - g_straySinceMs >= kStrayMs && ...)
```

The planner's is the same edit against its `g_framesLeft` countdown: stamp a deadline when the
request is armed, and test the clock instead of decrementing. ~~Both already have `now` in
scope.~~ **STRUCK (S152): `path_planner.cpp` has no `<windows.h>` anywhere in its include graph,
directly or transitively, and needed a new include. Only `audio_beacon.cpp` had `now` in scope.**

**The sketch above is also incomplete in a way that matters** — it shows the arm and the compare
but not the DISARM. The frame counter it replaces is zeroed the moment the player is back inside
`kStrayDist`, so it measures one CONTINUOUS stray run. A deadline without the matching
`g_straySinceMs = 0` in the `else` branch silently means "has been stray at some point in the last
750 ms", which is looser than the code it replaced and fires on a player oscillating across the
boundary. Session 152 added that clear and moved the reset into `ResetPhase()`.

## Tier B — wall-clock logic, and the game-speed caveat

`auto_walk.cpp` (`kPendingFreshMs`, `kMaskStaleMs`, `kMinHoldMs`, `kFieldGapMs`, `kNoProgressMs`,
`kUnstickMs`), `audio_beacon.cpp` (ping interval, `kStuckMs`, `kReplanCooldownMs`),
`stall_probe.cpp` (`kStallMs`, `kStallLogIntervalMs`), `ingame_menu_reader.cpp` (the pane-pending
arm stamp). All keyed to `GetTickCount64()`, so **frame rate cannot move them**.

Game speed can, in one direction: these measure REAL time while the world moves faster. At 2x/4x the
player covers more ground inside every window, so the timeouts get *more* forgiving in game-distance
terms — `kNoProgressMs = 15000` becomes 60 s of travel, `kStuckMs = 1800` sees more displacement. That
is the safe direction, and nothing here needs changing on that account.

~~**What is NOT established:** whether FFXII's game-speed setting runs the field tick more often or
gives each tick a larger delta.~~ **ANSWERED, Session 152 — it does NEITHER, and the answer is in
the decompile.** See the Session 152 block below. Game speed runs the sim loop *inside* one hooked
call more times, so it cannot move a counter driven by the FIELD TICK — **but it multiplies the call
rate of anything driven from inside that loop, which is the open tester defect at the end of this
file. Do not read this line as "game speed is harmless".**

~~It is measurable today without new code — `StallProbe`'s `FrameTick` already records the
inter-frame gap on the input poll, so a short log at 1x and at 4x answers it.~~ **STRUCK (S152):
that experiment CANNOT WORK.** `FrameTick` computes the gap and then discards it below its warn
threshold (`stall_probe.cpp:190-201`, called with `gapWarnMs=100.0` at `dinput8_proxy.cpp:141`), so
an ordinary session emits nothing at all. The corpus proves it: **five `input-poll` GAP lines across
twenty logs, every one a boot stall.** A dedicated instrument was needed and now exists —
`core/frame_probe.{h,cpp}`.

## Tier B risk of a different shape — per-frame deltas against absolute distances

Two constants compare *a single frame's* movement against a fixed number of metres:

* `audio_beacon.cpp:70` `kMotionTeleportM = 5.0f` — `if (d < kMotionTeleportM) g_motionAccum += d;`
* `auto_walk.cpp:49` `kWalkDeltaCapM = 5.0f`

Both exist to reject a teleport/warp from a movement accumulator. The *shape* is wrong — a per-frame
delta is not a frame-rate-independent quantity — but the *numbers* are almost certainly safe: FFXII
walking is roughly 5-6 m/s, so even at 4x speed and 10 fps a real step is ~2.4 m, inside the 5 m gate.
**Flagged, not urgent.** The principled version divides by the frame's own elapsed time and compares
a SPEED (`d / dtSeconds > kTeleportSpeedMps`), which is correct at any frame rate and any game speed.
Do this if the accumulator is ever seen under-counting; do not churn it speculatively.

## Tier A — per-frame hooks that are correctly shaped (leave alone)

These all hook a function the game calls every frame and gate their output on a change in a value
the GAME owns. Output is O(events).

| hook | game function | the change-check |
|---|---|---|
| `NavHooks::HookedFieldFrame` | `FUN_0022a770` field tick | dispatcher; each callee is O(1) when idle |
| `NavHooks::HookedWorldStep` | `FUN_0069f070` physics step | caches the world pointer |
| `DialogueReader::HookedTextWalk` | `FUN_002a8c50` | `(widget, base, widget+0x8A page cursor)` |
| `ChoiceReader::HookedChoiceTick` | `FUN_002a9980` | `widget+0x58` option cursor |
| `BattleTargetReader` | `FUN_002bfd20` nameplate render | `g_lastHandle` |
| `TextCapture` Imm/Obj1/Obj2/Painter | `FUN_002b0280`/`002abec0`/`002abf20`/`002d28e0` | captures into a ring/map; **never speaks** |
| `IngameMenuReader` window proc | per-draw, cat `0x13` repeats | one-shot pending slot |
| `ShopReader` | `FUN_0056d370` panel proc | quantity/step change only |
| `GambitReader` | cat-`0xA` handler | state filter |
| `PrimerReader` | `FUN_00572e10` | change-check (documented) |
| `EntityList::OnFieldFrame` | via the field tick | container-count change |
| `dinput8_proxy::HookedGetDeviceState` | the game's own input poll | edge detection + `StallProbe::FrameTick` |

**The dialogue answer, for the record, since it prompted this audit.** The mod does **not** reconstruct
dialogue. The complete codec text is in memory at `widget+0x28` from the moment the message is
installed, and `widget+0x8A` says which page is showing; on a cursor change we decode that page in one
pass and speak it. Nothing accumulates across frames. **The typewriter counter `widget+0x8C` is never
read** — the full page is spoken the instant the cursor lands on it, so reveal speed, frame rate and
game speed cannot change what is said or how often.

## Beyond change-checks — the alternatives worth reaching for

Ranked by preference, because "make it event-driven" is not always available:

1. **Hook the WRITER of the state instead of the reader.** The strongest fix and the one the project
   already leans on (`feedback_games_fire_events_check_trace.md`). S149's page-key re-arm moved from a
   per-frame flag to `FUN_002e16b0`, the function that installs a message in the registry slot — an
   exact event, zero per-frame cost. **Before adding any per-frame check, spend the search budget
   looking for the function that writes the value.**
2. **Wall-clock deadline instead of a frame countdown.** The tier-C fix above. Cheap, mechanical, and
   it makes the constant say what the comment already claimed.
3. **Deadline in the GAME's own clock, where one exists.** If a tier-C timeout should scale with game
   speed rather than real time, the field tick is the place to find a simulation timestamp. Not
   located yet; worth one search pass if a tester reports a speed-specific timeout.
4. **Let the per-frame call be a pure notification and do the work on the transition.** Already the
   house pattern; the cost is then O(1) per frame and the correctness is frame-rate independent by
   construction.
5. **Measure before tuning.** Every tier-C constant above is a guess dressed as a measurement. The
   stall probe can time these paths; a log at two frame rates is worth more than a re-tuned constant.

## ~~What to do next, in order~~ — done in Session 152; see the block below

1. ~~**Settle the game-speed mechanism** from a `FrameTick` log at 1x and 4x.~~ Settled from the
   decompile instead; the `FrameTick` route could not have worked.
2. ~~**Convert the three live tier-C sites**~~ — done, and a fourth with them.
3. **Re-measure with a tester at high FPS** before calling any of it fixed. **STILL OUTSTANDING.**
4. Leave tier A alone. The per-frame *cost* there is real (`HookedTextWalk` does eight guarded reads
   plus a mutex per call) but it is bounded, it is already bracketed by `STALL_SCOPE`, and no
   behaviour depends on how often it runs. **Unchanged and still correct.**

---

# Session 152, 2026-08-12 — the conversions, and what the audit had wrong

## 1. GAME SPEED CANNOT MOVE A FRAME COUNTER. Settled in the decompile.

`FUN_0022a770` — the function the mod hooks as `FIELD_FRAME` (`nav_rva.h:702`, RVA `0x10A770`) —
**contains** the simulation loop:

```
0022a770_FUN_0022a770.c:105-109   ac4 = (gate == 0) ? 1.0 : speedTable[speedIndex]
0022a770_FUN_0022a770.c:250       acc += ac8 * ac4        // once per call
0022a770_FUN_0022a770.c:139       while (1.0 <= acc) {    // the sim loop
0022a770_FUN_0022a770.c:185           acc -= 1.0
```

The mod detours the **outer** function, so `HookedFieldFrame` and all five callees fire **once per
call at 1x, 2x and 4x alike**. Speed runs the *inner* loop more times. Speed index is
`i32 @ RVA 0x1EB4A98`; the multiplier table is `&DAT_00908ba8` at RVA `0x7E8BA8`, whose three
floats are `.rdata` and were only ever a 0.85-confidence `{1,2,4}` guess — the new probe prints
them.

**Only frame RATE can move a counter driven by the FIELD TICK.**

> ### ⚠ SCOPE CORRECTION, same session — this claim is TRUE ONLY FOR THE FIELD-TICK CALLEES
>
> The sentence originally written here was *"Game speed is not a factor anywhere in this file"*, and
> that is **an overreach that must not be built on.** It holds for the six callbacks dispatched from
> `HookedFieldFrame` — `EntityList`, `PathPlanner`, `AudioBeacon`, `AutoWalk`, `NavProbe`,
> `ShoutMeter` — because those hang off the OUTER function.
>
> **It says nothing about the tier-A hooks.** `FUN_0022a770`'s sim loop body calls a dozen subsystem
> updates, including `FUN_00314020` — *the mod's own former drain point*, which `nav_hooks.cpp:137`
> still names. **Anything reached from inside that loop runs N times per rendered frame at speed N.**
> Whether the dialogue text walk, the choice tick, the nameplate render or the painter is among them
> is **NOT ESTABLISHED** — see the tester evidence below.

## 2. The "0.63 s at 144 fps" figure is UNVERIFIED, and the decompile makes it doubtful

`ac8` — the per-frame delta, *in sim ticks, not seconds* — is a hard `1.0f` at every reachable
writer (`0022a0b0:44`, `002628b0:165`, `003601d0:13`). Its only variable writer, `FUN_00343ee0`,
has **zero callers** in the 33k-function corpus. So the sim advances exactly one tick per call.

If that call followed display refresh, the whole game would run 2.4x fast at 144 Hz — which nobody
reports. The likelier reading is that the loop is paced (there is a vsync-interval global
`DAT_02064ACC`, RVA `0x1F44ACC`, derived at `0022a0b0:40`). **Not established either way, and it is
runtime-only state**, so it is the documented exception to decompile-first, not a decompile
question.

Two consequences, both binding:

* **Do not re-tune any constant against the 144 fps number.** The rate reachable through a
  supported in-game setting is *30*, where every budget got **longer** (90 frames = 3.0 s) — the
  opposite direction from this file's original narrative.
* The conversions are still right regardless: a millisecond deadline is correct at every rate and
  is behaviour-identical at 60 fps, so it carried no regression risk and needed no measurement
  first.

## 3. A fifth site this file missed — `menu_reader.cpp` `kMaxPaintRetries = 8`

Driven by the paint callback, which is frame-paced: in our own corpus
(`x64/logs/FFXII-Screen-Reader-2026-08-10_11-05-54.log:699-705`) consecutive retries sit **15–16 ms
apart**, and the budget **exhausted four times** in that one session, each logging
`TEXT NEVER PAINTED … this surface is MUTE`.

Honest caveat, so it is not over-read later: those four may be genuinely mute surfaces rather than
budget shortfalls. What is *proven* is that the budget is reachable in ordinary play at 60 fps, so
at any higher rate the same surfaces get proportionally less wall-clock — and giving up costs a
blind player the announcement outright.

**Fixed by ORing a wall-clock floor onto the count, never ANDing:**
`(g_retryCount < kMaxPaintRetries) || (now - g_retryFirstMs < kPaintWaitMs)`, `kPaintWaitMs = 150`.
The budget is then never *smaller* than before at any rate — 30 fps keeps its 8 paints, 144 fps
gets ~20. The one-shot give-up notice moved from an equality test on the counter to an explicit
`g_retryGaveUp` latch, because a count that can overshoot never equals its cap and the line would
simply have stopped appearing.

## 4. "Make it event-driven" was investigated and is NOT available for the nav waits

`PlayerState::IsFieldNavSafe()` (`player_state.cpp:98-104`) is a conjunction of six predicates owned
by four independent subsystems. The planner already re-evaluates it on the game's own field-tick
event and fires on the **first frame it is true** — it is level-driven, not a poll loop. The only
frame-derived quantity was *how long before apologising to the player*, and there is no game event
for "this will never become ready": a request during a cutscene, or on a Ridorana-style map whose
terminal state never clears (`debug.md` S93), must still answer the keypress.

Writers were traced anyway, per the house preference:

* **`FUN_0026e960` (RVA `0x14E960`)** is the sole writer of `DAT_0209a670` / `DAT_0209a678`, the
  walkmap globals `MapQuery::HasWorld()` reads. `FUN_00269c70` writes the leader pointer.
* Hooking either still leaves the conjunction to re-evaluate — which is what `IsFieldNavSafe()`
  already does, cheaply, once per frame. **Both are below the 0.98 bar without a Frida probe**, and
  neither removes the need for a deadline.

Also corrected: `NavHooks::HookedWorldStep` caches the **Bullet** context, a different world from
the SQEX one `CondWorld` tests. The existing world-pointer cache says nothing about nav readiness.

## 5. The instrument — `core/frame_probe.{h,cpp}`

Ships with the fix, because the severity question above is still open and nothing in the corpus can
close it. Installs **no hook**; it is ticked from the field-frame hook and the DirectInput poll,
both of which already exist. One `[PERF]` line per 10 s:

```
frame pacing: render 59.9 fps (599 frames), field tick 59.9/s (599) over 10000 ms | speedIdx=0 ac4=1.000 ac8=1.000 accum=0.412
```

plus one line at init with the multiplier table. What each reading settles:

* `render` vs `field tick` diverging outside menus ⇒ the field tick is not display-paced.
* `render` above 60 ⇒ finding 2 resolves toward the audit's original worry.
* Changing Speed Mode must move `ac4` and leave **both** rates untouched ⇒ finding 1 confirmed live.
* `ac8` anything but `1.000` ⇒ the `FUN_00343ee0` path is reachable after all; re-open finding 2.

---

# ⚠ OPEN — THE TESTER SAYS IT IS GAME SPEED, NOT FRAME RATE (Session 152, 2026-08-12)

**This is the most important open item in this file, and it points at a class of defect the whole
audit was looking past.**

## What was reported

The tester who prompted this audit states the problems appear **specifically at raised game speed**
— not at raised frame rate. Named symptoms: **dialogue reading incorrectly**, and an
**"aaaaaaaaa" runaway loop**. **No other tester has reported these**, and they have not been
reproduced on the dev machine.

## Why this contradicts the finding above, and why the finding is still right

The field tick fires once per rendered frame at every speed — that is settled. So **nothing hanging
off `HookedFieldFrame` can produce a speed-only symptom.** Both facts are true at once, and the only
way they fit together is:

> **THE DEFECT IS NOT ON THE FIELD TICK. It is on a hook reached from INSIDE the sim loop.**

`FUN_0022a770:139-184` runs a dozen subsystem updates per iteration of `while (1.0 <= acc)`, and the
loop iterates `ac4` times per rendered frame. Anything called from in there fires **2x or 4x per
rendered frame** when the player raises speed, while the render rate is unchanged.

**The dialogue text walk is the prime suspect, and it fits the symptom exactly.** S149's defect was
an end latch (`widget+0xC0`) that is a LEVEL, not an edge — it oscillates `1, 0, 1, 0` and produced
speech at FPS/2. If the walk runs inside the sim loop, **4x speed means 4x that rate**, which is
what an *"aaaaaaaaa" runaway* sounds like. "Dialogue reading incorrectly" is the same mechanism at
lower intensity: a page re-decoded and re-spoken part-way through.

## Why this could not be settled offline

`FUN_002a8c50` (text walk) and `FUN_002a9980` (choice tick) have **zero static callers in the
33,105-function decompile** — they are dispatch-table slots (`PTR_FUN_009164c8`, slot 0 and slot 2),
reached indirectly. **Who drives them is a runtime fact, not a decompile fact.** That is the
documented exception, and it is why the instrument now measures it rather than reasoning about it.

## The measurement — one line, already shipped

`core/frame_probe.cpp` counts text-walk calls alongside the render frames, and prints the ratio:

```
frame pacing: render 59.9 fps (599 frames), field tick 59.9/s (599) over 10000 ms |
              textWalk 59.9/s (599, 1.00/frame) | speedIdx=0 ac4=1.000 ac8=1.000 accum=0.412
```

**Open a dialogue box, let it sit, and change Speed Mode.** Read `textWalk … /frame`:

| reading at 4x | conclusion |
|---|---|
| stays ~`1.00/frame` | the walk is render-paced. Game speed is NOT the mechanism; the tester's defect is something else and this hypothesis is **refuted**. |
| rises to ~`4.00/frame` | **the walk runs inside the sim loop.** The hypothesis is confirmed, and **every tier-A hook in this file must be re-checked for the same thing** — the "output is O(changes), not O(frames)" argument silently assumed one call per rendered frame. |

`field tick` staying at `1.00/frame` in both cases is the control: it proves the speed change took
effect without moving the outer cadence.

## Rules this changes if confirmed

1. **Tier A is not automatically safe.** This file's tier-A table certifies twelve hooks as
   "correctly shaped" on the grounds that their output is O(changes). That argument holds only if a
   *change* is a real change — a level that oscillates is a change every call, and the call rate is
   then whatever the sim loop says it is.
2. **"Once per frame" needs to say WHICH frame.** Rendered frame and sim tick are the same thing
   only at 1x. Every comment in the mod that says "per frame" was written assuming they are one.
3. **A tester-only, speed-only defect is evidence about a CALL PATH, not about their machine.** The
   instinct to file this under "cannot reproduce" is what kept it open; the reproduction condition
   was in the report all along.

## Do NOT do

- Do not re-tune any tier-C constant against this. The four converted budgets are on the wall clock
  and are unaffected either way.
- Do not add a dedup or a rate-limit to the dialogue reader to make the runaway stop. That is the
  standing NO-DEDUP rule, and here it would hide the measurement that identifies the call path.

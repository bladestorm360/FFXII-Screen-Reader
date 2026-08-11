# Per-frame work audit — what runs every frame, and what is FRAME-COUPLED

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

## Tier C — the frame-counted logic (FIX THESE)

Four sites. Every one silently assumes 60 fps.

| site | constant | at 30 fps | at 60 fps | at 144 fps |
|---|---|---|---|---|
| `navigation/path_planner.cpp:83` | `kWaitFrames = 90` | 3.0 s | 1.5 s | **0.63 s** |
| `navigation/nav_probe.cpp:36` | `kWaitFrames = 90` (comment says "~1.5 s") | 3.0 s | 1.5 s | **0.63 s** |
| `navigation/audio_beacon.cpp:46` | `kStrayFrames = 45` (comment says "~0.75 s at 60 fps") | 1.5 s | 0.75 s | **0.31 s** |
| `ui/dialogue_reader.cpp` | `kIdleReportAt = 64` | — | — | — (log-only, harmless) |

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
request is armed, and test the clock instead of decrementing. Both already have `now` in scope.

## Tier B — wall-clock logic, and the game-speed caveat

`auto_walk.cpp` (`kPendingFreshMs`, `kMaskStaleMs`, `kMinHoldMs`, `kFieldGapMs`, `kNoProgressMs`,
`kUnstickMs`), `audio_beacon.cpp` (ping interval, `kStuckMs`, `kReplanCooldownMs`),
`stall_probe.cpp` (`kStallMs`, `kStallLogIntervalMs`), `ingame_menu_reader.cpp` (the pane-pending
arm stamp). All keyed to `GetTickCount64()`, so **frame rate cannot move them**.

Game speed can, in one direction: these measure REAL time while the world moves faster. At 2x/4x the
player covers more ground inside every window, so the timeouts get *more* forgiving in game-distance
terms — `kNoProgressMs = 15000` becomes 60 s of travel, `kStuckMs = 1800` sees more displacement. That
is the safe direction, and nothing here needs changing on that account.

**What is NOT established:** whether FFXII's game-speed setting runs the field tick more often or
gives each tick a larger delta. That distinction decides whether tier C is *worse* at high speed
(more ticks per second) or unaffected. It is measurable today without new code — `StallProbe`'s
`FrameTick` already records the inter-frame gap on the input poll, so a short log at 1x and at 4x
answers it. **Do that before tuning any of the tier-C constants**, and record the answer here.

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

## What to do next, in order

1. **Settle the game-speed mechanism** (more ticks vs bigger delta) from a `FrameTick` log at 1x and
   4x. One play session, no code.
2. **Convert the three live tier-C sites to wall-clock deadlines** — `path_planner` first, it is the
   one that can produce a wrong answer rather than a mistimed one.
3. **Re-measure with a tester at high FPS** before calling any of it fixed.
4. Leave tier A alone. The per-frame *cost* there is real (`HookedTextWalk` does eight guarded reads
   plus a mutex per call) but it is bounded, it is already bracketed by `STALL_SCOPE`, and no
   behaviour depends on how often it runs.

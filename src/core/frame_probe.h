#pragma once

#include <cstdint>

// FRAME-PACING PROBE — how fast is the game actually running, and does game speed change it?
//
// WHY THIS EXISTS. `Docs\PerFrameAudit.md` (Session 149) flagged four sites that derived behaviour
// from a frame COUNT, and estimated their severity from "at 144 fps a 90-frame budget is 0.63 s".
// Nothing in the project could check that number. Two facts were missing and both are measurable:
//
//   1. Does the field tick follow the display refresh rate? `FUN_0022a770` adds `ac8 * ac4` to the
//      sim accumulator once per call and drains it one whole tick at a time, and `ac8` is a hard
//      1.0f at every reachable writer — so one call is one sim tick at 1x. If that call tracked a
//      144 Hz display the whole game would run 2.4x fast, which nobody reports; the loop is more
//      likely paced. Until this is measured, the 0.63 s figure is a guess and no tier-C constant
//      should be re-tuned against it.
//   2. Does game speed change the rate our hooks fire? It should not. `FUN_0022a770` CONTAINS the
//      sim loop, and the mod hooks the outer function, so 2x/4x runs the loop inside one hooked
//      call more times. This probe is what turns that decompile reading into a measurement.
//
// WHY NOT StallProbe::FrameTick. It records the inter-frame gap but discards every value below its
// warn threshold (100 ms at the only call site), so an ordinary session emits nothing at all — the
// audit's "one play log settles it with no code" experiment cannot work, and the corpus confirms it
// (five such lines across twenty logs, all boot stalls). StallProbe stays what it is: a stall
// detector on QPC. This is a separate, low-volume cadence report on the wall clock.
//
// READ-ONLY, and every address is a pure load of a global already documented in
// `Docs\combat_system.md` §7.5, which carries a standing read-only prohibition on that cluster.
// No hook of its own: it is ticked from two hooks that already exist.
//
// OUTPUT BUDGET: one `[PERF]` line per reporting interval, plus one line at init. O(1) work per
// frame the rest of the time.
namespace FrameProbe {

// Log the speed-multiplier table once. Call after Hooks::Init, before the game reaches the field.
void Init();

// GAME THREAD. Tick from the field-frame hook (`FUN_0022a770`). Counts field ticks.
void OnFieldFrame();

// GAME THREAD. Tick from the dialogue text walk (`FUN_002a8c50`).
//
// THIS IS THE ONE THAT ANSWERS THE TESTER'S REPORT. A tester sees dialogue misread and an
// "aaaaaaaa" runaway ONLY at raised game speed. That should be impossible for anything hanging off
// the field tick -- 2x/4x runs the sim loop INSIDE one hooked call, so `OnFieldFrame` above fires
// once per rendered frame at every speed. But the text walk is NOT reached from the field tick: it
// is dispatch-table slot 0 (`PTR_FUN_009164c8`) with no static caller anywhere in the 33k-function
// decompile, so whether it is driven from inside that sim loop CANNOT be settled offline.
//
// If it is inside, it fires N times per rendered frame at speed N -- and S149's end-latch defect
// (a level that oscillates 1,0,1,0 and produced speech at FPS/2) would produce N times the spurious
// speech at 4x. That is exactly the reported symptom.
//
// The test is the ratio in the report line: textWalk/s against render fps. Unchanged by a speed
// change ⇒ the walk is render-paced and the tester's defect is something else. Multiplying by the
// speed factor ⇒ the walk is inside the sim loop, and every tier-A hook needs re-examining for it.
void OnTextWalk();

// INPUT THREAD. Tick from the DirectInput poll, which keeps running in menus and loads where the
// field tick does not. This anchor is what makes "the field tick stopped" and "the game stalled"
// distinguishable, and it owns the reporting interval.
void OnInputPoll();

} // namespace FrameProbe

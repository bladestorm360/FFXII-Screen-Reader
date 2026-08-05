#pragma once

// SPOKEN INFAMY METER for Bhujerba's shout minigame, plus the two on-demand puzzle keys.
//
// The game draws the gauge as art with no number and no label behind it, so a blind player has no
// access to it at all: the only text the sequence produces is "<n> Bhujerbans heed your words",
// which says nothing about how full the meter is or how far 100 still is.
//
// WHAT SPEAKS, AND WHEN:
//   * automatically, once per CHANGE BURST, in both directions -- a shout that lands, and the
//     Imperial penalty that takes 30 points back;
//   * on `B`, the meter on demand;
//   * on `N`, the NPCs nearest the player, by the game's own names, with bearing and distance.
// Both keys are silent no-ops on any map that is not running a shout script, which is what the
// tester asked for ("map specific ... no-op on non-puzzle maps").
//
// THE BURST COALESCER IS NOT SPEECH DEDUP (CLAUDE.md bans that). The scripts do not set the gauge
// once per event: a shout runs `setgaugecounter(v+1)` in a loop, and the Imperial penalty runs
// `setgaugecounter(v-1)` THIRTY times with a one-frame wait between each -- measured in byu_a01 at
// 0x34C35. Announcing per call would say thirty numbers for one event. What ships is a state
// machine that detects the end of a change (a field frame with no new sets) and speaks the value
// it settled on: every burst announces, exactly once. Nothing is ever suppressed for being a
// repeat, and re-entering the map speaks again.
namespace ShoutMeter {

// Installs the gauge-writer hook. Non-fatal on failure: the feature simply never speaks.
bool Init();

// GAME THREAD, once per field tick. Refreshes which script module is live, drains the two key
// requests, and closes a finished burst. O(a few pointer reads) when nothing is happening.
void OnFieldFrame();

// GAME THREAD, from the field-teardown hook. Clears the module cache, the burst, and the log
// latches -- a module identity is only valid inside the map it was read on.
void OnMapTeardown();

// INPUT THREAD. Both only raise a flag; the work happens on the next field frame, because reading
// live transforms and the game's own name tables is not safe off the game thread (the `'` probe
// precedent, nav_commands.cpp).
void RequestMeterCheck();   // B
void RequestGuardCheck();   // N

} // namespace ShoutMeter

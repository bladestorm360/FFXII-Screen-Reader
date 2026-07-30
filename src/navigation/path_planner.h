#pragma once

#include <cstdint>
#include <string>
#include "navigation/nav_types.h"

// Turn-by-turn route planner (Layer 3). A* over a lazily-sampled walkability grid,
// producing either spoken turn-by-turn legs ("North 8, then West 5. 13 steps.") or a
// trustworthy "No path" — never a crow-flies fallback (that is the `/` describe key).
//
// CRASH-SAFETY MODEL (the reason this is not on the input thread):
//   - The grid is sampled with many Bullet raycasts. Doing that on the mod's input
//     thread would race the game's physics step and read half-loaded / half-freed map
//     data during transitions. So the whole computation runs ON THE GAME THREAD,
//     drained once per field frame by the FUN_0022a770 hook (nav_hooks), at that
//     function's entry (before its own teardown driver runs), and only when
//     PlayerState::IsFieldNavSafe() confirms the map is fully live.
//   - A monotonic map epoch (bumped by OnMapTeardown from the teardown hook) makes any
//     request captured before a transition un-revivably stale, so a route can never be
//     computed against a torn-down map even if the keypress and the transition race.
//   - Work is bounded per frame (ray + expansion budget) so the game thread never
//     stalls; a search that exceeds the budget reports "No path" honestly.
namespace PathPlanner {

bool Init();
void Shutdown();

// INPUT THREAD. Queue a route to a world-space target (the focused entity's live
// position) with a spoken destination label. Captures the current map epoch; the
// actual planning happens on the next safe game frame. Overwrites any prior request.
//
// `isTransition`: the target is a map-jump surface, so arriving at it IS crossing it. Near one, the
// planner says "At the exit" rather than grinding out two-metre legs across the seam. False for
// anything else.
// `bandLo`/`bandHi`: the target's interaction band (InteractTarget::ReadBandFor) -- the range of
// player Y from which the engine will let you interact with it.
// `reachRadius`: the engine's own horizontal interaction reach for that target
// (InteractTarget::ReadReachFor, `radiusMin`).
//
// Supplied together, they let the search stop at the first poly the player can both stand on AND
// interact from, which is what removes the 3-4 step overshoot past the point where `;` starts
// answering. Either one missing (inverted band, or a zero radius) routes to the target's own poly,
// which is the older behaviour.
// `seedBeacon`: arm the audio beacon on the resulting route. True for `\` (the route the player
// asked to be led along); FALSE for `p`, whose target is a moving enemy -- static leg corners would
// be pointing at where it used to be within a second. In combat the beacon tracks the active target
// on its own (see audio_beacon.cpp), so `p` needs no beacon wiring.
void Request(const FVec3& target, const std::wstring& label, bool isTransition = false,
             float bandLo = 1.0f, float bandHi = -1.0f, float reachRadius = 0.0f,
             bool seedBeacon = false);

// GAME THREAD. Re-run the BEACON'S OBJECTIVE, silently -- no speech on any outcome, including failure.
// This is the audio beacon's off-route recovery: the player has wandered, so the leg corners it is
// steering by are stale, but they were never told a new route and must not suddenly be read one.
// Returns false if no objective has been set on this map.
//
// THE OBJECTIVE IS ITS OWN MEMORY, NOT "THE LAST REQUEST" (Session 95). It used to be the latter, and
// `p` overwrote it: route to an enemy mid-fight, and the next off-route re-plan silently re-aimed the
// beacon at that enemy instead of at the exit the player had asked to be led to. The tester read the
// symptom exactly right -- "the beacon only remembers the last leg it was on and considers that the
// destination" -- and the log names the culprit outright: `replan: silent re-run of last
// target=(66.23,6.85,108.37)` resolving to `target="Steeling A"`, ten seconds after the fight ended.
//
// `p` already declares it has no business with the beacon by passing `seedBeacon=false`; that flag now
// also decides whether the request is allowed to become the objective. Only a request that ARMS the
// beacon can redirect it.
bool RequestReplan();

// The current map generation. Bumped by OnMapTeardown; anything holding route geometry compares
// against it to notice a map change without needing a hook of its own.
uint32_t CurrentEpoch();

// GAME THREAD. Called once per field frame from the FUN_0022a770 hook (at entry). If a
// request is pending and still valid for this map and the field is fully live, plan the
// route and speak the result; otherwise retry for a bounded number of frames, then give
// up with "Route unavailable". O(1) (one atomic load) when nothing is pending.
void OnGameFrame();

// GAME THREAD. Called from the FUN_002695a0 teardown hook (at entry). Bumps the map
// epoch and drops any pending request so no route is ever computed on a dying map.
void OnMapTeardown();

} // namespace PathPlanner

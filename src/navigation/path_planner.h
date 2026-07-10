#pragma once

#include <string>
#include "navigation/nav_types.h"

// Turn-by-turn route planner (Layer 3). A* over a lazily-sampled walkability grid,
// producing either spoken turn-by-turn legs ("North 8, then West 5. 13 steps.") or a
// trustworthy "No path" — never a crow-flies fallback (that is the `\` describe key).
//
// CRASH-SAFETY MODEL (the reason this is not on the input thread):
//   - The grid is sampled with many Bullet raycasts. Doing that on the mod's input
//     thread would race the game's physics step and read half-loaded / half-freed map
//     data during transitions. So the whole computation runs ON THE GAME THREAD,
//     drained once per field frame by the FUN_00314020 hook (nav_hooks), at that
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
void Request(const FVec3& target, const std::wstring& label);

// GAME THREAD. Called once per field frame from the FUN_00314020 hook (at entry). If a
// request is pending and still valid for this map and the field is fully live, plan the
// route and speak the result; otherwise retry for a bounded number of frames, then give
// up with "Route unavailable". O(1) (one atomic load) when nothing is pending.
void OnGameFrame();

// GAME THREAD. Called from the FUN_002695a0 teardown hook (at entry). Bumps the map
// epoch and drops any pending request so no route is ever computed on a dying map.
void OnMapTeardown();

} // namespace PathPlanner

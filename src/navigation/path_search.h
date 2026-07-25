#pragma once

#include <cstdint>
#include <vector>
#include "navigation/nav_types.h"

// The route SEARCH: A* over the fine walkability grid, plus the recovery passes that keep a doorway from
// being reported unreachable. Split out of path_planner.cpp when that file passed the project's 500-line
// ceiling. The planner owns the request lifecycle (queue on the input thread, drain on a safe game frame,
// speak the result); this owns the geometry.
//
// GAME THREAD ONLY — every walkability sample is a raycast against live map geometry.
namespace PathSearch {

enum class Plan { Route, NoPath };

// Diagnostic counters. `pass` names which stage produced the answer, and on a failure `nearDist` is the
// single most useful number in the log: it separates "the goal was islanded one cell short of the
// doorway" from "the goal is genuinely behind a wall", which want completely different fixes.
struct Stats {
    int  rays = 0, expands = 0, cells = 0;
    int  tx = 0, tz = 0;
    bool startFloorHit = false;
    const char* pass   = "strict";   // strict | bridge | near-goal
    float nearDist     = -1.0f;      // metres from the goal to the closest cell actually reached
    int   nearC = 0, nearR = 0;
};

// Plan a route from `from` to `to` on the map identified by `epoch`. `rawPoly` is the raw cell path (what
// the spoken legs are measured from); `outPoly` is the string-pulled, wall-validated geometry.
//
// `bandLo`/`bandHi` are the target's INTERACTION BAND (InteractTarget::ReadBandFor): the range of
// player Y from which the engine will let you interact with it. The search routes to the nearest
// REACHABLE cell whose floor lies inside that band, rather than to the target's own cell -- which is
// frequently somewhere you cannot stand (a dais, a counter, a ledge). Pass an inverted range
// (`bandLo > bandHi`) to disable the goal set and get the pre-Session-73 single-cell behaviour.
Plan Run(const FVec3& from, const FVec3& to, uint32_t epoch, float bandLo, float bandHi,
         std::vector<FVec3>& rawPoly, std::vector<FVec3>& outPoly, Stats& stats);

} // namespace PathSearch

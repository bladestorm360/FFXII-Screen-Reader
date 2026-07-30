#pragma once

#include <cstdint>
#include <vector>
#include "navigation/nav_types.h"

// The route SEARCH: A* over the GAME'S OWN NAVMESH (NavMesh), not over a grid of our own invention.
//
// WHAT THIS REPLACED AND WHY (Session 74). The previous search ran over a uniform 1.5 m grid that the
// mod sampled itself, storing one floor height per cell and synthesising edges between neighbouring
// cells behind a step-height gate. It produced a confident, wrong answer on any map with vertical
// structure: Upper Apartments announced its Highhall exit (7.8 m up) and its Lower Apartments exit
// (1.9 m down) one step apart, because both collapsed onto the same flat cells -- and the Highhall
// "route" was actually a near-goal FALLBACK that stopped 3 m short and was spoken as if it had
// arrived.
//
// Three things went with the grid, and none of them are coming back:
//   * `kMaxStep` / `kStepDiscont` -- the engine imposes NO step limit between adjacent polys
//     (walkability is a flag test), so these refused edges the game walks. Any staircase with a
//     riser over 0.35 m was unroutable.
//   * `SnapToWalkable` -- a 9 m ring search for "somewhere walkable near the goal", which is how a
//     goal 7.8 m overhead got answered with the floor beneath it.
//   * the near-goal and bridge recovery passes -- both existed to paper over the grid's invented
//     connectivity. A search over real adjacency either reaches the poly or genuinely cannot.
//
// GAME THREAD ONLY -- the volume check on each expanded edge is a walk-class segment cast.
namespace PathSearch {

// `Frontier` is a route to the reachable point CLOSEST to the goal, for when the goal itself cannot be
// reached. It exists because "no route" is not an acceptable answer to a blind player: they cannot see
// which way to walk to get around whatever is in the way, so refusing to route strands them.
//
// IT IS A DISTINCT ENUM VALUE, NOT A FLAG ON `Route`, deliberately -- the compiler then forces every
// consumer's switch to decide what to say about it. The failure this guards against is recorded at the
// very line this replaced: the old grid search emitted a near-goal fallback and SPOKE IT AS A NORMAL
// ROUTE, which walked the tester confidently to a spot 3 m from an exit 7.8 m overhead. A frontier route
// MUST announce its shortfall.
enum class Plan { Route, Frontier, NoPath };

// Diagnostic counters. On a failure `nearDist` is the single most useful number: it separates "the
// goal poly is one edge away behind a closed door" from "the goal is in a different component".
struct Stats {
    int  expands = 0;             // polys popped
    int  touched = 0;             // polys seen
    int  rays    = 0;             // body sweeps + footprint tests performed
    int  startPoly = -1;          // -1 = the player is not standing on any readable floor poly
    int  goalPoly  = -1;
    int  endPoly   = -1;          // the poly the search actually finished on
    float nearDist = -1.0f;       // metres from the goal centroid to the closest poly reached
    const char* pass = "mesh";
    int  attempts  = 1;           // A* passes run; > 1 means a breach was found and re-searched around
    int  bannedEdges = 0;         // portals re-costed across all attempts
    // Corridor waypoints spliced back into a leg whose taut chord did not walk (Session 96). Non-zero
    // means the route shipped is the un-pulled one -- the corridor was fine and the shortcut was not.
    int  repaired = 0;
    float shortfall = 0.0f;       // Frontier only: metres from the frontier point to the goal
};

// Plan a route from `from` to `to` on the map identified by `epoch`.
//
// `bandLo`/`bandHi` are the target's INTERACTION BAND and `reachRadius` the engine's measured
// horizontal interaction reach (InteractTarget::ReadReachFor). When both are supplied the search
// stops at the FIRST poly it can both stand on and interact from -- which is the nearest such poly
// by walking distance, so the route no longer overshoots past the point where `;` starts answering.
// Pass an inverted band (`bandLo > bandHi`) or a zero radius to route to the target's own poly.
//
// `rawPoly` is the portal path the spoken legs are measured from; `outPoly` is currently the same
// polyline (PathDirections does its own simplification).
Plan Run(const FVec3& from, const FVec3& to, uint32_t epoch,
         float bandLo, float bandHi, float reachRadius,
         std::vector<FVec3>& rawPoly, std::vector<FVec3>& outPoly, Stats& stats);

} // namespace PathSearch

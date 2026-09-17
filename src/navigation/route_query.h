#pragma once

#include <cstdint>
#include <vector>
#include "navigation/nav_types.h"
#include "navigation/path_search.h"

// THE ROUTE KEY'S REQUEST, AND THE SEARCH IT RUNS, IN ONE PLACE (Session 182).
//
// `For` is the request `\` builds for an entity (the interaction band and reach for an object, the arrival
// band for an exit), moved verbatim from nav_commands' RouteToCurrent. `AtTransition` and `Search` are the
// planner's "At the exit" test and its seam-set + PathSearch::Run, moved verbatim from the drain. Nothing
// about a route changed in the move.
//
// It was split out for S182's first Unreachable filter, which ran these searches in the background. That
// filter was REVOKED by the user before it was deployed (game-thread stalls; CLAUDE.md, L-88), and
// nothing but the route key calls `Search` now. `AnsweredAboutTarget` is what the filter still uses: it
// decides whether a spoken answer is about the target at all.
namespace RouteQuery {

// Everything the router is told about a target besides where the player stands.
struct Params {
    FVec3 target{};
    bool  isTransition = false;          // a map-jump surface: arriving there IS crossing it
    float bandLo = 1.0f, bandHi = -1.0f; // inverted = no band known -> the target's own poly
    float reach  = 0.0f;                 // 0 = no reach -> the target's own poly
    int   seamGroup = 0;                 // the target's map-jump group, 0 when it is not a surface
    // For the `route reach:` log line only. `reachRead` is the radius the reach model produced, which
    // the line has always printed even when no band came with it (and `reach` above is then 0).
    float       reachRead   = 0.0f;
    const char* reachSource = "none";
};

// INPUT THREAD (memory reads of the target object). The request `\` makes for this entity: the
// engine's interaction band and reach for an object, the arrival band for an exit.
Params For(const FVec3& pos, bool isTransition, void* sceneObj, int seamGroup);

// Is the player already standing on this transition -- the "At the exit" answer, for which no search
// is run? `dist`/`dy` receive the measured gaps (2D and vertical) whatever the answer.
bool AtTransition(const Params& q, const FVec3& from, float& dist, float& dy);

// GAME THREAD. Run the search for `q` from `from`, with the target's whole map-jump surface when it has
// one and the seam sweep has run.
PathSearch::Plan Search(const Params& q, const FVec3& from, uint32_t epoch,
                        std::vector<FVec3>& rawPoly, std::vector<FVec3>& poly, PathSearch::Stats& st);

// Did that search actually get to ask the question? False when it could not place the PLAYER on the mesh
// (no floor poly under them, a torn read, no collision world): the "No path" it returns then describes
// where the player happens to be standing, not the target, and must not be recorded as the target's
// answer -- one such moment would otherwise mark every entity checked during it unreachable.
bool AnsweredAboutTarget(const PathSearch::Stats& st);

} // namespace RouteQuery

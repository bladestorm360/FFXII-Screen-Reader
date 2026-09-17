#pragma once

#include <cstdint>
#include <vector>
#include "navigation/nav_types.h"
#include "navigation/path_search.h"

// A ROUTE NEVER BECOMES INVALID MID-WALK (S183, the user's rule: "pathways should never become suddenly
// invalid mid walk, that is a bug").
//
// WHAT WENT WRONG. A spoken route could be deleted by a re-plan that failed. The beacon's stuck and
// off-route detectors re-run the objective silently from wherever the player is, and the drain seeded the
// beacon with whatever came back -- an empty list on failure, which STOPPED the route. Destiny's March
// (09-17 06:26 log) did it three times in five minutes: seq 152, 214 and 218, each from a spot beside a
// door or just round a corner, where the first leg's straight line clips the door frame, every repair rung
// fails, and the search gives up. Its own oracle said so each time ("goal poly ... is IN the start poly's
// adjacency component -- this is the SEARCH giving up, not an unreachable goal"). Pressing `\` again from
// the same spot then answered "No path" too (seq 215, 216), while from four metres further on it routed.
//
// THE RULE. A request to the SAME objective the beacon is already leading to, that comes back without a
// Route, keeps the live route instead:
//   * a silent re-plan leaves the beacon exactly as it is and holds further automatic re-plans on that leg
//     (AudioBeacon::HoldAutoReplans) -- so this adds no game-thread work the old behaviour did not make;
//   * the player's own `\` re-speaks the live route's remaining legs from where they stand and re-seeds the
//     beacon with them (the user's choice, 2026-09-17).
//
// TWO FAILURES STILL END THE ROUTE, because they are facts about the goal rather than about the spot:
//   * the oracle proves the goal is not in the player's walkable component at all;
//   * the rest of the live route now crosses a floor a SCRIPT has closed (a door or waterfall shut across
//     it since it was spoken) -- S182's "cut what the game declares", applied to the route already walking.
namespace RouteKeep {

// GAME THREAD, on a request's failure path only. `sameObjective` is the planner's own answer to "is this the
// destination the beacon is leading to". On true, `corners` holds the live route's remaining corners. Logs
// its verdict either way (`keep-route:`).
bool Decide(bool sameObjective, const FVec3& from, const FVec3& target, uint32_t epoch,
            const PathSearch::Stats& st, uint64_t seq, std::vector<FVec3>& corners);

} // namespace RouteKeep

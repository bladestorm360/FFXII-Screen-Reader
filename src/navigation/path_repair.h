#pragma once

#include <vector>
#include "navigation/nav_types.h"
#include "navigation/path_funnel.h"
#include "navigation/path_validate.h"

// REPAIRING A ROUTE THE BODY WOULD NOT WALK.
//
// Split out of `path_search.cpp` (Session 97) on the seam `PerformanceIssues.md` has named since
// Session 96. It is the whole of the failure path: `PathSearch::Run` calls `Mend` once, on a breach,
// and either gets a polyline the engine's own body walked end to end or gets nothing back and carries
// on to re-cost and re-search exactly as before.
//
// WHY IT IS A REAL SEAM AND NOT A LINE-COUNT TRICK: everything in here answers ONE question -- "the
// chord across this corridor did not walk; is there another polyline through the SAME corridor that
// does?" -- and it needs none of `Run`'s search state to answer it. No A*, no ban list, no centroid
// cache, no frontier. Its whole input is the corridor (`portals`), the polyline drawn across it
// (`poly` + `polyIdx`), and the validator's verdict on that polyline (`rep`).
//
// A BREACH IS A VERDICT ON THE CHORD, NOT ON THE CORRIDOR (Session 96). The corridor A* returns is
// walkable by construction -- every portal was measured with the body's own footprint and sweep before
// the edge was expanded. The taut line the funnel draws across it is an optimisation, and on this mesh,
// where one triangle is often an entire room, that line can leave the walkable strip while every portal
// it skipped stays perfectly crossable. Map 311 proved it both ways in one request: the chord's leg 3
// stopped the body at 6.23 m of 9.00 m on four consecutive attempts, while the frontier's less-taut
// polyline walked the same ground with `cutByValidation=0`.
//
// So a breach is repaired LOCALLY. Re-searching the whole graph answers a local question globally, and
// it was costing the route: every attempt came back with the same chord and the same breach until the
// attempts ran out.
//
// GAME THREAD, on a nav-safe frame -- every rung re-validates, and validation makes engine calls.
namespace PathRepair {

// What a successful repair hands back. `ok == false` means every rung was tried or declined and the
// route is unchanged -- the caller re-costs and searches again, exactly as it did before this existed.
struct Result {
    bool  ok     = false;
    int   probes = 0;               // spent across EVERY rung attempted, win or lose; caller must
                                    // subtract this from its budget whether or not `ok`
    int   detail = 0;               // the winning rung's own number (waypoints spliced, cm reached...)
    const char* rung = nullptr;     // its name, for the caller's stats -- never user-facing text
    std::vector<FVec3>        poly; // the polyline the body walked
    PathValidate::LegReport   report;  // its validation: `ok && !truncated` by construction
};

// THE LADDER, in order, every rung entirely on the failure path:
//
//   1a. un-pull the failing leg (and, when interior, the corner it AIMED AT)
//   1b. replace the corner it DEPARTED FROM
//   2.  retreat to `badStopAt`, the engine's own resolved position
//   3.  the full corridor, with no string-pull at all
//
// Shortest thing that works is what gets returned, and **a route that validates on the chord never
// reaches here**, so nothing in this file can change one. That property is what made it safe to ship
// after a session in which three global changes interacted into a regression.
//
// EVERY RUNG IS ACCEPTED BY A MEASUREMENT, never by a comparison: a candidate is kept only when
// `PathValidate::CheckLegs` reports `ok && !truncated`, i.e. the body walked every leg of it. A rung
// that fails leaves the route exactly as it was, so the worst a rung can cost is its own probes.
//
// THE REACH OF THE LADDER WAS THE BUG (Session 97). It repaired 17 of 17 breaches it was offered in the
// tester's session, and all 7 "No path" results on the maps that otherwise route were breaches all
// three rungs declined by their own guards -- `firstBad == total`, a final-leg breach, every one.
//
// `poly` and `polyIdx` must be the same length: the index tells each corner which portal it came off,
// and a mapping that is silently out of step addresses the wrong stretch of corridor.
Result Mend(const std::vector<FVec3>& poly,
            const std::vector<int>& polyIdx,
            const std::vector<PathFunnel::Portal>& portals,
            const PathValidate::LegReport& rep,
            const FVec3& from, const FVec3& to,
            int probeBudget, float arrivalTol);

} // namespace PathRepair

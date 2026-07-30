#pragma once

#include <cstddef>
#include <vector>
#include "navigation/nav_types.h"
#include "navigation/nav_mesh.h"

// The string-pull half of routing, split out of path_search.cpp (Session 93) when that file hit 667
// lines against a 500-line cap. A* and corridor reconstruction stayed; the funnel, the passed-waypoint
// drop, the corner inset and the length invariants moved here. Pure geometry over a portal list -- no
// game calls except the inset's clearance test, and no state.
namespace PathFunnel {

using NavMesh::PolyId;

// A portal is the OPENING the route crosses, not the whole shared edge (Session 86). LEFT is always
// v[e] and RIGHT is always v[(e+1)%3]; that is forced by the engine's own containment test, not
// assumed. See the collection loop in path_search.cpp for the derivation.
struct Portal { FVec3 left, right; };

// Ground-plane equality, at the tolerance the funnel's duplicate-collapse needs.
bool SameXZ(const FVec3& a, const FVec3& b);

// Ground-plane length of a polyline. The funnel's whole claim is that it returns the SHORTEST path
// through a given corridor, so this is what checks the claim.
float PathLenXZ(const std::vector<FVec3>& pts);

// Run the funnel BOTH ways and keep the shorter path.
//
// THE POLARITY IS MEASURED, NOT DERIVED -- and it is now a SELF-CHECK rather than a crutch (S86). With
// the portals labelled from the mesh winding and the funnel's comparisons matching TriArea2's own sign,
// `as-labelled` must win every route with a real corridor; `flipped` on >= 2 portals means one of those
// two facts is wrong. `lenKept`/`lenOther` are reported so the log can say which.
void BestPolarity(const FVec3& start, const FVec3& end, const std::vector<Portal>& portals,
                  std::vector<FVec3>& out, bool& flipped, float& lenKept, float& lenOther);

// Drop leading waypoints the player has already walked past, returning how many went.
//
// The single biggest cause of the reversal complaint: 87 of 109 reversals across 1,059 archived routes
// were leg 0 -> leg 1, because a moving target makes the player re-press while walking and the route's
// first corner ends up behind them. MUST run AFTER any step that rebuilds the polyline -- Session 93
// found it was being bypassed on every breaching route, because the fallback swapped in a freshly built
// vector while this had only ever mutated the discarded one.
int DropPassedWaypoints(const FVec3& from, std::vector<FVec3>& poly);

// Pull interior corners off the boundary they are sitting on.
//
// MEASURED, not hypothesised (Session 93). A taut corner is by construction a PORTAL ENDPOINT, i.e. a
// mesh vertex on the edge of the walkable region -- and the engine's own border-clearance test
// (NavFootprint) refuses to let the body's footprint overlap such an edge, pushing the character back
// to tangency. So every taut corner the mod spoke was a point the engine actively pushes the player
// off. The tester's Giza log shows it outright: the breach leg ran (260.9,104.5) -> (266.2,104.8) and
// the same line lists `geom portals L|R: ... |(260.9,104.5) ... |(265.8,104.4)`.
//
// Each interior corner is moved along the bisector of its two legs by the body radius -- the engine's
// own quantity, from NavFootprint::BodyRadius(), never a chosen constant -- and the move is KEPT ONLY
// IF the footprint test is happier there. Endpoints never move: the first is the player's own position
// and the last is the target, which `/` also measures to (S76).
// Returns how many corners were inset.
int InsetCorners(std::vector<FVec3>& poly);

} // namespace PathFunnel

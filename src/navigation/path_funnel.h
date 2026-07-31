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
// `outIdx` (optional) receives, for each point in `out`, WHICH PORTAL it came from: -1 for the start,
// `portals.size()` for the end, and the portal index for every taut corner in between. That is what
// lets a caller undo the string-pull on ONE leg -- see PathFunnel::Unpull.
void BestPolarity(const FVec3& start, const FVec3& end, const std::vector<Portal>& portals,
                  std::vector<FVec3>& out, bool& flipped, float& lenKept, float& lenOther,
                  std::vector<int>* outIdx = nullptr);

// UNDO THE STRING-PULL ON ONE LEG (Session 96).
//
// The corridor A* returns is walkable BY CONSTRUCTION -- every portal in it was measured with the
// body's own footprint and sweep before the edge was ever expanded. The taut chord the funnel then
// draws across it is an OPTIMISATION, and on a corridor that bends inside wide triangles the chord can
// leave the walkable strip entirely. Measured on map 311: the chord's leg 3 stopped the body at 6.23 m
// of 9.00 m, four attempts running, while the frontier's less-taut polyline crossed the same ground
// with `cutByValidation=0`.
//
// So a breach is repaired LOCALLY: put the corridor's own portal midpoints back between the two corners
// the failing leg runs between, and leave every other leg taut. The response to a bad chord used to be
// to re-search the whole graph, which answers a local question globally and costs the route.
//
// It also REPLACES the corner the failing leg was aiming at, when that corner is interior, with its
// own portal's span midpoint. Splicing into the approach alone is a no-op whenever the unreachable
// thing is the corner -- which the log showed nine times running -- because a taut corner is a portal
// ENDPOINT and the crossing test never samples endpoints. The final point is never moved: it is the
// caller's destination and the `/` key measures to the same FVec3 (S76).
//
// Returns the number of waypoints spliced in; 0 means there was nothing between those corners to
// restore and the leg is as un-pulled as it can get.
int Unpull(const std::vector<FVec3>& poly, const std::vector<int>& idx,
           const std::vector<Portal>& portals, size_t badLeg, std::vector<FVec3>& out);

// REPLACE THE CORNER THE FAILING LEG DEPARTS FROM (Session 97).
//
// `Unpull` above can only ever move the corner a leg is aiming AT. That leaves the exact mirror image
// unrepairable, and it was 7 of the 11 "No path" results in the tester's session.
//
// Be precise about when `Unpull` bows out, because it is NOT "every final leg": its corner replacement
// is gated on `interior`, but its SPLICE still fires on a last leg whenever portals sit strictly
// between the two corners -- map 311 `seq=7` breached on leg 2 of 2 and `Unpull` repaired it 3 -> 4
// points. What it cannot do is anything at all when `idx[badLeg-1] + 1 > idx[badLeg] - 1`, i.e. the two
// corners come off adjacent portals. Then it returns 0 without even logging, and before this function
// existed that was the end of the route.
//
// The two cases are the SAME BAD CORNER seen from either side, and the log proves it inside one map.
// Map 321, target (47.0,-0.00,150.75), corner (47.0,-0.00,156.0) in every route:
//   from (44.60,157.01): breach on leg 1 -> `Unpull` replaces that corner -> OK, route spoken.
//   from (43.17,159.42): breach on leg 2, same corner now the DEPARTURE point -> no rung -> "No path",
//   5.2 m short of a 10 m route.
// Pass and fail on the same geometry, decided by which side of the corner the player is standing.
//
// WHEN TO PREFER IT: `badReached` near zero. That is the measured signature of a body that never left
// the corner it started on -- (47.0,156.0) gave `reached=0.27m` of a 5.25 m leg with the engine
// resolving the body 0.3 m BACKWARDS, which is depenetration and nothing else. Splicing waypoints
// further down such a leg cannot help; the first point is the one the body cannot stand on.
//
// The replacement is the portal's own span midpoint -- a point `EdgeClearSpan` MEASURED the body
// through -- exactly as `Unpull` uses for the aimed-at corner. Index 0 is never touched: that is the
// player's live position, not a corner, and it carries `idx == -1`.
//
// Returns 1 if the corner was replaced, 0 if there was nothing to replace it with.
int UnpullDeparture(const std::vector<FVec3>& poly, const std::vector<int>& idx,
                    const std::vector<Portal>& portals, size_t badLeg, std::vector<FVec3>& out);

// The corridor with NO string-pull at all: start, every portal's span midpoint in order, target.
//
// The last rung of the repair ladder, for when a breach on the FIRST leg leaves nothing else -- the
// re-cost is refused (it would strand the seed), Unpull has already failed, and `ProvenPrefix` on
// `firstBad == 1` yields one point so the frontier has nothing to speak either. Measured: that
// combination produced "No path" from 3 m away from a reachable exit, nine times.
//
// Wordier to walk than a taut route, and that is the trade: every point in it is one the body was
// measured to fit through, which is exactly what the chord across them is not.
int FullCorridor(const FVec3& from, const FVec3& to, const std::vector<Portal>& portals,
                 std::vector<FVec3>& out);

// Drop leading waypoints the player has already walked past, returning how many went.
//
// The single biggest cause of the reversal complaint: 87 of 109 reversals across 1,059 archived routes
// were leg 0 -> leg 1, because a moving target makes the player re-press while walking and the route's
// first corner ends up behind them. MUST run AFTER any step that rebuilds the polyline -- Session 93
// found it was being bypassed on every breaching route, because the fallback swapped in a freshly built
// vector while this had only ever mutated the discarded one.
// `idx`, when given, is kept in step with `poly` so the portal mapping survives the erase.
int DropPassedWaypoints(const FVec3& from, std::vector<FVec3>& poly, std::vector<int>* idx = nullptr);

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

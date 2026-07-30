#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "navigation/nav_types.h"
#include "navigation/nav_mesh.h"
#include "navigation/path_funnel.h"
#include "navigation/path_validate.h"

// Turning A*'s parent links into a polyline we are willing to SPEAK.
//
// Split out of path_search.cpp (Session 95), which owns the search itself and had reached its 500-line
// cap. Two jobs live here, and they belong together because the second is the first applied to a
// different end poly:
//
//   * `Build` -- walk the parent chain back from any poly the search reached and collect the portal
//     OPENINGS it crosses. The main route has always done this for the goal; the FRONTIER never did,
//     which was the defect (see below).
//   * `BuildFrontier` -- the "how far CAN they get" answer, built, string-pulled, VALIDATED, and cut
//     back to the part that passed.
//
// WHAT WAS WRONG (Session 95, from the tester's log). 18 of 35 routes in one session shipped as
// Frontier, 17 of them because "validation never passed" -- and the frontier route was emitted with NO
// validation at all. Worse, it funnelled `best.portals`, the corridor to the GOAL poly, toward a point
// on a DIFFERENT poly, so the portal sequence and the endpoint disagreed; in the genuinely-unreachable
// case the portal list was empty and the "route" was a bare straight line from the player to a point
// several polys away. Either way the mod spoke legs it had just proved unwalkable, which is exactly the
// failure Plan::Frontier was introduced to prevent -- it announced the shortfall while walking the
// player through a wall to reach it.
//
// GAME THREAD ONLY -- `BuildFrontier` validates, and validation makes engine calls.
namespace PathCorridor {

using NavMesh::PolyId;

// One step of A*'s parent chain: how the search arrived at a poly.
struct Came {
    PolyId parent = NavMesh::kNoPoly;
    int    edge   = -1;      // edge of `parent` we crossed to get here
};

// One portal, plus where it came from, so a breaching leg can be traced back to the edge to ban.
struct PortalRef {
    PathFunnel::Portal p;
    PolyId poly;   // the parent whose edge this is
    int    edge;
};

using CameMap = std::unordered_map<PolyId, Came>;

// Walk `came` back from `end` to the seed and collect the portal openings the chain crosses.
// `clipped` counts edges whose clear span was narrower than the whole edge, `blocked` edges where no
// part tested clear (the full edge is kept for those -- a hole would let the funnel thread an
// unvalidated chord across the gap).
void Build(const CameMap& came, PolyId end,
           std::vector<PolyId>& chain, std::vector<PortalRef>& portals,
           int& clipped, int& blocked);

// How many POINTS of a polyline the validator actually proved walkable.
//
// A breach on leg k means legs 1..k-1 are clear, i.e. points [0, k-1] -- so k points. A truncated
// report means only `checked` legs were ever tested, so `checked + 1` points. Anything longer is a
// claim the report does not support, and speaking it is the bug this exists to stop.
size_t ProvenPrefix(const PathValidate::LegReport& rep, size_t points);

struct FrontierRoute {
    std::vector<FVec3> poly;                  // empty => nothing provable; the caller must say No path
    float  shortfall = 0.0f;                  // metres from where the route REALLY ends to the goal
    int    probes    = 0;
    PolyId endPoly   = NavMesh::kNoPoly;
    size_t cutFrom   = 0;                     // points the validation cut off; 0 == none
};

// Route from `from` toward the goal, ending at the closest point on `frontierPoly`, then keep only the
// part that validates. The point TESTED is the point ARRIVED at (S76): ClosestPointOnPoly clamps to the
// triangle, so this is a place on the mesh rather than a centroid several metres away.
bool BuildFrontier(const CameMap& came, PolyId frontierPoly,
                   const FVec3& from, const FVec3& to, int probeBudget,
                   FrontierRoute& out);

} // namespace PathCorridor

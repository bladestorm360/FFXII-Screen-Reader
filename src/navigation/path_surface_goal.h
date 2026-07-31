#pragma once

#include <vector>

#include "navigation/nav_types.h"
#include "navigation/nav_mesh.h"
#include "navigation/path_corridor.h"

// A TRANSITION'S DESTINATION IS A SURFACE, NOT A POINT (the Session 98 diagnosis, finally acted on).
//
// A map-jump surface fires when the player walks onto ANY part of it (S64). The exit record carries
// ONE point of it -- `MapQuery::NearestPointOnSurface`, the nearest tagged VERTEX in XZ -- and that
// point answers "how far away is this exit" correctly and "where should the route end" wrongly, in
// two independent ways:
//
//   1. A VERTEX IS NOT A PLACE TO STAND. It is on the walkable boundary by construction, so the
//      body can never finish on it; `kArrivalTol = 3.0` has been absorbing that on every map since
//      S75.
//   2. STRAIGHT-LINE NEAREST IS NOT WALKING NEAREST. On map 315's 27 m seam the nearest vertex is
//      the corner the walkable approach reaches LAST, so the route drives 20 m ALONG the surface to
//      get to it. The route arrives; the arithmetic says it has not, and a mid-route replan from
//      the bank comes back "No path" 16.4 m short.
//
// This module answers the other question: given that the search has already TOUCHED the surface,
// where does walking first reach it? The endpoint is the opening the corridor crosses to get on --
// a portal between two mesh triangles -- and nothing about it is derived from the route's own
// progress, which is the S99 rule that killed the Session 98 seam pass. See path_search.h.
//
// WHY NOT A GOAL SET INSIDE A*: because a route that works today must not change. The surface
// member is OBSERVED during the search (first pop, no decision altered) and this route is only
// built where the ordinary search failed to produce a route worth speaking. A validated route
// returns before this is ever called.
//
// GAME THREAD ONLY -- it validates, and validation makes engine calls.
namespace PathSurfaceGoal {

// How far INSIDE the surface to aim, past the portal the corridor crosses onto it. The portal is the
// shared edge of two triangles, so a point exactly on it belongs to both and `FindPolyAt` may answer
// either; half a metre in makes the arrival poly unambiguously the transition surface. Clamped back
// onto the triangle afterwards, so a seam poly narrower than this is still handled exactly.
constexpr float kStepIn = 0.5f;

struct Result {
    std::vector<FVec3> poly;                          // empty => nothing shippable; caller falls through
    NavMesh::PolyId    endPoly   = NavMesh::kNoPoly;  // the surface poly the route ends on
    NavMesh::PolyId    entryFrom = NavMesh::kNoPoly;  // the poly it steps onto the surface FROM
    int                entryEdge = -1;
    FVec3              aim{};                         // the arrival point, on the surface
    int                probes    = 0;                 // spent whether or not the route was accepted
};

// Build and PROVE a route that ends where walking first touches `surfacePoly`.
//
// Returns true only when the polyline validated whole -- `ok && !truncated` -- through the same
// PathValidate::CheckLegs the main route runs, with the same corner handling and the same arrival
// tolerance. A partial answer is never returned: the caller's frontier already owns that case, and
// speaking a partial route as a route is the S73/S74 failure Plan::Frontier exists to prevent.
bool Route(const PathCorridor::CameMap& came, NavMesh::PolyId surfacePoly,
           const FVec3& from, int probeBudget, float arrivalTol, Result& out);

} // namespace PathSurfaceGoal

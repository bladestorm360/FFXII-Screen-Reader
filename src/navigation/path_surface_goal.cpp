#include "navigation/path_surface_goal.h"
#include "navigation/path_funnel.h"
#include "navigation/path_validate.h"
#include "core/logger.h"

#include <cmath>
#include <cstdio>

namespace PathSurfaceGoal {

namespace {

using NavMesh::PolyId;
using NavMesh::kNoPoly;

// The point at which the corridor crosses ONTO the surface, pushed just inside it.
//
// The clear SPAN rather than the whole edge, for the reason path_corridor.cpp gives: a shared edge
// on this mesh runs 8-16 m and A* only certifies that a crossing exists somewhere on it. Falling
// back to the full edge when no span reads is the same fail-open the corridor builder uses -- the
// body walk downstream is what decides, and inventing a refusal here would hide a route that walks.
bool EntryPoint(PolyId parent, int edge, PolyId surfacePoly, FVec3& out) {
    FVec3 a{}, b{};
    if (!NavMesh::EdgeClearSpan(parent, edge, surfacePoly, a, b) &&
        !NavMesh::EdgePortal(parent, edge, a, b))
        return false;

    FVec3 mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };

    // Step INTO the surface along the ground plane, toward the poly's own centre. Y is not steered
    // by: the clamp below takes it from the triangle's plane, which is the only honest source.
    FVec3 c{};
    if (NavMesh::PolyCentroid(surfacePoly, c)) {
        const float dx = c.x - mid.x, dz = c.z - mid.z;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len > 1e-3f) {
            const float t = (kStepIn < len) ? kStepIn : len;
            mid.x += dx / len * t;
            mid.z += dz / len * t;
        }
    }

    // Clamp onto the triangle. THE POINT TESTED IS THE POINT ARRIVED AT (S76): ClosestPointOnPoly
    // both bounds the step-in and supplies the Y from the surface's own plane.
    if (!NavMesh::ClosestPointOnPoly(surfacePoly, mid.x, mid.z, out)) out = mid;
    return true;
}

} // namespace

bool Route(const PathCorridor::CameMap& came, PolyId surfacePoly,
           const FVec3& from, int probeBudget, float arrivalTol, Result& out) {
    out = Result{};
    if (surfacePoly == kNoPoly) return false;

    const auto it = came.find(surfacePoly);
    if (it == came.end() || it->second.parent == kNoPoly || it->second.edge < 0) return false;
    out.entryFrom = it->second.parent;
    out.entryEdge = it->second.edge;

    if (!EntryPoint(out.entryFrom, out.entryEdge, surfacePoly, out.aim)) return false;

    // THE CORRIDOR IS REBUILT FOR THIS END POLY -- the same helper the frontier uses, for the same
    // reason: funnelling one poly's portal sequence toward a point on another is not a path in any
    // sense the funnel can repair.
    std::vector<PolyId>                chain;
    std::vector<PathCorridor::PortalRef> portals;
    int clipped = 0, blocked = 0;
    PathCorridor::Build(came, surfacePoly, chain, portals, clipped, blocked);

    std::vector<PathFunnel::Portal> plain;
    plain.reserve(portals.size());
    for (const PathCorridor::PortalRef& pr : portals) plain.push_back(pr.p);

    bool  anomaly = false;
    float lenKept = 0.0f, lenOther = 0.0f;
    PathFunnel::BestPolarity(from, out.aim, plain, out.poly, anomaly, lenKept, lenOther);
    // THE SAME ORDER AS THE MAIN ROUTE, and for the same reasons. InsetCorners pulls taut corners
    // off the boundary they sit on -- the Session 100 fix without which map 315's bank route stops
    // one body radius short of every funnel corner -- and it runs BEFORE the passed-waypoint drop,
    // because an inset can move a corner past the player and the drop is what notices.
    const int inset     = PathFunnel::InsetCorners(out.poly);
    const int droppedWp = PathFunnel::DropPassedWaypoints(from, out.poly);

    // THE FULL VALIDATION, NOT A LIGHTER ONE. Same function, same arrival tolerance, same corner
    // footprint tests, same adjacency march, same pinned-corner acceptance. A route that reaches a
    // surface is not owed an easier proof than one that reaches a poly.
    const PathValidate::LegReport rep = PathValidate::CheckLegs(out.poly, probeBudget, arrivalTol);
    out.probes = rep.probes;

    const bool accept = rep.ok && !rep.truncated && out.poly.size() >= 2;

    // This call site used to discard the polarity flag unlogged -- the one BestPolarity consumer
    // whose accepted result is spoken, beacon-seeded and auto-walked, and the one with no anomaly
    // visibility. The suffix closes that: the returned polyline is as-labelled either way.
    char m[448];
    snprintf(m, sizeof(m),
             "surface-goal: surface poly %d entered from %d:%d, aim=(%.2f,%.2f,%.2f) "
             "corners=%zu/%zu inset=%d droppedWp=%d probes=%d -> %s%s%s",
             surfacePoly, out.entryFrom, out.entryEdge, out.aim.x, out.aim.y, out.aim.z,
             out.poly.size(), plain.size(), inset, droppedWp, rep.probes,
             accept ? "ACCEPTED" : "REJECTED",
             accept                   ? ""
             : rep.truncated          ? " (budget ran out -- NOT verified)"
             : (rep.firstBad != 0)    ? " (breach; the frontier owns this case)"
                                      : " (nothing to speak)",
             anomaly ? " | MESH LABELLING ANOMALY: mirrored funnel measured shorter; as-labelled kept"
                     : "");
    Log::Write("NAV-ROUTE", m);

    if (!accept) { out.poly.clear(); return false; }

    // The end poly is read back from the geometry rather than assumed, exactly as BuildFrontier
    // does: what the route ends ON is a question about the point, not about the search.
    out.endPoly = NavMesh::FindPolyAt(out.poly.back().x, out.poly.back().y, out.poly.back().z);
    if (out.endPoly == kNoPoly) out.endPoly = surfacePoly;
    return true;
}

} // namespace PathSurfaceGoal

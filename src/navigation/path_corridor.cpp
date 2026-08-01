#include "navigation/path_corridor.h"
#include "navigation/nav_common.h"
#include "navigation/nav_footprint.h"
#include "navigation/path_march.h"

#include <algorithm>

namespace PathCorridor {

void Build(const CameMap& came, PolyId end,
           std::vector<PolyId>& chain, std::vector<PortalRef>& portals,
           int& clipped, int& blocked) {
    chain.clear();
    portals.clear();
    clipped = 0;
    blocked = 0;
    if (end == NavMesh::kNoPoly) return;

    for (PolyId p = end; p != NavMesh::kNoPoly; ) {
        chain.push_back(p);
        // A parent chain cannot cycle -- A* only writes `came[n]` when it improves n's g-score -- but
        // this walks a map that a torn read could corrupt, and one bad link would hang the game thread.
        if (chain.size() > came.size() + 1) break;
        auto it = came.find(p);
        if (it == came.end() || it->second.parent == NavMesh::kNoPoly) break;
        p = it->second.parent;
    }
    std::reverse(chain.begin(), chain.end());

    // LEFT IS ALWAYS v[e]; RIGHT IS ALWAYS v[(e+1)%3]. Not an assumption about the mesh -- it is forced
    // by the engine's own containment test (MapQuery::PolyContainsXZDetail replicates FUN_002324f0,
    // whose crossY is identically -TriArea2(v[i], v[j], p), so an interior point lies to the RIGHT of
    // v[e] -> v[e+1] under path_funnel's convention). Every time, no test.
    //
    // AND THE PORTAL IS THE OPENING, NOT THE WHOLE EDGE (S86). A* certifies that a crossing EXISTS on
    // each shared edge; it does not certify where. On this mesh an edge runs 8-16 m, and the field log
    // caught the taut path threading a portal 4.8 m from the only point that had been tested.
    portals.reserve(chain.size());
    for (size_t i = 1; i < chain.size(); ++i) {
        auto it = came.find(chain[i]);
        if (it == came.end() || it->second.edge < 0) continue;
        FVec3 fullA{}, fullB{};
        if (!NavMesh::EdgePortal(it->second.parent, it->second.edge, fullA, fullB)) continue;
        FVec3 v0 = fullA, v1 = fullB;
        if (NavMesh::EdgeClearSpan(it->second.parent, it->second.edge, chain[i], v0, v1)) {
            if (!PathFunnel::SameXZ(v0, fullA) || !PathFunnel::SameXZ(v1, fullB)) ++clipped;
        } else {
            ++blocked;
            v0 = fullA; v1 = fullB;
        }
        portals.push_back(PortalRef{ PathFunnel::Portal{ v0, v1 }, it->second.parent, it->second.edge });
    }
}

size_t ProvenPrefix(const PathValidate::LegReport& rep, size_t points) {
    if (points < 2) return points;
    if (!rep.ok && rep.firstBad >= 1) return (rep.firstBad < points) ? rep.firstBad : points;
    if (rep.truncated)                return (rep.checked + 1 < points) ? (rep.checked + 1) : points;
    return points;
}

bool BuildFrontier(const CameMap& came, PolyId frontierPoly,
                   const FVec3& from, const FVec3& to, int probeBudget,
                   FrontierRoute& out) {
    out = FrontierRoute{};
    if (frontierPoly == NavMesh::kNoPoly) return false;

    FVec3 fpt{};
    if (!NavMesh::ClosestPointOnPoly(frontierPoly, to.x, to.z, fpt)) return false;

    // THE CORRIDOR IS REBUILT FOR THIS END POLY. The old code reused the corridor to the GOAL and then
    // funnelled it toward a point on a different poly -- a portal sequence and an endpoint that do not
    // belong to each other, which is not a path in any sense the funnel can repair.
    std::vector<PolyId>    chain;
    std::vector<PortalRef> portals;
    int clipped = 0, blocked = 0;
    Build(came, frontierPoly, chain, portals, clipped, blocked);

    std::vector<PathFunnel::Portal> plain;
    plain.reserve(portals.size());
    for (const PortalRef& pr : portals) plain.push_back(pr.p);

    bool  flipped = false;
    float lenKept = 0.0f, lenOther = 0.0f;
    PathFunnel::BestPolarity(from, fpt, plain, out.poly, flipped, lenKept, lenOther);
    // Inset BEFORE dropping passed waypoints, because an inset can move a corner past the player and
    // the drop is what notices -- same order as the main route.
    PathFunnel::InsetCorners(out.poly);
    PathFunnel::DropPassedWaypoints(from, out.poly);

    const PathValidate::LegReport rep = PathValidate::CheckLegs(out.poly, probeBudget);
    out.probes = rep.probes;

    const size_t keep = ProvenPrefix(rep, out.poly.size());
    if (keep < out.poly.size()) {
        out.cutFrom = out.poly.size() - keep;
        out.poly.resize(keep);
    }
    if (out.poly.size() < 2) { out.poly.clear(); return false; }

    // The shortfall is measured from where the route REALLY ends, not from the point we aimed at. That
    // is what makes "Blocked, N steps" an honest number after a cut.
    out.endPoly = NavMesh::FindPolyAt(out.poly.back().x, out.poly.back().y, out.poly.back().z);
    if (out.endPoly == NavMesh::kNoPoly) out.endPoly = frontierPoly;
    out.shortfall = NavCommon::Distance2D(out.poly.back(), to);
    return true;
}

CorridorMarch MarchCorridor(const FVec3& from, const FVec3& to,
                            const std::vector<PathFunnel::Portal>& portals, float arrivalTol) {
    CorridorMarch out;
    if (portals.empty()) return out;

    // THE SAME POLYLINE `repair[full-corridor]` BUILDS, from the same function -- one definition of
    // "the corridor as a walkable line", so this check and that rung can never describe different
    // things. The difference is only that this one costs nothing and is asked FIRST.
    std::vector<FVec3> pts;
    if (PathFunnel::FullCorridor(from, to, portals, pts) <= 0 || pts.size() < 2) return out;
    out.hops = pts.size() - 1;

    // Tolerances mirror PathValidate::CheckLegs exactly: the body's own reach mid-route, the
    // caller's arrival tolerance on the last hop. Two instruments that forgive different things
    // would disagree about routes neither of them objects to.
    const float tol = NavFootprint::BodyRadius() + PathMarch::kEndSlack;

    for (size_t i = 1; i < pts.size(); ++i) {
        const bool  last   = (i + 1 == pts.size());
        const float legTol = (last && arrivalTol > tol) ? arrivalTol : tol;

        const PathMarch::MarchResult m = PathMarch::MarchLeg(pts[i - 1], pts[i], legTol);
        out.grazes += m.grazes;
        // FAIL OPEN. An undecidable hop is counted, never promoted to a breach -- see the header.
        if (m.verdict != PathMarch::MarchVerdict::Breach) {
            if (m.verdict == PathMarch::MarchVerdict::NoVerdict) ++out.noVerdict;
            continue;
        }
        out.breached = true;
        out.hop      = i;
        out.hitPoint = m.hitPoint;
        out.fromPoly = m.fromPoly;
        out.edge     = m.edge;
        out.nbr      = m.nbr;
        out.nbrEff   = m.nbrEff;
        return out;                 // the FIRST refusal is the one to price; later ones are its shadow
    }
    return out;
}

} // namespace PathCorridor

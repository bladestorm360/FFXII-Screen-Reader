#include "navigation/path_search.h"
#include "navigation/nav_mesh.h"
#include "navigation/nav_footprint.h"
#include "navigation/map_query.h"
#include "navigation/nav_common.h"
#include "navigation/path_funnel.h"
#include "navigation/path_validate.h"
#include "core/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <queue>
#include <unordered_map>
#include <vector>

namespace PathSearch {

namespace {

using NavMesh::PolyId;
using NavMesh::kNoPoly;
using PathFunnel::Portal;
using PathFunnel::SameXZ;

// Expansion budget. A poly is far coarser than the old 1.5 m cell -- a whole corridor is often two
// triangles -- so this is generous. It exists only so a torn read cannot spin forever.
constexpr int kMaxExpand = 20000;

// THE RE-SEARCH IS CAPPED BY WORK, NOT BY ATTEMPTS (Session 93).
//
// A retry count is the wrong bound because attempts are not equal: four passes over the Giza corridor
// (1827 expands each) is a different proposition from four passes over a small room, and "slower than
// vanilla = OUR code" is a standing CRITICAL rule. So attempts stop when either bound is hit, and the
// log says which -- a map that habitually exhausts the work budget announces itself instead of quietly
// costing frames.
constexpr int kMaxAttempts   = 4;
constexpr int kMaxTotalExpand = 40000;
// Validation probe budget for the whole request. The Giza route's 140 legs need ~280 probes; this leaves
// room for four attempts at that scale without letting a pathological map run away.
constexpr int kProbeBudget   = 1600;

struct Node {
    float  f;
    PolyId p;
    bool operator>(const Node& o) const { return f > o.f; }
};

struct Came {
    PolyId parent = kNoPoly;
    int    edge   = -1;      // edge of `parent` we crossed to get here
};

// A portal the search must not use on a later attempt, because the taut path through it turned out not
// to be walkable. Scoped to ONE request -- never cached across presses, because the obstacle may be a
// door that opens, and a permanent ban would be exactly the "learned label" this project forbids.
struct BannedEdge { PolyId poly; int edge; };

inline float Dist3(const FVec3& a, const FVec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// One portal, plus where it came from, so a breaching leg can be traced back to the edge to ban.
struct PortalRef {
    Portal p;
    PolyId poly;   // the parent whose edge this is
    int    edge;
};

// ---- One A* pass ---------------------------------------------------------------------------------
struct PassResult {
    bool   reachedGoal = false;
    PolyId endPoly     = kNoPoly;
    PolyId bestNear    = kNoPoly;
    float  bestNearD   = -1.0f;
    bool   usedFallback = false;
    FVec3  fallbackPoint{};
    std::vector<PolyId>    chain;
    std::vector<PortalRef> portals;
    int clipped = 0, blocked = 0;
    const char* fail = nullptr;   // non-null => this pass produced no corridor at all
};

} // namespace

Plan Run(const FVec3& from, const FVec3& to, uint32_t epoch,
         float bandLo, float bandHi, float reachRadius,
         std::vector<FVec3>& rawPoly, std::vector<FVec3>& outPoly, Stats& stats) {
    rawPoly.clear();
    outPoly.clear();
    if (!MapQuery::HasWorld()) return Plan::NoPath;
    NavMesh::EnsureEpoch(epoch);

    // Both endpoints are located WITH THEIR Y. That single fact is what separates the Highhall seam
    // (7.8 m up) from the floor beneath it -- the old grid had only an (x,z) and could not.
    //
    // FindPolyAt deliberately keeps the RAW walkable test and gets no footprint check. A player standing
    // in a doorway or hard against a wall is legitimately within a body radius of a boundary, and
    // rejecting their own poly here would return `no-start-poly` -> "No path" from a position they are
    // demonstrably standing on. You can always leave the poly you are on.
    const PolyId start = NavMesh::FindPolyAt(from.x, from.y, from.z);
    const PolyId goal  = NavMesh::FindPolyAt(to.x,   to.y,   to.z);
    stats.startPoly = start;
    stats.goalPoly  = goal;

    if (start == kNoPoly) { stats.pass = "no-start-poly"; return Plan::NoPath; }

    // ---- A target with NO POLYGON OF ITS OWN is still reachable ------------------------------------
    // A notice board bolted to a wall at y=2.0, a chest on a ledge, an NPC behind a counter: the
    // object's own point is off the mesh, so FindPolyAt legitimately returns nothing. That is NOT
    // "no route" -- the game shows an interact prompt, so a place to stand exists.
    //
    // Route by the INTERACTION CYLINDER instead (S73: interaction distance is a cylinder, not a point).
    const bool goalOffMesh = (goal == kNoPoly);
    if (goalOffMesh && reachRadius <= 0.01f) {
        stats.pass = "no-goal-poly-no-reach";
        return Plan::NoPath;
    }

    FVec3 goalC{};
    if (goalOffMesh) {
        goalC = to;   // heuristic reference: the target's own point, which is a real world position
    } else if (!NavMesh::PolyCentroid(goal, goalC)) {
        stats.pass = "goal-unreadable";
        return Plan::NoPath;
    }

    const bool haveBand  = (bandHi >= bandLo);
    const bool haveReach = (reachRadius > 0.01f);

    std::vector<BannedEdge> banned;
    PassResult best{};                 // the last pass that produced a corridor at all
    std::vector<FVec3> bestPoly;
    PathValidate::LegReport bestReport{};
    int probesLeft = kProbeBudget;

    // ---- attempt loop: validate, then ban the offending portal and search AGAIN -------------------
    //
    // THIS IS THE STRUCTURAL FIX (Session 93). Validation used to sit after the search as a lambda over
    // a corridor A* had already committed to, where its only possible outputs were accept, substitute
    // one pre-built alternative, or accept the thing it had just disproved -- and it took the third
    // option on 9 of 53 routes in the tester's log, shipping a path it had proved unwalkable with no
    // change to the speech. A validator in that position can never route AROUND anything.
    //
    // Moving the decision into a loop is what turns "report the problem better" into "solve it": a
    // failed validation now changes the GRAPH the next pass searches, so the detour is found by A*
    // itself rather than approximated by a fallback polyline.
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        stats.attempts = attempt;

        PassResult pr;
        pr.bestNear  = start;

        // Centroids are read once per poly and reused by the heuristic, the edge cost and the goal test.
        std::unordered_map<PolyId, FVec3> centroid;
        auto Centroid = [&](PolyId p, FVec3& out) -> bool {
            auto it = centroid.find(p);
            if (it != centroid.end()) { out = it->second; return true; }
            FVec3 c{};
            if (!NavMesh::PolyCentroid(p, c)) return false;
            centroid.emplace(p, c);
            out = c;
            return true;
        };

        // THE GOAL IS THE TARGET'S OWN POLY. Nothing else. (Accepting any poly with a corner in reach
        // stopped the search on its first pop -- `expands=1 touched=0` on 20 of 30 routes.)
        auto IsGoal = [&](PolyId p) -> bool { return !goalOffMesh && p == goal; };

        // The fallback, for a target the player cannot stand on. Recorded DURING the search: the first
        // qualifying poly A* pops is the one it would have walked to anyway. The point tested IS the
        // point arrived at (S76).
        PolyId fallbackPoly = kNoPoly;
        FVec3  fallbackPoint{};
        auto NoteFallback = [&](PolyId p) {
            if (fallbackPoly != kNoPoly || !haveBand || !haveReach) return;
            FVec3 cp{};
            if (!NavMesh::ClosestPointOnPoly(p, to.x, to.z, cp)) return;
            if (cp.y < bandLo || cp.y > bandHi) return;                    // engine's vertical gate
            const float dx = cp.x - to.x, dz = cp.z - to.z;
            if (dx * dx + dz * dz >= reachRadius * reachRadius) return;    // cylinder: Y excluded
            fallbackPoly  = p;
            fallbackPoint = cp;
        };

        auto IsBanned = [&](PolyId p, int e) -> bool {
            for (const BannedEdge& b : banned) if (b.poly == p && b.edge == e) return true;
            return false;
        };

        std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
        std::unordered_map<PolyId, float> gScore;
        std::unordered_map<PolyId, Came>  came;

        FVec3 startC{};
        if (!Centroid(start, startC)) { stats.pass = "start-unreadable"; return Plan::NoPath; }

        gScore[start] = 0.0f;
        came[start] = Came{};
        open.push(Node{ Dist3(startC, goalC), start });

        PolyId reached = kNoPoly;
        pr.bestNearD = Dist3(startC, goalC);

        while (!open.empty() && stats.expands < kMaxTotalExpand) {
            const Node cur = open.top();
            open.pop();
            const PolyId p = cur.p;
            ++stats.expands;

            if (IsGoal(p)) { reached = p; break; }
            NoteFallback(p);
            if (goalOffMesh && fallbackPoly != kNoPoly) break;

            FVec3 pc{};
            if (!Centroid(p, pc)) continue;
            const float dGoal = Dist3(pc, goalC);
            if (dGoal < pr.bestNearD) { pr.bestNearD = dGoal; pr.bestNear = p; }

            const auto gIt = gScore.find(p);
            if (gIt == gScore.end()) continue;
            const float gCur = gIt->second;

            for (int e = 0; e < 3; ++e) {
                const PolyId n = NavMesh::Neighbor(p, e);
                if (n == kNoPoly) continue;                 // boundary edge: map edge or wall

                FVec3 nc{};
                if (!Centroid(n, nc)) continue;

                // COST THE CROSSING, NOT THE CENTROID HOP. On this mesh a triangle is often an entire
                // corridor, so centroid-to-centroid optimises a quantity that is not the distance
                // walked. The heuristic stays plain Euclidean, so the search is still admissible.
                FVec3 mid{};
                const float step = NavMesh::EdgeMidpoint(p, e, mid)
                                       ? Dist3(pc, mid) + Dist3(mid, nc)
                                       : Dist3(pc, nc);
                const float tentative = gCur + step;

                const auto nIt = gScore.find(n);
                if (nIt != gScore.end() && nIt->second <= tentative) continue;

                // The banned set is consulted at the EXISTING passability call site, so a ban is
                // indistinguishable to A* from a wall -- which is the point. It routes around a ban the
                // same way it routes around geometry, by not expanding through it.
                if (IsBanned(p, e)) continue;

                // Walkability + the body test. EdgePassable now asks whether the CHARACTER fits, not
                // whether a line can be drawn: floor adjacency knows nothing about doors, and a hairline
                // ray knows nothing about body width.
                ++stats.rays;
                if (!NavMesh::EdgePassable(p, e, n)) continue;

                ++stats.touched;
                gScore[n] = tentative;
                came[n] = Came{ p, e };
                open.push(Node{ tentative + Dist3(nc, goalC), n });
            }
        }

        if (reached == kNoPoly && fallbackPoly != kNoPoly) {
            reached          = fallbackPoly;
            pr.usedFallback  = true;
            pr.fallbackPoint = fallbackPoint;
            char fm[208];
            snprintf(fm, sizeof(fm),
                     "mesh: goal %s; standing at poly %d (%.2f,%.2f,%.2f), %.2fm from it",
                     goalOffMesh ? "point is OFF-MESH (no polygon of its own -- wall-mounted or elevated)"
                                 : "poly unreachable",
                     fallbackPoly, fallbackPoint.x, fallbackPoint.y, fallbackPoint.z,
                     NavCommon::Distance2D(fallbackPoint, to));
            Log::Write("NAV-ROUTE", fm);
        }

        if (reached == kNoPoly) {
            pr.fail = (stats.expands >= kMaxTotalExpand) ? "budget"
                    : goalOffMesh                        ? "off-mesh-nothing-in-reach"
                                                         : "unreachable";
            best = pr;
            break;                     // no corridor at all -> banning cannot help; go to the frontier
        }

        pr.reachedGoal = true;
        pr.endPoly     = reached;

        // ---- Reconstruct: STRING-PULL the corridor -------------------------------------------------
        for (PolyId p = reached; p != kNoPoly; ) {
            pr.chain.push_back(p);
            auto it = came.find(p);
            if (it == came.end() || it->second.parent == kNoPoly) break;
            p = it->second.parent;
        }
        std::reverse(pr.chain.begin(), pr.chain.end());

        // Collect the portals the route crosses, each as a LEFT/RIGHT pair.
        //
        // LEFT IS ALWAYS v[e]; RIGHT IS ALWAYS v[(e+1)%3]. Not an assumption about the mesh -- it is
        // forced by the engine's own containment test (MapQuery::PolyContainsXZDetail replicates
        // FUN_002324f0, whose crossY is identically -TriArea2(v[i], v[j], p), so an interior point lies
        // to the RIGHT of v[e] -> v[e+1] under this file's convention). Every time, no test.
        //
        // AND THE PORTAL IS THE OPENING, NOT THE WHOLE EDGE (S86). A* certifies that a crossing EXISTS
        // on each shared edge; it does not certify where. On this mesh an edge runs 8-16 m, and the field
        // log caught the taut path threading a portal 4.8 m from the only point that had been tested.
        pr.portals.reserve(pr.chain.size());
        for (size_t i = 1; i < pr.chain.size(); ++i) {
            auto it = came.find(pr.chain[i]);
            if (it == came.end() || it->second.edge < 0) continue;
            FVec3 fullA{}, fullB{};
            if (!NavMesh::EdgePortal(it->second.parent, it->second.edge, fullA, fullB)) continue;
            FVec3 v0 = fullA, v1 = fullB;
            if (NavMesh::EdgeClearSpan(it->second.parent, it->second.edge, pr.chain[i], v0, v1)) {
                if (!SameXZ(v0, fullA) || !SameXZ(v1, fullB)) ++pr.clipped;
            } else {
                // No part of this edge tested clear, yet the search crossed it. Keep the full edge: a
                // hole in the sequence would let the funnel thread an unvalidated chord across the gap.
                ++pr.blocked;
                v0 = fullA; v1 = fullB;
            }
            pr.portals.push_back(PortalRef{ Portal{ v0, v1 }, it->second.parent, it->second.edge });
        }

        // ---- string-pull, then repair the corners, then validate -----------------------------------
        std::vector<Portal> plain;
        plain.reserve(pr.portals.size());
        for (const PortalRef& pref : pr.portals) plain.push_back(pref.p);

        bool  flipped = false;
        float lenKept = 0.0f, lenOther = 0.0f;
        std::vector<FVec3> poly;
        PathFunnel::BestPolarity(from, to, plain, poly, flipped, lenKept, lenOther);

        // Pull corners off the boundary BEFORE dropping passed waypoints, because an inset can move a
        // corner past the player and the drop is what notices.
        const int inset = PathFunnel::InsetCorners(poly);
        // AFTER any step that rebuilds or moves the polyline. The old code ran this once, before a
        // fallback swapped in a freshly built vector -- so on every breaching route the S78 leg-0
        // reversal fix was silently bypassed.
        const int droppedWp = PathFunnel::DropPassedWaypoints(from, poly);

        {
            char fm[288];
            snprintf(fm, sizeof(fm),
                     "funnel: attempt %d polarity=%s kept=%.1fm other=%.1fm corners=%zu/%zu "
                     "clipped=%d blocked=%d inset=%d droppedWp=%d%s",
                     attempt, flipped ? "FLIPPED" : "as-labelled", lenKept, lenOther,
                     poly.size(), plain.size(), pr.clipped, pr.blocked, inset, droppedWp,
                     (flipped && plain.size() >= 2)
                         ? "   <== POLARITY SELF-CHECK FAILED: labelling and comparison signs disagree"
                         : "");
            Log::Write("NAV-ROUTE", fm);
        }

        const PathValidate::LegReport rep = PathValidate::CheckLegs(poly, probesLeft);
        probesLeft -= rep.probes;
        stats.rays += rep.probes;

        {
            char vm[288];
            snprintf(vm, sizeof(vm),
                     "validate: attempt %d legs checked=%zu/%zu probes=%d worstFrac=%.2f %s%s",
                     attempt, rep.checked, rep.total, rep.probes, rep.worstFraction,
                     rep.ok ? "OK" : "BREACH",
                     rep.truncated ? "  <== TRUNCATED: budget ran out, remaining legs NOT tested" : "");
            Log::Write("NAV-ROUTE", vm);
        }

        best       = pr;
        bestPoly   = poly;
        bestReport = rep;

        if (rep.ok && !rep.truncated) {
            rawPoly = poly;
            break;                      // fully verified -- this is the route
        }
        if (rep.ok) break;              // truncated but no breach found; the log already said so

        // Ban the portal the breaching leg crosses. Which one is a geometric question: the leg that
        // failed runs between two taut corners, and the portal it crosses is the one whose span sits
        // closest to that leg's midpoint. Ban that (poly, edge) and search again.
        if (rep.firstBad == 0 || rep.firstBad >= poly.size() || plain.empty()) break;
        const FVec3 a = poly[rep.firstBad - 1], b = poly[rep.firstBad];
        const FVec3 legMid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };
        size_t bestIdx = 0;
        float  bestD2  = -1.0f;
        for (size_t k = 0; k < pr.portals.size(); ++k) {
            const Portal& q = pr.portals[k].p;
            const float mx = (q.left.x + q.right.x) * 0.5f, mz = (q.left.z + q.right.z) * 0.5f;
            const float dx = mx - legMid.x, dz = mz - legMid.z;
            const float d2 = dx * dx + dz * dz;
            if (bestD2 < 0.0f || d2 < bestD2) { bestD2 = d2; bestIdx = k; }
        }
        const PortalRef& kill = pr.portals[bestIdx];
        // The START poly's own edges are never banned: banning one can strand the search on its seed and
        // reproduce S76's `expands=1 touched=0` (the search never ran).
        if (kill.poly == start) {
            Log::Write("NAV-ROUTE",
                       "replan: the breaching leg crosses the START poly's own edge -- not banning it "
                       "(that would strand the seed); going to the frontier instead");
            break;
        }
        banned.push_back(BannedEdge{ kill.poly, kill.edge });
        stats.bannedEdges = static_cast<int>(banned.size());
        {
            char rm[240];
            snprintf(rm, sizeof(rm),
                     "replan: breach on leg %zu/%zu (%.1f,%.1f)->(%.1f,%.1f); banning portal "
                     "(poly %d, edge %d) and searching again -- attempt %d/%d, expands %d/%d",
                     rep.firstBad, rep.total, a.x, a.z, b.x, b.z,
                     kill.poly, kill.edge, attempt, kMaxAttempts, stats.expands, kMaxTotalExpand);
            Log::Write("NAV-ROUTE", rm);
        }
        if (probesLeft <= 0 || stats.expands >= kMaxTotalExpand) {
            Log::Write("NAV-ROUTE", "replan: work budget exhausted -- going to the frontier");
            break;
        }
    }

    // ---- outcome ----------------------------------------------------------------------------------
    stats.endPoly  = best.reachedGoal ? best.endPoly : best.bestNear;
    stats.nearDist = best.reachedGoal ? 0.0f : best.bestNearD;

    if (best.reachedGoal && !bestPoly.empty() && bestReport.ok) {
        rawPoly = bestPoly;
        stats.pass = "mesh";
        outPoly = rawPoly;
        char m[240];
        snprintf(m, sizeof(m),
                 "mesh: start=%d goal=%d end=%d polys=%zu portals=%zu corners=%zu "
                 "expands=%d touched=%d probes=%d attempts=%d banned=%d",
                 stats.startPoly, stats.goalPoly, stats.endPoly, best.chain.size(),
                 best.portals.size(), rawPoly.size(), stats.expands, stats.touched, stats.rays,
                 stats.attempts, stats.bannedEdges);
        Log::Write("NAV-ROUTE", m);
        return rawPoly.size() >= 2 ? Plan::Route : Plan::NoPath;
    }

    // ---- FRONTIER: never dead-end ------------------------------------------------------------------
    //
    // Everything above failed to produce a route we are willing to speak as a route. The work to answer
    // "then how far CAN they get" is already done -- `bestNear` has been maintained in the A* loop since
    // Session 74 and was written into stats and then thrown away by `return Plan::NoPath`.
    //
    // The risk here is struck IN THE CODE at the line this replaces, so it is worth restating: the grid
    // era emitted a near-goal fallback and spoke it as a normal set of legs, walking the tester
    // confidently to a spot 3 m from an exit 7.8 m overhead. The defence is not to withhold the route --
    // that strands a player who cannot see the obstacle -- it is that Plan::Frontier is a SEPARATE enum
    // value whose consumers are forced to announce the shortfall.
    {
        const PolyId fp = best.bestNear;
        if (fp == kNoPoly || fp == start) {
            stats.pass = best.fail ? best.fail : "no-frontier";
            char nm[208];
            snprintf(nm, sizeof(nm),
                     "mesh: NO route and NO frontier (frontier poly %d == start %d) pass=%s expands=%d",
                     fp, start, stats.pass, stats.expands);
            Log::Write("NAV-ROUTE", nm);
            return Plan::NoPath;
        }

        FVec3 fpt{};
        if (!NavMesh::ClosestPointOnPoly(fp, to.x, to.z, fpt)) {
            stats.pass = "frontier-unreadable";
            return Plan::NoPath;
        }

        // Route to the frontier point with a plain second pass over the corridor we already have. The
        // point TESTED is the point ARRIVED at (S76) -- ClosestPointOnPoly clamps to the triangle, so
        // this is a place on the mesh, not a centroid several metres away.
        std::vector<Portal> plain;
        plain.reserve(best.portals.size());
        for (const PortalRef& pref : best.portals) plain.push_back(pref.p);
        bool  flipped = false;
        float lenKept = 0.0f, lenOther = 0.0f;
        PathFunnel::BestPolarity(from, fpt, plain, rawPoly, flipped, lenKept, lenOther);
        PathFunnel::InsetCorners(rawPoly);
        PathFunnel::DropPassedWaypoints(from, rawPoly);

        stats.pass      = "frontier";
        stats.endPoly   = fp;
        stats.shortfall = NavCommon::Distance2D(fpt, to);
        stats.nearDist  = stats.shortfall;
        outPoly = rawPoly;

        char fm[288];
        snprintf(fm, sizeof(fm),
                 "frontier: goal unreachable (%s); ending at poly %d, %.1fm short. "
                 "tested (%.2f,%.2f,%.2f) arriving (%.2f,%.2f,%.2f) corners=%zu attempts=%d banned=%d",
                 best.fail ? best.fail : "validation never passed", fp, stats.shortfall,
                 fpt.x, fpt.y, fpt.z, fpt.x, fpt.y, fpt.z, rawPoly.size(),
                 stats.attempts, stats.bannedEdges);
        Log::Write("NAV-ROUTE", fm);

        return rawPoly.size() >= 2 ? Plan::Frontier : Plan::NoPath;
    }
}

} // namespace PathSearch

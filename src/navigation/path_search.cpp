#include "navigation/path_search.h"
#include "navigation/nav_mesh.h"
#include "navigation/map_query.h"
#include "navigation/nav_common.h"
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

// Expansion budget. A poly is far coarser than the old 1.5 m cell -- a whole corridor is often two
// triangles -- so this is generous. It exists only so a torn read cannot spin forever.
constexpr int kMaxExpand = 20000;

struct Node {
    float  f;
    PolyId p;
    bool operator>(const Node& o) const { return f > o.f; }
};

struct Came {
    PolyId parent = kNoPoly;
    int    edge   = -1;      // edge of `parent` we crossed to get here
};

inline float Dist3(const FVec3& a, const FVec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---- The funnel string-pull -----------------------------------------------------------------------
//
// WHY THIS EXISTS. A* gives a CORRIDOR of triangles, not a line. The obvious line -- through the
// midpoint of each shared edge -- is what shipped in Session 75, and it made routes double back on
// themselves: "South 2, North 7, West 5, ...". On this mesh a triangle is often an entire corridor,
// so consecutive edge midpoints sit at opposite ends of their portals and the polyline saws between
// them. It also produced a spurious FIRST leg whenever the player stood at one end of their own
// triangle and the first portal's midpoint was behind them.
//
// The funnel ("simple stupid funnel", Mononen) walks the portals keeping a left and a right bound and
// emits a corner only when the funnel would invert. What it returns is the SHORTEST path inside the
// corridor -- and a shortest path cannot doubleback, which is the property being bought here rather
// than a heuristic that usually helps. O(n), no raycasts.
struct Portal { FVec3 left, right; };

// 2D cross product in the ground plane: > 0 means `c` is counter-clockwise of a->b, i.e. to its LEFT.
inline float TriArea2(const FVec3& a, const FVec3& b, const FVec3& c) {
    return (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
}

inline bool SameXZ(const FVec3& a, const FVec3& b) {
    return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.z - b.z) < 1e-4f;
}

// `out` receives start, the corners, and end. Y rides along on the portal vertices, which are real
// mesh vertices sitting on their own surfaces -- so a route up stairs still describes correctly
// without the funnel itself ever reasoning about height.
void Funnel(const FVec3& start, const FVec3& end, const std::vector<Portal>& portals,
            std::vector<FVec3>& out) {
    out.clear();
    out.push_back(start);

    FVec3 apex = start, pLeft = start, pRight = start;
    size_t apexIdx = 0, leftIdx = 0, rightIdx = 0;

    for (size_t i = 0; i <= portals.size(); ++i) {
        // The terminal "portal" is the destination collapsed to a point, so the last leg is pulled
        // taut against the real end rather than against the final edge.
        const FVec3 left  = (i < portals.size()) ? portals[i].left  : end;
        const FVec3 right = (i < portals.size()) ? portals[i].right : end;

        // Tighten the RIGHT bound.
        if (TriArea2(apex, pRight, right) <= 0.0f) {
            if (SameXZ(apex, pRight) || TriArea2(apex, pLeft, right) > 0.0f) {
                pRight = right; rightIdx = i;
            } else {
                // Right crossed left: the left bound is a corner. Emit it and restart from there.
                out.push_back(pLeft);
                apex = pLeft; apexIdx = leftIdx;
                pLeft = apex; pRight = apex;
                leftIdx = rightIdx = apexIdx;
                i = apexIdx;                 // ++i makes this apexIdx+1
                continue;
            }
        }
        // Tighten the LEFT bound.
        if (TriArea2(apex, pLeft, left) >= 0.0f) {
            if (SameXZ(apex, pLeft) || TriArea2(apex, pRight, left) < 0.0f) {
                pLeft = left; leftIdx = i;
            } else {
                out.push_back(pRight);
                apex = pRight; apexIdx = rightIdx;
                pLeft = apex; pRight = apex;
                leftIdx = rightIdx = apexIdx;
                i = apexIdx;
                continue;
            }
        }
    }

    // ALWAYS finish on the target itself. `to` is the exact same FVec3 the `/` describe key measures
    // to, so the two keys can never name different destinations (Session 76).
    if (out.empty() || !SameXZ(out.back(), end)) out.push_back(end);
}

// Ground-plane length of a polyline. The funnel's whole claim is that it returns the SHORTEST path
// through a given corridor, so this is what checks the claim.
float PathLenXZ(const std::vector<FVec3>& pts) {
    float len = 0.0f;
    for (size_t i = 1; i < pts.size(); ++i) {
        const float dx = pts[i].x - pts[i - 1].x, dz = pts[i].z - pts[i - 1].z;
        len += std::sqrt(dx * dx + dz * dz);
    }
    return len;
}

// THE POLARITY IS MEASURED, NOT DERIVED.
//
// Which portal vertex is "left" depends on a sign convention that has to agree in two places at once:
// how the portals are labelled, and how the funnel's own area tests compare. FFXII's frame has north
// at -Z, which flips the handedness relative to every reference implementation of this algorithm, and
// getting it backwards inverts the funnel so it emits a corner at EVERY portal -- exactly what the
// Session 77 log showed (`portals=6 corners=7`, three routes out of three).
//
// I derived the convention twice and traced both branches against the reference twice; the code
// looked right and the log said it was not. So the sign is no longer an argument to win: run the
// funnel BOTH ways and keep the shorter path. The correct polarity is the shortest path through the
// corridor by definition, and the inverted one is the zigzag. O(n) twice, and impossible to get wrong.
//
// `flipped` reports which one won, so the next session can collapse this to one branch ON EVIDENCE.
void FunnelBestPolarity(const FVec3& start, const FVec3& end, const std::vector<Portal>& portals,
                        std::vector<FVec3>& out, bool& flipped, float& lenKept, float& lenOther) {
    std::vector<Portal> mirrored;
    mirrored.reserve(portals.size());
    for (const Portal& p : portals) mirrored.push_back(Portal{ p.right, p.left });

    std::vector<FVec3> a, b;
    Funnel(start, end, portals,  a);
    Funnel(start, end, mirrored, b);
    const float la = PathLenXZ(a), lb = PathLenXZ(b);

    flipped  = (lb < la);
    out      = flipped ? b : a;
    lenKept  = flipped ? lb : la;
    lenOther = flipped ? la : lb;
}

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
    const PolyId start = NavMesh::FindPolyAt(from.x, from.y, from.z);
    const PolyId goal  = NavMesh::FindPolyAt(to.x,   to.y,   to.z);
    stats.startPoly = start;
    stats.goalPoly  = goal;

    if (start == kNoPoly) { stats.pass = "no-start-poly"; return Plan::NoPath; }
    if (goal  == kNoPoly) { stats.pass = "no-goal-poly";  return Plan::NoPath; }

    FVec3 goalC{};
    if (!NavMesh::PolyCentroid(goal, goalC)) { stats.pass = "goal-unreadable"; return Plan::NoPath; }

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

    // THE GOAL IS THE TARGET'S OWN POLY. Nothing else.
    //
    // The previous version also accepted any poly with a corner inside the interaction band and
    // reach, on the theory that stopping early beat walking onto the target. It stopped FAR too
    // early: a navmesh triangle is often a whole corridor, so the triangle the player is already
    // standing on nearly always has a corner within ~1.6 m of a target six steps away. The search
    // terminated on its first pop -- `expands=1 touched=0` on 20 of 30 routes in the log -- and the
    // "route" became that triangle's centroid, which is why the spoken direction pointed south at a
    // target to the north and swung around as the player moved inside one polygon.
    //
    // It is also unnecessary. The polyline now ends exactly ON the target, and a path that ends at
    // the target cannot end past it -- which was the original overshoot complaint. Ending here also
    // makes the route's destination IDENTICAL to the crow-flies destination, because both are now
    // the same FVec3 from the same source.
    //
    // Standing where you can interact is handled by the caller's second pass (see path_search.h),
    // which only runs when the target's own poly is genuinely unreachable, and which arrives at the
    // exact point it tested rather than at a centroid.
    auto IsGoal = [&](PolyId p) -> bool { return p == goal; };

    // ---- The fallback, for a target the player cannot stand on -------------------------------------
    // Montblanc's dais, an NPC behind a counter, a chest on a ledge: the game shows an interact
    // prompt, so somewhere IS valid to stand, but the target's own polygon is not reachable.
    //
    // Recorded DURING the main search rather than by a second pass -- the first qualifying poly A*
    // pops is the one it would have walked to anyway, so this costs one predicate per expansion and
    // nothing at all in the common case where the target's own poly is reached.
    //
    // The point tested IS the point arrived at. That is the correction for the previous version,
    // which tested a triangle's corners and then arrived at its centroid -- somewhere else entirely.
    const bool haveBand  = (bandHi >= bandLo);
    const bool haveReach = (reachRadius > 0.01f);
    PolyId fallbackPoly  = kNoPoly;
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

    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    std::unordered_map<PolyId, float> gScore;
    std::unordered_map<PolyId, Came>  came;

    FVec3 startC{};
    if (!Centroid(start, startC)) { stats.pass = "start-unreadable"; return Plan::NoPath; }

    gScore[start] = 0.0f;
    came[start] = Came{};
    open.push(Node{ Dist3(startC, goalC), start });

    PolyId reached  = kNoPoly;
    PolyId bestNear = start;
    float  bestNearD = Dist3(startC, goalC);

    while (!open.empty() && stats.expands < kMaxExpand) {
        const Node cur = open.top();
        open.pop();
        const PolyId p = cur.p;
        ++stats.expands;

        if (IsGoal(p)) { reached = p; break; }
        NoteFallback(p);

        FVec3 pc{};
        if (!Centroid(p, pc)) continue;
        const float dGoal = Dist3(pc, goalC);
        if (dGoal < bestNearD) { bestNearD = dGoal; bestNear = p; }

        const auto gIt = gScore.find(p);
        if (gIt == gScore.end()) continue;
        const float gCur = gIt->second;

        for (int e = 0; e < 3; ++e) {
            const PolyId n = NavMesh::Neighbor(p, e);
            if (n == kNoPoly) continue;                 // boundary edge: map edge or wall

            FVec3 nc{};
            if (!Centroid(n, nc)) continue;

            // COST THE CROSSING, NOT THE CENTROID HOP.
            //
            // This used to be `Dist3(pc, nc)` -- centroid to centroid. On this mesh a triangle is
            // often an entire corridor, so two adjacent ones have centroids far apart even when the
            // player barely clips the shared edge, and the search optimises a quantity that is not
            // the distance walked. Measuring through the portal the player actually crosses is much
            // closer to ground truth, and it is the same point the string-pull threads.
            //
            // The heuristic stays a plain Euclidean distance to the goal, so it is still admissible:
            // no route through a portal can be shorter than the straight line to the goal.
            FVec3 mid{};
            const float step = NavMesh::EdgeMidpoint(p, e, mid)
                                   ? Dist3(pc, mid) + Dist3(mid, nc)
                                   : Dist3(pc, nc);          // unreadable edge -> old behaviour
            const float tentative = gCur + step;

            const auto nIt = gScore.find(n);
            if (nIt != gScore.end() && nIt->second <= tentative) continue;

            // Walkability + the volume check. This is the one raycast left in routing, and it is
            // here because floor adjacency knows nothing about doors: the floor under a closed gate
            // is still adjacent to the floor before it.
            ++stats.rays;
            if (!NavMesh::EdgePassable(p, e, n)) continue;

            ++stats.touched;
            gScore[n] = tentative;
            came[n] = Came{ p, e };
            open.push(Node{ tentative + Dist3(nc, goalC), n });
        }
    }

    // The target's own poly was unreachable, but somewhere in range of it was. Route there -- that
    // is a place the engine will genuinely let the player interact from, not a guess.
    bool usedFallback = false;
    if (reached == kNoPoly && fallbackPoly != kNoPoly) {
        reached      = fallbackPoly;
        usedFallback = true;
        char fm[176];
        snprintf(fm, sizeof(fm),
                 "mesh: goal poly %d unreachable; standing at poly %d (%.2f,%.2f,%.2f), %.2fm from it",
                 stats.goalPoly, fallbackPoly, fallbackPoint.x, fallbackPoint.y, fallbackPoint.z,
                 NavCommon::Distance2D(fallbackPoint, to));
        Log::Write("NAV-ROUTE", fm);
    }

    if (reached == kNoPoly) {
        stats.pass     = (stats.expands >= kMaxExpand) ? "budget" : "unreachable";
        stats.endPoly  = bestNear;
        stats.nearDist = bestNearD;
        // NO partial route. The old grid search emitted a near-goal fallback here and spoke it as a
        // normal set of legs, which is how the tester was walked confidently to a spot 3 m from an
        // exit 7.8 m overhead. If the mesh says there is no path, say so and let the log explain.
        return Plan::NoPath;
    }

    stats.endPoly  = reached;
    stats.nearDist = 0.0f;

    // ---- Reconstruct: STRING-PULL the corridor ----------------------------------------------------
    std::vector<PolyId> chain;
    for (PolyId p = reached; p != kNoPoly; ) {
        chain.push_back(p);
        auto it = came.find(p);
        if (it == came.end() || it->second.parent == kNoPoly) break;
        p = it->second.parent;
    }
    std::reverse(chain.begin(), chain.end());

    // Collect the portals the route crosses, each as a LEFT/RIGHT pair.
    //
    // Left and right are decided by the sign of the 2D cross product against the direction of travel,
    // NOT by the triangle's vertex order. The mesh's winding looks consistent (map_query's containment
    // test implies it), but that is an inference, and getting it backwards would silently invert the
    // funnel. Two multiplies buys not depending on it.
    std::vector<Portal> portals;
    portals.reserve(chain.size());
    for (size_t i = 1; i < chain.size(); ++i) {
        auto it = came.find(chain[i]);
        if (it == came.end() || it->second.edge < 0) continue;
        FVec3 v0{}, v1{};
        if (!NavMesh::EdgePortal(it->second.parent, it->second.edge, v0, v1)) continue;
        FVec3 ca{}, cb{};
        if (!Centroid(it->second.parent, ca) || !Centroid(chain[i], cb)) continue;
        // TriArea2(ca, cb, v) > 0 == v is counter-clockwise of the travel direction == LEFT.
        portals.push_back((TriArea2(ca, cb, v0) > 0.0f) ? Portal{ v0, v1 } : Portal{ v1, v0 });
    }

    bool  flipped = false;
    float lenKept = 0.0f, lenOther = 0.0f;
    FunnelBestPolarity(from, to, portals, rawPoly, flipped, lenKept, lenOther);

    // ---- DROP LEADING WAYPOINTS THE PLAYER HAS ALREADY WALKED PAST --------------------------------
    //
    // This is the single biggest cause of the reversal complaint, and it is older than any of the
    // routing rewrites. Log forensics over 1,059 archived routes: 109 contained an immediate
    // reversal, and **87 of them (80%) were leg 0 -> leg 1** -- in BOTH the grid era and the navmesh
    // era. The distribution names the mechanism outright:
    //
    //     Dire Rat 1   14/22 (64%)      Save Crystal        0/36 (0%)
    //     Rogue Tomato 38/113 (34%)     Stair to Lowtown    0/55 (0%)
    //     Montblanc    25/183 (14%)     Rabanastre exits    0/41 (0%)
    //
    // Static targets are essentially IMMUNE; moving ones dominate. The reason is not the target -- it
    // is that a moving target makes the player re-press while walking. The route's first corner is a
    // fixed point; the player drifts across it; and from a step past it, leg 0 points BACKWARDS to it
    // and leg 1 immediately turns around. The archive caught the same path spoken four ways in six
    // seconds as the player rocked back and forth over one waypoint.
    //
    // So: if the player has already passed the first corner, it is not a waypoint any more, it is
    // history. Project their position onto the corner->next-corner segment; a positive parameter
    // means they are beyond it. Repeat, because they may have passed several.
    {
        int dropped = 0;
        while (rawPoly.size() >= 3) {
            const FVec3 c1 = rawPoly[1], c2 = rawPoly[2];
            const float ex = c2.x - c1.x, ez = c2.z - c1.z;
            const float len2 = ex * ex + ez * ez;
            if (len2 < 1e-6f) break;
            const float t = ((from.x - c1.x) * ex + (from.z - c1.z) * ez) / len2;
            if (t <= 0.0f) break;                       // still ahead of the player -- a real waypoint
            rawPoly.erase(rawPoly.begin() + 1);
            ++dropped;
        }
        if (dropped > 0) {
            char dm[176];
            snprintf(dm, sizeof(dm),
                     "funnel: dropped %d leading waypoint(s) the player has already passed "
                     "(the leg-0 reversal, 80%% of all reversals in the log archive)", dropped);
            Log::Write("NAV-ROUTE", dm);
        }
    }

    // THE INVARIANT THAT WOULD HAVE CAUGHT THIS IMMEDIATELY.
    //
    // The funnel returns the shortest path through this corridor, so it can never be longer than the
    // naive line through the portal midpoints. If it is, the funnel is broken -- and that single
    // comparison separates "the funnel is wrong" from "the corridor is wrong", which is the exact
    // distinction three sessions of reading code could not make.
    {
        std::vector<FVec3> mid;
        mid.push_back(from);
        for (const Portal& p : portals)
            mid.push_back(FVec3{ (p.left.x + p.right.x) * 0.5f,
                                 (p.left.y + p.right.y) * 0.5f,
                                 (p.left.z + p.right.z) * 0.5f });
        mid.push_back(to);
        const float midLen    = PathLenXZ(mid);
        const float straight  = NavCommon::Distance2D(from, to);
        char fm[256];
        snprintf(fm, sizeof(fm),
                 "funnel: polarity=%s kept=%.1fm other=%.1fm midpoints=%.1fm straight=%.1fm "
                 "corners=%zu/%zu portals%s",
                 flipped ? "FLIPPED" : "as-labelled", lenKept, lenOther, midLen, straight,
                 rawPoly.size(), portals.size(),
                 (lenKept > midLen + 0.01f) ? "   <== LONGER THAN MIDPOINTS: funnel is broken" : "");
        Log::Write("NAV-ROUTE", fm);

        // And the corridor's own quality, which the funnel cannot fix: if the taut path through the
        // triangle sequence is far longer than the straight line, A* chose a wandering corridor and
        // the fault is upstream in the search, not in the string-pull.
        if (straight > 1.0f && lenKept > straight * 1.8f) {
            snprintf(fm, sizeof(fm),
                     "funnel: corridor is %.1fx the straight line (%.1fm vs %.1fm) -- A* chose a "
                     "wandering triangle sequence; the string-pull cannot shorten past its corridor",
                     lenKept / straight, lenKept, straight);
            Log::Write("NAV-ROUTE", fm);
        }
    }

    stats.pass = "mesh";

    // THE ROUTE'S ACTUAL GEOMETRY. Every session spent on the reversal bug has ended with me wanting
    // exactly these numbers and not having them, so they are no longer optional. Bounded to the first
    // few of each: enough to see a doubling-back, nowhere near enough to flood the log.
    {
        constexpr size_t kMaxDump = 10;
        char line[600]; int off = 0;
        off += snprintf(line + off, sizeof(line) - static_cast<size_t>(off), "geom corridor:");
        for (size_t i = 0; i < chain.size() && i < kMaxDump && off < 520; ++i)
            off += snprintf(line + off, sizeof(line) - static_cast<size_t>(off), " %d", chain[i]);
        if (chain.size() > kMaxDump)
            snprintf(line + off, sizeof(line) - static_cast<size_t>(off), " (+%zu)", chain.size() - kMaxDump);
        Log::Write("NAV-ROUTE", line);

        off = snprintf(line, sizeof(line), "geom corners:");
        for (size_t i = 0; i < rawPoly.size() && i < kMaxDump && off < 520; ++i)
            off += snprintf(line + off, sizeof(line) - static_cast<size_t>(off),
                            " (%.1f,%.1f)", rawPoly[i].x, rawPoly[i].z);
        Log::Write("NAV-ROUTE", line);

        off = snprintf(line, sizeof(line), "geom portals L|R:");
        for (size_t i = 0; i < portals.size() && i < kMaxDump && off < 500; ++i)
            off += snprintf(line + off, sizeof(line) - static_cast<size_t>(off),
                            " (%.1f,%.1f)|(%.1f,%.1f)",
                            portals[i].left.x, portals[i].left.z,
                            portals[i].right.x, portals[i].right.z);
        Log::Write("NAV-ROUTE", line);
    }
    outPoly = rawPoly;

    char m[192];
    snprintf(m, sizeof(m),
             "mesh: start=%d goal=%d end=%d polys=%zu portals=%zu corners=%zu "
             "expands=%d touched=%d rays=%d",
             stats.startPoly, stats.goalPoly, stats.endPoly, chain.size(), portals.size(),
             rawPoly.size(), stats.expands, stats.touched, stats.rays);
    Log::Write("NAV-ROUTE", m);

    return rawPoly.size() >= 2 ? Plan::Route : Plan::NoPath;
}

} // namespace PathSearch

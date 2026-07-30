#include "navigation/path_search.h"
#include "navigation/nav_mesh.h"
#include "navigation/nav_footprint.h"
#include "navigation/map_query.h"
#include "navigation/nav_common.h"
#include "navigation/path_funnel.h"
#include "navigation/path_validate.h"
#include "navigation/path_corridor.h"
#include "navigation/nav_blocked.h"
#include "navigation/player_state.h"
#include "core/logger.h"

#include <windows.h>

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
using PathCorridor::Came;        // A*'s parent links; corridor reconstruction lives in path_corridor
using PathCorridor::PortalRef;

// (kMaxExpand was removed in Session 96 -- declared, never read. kMaxTotalExpand below is the only
// expansion bound, and it is the one the log reports.)

// THE RE-SEARCH IS CAPPED BY WORK, NOT BY ATTEMPTS (Session 93). Attempts are not equal -- four passes
// over the Giza corridor is a different proposition from four over a small room, and "slower than
// vanilla = OUR code" is a standing CRITICAL rule -- so attempts stop on either bound and the log says
// which, rather than quietly costing frames.
constexpr int kMaxAttempts   = 4;
constexpr int kMaxTotalExpand = 40000;
// Validation probe budget for the whole request: room for four attempts at Giza scale (~280 probes).
constexpr int kProbeBudget   = 1600;
// The frontier gets its own floor on top of whatever the attempts left: a route we are about to SPEAK
// has to be checked, and "the retries used up the budget" is not a reason to skip it.
constexpr int kFrontierMinProbes = 128;
// Final-leg arrival tolerance -- see PathValidate::CheckLegs. Not a tuning knob: it is path_planner's
// kAtExitDist, the distance at which the planner already says "At the exit", so the two agree.
constexpr float kArrivalTol = 3.0f;

struct Node {
    float  f;
    PolyId p;
    bool operator>(const Node& o) const { return f > o.f; }
};

// ---- NOTHING SEVERS THE GRAPH; EVERYTHING DIFFICULT IS EXPENSIVE (Session 96) ----------------------
//
// Every routing regression this session came from one move: taking a real measurement and using it to
// DELETE an edge. Delete enough and a reachable goal becomes unreachable -- and once it is unreachable
// there is nothing left to validate, repair, or honestly report. The measurements were right; using
// them as cuts was not.
//
// These are costs in METRES, added to the crossing cost, so the ordering is what matters and not the
// exact figure: clear << tight << terrain. A* takes any detour up to the penalty's worth rather than
// use the edge, which reproduces the old refusal wherever an alternative exists -- and still hands
// back a corridor when it is the only way through, which the deletion took away.
//
// The heuristic stays plain Euclidean and therefore stays admissible: penalties only ever ADD to the
// true cost, so a straight-line estimate can never overshoot it.
constexpr float kTightPenalty   = 500.0f;    // body does not fit anywhere along this edge
constexpr float kTerrainPenalty = 2000.0f;   // the party's floor class may not stand on the neighbour
constexpr float kBlockedPenalty = 2000.0f;   // the player PHYSICALLY failed to get past here
constexpr float kBreachPenalty  = 500.0f;    // a validated leg through this portal did not walk

// A portal a later attempt should avoid, because the taut path through it turned out not to be
// walkable. Scoped to ONE request -- never cached across presses, because the obstacle may be a door
// that opens, and a permanent ban would be exactly the "learned label" this project forbids.
//
// IT IS A PRICE, NOT A BAN (Session 96). A ban is binary and permanent for the request: ban the only
// opening and the goal is unreachable, which is exactly how a route that A* had already reached the
// goal with came back "No path". `pen` accumulates, so a portal that keeps failing keeps getting
// dearer and the search moves off it on its own, without any attempt ever losing the option.
struct BannedEdge { PolyId poly; int edge; float pen; };

inline float Dist3(const FVec3& a, const FVec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

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
    // What the corridor this pass returned had to PAY. `penTerrain > 0` means it crosses ground the
    // party's floor class may not stand on -- such a route is never spoken as a plain Route, however
    // well it sweeps, because the sweep does not refuse water and never did. `penOther` is pinches,
    // measured blocks and re-costed portals: all legal to walk, so it only ranks routes.
    float penTerrain = 0.0f;
    float penOther   = 0.0f;
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
    // FindPolyAt keeps the RAW walkable test and gets no footprint check: a player in a doorway is
    // legitimately within a body radius of a boundary, and rejecting their own poly would answer
    // "No path" from a position they are demonstrably standing on.
    const PolyId start = NavMesh::FindPolyAt(from.x, from.y, from.z);
    const PolyId goal  = NavMesh::FindPolyAt(to.x,   to.y,   to.z);
    stats.startPoly = start;
    stats.goalPoly  = goal;

    if (start == kNoPoly) { stats.pass = "no-start-poly"; return Plan::NoPath; }

    // WHAT DO THE TWO ENDPOINTS ACTUALLY LOOK LIKE? Without this a NoPath is unreadable: the poly ids
    // alone cannot say whether the goal was refused for the party (bit 23 -- water, out of bounds) or
    // simply disconnected. FindPolyAt uses RAW flags and does NOT consult walkability, so `goal` being
    // found says nothing about whether A* is allowed to enter it.
    {
        uint32_t sr = 0, se = 0, gr = 0, ge = 0;
        NavMesh::PolyFlags(start, sr, se);
        if (goal != kNoPoly) NavMesh::PolyFlags(goal, gr, ge);
        char m[224];
        snprintf(m, sizeof(m),
                 "ends: start=%d eff=0x%08X walk=%d | goal=%d eff=0x%08X walk=%d | class=%u",
                 start, se, NavMesh::Walkable(start) ? 1 : 0,
                 goal, ge, (goal != kNoPoly && NavMesh::Walkable(goal)) ? 1 : 0,
                 PlayerState::PartyMovementClass());
        Log::Write("NAV-ROUTE", m);
    }

    // ---- A target with NO POLYGON OF ITS OWN is still reachable ------------------------------------
    // A wall-mounted board, a chest on a ledge, an NPC behind a counter: the object's own point is off
    // the mesh, but the game shows an interact prompt, so a place to stand exists. Route by the
    // INTERACTION CYLINDER instead (S73: interaction distance is a cylinder, not a point).
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

    // ---- WHY DID EXPANSION STOP? -----------------------------------------------------------------
    // A NoPath currently says only "unreachable", which cannot distinguish "the map really is split
    // here" from "we are refusing something we should not". These count every neighbour A* declined and
    // WHY, and for the walkability refusals they keep the distinct effective-flags words -- so the log
    // names the exact terrain class that walled the search in, against the census from the ' key.
    int refNoPoly = 0, refUnwalkable = 0, refEdge = 0, refBanned = 0, refBlocked = 0;
    uint32_t refFlags[6] = {};
    int      refFlagN[6] = {};
    int      refFlagCount = 0;
    auto NoteRefusedFlags = [&](uint32_t eff) {
        for (int i = 0; i < refFlagCount; ++i)
            if (refFlags[i] == eff) { ++refFlagN[i]; return; }
        if (refFlagCount < 6) { refFlags[refFlagCount] = eff; refFlagN[refFlagCount] = 1; ++refFlagCount; }
    };

    // Read once per request, not per edge.
    const uint64_t nowMs = GetTickCount64();
    const bool blockedActive = NavBlocked::Any();

    std::vector<BannedEdge> banned;
    PassResult best{};                 // the last pass that produced a corridor at all
    std::vector<FVec3> bestPoly;
    PathValidate::LegReport bestReport{};
    int probesLeft = kProbeBudget;
    NavMesh::ResetCrossingCounters();   // so the `refused:` line counts THIS request, not the session

    // THE FURTHEST-REACHING PROVEN PREFIX, kept ACROSS attempts (Session 95): a breaching attempt
    // still established that its first N legs are walkable. It must survive the loop because attempts
    // get WORSE as bans accumulate (one route went 211.6m -> 233.5m), so falling back on the last
    // attempt means falling back on the worst one.
    std::vector<FVec3> bestPrefix;
    float bestPrefixDist = -1.0f;      // metres from the prefix's last point to the goal
    // A*'s parent links from the LAST pass run. Hoisted out of the attempt loop because the frontier
    // rebuilds a corridor to a DIFFERENT end poly than the search aimed at; `best` is likewise always
    // the last pass, so the two always describe the same search.
    PathCorridor::CameMap came;

    // ---- attempt loop: validate, then ban the offending portal and search AGAIN -------------------
    // THE STRUCTURAL FIX (Session 93). Validation used to sit AFTER the search, where its only options
    // were accept, substitute, or accept what it had just disproved -- it took the third on 9 of 53
    // routes. In a loop, a failed validation changes the GRAPH, so A* finds the detour itself.
    //
    // A BAN IS ONLY AS GOOD AS THE VERDICT BEHIND IT (Session 95): while a false breach was possible,
    // this loop banned good portals and every retry came back longer. See path_validate.h.
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
            // IT HAS TO BE SOMEWHERE THE PARTY CAN STAND (Session 96). Water polys are now EXPANDED
            // rather than skipped -- that is the whole point of pricing them -- so every consumer that
            // used to get walkability for free from the search has to ask for it. This one names the
            // spot the player is told to walk to in order to reach the target.
            if (!NavMesh::Walkable(p)) return;
            FVec3 cp{};
            if (!NavMesh::ClosestPointOnPoly(p, to.x, to.z, cp)) return;
            if (cp.y < bandLo || cp.y > bandHi) return;                    // engine's vertical gate
            const float dx = cp.x - to.x, dz = cp.z - to.z;
            if (dx * dx + dz * dz >= reachRadius * reachRadius) return;    // cylinder: Y excluded
            fallbackPoly  = p;
            fallbackPoint = cp;
        };

        auto BanPenalty = [&](PolyId p, int e) -> float {
            for (const BannedEdge& b : banned) if (b.poly == p && b.edge == e) return b.pen;
            return 0.0f;
        };

        std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
        std::unordered_map<PolyId, float> gScore;
        // The penalty carried along the best-known path to each poly, split so the terrain half can veto
        // on its own. Same parent links as gScore, so the two always describe the same route.
        std::unordered_map<PolyId, float> gPenT, gPenO;
        came.clear();

        FVec3 startC{};
        if (!Centroid(start, startC)) { stats.pass = "start-unreadable"; return Plan::NoPath; }

        gScore[start] = 0.0f;
        gPenT[start]  = 0.0f;
        gPenO[start]  = 0.0f;
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
            // ...and the same for the frontier. `bestNear` is where a shortfall route ENDS, so it is a
            // place the player gets told to stand; a priced-but-expanded water poly must never win it.
            const float dGoal = Dist3(pc, goalC);
            if (dGoal < pr.bestNearD && NavMesh::Walkable(p)) { pr.bestNearD = dGoal; pr.bestNear = p; }

            const auto gIt = gScore.find(p);
            if (gIt == gScore.end()) continue;
            const float gCur = gIt->second;

            const float penTCur = gPenT[p], penOCur = gPenO[p];

            for (int e = 0; e < 3; ++e) {
                const PolyId n = NavMesh::Neighbor(p, e);
                // THE ONLY TRUE CUT LEFT. No neighbour is not an expensive edge, it is the absence of
                // one -- there is nothing on the other side to price.
                if (n == kNoPoly) { ++refNoPoly; continue; }

                FVec3 nc{};
                if (!Centroid(n, nc)) continue;

                // COST THE CROSSING, NOT THE CENTROID HOP. On this mesh a triangle is often an entire
                // corridor, so centroid-to-centroid optimises a quantity that is not the distance
                // walked. The heuristic stays plain Euclidean, so the search is still admissible.
                FVec3 mid{};
                const float step = NavMesh::EdgeMidpoint(p, e, mid)
                                       ? Dist3(pc, mid) + Dist3(mid, nc)
                                       : Dist3(pc, nc);

                // ---- price the crossing -------------------------------------------------------------
                float penT = 0.0f, penO = 0.0f;
                if (!NavMesh::Walkable(n)) {
                    ++refUnwalkable;
                    uint32_t nr = 0, ne = 0;
                    if (NavMesh::PolyFlags(n, nr, ne)) NoteRefusedFlags(ne);
                    penT += kTerrainPenalty;
                }
                const float bp = BanPenalty(p, e);
                if (bp > 0.0f) { ++refBanned; penO += bp; }
                if (blockedActive && NavBlocked::Contains(nc, epoch, nowMs)) {
                    ++refBlocked;
                    penO += kBlockedPenalty;
                }

                // The body test is the expensive one -- seven straddle sweeps -- so it is asked LAST and
                // only when the answer can still change the ordering. The early-out below uses the
                // cheapest price this edge could possibly carry; a pinch can only make it dearer, so
                // skipping the test on an edge that already cannot improve `n` is exact, not a shortcut.
                const auto nIt = gScore.find(n);
                if (nIt != gScore.end() && nIt->second <= gCur + step + penT + penO) continue;
                if (penT == 0.0f) {                       // terrain already dominates; nothing to add
                    ++stats.rays;
                    if (!NavMesh::EdgePassable(p, e, n)) { ++refEdge; penO += kTightPenalty; }
                }

                const float tentative = gCur + step + penT + penO;
                if (nIt != gScore.end() && nIt->second <= tentative) continue;

                ++stats.touched;
                gScore[n] = tentative;
                gPenT[n]  = penTCur + penT;
                gPenO[n]  = penOCur + penO;
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
        // WHAT THIS CORRIDOR COST. Read off the same parent links the corridor is rebuilt from, so it
        // describes this route and no other.
        {
            const auto tIt = gPenT.find(reached), oIt = gPenO.find(reached);
            pr.penTerrain = (tIt != gPenT.end()) ? tIt->second : 0.0f;
            pr.penOther   = (oIt != gPenO.end()) ? oIt->second : 0.0f;
        }
        if (pr.penTerrain > 0.0f || pr.penOther > 0.0f) {
            char pm[200];
            snprintf(pm, sizeof(pm),
                     "cost: attempt %d corridor pays terrain=%.0f other=%.0f "
                     "(terrain > 0 => crosses ground the party's class may not stand on)",
                     attempt, pr.penTerrain, pr.penOther);
            Log::Write("NAV-ROUTE", pm);
        }
        pr.endPoly     = reached;

        // ---- Reconstruct the corridor, then STRING-PULL it -----------------------------------------
        PathCorridor::Build(came, reached, pr.chain, pr.portals, pr.clipped, pr.blocked);

        // ---- string-pull, then repair the corners, then validate -----------------------------------
        std::vector<Portal> plain;
        plain.reserve(pr.portals.size());
        for (const PortalRef& pref : pr.portals) plain.push_back(pref.p);

        bool  flipped = false;
        float lenKept = 0.0f, lenOther = 0.0f;
        std::vector<FVec3> poly;
        std::vector<int>   polyIdx;      // which portal each corner came from -- see PathFunnel::Unpull
        PathFunnel::BestPolarity(from, to, plain, poly, flipped, lenKept, lenOther, &polyIdx);

        // Pull corners off the boundary BEFORE dropping passed waypoints, because an inset can move a
        // corner past the player and the drop is what notices.
        const int inset = PathFunnel::InsetCorners(poly);
        // AFTER any step that rebuilds or moves the polyline. The old code ran this once, before a
        // fallback swapped in a freshly built vector -- so on every breaching route the S78 leg-0
        // reversal fix was silently bypassed.
        const int droppedWp = PathFunnel::DropPassedWaypoints(from, poly, &polyIdx);

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

        const PathValidate::LegReport rep = PathValidate::CheckLegs(poly, probesLeft, kArrivalTol);
        probesLeft -= rep.probes;
        stats.rays += rep.probes;

        {
            // `resweep`/`rescued` are the Session 95 proof line, and `bad=` prints the breaching leg's
            // length against how far the body got -- so a real wall and a measurement artefact read
            // differently at a glance. Field meanings live on PathValidate::LegReport.
            // `why=` and `stopPoly=` are the Session 96 additions. A wall met mid-leg and a floor the
            // body sweep refuses stop at the same coordinates, and the poly under the stop -- with its
            // effective flags -- is what says whether the thing in the way is water (bit 23) or an
            // obstacle standing on ground the party may walk.
            char bad[352] = "";
            if (rep.firstBad)
                snprintf(bad, sizeof(bad),
                         " bad=%zu len=%.2fm reached=%.2fm stop=(%.1f,%.2f,%.1f) why=%s "
                         "stopPoly=%d walk=%d eff=0x%08X | corner: poly=%d clear=%d margin=%.2fm  "
                         "vol@stop=%d vol@+0.3m=%d%s",
                         rep.firstBad, rep.badLength, rep.badReached,
                         rep.badStopAt.x, rep.badStopAt.y, rep.badStopAt.z,
                         PathValidate::CauseName(rep.badCause),
                         rep.badStopPoly, rep.badStopWalk ? 1 : 0, rep.badStopFlags,
                         rep.badCornerPoly, rep.badCornerClear ? 1 : 0, rep.badCornerMargin,
                         rep.badStopInVolume ? 1 : 0, rep.badAheadInVolume ? 1 : 0,
                         rep.walls ? " WALL" : "");
            char vm[672];
            snprintf(vm, sizeof(vm),
                     "validate: attempt %d legs checked=%zu/%zu probes=%d worstFrac=%.2f tight=%d@%zu "
                     "resweep=%d rescued=%d swept=%d blind=%d walls=%d %s%s%s",
                     attempt, rep.checked, rep.total, rep.probes, rep.worstFraction,
                     rep.tightCorners, rep.firstTight, rep.resweeps, rep.rescued,
                     rep.swept, rep.blind, rep.walls,
                     rep.ok ? "OK" : "BREACH", bad,
                     rep.blind     ? "  <== BLIND: no collision world for some legs; NOT verified"
                     : rep.truncated ? "  <== TRUNCATED: budget ran out, remaining legs NOT tested" : "");
            Log::Write("NAV-ROUTE", vm);
        }

        // THE CORNERS, WITH THEIR HEIGHTS, whenever validation fails. Every other line in this log
        // prints X and Z only, and on a route that drops from a walkway into a channel the Y is the
        // whole question: a corner that takes its height from the wrong side of a step makes the leg
        // into it a diagonal through a wall, and the body sweep then reports a breach that has nothing
        // to do with the ground being unwalkable. Bounded to the first few corners; this fires only on
        // a failure, never on a good route.
        if (!rep.ok || rep.truncated) {
            char pts[240]; int q = 0;
            for (size_t i = 0; i < poly.size() && i < 7 && q < static_cast<int>(sizeof(pts)) - 30; ++i)
                q += snprintf(pts + q, sizeof(pts) - static_cast<size_t>(q), "%s(%.1f,%.2f,%.1f)",
                              i ? " " : "", poly[i].x, poly[i].y, poly[i].z);
            char m[320];
            snprintf(m, sizeof(m), "corners(xyz): %s%s", pts,
                     poly.size() > 7 ? " ..." : "");
            Log::Write("NAV-ROUTE", m);
        }

        // ---- REPAIR BEFORE RE-SEARCHING -------------------------------------------------------------
        // A breach is a verdict on the CHORD, not on the corridor. The corridor was measured edge by
        // edge with the body's own footprint before A* ever expanded through it; the taut line the
        // funnel drew across it was an optimisation, and on this mesh -- where one triangle is often an
        // entire room -- that line can leave the walkable strip while every portal it skipped stays
        // perfectly crossable. Map 311 proved it both ways in one request: the chord's leg 3 stopped the
        // body at 6.23 m of 9.00 m on four consecutive attempts, and the frontier's less-taut polyline
        // walked the same ground with `cutByValidation=0`.
        //
        // So undo the pull on the ONE leg that failed and re-validate. Re-searching the whole graph
        // answers a local question globally, and it was costing the route: every attempt came back with
        // the same chord and the same breach until the attempts ran out.
        // THE LADDER: taut chord -> un-pulled leg -> retreat to where the body got -> full corridor.
        // Shortest thing that works is what gets spoken, and every rung runs ONLY after a breach, so a
        // route that validates on the chord executes none of it and cannot be changed by any of it.
        if (!rep.ok && rep.firstBad > 0 && !plain.empty() && probesLeft > 0) {
            const size_t bad = rep.firstBad;
            bool mendedOk = false;

            auto tryPoly = [&](std::vector<FVec3>& cand, const char* how, int detail) -> bool {
                if (cand.size() < 2 || probesLeft <= 0) return false;
                PathFunnel::InsetCorners(cand);
                const PathValidate::LegReport r2 =
                    PathValidate::CheckLegs(cand, probesLeft, kArrivalTol);
                probesLeft -= r2.probes;
                stats.rays += r2.probes;
                const bool good = r2.ok && !r2.truncated;
                char rm[288];
                snprintf(rm, sizeof(rm),
                         "repair[%s]: leg %zu, %d -- %zu->%zu points, probes=%d -> %s",
                         how, bad, detail, poly.size(), cand.size(), r2.probes,
                         good ? "OK" : "still breaching");
                Log::Write("NAV-ROUTE", rm);
                if (!good) return false;
                best = pr; bestPoly = cand; bestReport = r2; rawPoly = cand;
                stats.repaired = detail;
                return true;
            };

            // 1. Un-pull the failing leg (and, when it is interior, the corner it aimed at).
            {
                std::vector<FVec3> mended;
                const int spliced = PathFunnel::Unpull(poly, polyIdx, plain, bad, mended);
                if (spliced > 0) mendedOk = tryPoly(mended, "unpull", spliced);
            }

            // 2. RETREAT TO WHERE THE BODY ACTUALLY GOT. `badStopAt` is the engine's own resolved
            //    position, so it is reachable whatever is in the way -- floor border, wall volume, or
            //    something we have not thought of. That is the point: this rung needs no theory. Only
            //    for an INTERIOR corner; the final point is the destination and is not ours to move.
            if (!mendedOk && bad + 1 < poly.size() &&
                rep.badReached > NavFootprint::BodyRadius()) {
                std::vector<FVec3> pulled(poly.begin(), poly.begin() + static_cast<ptrdiff_t>(bad));
                pulled.push_back(rep.badStopAt);
                pulled.insert(pulled.end(), poly.begin() + static_cast<ptrdiff_t>(bad) + 1, poly.end());
                mendedOk = tryPoly(pulled, "retreat", static_cast<int>(rep.badReached * 100.0f));
            }

            // 3. LAST RUNG: the whole corridor, un-pulled. Reserved for the case that has no other
            //    move at all -- a breach on leg 1, where re-costing is refused because the portal is
            //    the start poly's own and the frontier's proven prefix is a single point. Measured:
            //    that combination spoke "No path" from 3 m away from a reachable exit, nine times.
            if (!mendedOk && bad == 1) {
                std::vector<FVec3> full;
                const int pts = PathFunnel::FullCorridor(from, to, plain, full);
                if (pts > 0) mendedOk = tryPoly(full, "full-corridor", pts);
            }

            if (mendedOk) break;            // the corridor walks; only the shortcut across it did not
        }

        best       = pr;
        bestPoly   = poly;
        bestReport = rep;

        // Bank the part of THIS attempt that validated, before deciding whether to ban and try again.
        // Whatever happens next, these legs stay proven.
        {
            const size_t keep = PathCorridor::ProvenPrefix(rep, poly.size());
            if (keep >= 2) {
                const float d = NavCommon::Distance2D(poly[keep - 1], to);
                if (bestPrefixDist < 0.0f || d < bestPrefixDist) {
                    bestPrefix.assign(poly.begin(), poly.begin() + static_cast<ptrdiff_t>(keep));
                    bestPrefixDist = d;
                }
            }
        }

        // THE BODY WALK IS THE AUTHORITY. There was briefly a terrain veto here -- reject a validated
        // corridor whose flag-based terrain price was non-zero -- written while `Walkable` still meant
        // "the party's class may stand here". It doesn't any more: the tester walks the shallow water
        // that reading refused, so `Walkable` is back to the floor-type test and `penTerrain` now only
        // means "unreadable or not a floor poly". Vetoing a route on THAT would turn a transient mesh
        // read into "No path", which is the same over-refusal by a shorter road. Priced, logged, and
        // left to the walk -- which is the only instrument that has ever answered "can the character
        // get there" rather than "what is this made of".
        if (rep.ok && !rep.truncated) {
            rawPoly = poly;
            break;                      // fully verified by the engine's own body walk
        }
        // TRUNCATED IS NOT VERIFIED. This used to `break` here and the outcome block then shipped the
        // whole polyline as Plan::Route -- the exact claim path_validate.h forbids the caller to make.
        // It now falls to the frontier, which speaks only the legs that were actually tested and says
        // how far short they stop.
        if (rep.ok) break;

        // RE-COST the portal the breaching leg crosses. Which one is a geometric question: the leg that
        // failed runs between two taut corners, and the portal it crosses is the one whose span sits
        // closest to that leg's midpoint. Make that (poly, edge) dearer and search again.
        // NEVER RE-COST A FINAL-APPROACH BREACH: the portal nearest the last leg is the one that gets
        // you TO the target, and banning it is how a route within 2.55 m of an exit became "goal
        // unreachable" and a frontier 15.2 m short. The banked prefix already carries the honest answer.
        if (rep.firstBad == 0 || rep.firstBad >= poly.size() - 1 || plain.empty()) break;
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
        // The START poly's own edges are left alone: pricing one can push the search off its own seed
        // and reproduce S76's `expands=1 touched=0` (the search never ran).
        if (kill.poly == start) {
            Log::Write("NAV-ROUTE",
                       "replan: the breaching leg crosses the START poly's own edge -- not re-costing it "
                       "(that would strand the seed); going to the frontier instead");
            break;
        }
        float newPen = kBreachPenalty;
        bool  seen   = false;
        for (BannedEdge& b : banned)
            if (b.poly == kill.poly && b.edge == kill.edge) { b.pen += kBreachPenalty; newPen = b.pen; seen = true; break; }
        if (!seen) banned.push_back(BannedEdge{ kill.poly, kill.edge, kBreachPenalty });
        stats.bannedEdges = static_cast<int>(banned.size());
        {
            char rm[272];
            snprintf(rm, sizeof(rm),
                     "replan: breach on leg %zu/%zu (%.1f,%.1f)->(%.1f,%.1f); portal (poly %d, edge %d) "
                     "re-costed to %.0f and searching again -- attempt %d/%d, expands %d/%d",
                     rep.firstBad, rep.total, a.x, a.z, b.x, b.z,
                     kill.poly, kill.edge, newPen, attempt, kMaxAttempts,
                     stats.expands, kMaxTotalExpand);
            Log::Write("NAV-ROUTE", rm);
        }
        if (probesLeft <= 0 || stats.expands >= kMaxTotalExpand) {
            Log::Write("NAV-ROUTE", "replan: work budget exhausted -- going to the frontier");
            break;
        }
    }

    // The pricing histogram. It used to print only when the goal was not reached -- which, now that
    // nothing severs the graph, is exactly the case that has become rare, so it would have gone quiet
    // just as it started to matter. It now also prints whenever the corridor had to PAY for something,
    // because that is the same information arriving one step earlier.
    if (!best.reachedGoal || best.penTerrain > 0.0f || best.penOther > 0.0f) {
        char fl[160]; int q = 0;
        for (int i = 0; i < refFlagCount && q < static_cast<int>(sizeof(fl)) - 24; ++i)
            q += snprintf(fl + q, sizeof(fl) - static_cast<size_t>(q), "%s0x%08X x%d",
                          i ? " " : "", refFlags[i], refFlagN[i]);
        if (q == 0) snprintf(fl, sizeof(fl), "none");
        // Every field but `noPoly` is now a PRICE, not a refusal -- the count of crossings that were
        // made expensive rather than deleted. `noPoly` is the one true cut (no neighbour to price).
        // `tightXing` is how many crossings the footprint test refused; `volXing` measured ZERO on the
        // map where walls were the leading theory, which is what retired that theory.
        char m[448];
        snprintf(m, sizeof(m),
                 "costed: noPoly=%d(cut) unwalkable=%d edge=%d rePriced=%d measuredBlock=%d "
                 "tightXing=%d volXing=%d | corridor paid terrain=%.0f other=%.0f "
                 "| unwalkable eff-flags: %s",
                 refNoPoly, refUnwalkable, refEdge, refBanned, refBlocked,
                 NavMesh::g_tightCrossings, NavMesh::g_volumeCrossings,
                 best.penTerrain, best.penOther, fl);
        Log::Write("NAV-ROUTE", m);
    }

    // ---- outcome ----------------------------------------------------------------------------------
    stats.endPoly  = best.reachedGoal ? best.endPoly : best.bestNear;
    stats.nearDist = best.reachedGoal ? 0.0f : best.bestNearD;

    if (best.reachedGoal && !bestPoly.empty() && bestReport.ok && !bestReport.truncated) {
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
    // Nothing above produced a route we are willing to speak as one. The defence is not to withhold a
    // route -- that strands a player who cannot see the obstacle -- it is that Plan::Frontier is a
    // SEPARATE enum value whose consumers must announce the shortfall. It is VALIDATED like any other
    // route (Session 95); path_corridor.h has the evidence.
    {
        // A -- the furthest-reaching prefix banked during the attempts; already funnelled and validated,
        // so it costs nothing. B -- a corridor to the nearest poly A* reached, rebuilt for THAT poly and
        // validated here; only worth its probes when it could finish nearer than A does.
        const PolyId fp = best.bestNear;
        PathCorridor::FrontierRoute fr;
        const bool tryNear = (fp != kNoPoly && fp != start) &&
                             (bestPrefixDist < 0.0f || best.bestNearD < bestPrefixDist);
        if (tryNear) {
            const int budget = probesLeft > kFrontierMinProbes ? probesLeft : kFrontierMinProbes;
            if (!PathCorridor::BuildFrontier(came, fp, from, to, budget, fr)) fr.poly.clear();
            stats.rays += fr.probes;   // counted whether or not it produced a route -- it was spent
        }

        const bool useNear = !fr.poly.empty() &&
                             (bestPrefixDist < 0.0f || fr.shortfall < bestPrefixDist);
        if (useNear) {
            rawPoly         = fr.poly;
            stats.endPoly   = fr.endPoly;
            stats.shortfall = fr.shortfall;
        } else if (bestPrefix.size() >= 2) {
            rawPoly         = bestPrefix;
            stats.endPoly   = NavMesh::FindPolyAt(rawPoly.back().x, rawPoly.back().y, rawPoly.back().z);
            stats.shortfall = bestPrefixDist;
        } else {
            stats.pass = best.fail ? best.fail : "no-frontier";
            char nm[240];
            snprintf(nm, sizeof(nm),
                     "mesh: NO route and NO provable frontier (near poly %d, start %d, prefix %zu pts) "
                     "pass=%s expands=%d attempts=%d banned=%d",
                     fp, start, bestPrefix.size(), stats.pass, stats.expands, stats.attempts,
                     stats.bannedEdges);
            Log::Write("NAV-ROUTE", nm);
            return Plan::NoPath;
        }

        stats.pass     = "frontier";
        stats.nearDist = stats.shortfall;
        outPoly = rawPoly;

        char fm[320];
        snprintf(fm, sizeof(fm),
                 "frontier: goal unreachable (%s); source=%s ending at poly %d (%.2f,%.2f,%.2f), "
                 "%.1fm short. corners=%zu cutByValidation=%zu attempts=%d banned=%d",
                 best.fail ? best.fail : "validation never passed",
                 useNear ? "near-poly corridor" : "banked proven prefix",
                 stats.endPoly, rawPoly.back().x, rawPoly.back().y, rawPoly.back().z,
                 stats.shortfall, rawPoly.size(), useNear ? fr.cutFrom : 0,
                 stats.attempts, stats.bannedEdges);
        Log::Write("NAV-ROUTE", fm);

        return rawPoly.size() >= 2 ? Plan::Frontier : Plan::NoPath;
    }
}

} // namespace PathSearch

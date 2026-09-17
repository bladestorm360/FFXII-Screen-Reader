#include "navigation/path_search.h"
#include "navigation/nav_mesh.h"
#include "navigation/nav_footprint.h"
#include "navigation/map_query.h"
#include "navigation/nav_common.h"
#include "navigation/path_funnel.h"
#include "navigation/path_validate.h"
#include "navigation/path_corridor.h"
#include "navigation/path_surface_goal.h"
#include "navigation/path_repair.h"
#include "navigation/nav_blocked.h"
#include "navigation/player_state.h"
#include "navigation/reach_gate.h"
#include "navigation/map_names.h"
#include "navigation/map_route_rules.h"
#include "navigation/path_march.h"
#include "core/logger.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <queue>
#include <unordered_map>
#include <unordered_set>
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
// How near the GOAL a breach's stop point has to be for the re-cost step to treat it as a
// final-approach breach and refuse to price the portal (Session 111). Twice the arrival tolerance:
// generous enough to cover the 2.55 m stop the protection was written for, tight enough that a walk
// dying 17 m from the target -- which is a corridor problem, not an arrival problem -- still earns a
// retry. Not a tuning knob; it is kArrivalTol restated, so the two move together.
constexpr float kFinalApproachDist = 2.0f * kArrivalTol;

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
//
// TWO CUTS SURVIVE, AND BOTH ARE THE GAME'S OWN WORD RATHER THAN OURS: an edge with no neighbour (there
// is nothing to price), and -- since S182 -- a floor a SCRIPT has closed (see below). Every measurement
// WE make stays a price.
constexpr float kTightPenalty   = 500.0f;    // body does not fit anywhere along this edge
constexpr float kTerrainPenalty = 2000.0f;   // the LEADER's floor class may not stand on the neighbour
constexpr float kBlockedPenalty = 2000.0f;   // the player PHYSICALLY failed to get past here
constexpr float kBreachPenalty  = 500.0f;    // a validated leg through this portal did not walk
// ...AND IT MULTIPLIES WHEN THE SAME BREACH COMES BACK (Session 115). A flat +500 per attempt cannot
// move A* off a corridor whose alternative is dearer than 2000: map 568 measured four attempts
// returning the IDENTICAL corridor with the IDENTICAL breach (`bad=3 len=22.60m reached=0.07m
// stop=(16.2,-7.72,120.4)` four times) while the price crawled 500->1000->1500. THE RE-COST CANNOT
// WIN AN ARGUMENT IT IS PRICED OUT OF. A retry that reproduces the same stop has PROVED the price is
// too low, so that -- and only that -- escalates: 500 -> 1500 -> 4500. A retry whose breach MOVED is
// progress and keeps the gentle additive step it has always had.
constexpr float kBreachEscalation = 3.0f;
// How near two breach stops must be to count as the SAME breach rather than a new one. A
// MEASUREMENT, not a leg index -- the same correction S111 applied to the final-approach rule. Well
// under the body radius, so "the walk died in the same spot again" and "the walk got further this
// time" cannot be confused.
constexpr float kIdenticalStopTol = 0.5f;
// Crossing a transition surface that is not the route's own goal fires a map jump the player did
// not ask for (S100: the 311<->321 auto-walk bounce). Same tier as a physical block: avoided
// whenever any alternative exists, still crossable when it is genuinely the only way.
constexpr float kForeignSeamPenalty = 2000.0f;
// A FLOOR A SCRIPT HAS CLOSED IS CUT, NOT PRICED (S182 -- the user's S181 ruling, "price what we infer,
// cut what the game declares"). There is no constant for it, because there is no price.
//
// What it replaced: S179 priced such a poly +20000, and only while the `Unreachable filter` row was On.
// With the row Off (the default) it paid the flat kTerrainPenalty like any bit-23 ledge, so a shut door
// lying across the short way was simply bought: Mirror of the Soul's route paid terrain=8000 through
// door 4's shut floor and the player stuck on it. Even at +20000 a target whose ONLY approach was
// through a shut door still got a confident route through it -- a wrong route, which for a blind player
// is worse than "No path" (L-75).
//
// WHY A CUT IS LEGITIMATE HERE WHEN S96 PROVED CUTS OVER-REFUSE: the evidence class. S96 cut on static
// terrain TYPE, our proxy for walkability, and the party wades that water. A script-closed floor --
// raw class bit clear, effective bit set (ReachGate::ScriptClosedFlags) -- is the engine's own runtime
// refusal after a `setmapidfloor`: measured on S179's doors and S181's waterfalls. The row no longer
// has any say in routing; it only decides what the LIST shows.
//
// The closed floor the TARGET or the PLAYER stands on is exempt, by material id: a route to a shut door
// ends on that door's own shut floor, and a player standing on a floor a script has just closed must
// still be able to route off it. The exemption is by MATERIAL, so a goal on the same material as a
// barrier also exempts that barrier -- tighten it to the goal's connected patch only if a case turns up.

// A portal a later attempt should avoid, because the taut path through it turned out not to be
// walkable. Scoped to ONE request -- never cached across presses, because the obstacle may be a door
// that opens, and a permanent ban would be exactly the "learned label" this project forbids.
//
// IT IS A PRICE, NOT A BAN (Session 96). A ban is binary and permanent for the request: ban the only
// opening and the goal is unreachable, which is exactly how a route that A* had already reached the
// goal with came back "No path". `pen` accumulates, so a portal that keeps failing keeps getting
// dearer and the search moves off it on its own, without any attempt ever losing the option.
//
// `stop` is where the body was refused when this portal earned its last price (Session 115). It is
// what tells "the retry produced the same disproved corridor" apart from "the retry got further" --
// see kBreachEscalation. Still a price: even the escalated figure is one A* will pay when the portal
// is genuinely the only way through, which is the property S96 bought and S108 re-bought.
struct BannedEdge { PolyId poly; int edge; float pen; FVec3 stop; };

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
         std::vector<FVec3>& rawPoly, std::vector<FVec3>& outPoly, Stats& stats,
         const std::vector<NavMesh::PolyId>* seamPolys) {
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
    // S100: crossings priced because the LEADER'S OWN FLOOR CLASS refuses the ground (bit 23 for
    // class 0 -- the flooded channels), and crossings priced because the poly belongs to a FOREIGN
    // map-jump surface (walking onto one fires a transition the route did not ask for).
    int refTerrain = 0, refForeignSeam = 0;
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
    // The GOAL's own map-jump group (0 for every non-transition target). Any OTHER group's polys
    // are a foreign transition surface: walking onto one teleports the player mid-route -- the
    // S100 tester round's 311<->321 bounce. Priced, never cut, and the goal's own seam is exempt
    // by construction.
    const int goalGroup = (!goalOffMesh) ? NavMesh::MapJumpGroup(goal) : 0;
    // S182: floors a script has closed are CUT (see the note above kTightPenalty's block), with the
    // start's and the goal's own closed material exempt. The class bit is read ONCE per request.
    const uint32_t closedBit = ReachGate::PartyRefuseBit();
    uint32_t closedExemptMats = 0;
    for (const PolyId q : { start, goal }) {
        uint32_t qr = 0, qe = 0;
        if (q == kNoPoly || !NavMesh::PolyFlags(q, qr, qe)) continue;
        if (ReachGate::ScriptClosedFlags(qr, qe, closedBit)) closedExemptMats |= 1u << ReachGate::Material(qr);
    }
    // The evidence for every cut, across all attempts: how many crossings, which materials, and one poly
    // to put a position on. A wrong cut then reads as a named map + material + place in one grep.
    int      refClosed        = 0;
    uint32_t closedMatsCut    = 0;
    // S185: closed-flag crossings the cut let through because no script on this map can open that
    // material. They are PRICED, not cut. Printed beside the cut so one log line answers both.
    int      refClosedPriced  = 0;
    uint32_t closedMatsPriced = 0;
    const uint32_t openableMats = ReachGate::OpenableFloorMask();
    PolyId   closedSample  = kNoPoly;

    // ---- PER-MAP RULE: class-refused ground is CUT on a flagged map (S183, map_route_rules.h) --------
    // Falls of Time only, by the user's ruling. The map id is read ONCE per request. The march refuses
    // to graze across the same ground for the whole request (the scope below), and one `map-rule:` line
    // on every request on a flagged map -- written by the logger's destructor, so every return path
    // after this point reports -- says how many crossings were cut and how many grazes were refused.
    const int  ruleMapId     = MapNames::CurrentMapId();
    const bool refuseTerrain = MapRouteRules::MapRefusesTerrain(static_cast<uint32_t>(ruleMapId));
    PathMarch::StrictTerrainScope strictTerrain(refuseTerrain);
    int    refRuleCut    = 0;
    PolyId ruleCutSample = kNoPoly;
    struct RuleLog {
        bool on; int mapId; const int& cut; const PolyId& sample;
        ~RuleLog() {
            if (!on) return;
            char where[96] = "";
            FVec3 sc{};
            if (sample != kNoPoly && NavMesh::PolyCentroid(sample, sc))
                snprintf(where, sizeof(where), "; first at poly %d (%.1f,%.1f,%.1f)", sample, sc.x, sc.y, sc.z);
            char m[288];
            snprintf(m, sizeof(m),
                     "map-rule: map %d refuses class-refused ground -- %d crossing(s) CUT, %d graze(s) "
                     "refused%s", mapId, cut, PathMarch::StrictGrazesRefused(), where);
            Log::Write("NAV-ROUTE", m);
        }
    } ruleLog{ refuseTerrain, ruleMapId, refRuleCut, ruleCutSample };

    // ---- THE GOAL SURFACE, observed but never obeyed ------------------------------------------
    // `seamPolys` is the map-jump surface the target belongs to, or null for everything that is not
    // a walk-onto transition. The search's behaviour does NOT change: this set is only tested on a
    // pop, and only to remember the FIRST member A* reaches. What that fact is for lives below the
    // validated-Route return, where it can only turn a "No path" into a route. See
    // path_surface_goal.h for why the endpoint is a portal and not a distance.
    std::unordered_set<PolyId> surfaceSet;
    if (seamPolys && !seamPolys->empty()) {
        surfaceSet.reserve(seamPolys->size() * 2);
        for (const PolyId sp : *seamPolys) surfaceSet.insert(sp);
    }

    std::vector<BannedEdge> banned;
    // The terrain price the PREVIOUS attempt's corridor paid, or -1 before any attempt has produced
    // one. It exists for one check, below: a retry that starts paying terrain where its predecessor
    // paid none has stopped looking for a detour and started buying its way onto ground the party's
    // floor class may not stand on. See the guard beside the re-cost.
    float prevPenTerrain = -1.0f;
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
    // The first member of the goal's map-jump surface the LAST pass popped. Hoisted for the same
    // reason as `came` and RESET WITH IT, so the touch and the parent links that reach it can never
    // come from different passes -- a corridor rebuilt from one pass's links to another pass's poly
    // is the defect path_corridor.h records BuildFrontier being written to fix.
    PolyId surfaceTouch = kNoPoly;

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
        surfaceTouch = kNoPoly;              // same lifetime as `came` -- see its declaration
        open.push(Node{ Dist3(startC, goalC), start });

        PolyId reached = kNoPoly;
        pr.bestNearD = Dist3(startC, goalC);

        while (!open.empty() && stats.expands < kMaxTotalExpand) {
            const Node cur = open.top();
            open.pop();
            const PolyId p = cur.p;
            ++stats.expands;

            if (IsGoal(p)) { reached = p; break; }
            // THE FIRST POP OF ANY SURFACE MEMBER is where walking reaches the transition soonest,
            // because A* pops in cost order. Recorded, never acted on here: the search goes on
            // exactly as it would have, and a route that validates never consults this.
            // `p != start` guards the degenerate case of already standing on the seam -- the
            // planner's kAtExitDist branch normally answers that before a search is ever run, and a
            // zero-length corridor is not something to hand the funnel.
            if (surfaceTouch == kNoPoly && p != start && surfaceSet.count(p) != 0) surfaceTouch = p;
            NoteFallback(p);
            if (goalOffMesh && fallbackPoly != kNoPoly) break;

            FVec3 pc{};
            if (!Centroid(p, pc)) continue;
            // ...and the same for the frontier. `bestNear` is where a shortfall route ENDS, so it is a
            // place the player gets told to stand; a priced-but-expanded water poly must never win it.
            // Since S100 that is enforced with the LEADER'S OWN floor test, not just the type test --
            // a frontier must never end the player in the flooded channel their class cannot enter.
            const float dGoal = Dist3(pc, goalC);
            if (dGoal < pr.bestNearD && NavMesh::Walkable(p) && !NavMesh::TerrainRefused(p)) {
                pr.bestNearD = dGoal;
                pr.bestNear  = p;
            }

            const auto gIt = gScore.find(p);
            if (gIt == gScore.end()) continue;
            const float gCur = gIt->second;

            const float penTCur = gPenT[p], penOCur = gPenO[p];

            for (int e = 0; e < 3; ++e) {
                const PolyId n = NavMesh::Neighbor(p, e);
                // THE ONLY TRUE CUT LEFT. No neighbour is not an expensive edge, it is the absence of
                // one -- there is nothing on the other side to price.
                if (n == kNoPoly) { ++refNoPoly; continue; }

                // THE OTHER TRUE CUT (S182, NARROWED S185): a floor a script has closed, unless it is
                // the start's or the goal's own. Never expanded, so no corridor, frontier, surface goal
                // or repair rung built from this search can lead across it.
                //
                // S185 ADDED THE SECOND HALF OF THE TEST, and S182's own comment is what asked for it:
                // "the falsifier for the cut: a map where this fires and the player walks that crossing
                // by hand". The Dreadnought Leviathan is that map. 44 crossings were cut here as
                // script-closed, the goal was in the start's own adjacency component the whole time,
                // and the player walked the route by hand while the mod said "No path" twenty-one
                // times -- progress-blocking for anyone who does not know to track by crow-flies.
                //
                // THE USER'S RULE DECIDES IT: price what we INFER, cut what the game DECLARES. The
                // closed-flag shape alone is an inference. A material some door routine can OPEN is a
                // declaration -- the script says it is a door and says how it opens. So the cut now
                // needs both. A closed floor no script on this map can open falls through to the
                // ordinary terrain PRICE below, which is S96's rule and what every map but 184 uses.
                {
                    uint32_t cr = 0, ce = 0;
                    if (NavMesh::PolyFlags(n, cr, ce) && ReachGate::ScriptClosedFlags(cr, ce, closedBit) &&
                        !((closedExemptMats >> ReachGate::Material(cr)) & 1u)) {
                        const uint32_t mat = ReachGate::Material(cr);
                        if ((openableMats >> mat) & 1u) {
                            ++refClosed;
                            closedMatsCut |= 1u << mat;
                            if (closedSample == kNoPoly) closedSample = n;
                            continue;
                        }
                        // Inferred, not declared: priced below like any other refused ground. Counted
                        // so the log can show what this change let through.
                        ++refClosedPriced;
                        closedMatsPriced |= 1u << mat;
                    }
                }

                // THE PER-MAP CUT (S183): on a map whose row refuses class-refused ground, water and
                // every other ground the leader's class refuses is not expanded -- except the route's own
                // start and goal polys and the goal's own map-jump surface, because two of Falls of
                // Time's exits sit on refused polys (2751, 2727) and a player standing on one must still
                // route off it.
                if (refuseTerrain && n != start && n != goal &&
                    !(goalGroup != 0 && NavMesh::MapJumpGroup(n) == goalGroup) &&
                    (!NavMesh::Walkable(n) || NavMesh::TerrainRefused(n))) {
                    ++refRuleCut;
                    if (ruleCutSample == kNoPoly) ruleCutSample = n;
                    continue;
                }

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
                } else if (NavMesh::TerrainRefused(n)) {
                    // THE LEADER'S OWN FLOOR CLASS REFUSES THIS GROUND (Session 100, both gates
                    // closed). The mover hands FUN_00230a40 the class FUN_002681d0 wrote -- 0 for
                    // the player-controlled character, whose branch requires bit 23 CLEAR -- so a
                    // bit-23 poly (the flooded channels) is a WALL to the leader in play: the 315
                    // walk stops at the exact poly-23|224 flag boundary, and STANDING-ON-REFUSED
                    // has never once fired. A PRICE, never a cut (the S96 lesson): a goal whose
                    // every approach is flooded still resolves, it just pays.
                    ++refTerrain;
                    uint32_t nr = 0, ne = 0;
                    const bool haveFlags = NavMesh::PolyFlags(n, nr, ne);
                    if (haveFlags) NoteRefusedFlags(ne);
                    penT += kTerrainPenalty;
                }
                // A FOREIGN TRANSITION SURFACE IS A TELEPORT, NOT A FLOOR (Session 100 tester
                // round: the replanned Lowtown route cornered ON the No. 10 Channel seam and
                // auto-walk bounced 311<->321 twice). Any seam group other than the goal's own is
                // priced so routes steer around it whenever an alternative exists.
                {
                    const int njg = NavMesh::MapJumpGroup(n);
                    if (njg != 0 && njg != goalGroup) { ++refForeignSeam; penO += kForeignSeamPenalty; }
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

        // WHICH GROUND THE CORRIDOR PAID FOR (S182). `terrain=4000` said two polys the leader's class
        // refuses were bought, and nothing said which: S181 read Falls of Time's as the waterfalls, while
        // every march breach in that log named static material-0 ground (`nbrEff=0x07800000`). The poly,
        // its effective flags, its material and where it is settle that in one line. Only a corridor
        // that pays terrain prints it.
        if (pr.penTerrain > 0.0f) {
            char tp[360]; int q = 0, shown = 0, total = 0;
            for (const PolyId cp : pr.chain) {
                if (cp == start) continue;                 // a price is paid on ENTERING a poly
                if (NavMesh::Walkable(cp) && !NavMesh::TerrainRefused(cp)) continue;
                ++total;
                if (shown >= 4 || q >= static_cast<int>(sizeof(tp)) - 72) continue;
                uint32_t cr = 0, ce = 0;
                FVec3 cc{};
                NavMesh::PolyFlags(cp, cr, ce);
                NavMesh::PolyCentroid(cp, cc);
                q += snprintf(tp + q, sizeof(tp) - static_cast<size_t>(q),
                              "%s%d eff=0x%08X mat=%u at (%.1f,%.1f,%.1f)", shown ? " | " : "", cp, ce,
                              ReachGate::Material(cr), cc.x, cc.y, cc.z);
                ++shown;
            }
            char tm[440];
            snprintf(tm, sizeof(tm), "terrain paid: attempt %d, %d poly(s) the party's class refuses: %s%s",
                     attempt, total, q ? tp : "none found on the chain", total > shown ? " ..." : "");
            Log::Write("NAV-ROUTE", tm);
        }

        // ---- string-pull, then repair the corners, then validate -----------------------------------
        std::vector<Portal> plain;
        plain.reserve(pr.portals.size());
        for (const PortalRef& pref : pr.portals) plain.push_back(pref.p);

        bool  anomaly = false;
        float lenKept = 0.0f, lenOther = 0.0f;
        std::vector<FVec3> poly;
        std::vector<int>   polyIdx;      // which portal each corner came from -- see PathFunnel::Unpull
        PathFunnel::BestPolarity(from, to, plain, poly, anomaly, lenKept, lenOther, &polyIdx);

        // Pull corners off the boundary BEFORE dropping passed waypoints, because an inset can move a
        // corner past the player and the drop is what notices.
        const int inset = PathFunnel::InsetCorners(poly);
        // AFTER any step that rebuilds or moves the polyline. The old code ran this once, before a
        // fallback swapped in a freshly built vector -- so on every breaching route the S78 leg-0
        // reversal fix was silently bypassed.
        const int droppedWp = PathFunnel::DropPassedWaypoints(from, poly, &polyIdx);

        {
            // The non-anomaly format is BYTE-IDENTICAL to the pre-S124 line on purpose: working-route
            // invariance is proven by diffing these lines across logs, so no field may move or rename.
            char fm[320];
            snprintf(fm, sizeof(fm),
                     "funnel: attempt %d polarity=as-labelled kept=%.1fm other=%.1fm corners=%zu/%zu "
                     "clipped=%d blocked=%d inset=%d droppedWp=%d%s",
                     attempt, lenKept, lenOther,
                     poly.size(), plain.size(), pr.clipped, pr.blocked, inset, droppedWp,
                     anomaly
                         ? "   <== MESH LABELLING ANOMALY: mirrored funnel measured shorter; "
                           "as-labelled kept (S86: the winding is authoritative)"
                         : "");
            Log::Write("NAV-ROUTE", fm);
        }

        const PathValidate::LegReport rep =
            PathValidate::CheckLegs(poly, probesLeft, kArrivalTol, goalGroup);
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
            char bad[448] = "";
            if (rep.firstBad) {
                int q = snprintf(bad, sizeof(bad),
                         // The trailing " WALL" flag is GONE with the veto that set it (Session 97). It
                         // was `rep.walls`, and on a wall verdict the branch returned BEFORE `Diagnose`
                         // ran -- so the two volume fields beside it printed their defaults, not
                         // measurements, on exactly the routes that flag appeared on. Every breach now
                         // comes from the body walk and every breach runs `Diagnose`, so `vol@stop` and
                         // `vol@+0.3m` mean what they say and a redundant flag would only re-open the
                         // habit of reading an unmeasured zero as an answer.
                         " bad=%zu len=%.2fm reached=%.2fm stop=(%.1f,%.2f,%.1f) why=%s "
                         "stopPoly=%d walk=%d eff=0x%08X | corner: poly=%d clear=%d margin=%.2fm  "
                         "vol@stop=%d vol@+0.3m=%d",
                         rep.firstBad, rep.badLength, rep.badReached,
                         rep.badStopAt.x, rep.badStopAt.y, rep.badStopAt.z,
                         PathValidate::CauseName(rep.badCause),
                         rep.badStopPoly, rep.badStopWalk ? 1 : 0, rep.badStopFlags,
                         rep.badCornerPoly, rep.badCornerClear ? 1 : 0, rep.badCornerMargin,
                         rep.badStopInVolume ? 1 : 0, rep.badAheadInVolume ? 1 : 0);
                // The march's own breach detail (Session 100): the exact crossing the mover's accept
                // rule refuses. `from=poly:edge`; `nbr=-1` is a true boundary (no neighbour), else
                // `nbrEff` says why the neighbour refused -- a script-flipped group reads at a glance.
                if (rep.badCause == PathValidate::StopCause::March && q > 0 &&
                    q < static_cast<int>(sizeof(bad)))
                    snprintf(bad + q, sizeof(bad) - static_cast<size_t>(q),
                             " | march: from=%d:%d nbr=%d nbrEff=0x%08X",
                             rep.badMarchPoly, rep.badMarchEdge, rep.badMarchNbr, rep.badMarchNbrEff);
            }
            char vm[768];
            // `volHit`/`volWalked` REPLACE `walls` (Session 97). `walls` was the count of legs the
            // volume probe VETOED -- 16 of 16 routes on map 315, against zero objections from the body
            // sweep. The probe now decides nothing, so the pair reads: how many legs it flagged, and how
            // many of those the body then walked anyway. `volWalked == volHit` on routes the tester
            // walks means the probe was measuring something the party does not collide with.
            // `long=` counts one-shot CLEARs above kLongLegResweep that were confirm-walked (S100);
            // `march=/marchBlind=/marchGraze=` are the S100 adjacency march -- see LegReport.
            snprintf(vm, sizeof(vm),
                     "validate: attempt %d legs checked=%zu/%zu probes=%d worstFrac=%.2f tight=%d@%zu "
                     "resweep=%d rescued=%d long=%d pinned=%d apron=%d swept=%d blind=%d volHit=%d volWalked=%d "
                     "march=%d marchBlind=%d marchGraze=%d %s%s%s",
                     attempt, rep.checked, rep.total, rep.probes, rep.worstFraction,
                     rep.tightCorners, rep.firstTight, rep.resweeps, rep.rescued, rep.longWalks,
                     rep.pinned, rep.apronArrivals, rep.swept, rep.blind, rep.volHit, rep.volWalked,
                     rep.march, rep.marchBlind, rep.marchGraze,
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

        // ---- WHICH KIND OF FAILURE IS THIS? (Session 116) -------------------------------------------
        // Asked BEFORE the repair ladder, because it decides whether the ladder is the right tool at
        // all -- and it costs nothing to ask (the adjacency march spends no probes). See
        // path_corridor.h: A* certifies CROSSINGS, never the TRAVEL BETWEEN two consecutive crossings,
        // so a corridor whose every opening is passable can still have a hop the body cannot make.
        //
        //   corridor CLEAR  -> a shape problem inside walkable ground. The ladder's own case; run it.
        //   corridor BREACH -> the corridor is not walkable. No rung can help (each one reshapes a
        //                      path through the same hop) and the re-cost has a MEASUREMENT to aim at
        //                      instead of the portal nearest the failing chord's midpoint.
        const PathCorridor::CorridorMarch cmarch =
            PathCorridor::MarchCorridor(from, to, plain, kArrivalTol);
        {
            char cmm[336];
            if (cmarch.breached)
                snprintf(cmm, sizeof(cmm),
                         "corridor march: BREACH at hop %zu/%zu -- crossing %d:%d -> nbr=%d "
                         "nbrEff=0x%08X at (%.1f,%.2f,%.1f), grazes=%d noVerdict=%d. The CORRIDOR is "
                         "not walkable, so the repair ladder is skipped (every rung would reshape a "
                         "path through this same hop) and this crossing is what gets re-costed",
                         cmarch.hop, cmarch.hops, cmarch.fromPoly, cmarch.edge, cmarch.nbr,
                         cmarch.nbrEff, cmarch.hitPoint.x, cmarch.hitPoint.y, cmarch.hitPoint.z,
                         cmarch.grazes, cmarch.noVerdict);
            else
                snprintf(cmm, sizeof(cmm),
                         "corridor march: CLEAR over %zu hop(s) (grazes=%d noVerdict=%d) -- the "
                         "corridor walks and only the taut chord did not, which is the repair "
                         "ladder's own case",
                         cmarch.hops, cmarch.grazes, cmarch.noVerdict);
            Log::Write("NAV-ROUTE", cmm);
        }

        // ---- REPAIR BEFORE RE-SEARCHING -------------------------------------------------------------
        // The ladder itself lives in `path_repair.{h,cpp}` -- it needs none of this function's search
        // state, and the reasoning behind each rung is long enough to belong beside the code it governs.
        // What stays here is the only part that is `Run`'s business: spending the budget and adopting
        // the result. A route that validates on the chord never enters `Mend` at all.
        if (!cmarch.breached) {
            const PathRepair::Result mend =
                PathRepair::Mend(poly, polyIdx, plain, rep, from, to, probesLeft, kArrivalTol);
            // Probes are spent whether or not a rung won, so they come off the budget either way.
            probesLeft -= mend.probes;
            stats.rays += mend.probes;
            if (mend.ok) {
                best       = pr;
                bestPoly   = mend.poly;
                bestReport = mend.report;
                rawPoly    = mend.poly;
                stats.repaired = mend.detail;
                break;                      // the corridor walks; only the shortcut across it did not
            }
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

        // ---- A PENALTY IS ONLY A DETOUR WHEN A DETOUR EXISTS (Session 115) ------------------------
        // This is S108's regression written down as a stop condition instead of trusted not to
        // happen. Session 106 priced the ground around map 568's guards, the discs sat across the
        // ONLY corridor, and the search did not take a wider berth -- it paid the price by leaving
        // through the next-cheapest thing available, which was ground the party's class cannot stand
        // on (`corridor paid terrain=8000` on every armed request), and validation then killed the
        // route. Door 2 became unroutable where it had worked.
        //
        // The escalation above makes prices rise faster, so the same failure is exactly what it could
        // buy. It cannot: the moment a retry's corridor starts paying terrain where its predecessor
        // paid none, the re-cost has stopped finding detours and started bidding for a worse KIND of
        // route, and the loop stops rather than spending its remaining attempts proving it. A route
        // that validates has already broken out above, so this can never refuse a walkable route --
        // and a corridor that pays terrain from attempt 1 is untouched, because nothing got worse.
        if (prevPenTerrain == 0.0f && pr.penTerrain > 0.0f) {
            char tm[240];
            snprintf(tm, sizeof(tm),
                     "replan: attempt %d's corridor now pays terrain=%.0f where the previous one paid "
                     "none -- the re-cost is buying its way onto ground the party's class may not "
                     "stand on (S108); stopping here and going to the frontier",
                     attempt, pr.penTerrain);
            Log::Write("NAV-ROUTE", tm);
            break;
        }
        prevPenTerrain = pr.penTerrain;

        // RE-COST the portal the breaching leg crosses. Which one is a geometric question: the leg that
        // failed runs between two taut corners, and the portal it crosses is the one whose span sits
        // closest to that leg's midpoint. Make that (poly, edge) dearer and search again.
        //
        // NEVER RE-COST A FINAL-APPROACH BREACH: the portal nearest the last leg is the one that gets
        // you TO the target, and banning it is how a route within 2.55 m of an exit became "goal
        // unreachable" and a frontier 15.2 m short. The banked prefix already carries the honest answer.
        //
        // BUT "FINAL APPROACH" IS A DISTANCE, NOT A LEG INDEX (Session 111). The old test was
        // `firstBad >= poly.size() - 1`, which on a TWO-corner route makes leg 1 both the first and
        // the last leg -- so any breach at all broke the attempt loop with `attempts=1 banned=0` and
        // no retry, even when the body stopped at the very start of a long leg. Map 568 measured it:
        // `bad=1 len=19.22m reached=1.79m stop=(21.5,-8.00,121.3)` against a target at (38.6,117.9)
        // -- the walk died 17 m from the goal and the search gave up, while a route from that exact
        // spot had validated seconds earlier. The protection is now keyed on what it was always
        // about: how close the BODY STOPPED to the target, at twice the arrival tolerance, so every
        // case its evidence came from (a stop 2.55 m out) is still protected.
        // ---- WHICH CROSSING GETS PRICED, AND FROM WHICH SOURCE (Session 116) ----------------------
        // TWO SOURCES, AND THE MEASURED ONE WINS. When the corridor march named a crossing, that IS
        // the obstruction -- the mover's own accept rule refused it, on the corridor's own openings,
        // and nothing about a chord is involved. Everything below it is the older, weaker question:
        // the corridor walks, the chord did not, so INFER which opening the chord failed near.
        PolyId killPoly = kNoPoly;
        int    killEdge = -1;
        FVec3  killStop = rep.badStopAt;   // what "the same breach again" means to the escalation
        FVec3  a{}, b{};                   // the breaching leg, for the log line

        if (cmarch.breached && cmarch.fromPoly != kNoPoly && cmarch.edge >= 0) {
            // THE SAME TWO PROTECTIONS THE INFERRED PATH HAS ALWAYS HAD, applied to the better
            // measurement rather than dropped because it is better.
            if (NavCommon::Distance2D(cmarch.hitPoint, to) <= kFinalApproachDist) {
                Log::Write("NAV-ROUTE",
                           "replan: the corridor's refused crossing IS the final approach -- not "
                           "re-costing it (that opening is the one that gets you to the target); "
                           "going to the frontier instead");
                break;
            }
            if (cmarch.fromPoly == start) {
                Log::Write("NAV-ROUTE",
                           "replan: the corridor's refused crossing is the START poly's own edge -- "
                           "not re-costing it (that would strand the seed); going to the frontier "
                           "instead");
                break;
            }
            killPoly = cmarch.fromPoly;
            killEdge = cmarch.edge;
            killStop = cmarch.hitPoint;
            if (rep.firstBad > 0 && rep.firstBad < poly.size()) {
                a = poly[rep.firstBad - 1];
                b = poly[rep.firstBad];
            }
        }

        if (killPoly == kNoPoly) {
            const bool onLastLeg  = (rep.firstBad >= poly.size() - 1);
            const bool stopIsNearTarget =
                NavCommon::Distance2D(rep.badStopAt, to) <= kFinalApproachDist;
            if (rep.firstBad == 0 || plain.empty() || (onLastLeg && stopIsNearTarget)) break;
            a = poly[rep.firstBad - 1];
            b = poly[rep.firstBad];
            // ON A MARCH BREACH, AIM AT THE MEASUREMENT (Session 100, failure-path-only). A 43 m leg's
            // midpoint can sit 20 m from the refused crossing; `badStopAt` is where the mover's own rule
            // said no, so the portal nearest THAT is the one to make expensive. Sweep/budget breaches
            // keep the midpoint -- their stop is already how far the walk got, not a single crossing.
            const bool  useStop = (rep.badCause == PathValidate::StopCause::March);
            const FVec3 legMid  = useStop
                ? rep.badStopAt
                : FVec3{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };
            size_t bestIdx = 0;
            float  bestD2  = -1.0f;
            for (size_t k = 0; k < pr.portals.size(); ++k) {
                const Portal& q = pr.portals[k].p;
                const float mx = (q.left.x + q.right.x) * 0.5f, mz = (q.left.z + q.right.z) * 0.5f;
                const float dx = mx - legMid.x, dz = mz - legMid.z;
                const float d2 = dx * dx + dz * dz;
                if (bestD2 < 0.0f || d2 < bestD2) { bestD2 = d2; bestIdx = k; }
            }
            PortalRef kill = pr.portals[bestIdx];
            // The START poly's own edges are left alone: pricing one can push the search off its own seed
            // and reproduce S76's `expands=1 touched=0` (the search never ran).
            //
            // BUT A LONG FIRST LEG'S MIDPOINT LIES ABOUT WHERE THE OBSTRUCTION IS (Session 111; first
            // built in S106, reverted in S108 on evidence that turned out to be contaminated -- the
            // danger zones had already deformed every route in that log, so this branch never ran and
            // was wrongly recorded as "never fires"). On this mesh the start triangle is often a whole
            // room, so leg 1 can be 8 m long with its midpoint nearest the seed's own edge while the
            // sweep stopped at the far end: map 568 measured `len=8.00m reached=7.04m
            // stop=(23.4,-8.00,121.5)` and then broke out with `attempts=1 banned=0`.
            //
            // Before conceding, re-attribute by the STOP POINT -- where the body was actually refused.
            // A portal there is a real candidate that the seed rule was never written to protect; only
            // when THAT portal is also the seed's own edge is the breach genuinely on the seed.
            if (kill.poly == start) {
                size_t stopIdx = 0;
                float  stopD2  = -1.0f;
                for (size_t k = 0; k < pr.portals.size(); ++k) {
                    const Portal& q = pr.portals[k].p;
                    const float mx = (q.left.x + q.right.x) * 0.5f, mz = (q.left.z + q.right.z) * 0.5f;
                    const float dx = mx - rep.badStopAt.x, dz = mz - rep.badStopAt.z;
                    const float d2 = dx * dx + dz * dz;
                    if (stopD2 < 0.0f || d2 < stopD2) { stopD2 = d2; stopIdx = k; }
                }
                if (pr.portals[stopIdx].poly != start) {
                    kill = pr.portals[stopIdx];
                    char am[224];
                    snprintf(am, sizeof(am),
                             "replan: midpoint attribution landed on the START poly's own edge; "
                             "re-attributed by the sweep stop (%.1f,%.1f) -> portal (poly %d, edge %d)",
                             rep.badStopAt.x, rep.badStopAt.z, kill.poly, kill.edge);
                    Log::Write("NAV-ROUTE", am);
                } else {
                    Log::Write("NAV-ROUTE",
                               "replan: the breaching leg crosses the START poly's own edge -- not re-costing it "
                               "(that would strand the seed); going to the frontier instead");
                    break;
                }
            }
            killPoly = kill.poly;
            killEdge = kill.edge;
        }
        // ---- PRICE IT, AND ESCALATE IF THIS IS THE SAME BREACH AGAIN (Session 115) ----------------
        // The old rule added a flat kBreachPenalty every time, which map 568 proved cannot work: the
        // same portal came back four times with the body stopping at the SAME COORDINATE, while the
        // price crawled 500 -> 1000 -> 1500 against alternatives dearer than that. An identical stop
        // is not new information about the route, it is a measurement that the price was too low --
        // so it multiplies. A stop that MOVED means the retry made progress and keeps the additive
        // step, because that case has never been the problem.
        // `killStop` -- not `rep.badStopAt` -- is what "the same breach again" means here, because the
        // crossing may have been named by the corridor march rather than inferred from the chord. Each
        // source carries its own notion of WHERE the refusal is, and comparing one to the other would
        // make an identical breach look like a moved one and quietly disable the escalation.
        float newPen    = kBreachPenalty;
        bool  seen      = false;
        bool  escalated = false;
        for (BannedEdge& bn : banned) {
            if (bn.poly != killPoly || bn.edge != killEdge) continue;
            seen = true;
            escalated = NavCommon::Distance2D(bn.stop, killStop) <= kIdenticalStopTol;
            bn.pen  = escalated ? bn.pen * kBreachEscalation : bn.pen + kBreachPenalty;
            bn.stop = killStop;
            newPen  = bn.pen;
            break;
        }
        if (!seen) banned.push_back(BannedEdge{ killPoly, killEdge, kBreachPenalty, killStop });
        stats.bannedEdges = static_cast<int>(banned.size());
        {
            char esc[152] = "";
            if (escalated)
                snprintf(esc, sizeof(esc),
                         " (ESCALATED x%.0f -- this crossing refused AGAIN at the same spot (%.1f,%.1f), "
                         "so the previous price was not enough to move the search)",
                         kBreachEscalation, killStop.x, killStop.z);
            char rm[416];
            snprintf(rm, sizeof(rm),
                     "replan: breach on leg %zu/%zu (%.1f,%.1f)->(%.1f,%.1f); crossing (poly %d, edge %d) "
                     "[%s] re-costed to %.0f%s and searching again -- attempt %d/%d, expands %d/%d",
                     rep.firstBad, rep.total, a.x, a.z, b.x, b.z,
                     killPoly, killEdge,
                     cmarch.breached ? "MEASURED by the corridor march" : "inferred from the chord",
                     newPen, esc,
                     attempt, kMaxAttempts, stats.expands, kMaxTotalExpand);
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
    if (!best.reachedGoal || best.penTerrain > 0.0f || best.penOther > 0.0f ||
        refTerrain > 0 || refForeignSeam > 0) {
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
                 "costed: noPoly=%d(cut) unwalkable=%d terrain=%d foreignSeam=%d edge=%d "
                 "rePriced=%d measuredBlock=%d tightXing=%d volXing=%d "
                 "| corridor paid terrain=%.0f other=%.0f | refused eff-flags: %s",
                 refNoPoly, refUnwalkable, refTerrain, refForeignSeam, refEdge, refBanned,
                 refBlocked, NavMesh::g_tightCrossings, NavMesh::g_volumeCrossings,
                 best.penTerrain, best.penOther, fl);
        Log::Write("NAV-ROUTE", m);
    }
    // Every request that met a script-closed floor says so, whether it routed or not (S182). The
    // falsifier for the cut: a map where this fires and the player walks that crossing by hand.
    if (refClosed > 0 || refClosedPriced > 0 || closedExemptMats != 0) {
        char mats[96]; int q = 0;
        for (int i = 0; i < 32 && q < 80; ++i)
            if ((closedMatsCut >> i) & 1u) q += snprintf(mats + q, sizeof(mats) - q, "%s%d", q ? "," : "", i);
        if (q == 0) snprintf(mats, sizeof(mats), "none");
        char where[96] = "";
        FVec3 sc{};
        if (closedSample != kNoPoly && NavMesh::PolyCentroid(closedSample, sc))
            snprintf(where, sizeof(where), "; first at poly %d (%.1f,%.1f,%.1f)", closedSample, sc.x, sc.y, sc.z);
        char priced[96]; int pq = 0;
        for (int i = 0; i < 32 && pq < 80; ++i)
            if ((closedMatsPriced >> i) & 1u) pq += snprintf(priced + pq, sizeof(priced) - pq, "%s%d", pq ? "," : "", i);
        if (pq == 0) snprintf(priced, sizeof(priced), "none");
        char m[448];
        snprintf(m, sizeof(m),
                 "closed-floor: %d crossing(s) CUT -- script-closed floor, material id(s) %s%s | exempt "
                 "material mask 0x%X (the start's / goal's own closed floor) | %d crossing(s) PRICED "
                 "not cut, material id(s) %s (no script on this map opens those -- inferred, not "
                 "declared; S185)",
                 refClosed, mats, where, closedExemptMats, refClosedPriced, priced);
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

    // ---- THE GOAL IS A SURFACE (Session 101) -------------------------------------------------------
    //
    // History, because two different things happened here and only one of them was wrong.
    //
    // Session 98's failure-path re-run ("aim at the seam member nearest the banked proven prefix's
    // end") was CIRCULAR on the map it was built for: the prefix already ended on a seam poly, so the
    // aim point WAS the reference (`0.0m from ref` on all 18 re-runs in the S99 log) and the re-run
    // validated the prefix it was derived from -- 253 confident steps to a dead end, spoken as a
    // route. That re-created the exact S73/S74 failure Plan::Frontier exists to prevent, and it was
    // REVERTED in Session 100. `pass=seam` must never appear in a log again.
    //
    // RULE (S99), still binding: a route may never be validated against a point derived from that
    // same route's own progress.
    //
    // What survived is the DIAGNOSIS: a transition fires on ANY part of its surface (S64), the exit's
    // `pos` is one boundary VERTEX chosen by straight-line distance, and on a 27 m seam that is the
    // corner walking reaches LAST -- so the final leg runs along the surface and a replan from the
    // bank comes back "No path" 16.4 m short.
    //
    // This is that fix, and it satisfies the S99 rule BY CONSTRUCTION: the endpoint is the PORTAL the
    // corridor crosses to step onto the surface -- mesh geometry, not a distance to anything this
    // route produced -- and the proof is the same body walk every other route gets, at the same
    // arrival tolerance, with the same corner handling. Reached only when nothing above was willing
    // to be spoken as a Route, so a working route cannot change. See path_surface_goal.h.
    if (surfaceTouch != kNoPoly) {
        const int budget = probesLeft > kFrontierMinProbes ? probesLeft : kFrontierMinProbes;
        PathSurfaceGoal::Result sr;
        const bool got = PathSurfaceGoal::Route(came, surfaceTouch, from, budget, kArrivalTol, sr);
        probesLeft -= sr.probes;
        stats.rays += sr.probes;             // spent whether or not it was accepted
        if (got) {
            rawPoly        = sr.poly;
            outPoly        = rawPoly;
            stats.endPoly  = sr.endPoly;
            stats.nearDist = 0.0f;
            stats.shortfall = 0.0f;
            stats.pass     = "surface-goal";
            char m[256];
            snprintf(m, sizeof(m),
                     "mesh: start=%d goal=%d end=%d corners=%zu expands=%d touched=%d probes=%d "
                     "attempts=%d banned=%d pass=surface-goal (ended ON the transition surface)",
                     stats.startPoly, stats.goalPoly, stats.endPoly, rawPoly.size(), stats.expands,
                     stats.touched, stats.rays, stats.attempts, stats.bannedEdges);
            Log::Write("NAV-ROUTE", m);
            return rawPoly.size() >= 2 ? Plan::Route : Plan::NoPath;
        }
    }

    // ---- WHY DID THIS FAIL? (Session 115 -- LOG-ONLY, and only once everything else has given up) ---
    //
    // Every "No path" in this project's history has been argued about from the same two words, and the
    // arguments went in circles because the log could not tell the two cases apart:
    //   * the goal is GENUINELY UNREACHABLE -- there is no chain of adjacent walkable polys to it, and
    //     refusing to route is the honest answer;
    //   * THE SEARCH GAVE UP -- a chain exists and something in the costing, the string-pull or the
    //     validation could not turn it into a walkable polyline.
    // These are opposite defects and they produce the identical line. The oracle answers it in one
    // measurement, on every map, for the whole "stuck in tight quarters" class the tester reports in
    // the Waterways as well as the palace.
    //
    // It reuses `NavMesh::FloodFrom` -- pure adjacency plus the party's own walkability test, no rays,
    // no volume tests, no costs, bounded by the mesh's own poly cap -- so it can neither be fooled by
    // this function's pricing nor cost a frame on any request that succeeded. Nothing reads its
    // result: it is printed and forgotten.
    if (!goalOffMesh && goal != kNoPoly && start != kNoPoly) {
        std::vector<PolyId> comp;
        const int n = NavMesh::FloodFrom(start, comp);
        bool inComponent = false;
        for (const PolyId p : comp) if (p == goal) { inComponent = true; break; }
        stats.goalConnected = inComponent ? 1 : 0;   // read by the planner's keep-the-live-route test
        char om[288];
        snprintf(om, sizeof(om),
                 "oracle: goal poly %d is %s the start poly %d's adjacency component (%d polys) -- %s",
                 goal, inComponent ? "IN" : "NOT IN", start, n,
                 inComponent
                     ? "the mesh connects these two, so this is the SEARCH giving up, not an "
                       "unreachable goal"
                     : "there is no walkable chain between them at all; \"No path\" is the honest "
                       "answer and no amount of re-costing can change it");
        Log::Write("NAV-ROUTE", om);
    }

    // THE CORRIDOR ITSELF, on a failure only. `corners(xyz)` above prints the string-pulled polyline,
    // which is what the body was asked to walk -- but not what A* actually found. The distinction is
    // the whole question when `repair[full-corridor]` fails: that rung re-inserts every portal
    // midpoint, so if the corridor's own openings do not walk either, the chord was never the problem
    // and no amount of un-pulling will help. Map 568 measured exactly that. Bounded to a dozen
    // entries; this fires only where the request already ends without a route.
    if (!best.chain.empty()) {
        char cm[256]; int q = 0;
        for (size_t i = 0; i < best.chain.size() && i < 12 && q < static_cast<int>(sizeof(cm)) - 12; ++i)
            q += snprintf(cm + q, sizeof(cm) - static_cast<size_t>(q), "%s%d", i ? "->" : "",
                          best.chain[i]);
        char m[320];
        snprintf(m, sizeof(m), "corridor: %zu polys %s%s", best.chain.size(), cm,
                 best.chain.size() > 12 ? " ..." : "");
        Log::Write("NAV-ROUTE", m);

        char pm[288]; q = 0;
        for (size_t i = 0; i < best.portals.size() && i < 8 && q < static_cast<int>(sizeof(pm)) - 32; ++i) {
            const Portal& pp = best.portals[i].p;
            q += snprintf(pm + q, sizeof(pm) - static_cast<size_t>(q), "%s%d:%d(%.1f,%.1f)",
                          i ? " " : "", best.portals[i].poly, best.portals[i].edge,
                          (pp.left.x + pp.right.x) * 0.5f, (pp.left.z + pp.right.z) * 0.5f);
        }
        char m2[352];
        snprintf(m2, sizeof(m2), "corridor openings (poly:edge at their midpoints): %s%s", pm,
                 best.portals.size() > 8 ? " ..." : "");
        Log::Write("NAV-ROUTE", m2);
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

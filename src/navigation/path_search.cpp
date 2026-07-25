#include "navigation/path_search.h"
#include "navigation/map_query.h"
#include "navigation/nav_grid.h"
#include "core/logger.h"

#include <vector>
#include <unordered_map>
#include <queue>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>

// The A* itself, split out of path_planner.cpp when that file passed the project's 500-line ceiling.
// The planner owns the REQUEST (queue it on the input thread, drain it on a safe game frame, speak the
// result); this file owns the SEARCH (grid, snapping, the two-stage recovery, the polyline). They were
// only one file because the search started as thirty lines.
//
// GAME THREAD ONLY: every walkability sample is a raycast against live map geometry.
namespace PathSearch {

namespace {

// ---- planner tunables (bound the game-thread cost per route) -----------------
// A* over a FINE (NavGrid::kFineCell ~1.5 m) walkability grid, lazily sampled with GroundAt
// against the actual floor mesh (NOT the walkmap's coarse 8 m native cells, which false-fail
// short routes). No distance cap; GroundAt returns false off the floor, so the search bounds
// itself at map edges. Budgets are safety ceilings for the pathological case.
constexpr int   kMaxExpand    = 20000;   // A* node-expansion cap
constexpr int   kMaxRays      = 60000;   // wall-ray budget (A* edges + string-pull validation)
constexpr float kMaxStep      = 1.5f;    // coarse cliff gate: floor-height delta between cell CENTRES
constexpr float kBodyPad      = 0.9f;    // ray height above floor for wall clearance
// STEP-DISCONTINUITY edge test (Session 68, RE-grounded). Decompile of the FIELD walkmap movement
// (FUN_0022cc50, FUN_00231900, FUN_0033bc80) shows the engine imposes NO walkable-slope limit -- the
// player can walk any CONTINUOUS slope (that is why stairs and hills work). The only geometric
// movement blockers are walls (SegmentClear, mask=4, already tested) and a STEP-HEIGHT DISCONTINUITY:
// FUN_0033bc80 reacts when the ground height jumps >= 0.3 world-units under one movement step. Our
// coarse kMaxStep (1.5 m between cell centres 1.5 m apart) is ~5x too loose, so A* stitched routes
// across a ~0.3-0.6 m ledge/lip the player's movement rejects (character jams; confirmed on Dalmasca
// Estersand). The prior 0.6 m / 0.5 m dense check was still looser than the real limit, so the route
// never changed. Fix: sub-sample the floor FINELY along any edge with a real height change and reject
// a sub-step that JUMPS more than kStepDiscont -- a ledge fails, a continuous slope / ramped staircase
// (whose per-0.25 m rise stays under the cap) passes. NOT a slope cap: a slope gate would wrongly
// reject slopes the player can walk. kMaxStep stays high on purpose so continuous slopes survive the
// coarse gate. Flat/near-level edges skip the whole check (fast path) -- city routing is unaffected.
// kStepDiscont is provisional; the shippable value is bracketed from the NAV-ROUTE route-profile dump
// (the descent's real ledge height vs the terrain the tester actually walks).
constexpr float kEdgeSubStep  = 0.25f;   // fine floor sub-sample spacing along an edge (m)
constexpr float kStepDiscont  = 0.35f;   // max floor JUMP per sub-step; a ledge/lip exceeds it, a
                                         // continuous slope/ramp does not (~0.3 m engine step limit)
constexpr float kStepTrigger  = 0.15f;   // only sub-sample edges with at least this much height change


// Lateral clearance (capsule radius) demanded of every edge. TWO passes, because 0.5 m turned out
// to ISLAND doorway cells: an edge only counts clear when the centre ray AND two rays offset
// +/-margin perpendicular are all clear, and at a narrow archway both offset rays strike the jambs.
// Measured on East End 2026-07-23: routes to Muthru Bazaar (cell 71,37) and Southern Plaza (21,37)
// both reported NoPath with `touched=2157` -- IDENTICAL from four different start positions and for
// both targets, i.e. the search exhausted one connected region and stopped, nowhere near its budget
// (2445-2585 expands of 20000). Neither goal produced a `snap:` line, so both goal cells DO have
// floor. Walkable, but with no passable edge into them. The relaxed pass exists to reach exactly
// those cells; it is only ever run when the strict pass has already failed, so normal routes keep
// their wall clearance.
constexpr float kMarginStrict  = 0.5f;
constexpr float kMarginRelaxed = 0.0f;

// Nearest-walkable ring search radius, shared by the goal and start snaps (~9 m at kFineCell).
constexpr int   kSnapMax       = 6;
// ---- Approach-cell goal set (Session 73) --------------------------------------------------------
// How far from the target we look for somewhere to STAND. The engine's interaction distance is a
// short horizontal reach, so this only has to cover "the cell beside it" -- 3 cells is 4.5 m at
// kFineCell, comfortably more than any interaction range, and small enough that the scan is 49
// WalkableAt calls (all cache hits after the first visit).
constexpr int    kApproachCells  = 3;
constexpr float  kApproachRadius = 4.0f;   // metres from the target, hard cap on an approach cell
// Cap on goals kept. heur() takes a min over this set on every expansion, so it belongs in the
// inner loop's budget; the nearest handful are the only ones worth standing in anyway.
constexpr size_t kMaxGoals       = 12;
// How hard to prefer an approach cell that is CLOSE to the target (Session 73, second round).
//
// The first build stopped at whichever goal A* popped first, because a plain min-over-goals
// heuristic makes every goal look equally good. Measured: `primary (356,141) d=0.9m` existed, and
// the search finished on (355,142) at `nearDist=2.1m` -- inside the band, but too far to interact,
// so the tester arrived and still had to wiggle on crow-flies directions.
//
// Treating the remaining gap as a TERMINAL COST fixes the preference: reaching goal g costs
// path(g) + weight*g.d, so a nearer-to-target cell wins unless it is much further to walk. The
// heuristic stays admissible (euclid <= true path cost), so the search is still correct, and a
// LARGER h is more informed -- expansions go down, not up.
//
// CRITICALLY, this changes only the PREFERENCE among goals, never the goal SET. Anything routable
// before this weight existed is still routable; the search can still fall back to a far approach
// cell when no near one is reachable. That is what keeps Montblanc reachable.
//
// PROVISIONAL: the honest fix is to cap kApproachRadius at the engine's real horizontal interaction
// reach (FUN_003da5a0's radii sum) instead of the made-up 4 m, which would make every goal
// interactable by construction and render this weight unnecessary. Bracketed by measurement so far:
// dist2D 0.51 PASSED the distance gate, 1.70 did not get chosen -- so the true reach is in between.
constexpr float  kGoalGapWeight  = 4.0f;

// A cell the player could stand in and still interact with the target. `d` is its distance to the
// target, which is a TERMINAL COST, not just a sort key -- see the heuristic.
struct GoalCell { int c, r; float wx, wz; float d2; float d; };
// When both passes fail, route to the closest cell the search actually reached if it is within this
// many cells of the goal. A route that stops a few metres short beats "No path" -- the player can
// cover the last stretch, but they cannot cover the whole map on a shrug.
constexpr int   kGoalNearCells = 4;      // 6 m at kFineCell
// Cell ceiling on the goal-side bridge flood. 256 cells is ~2048 rays (~2 ms) — enough to tunnel
// through any doorway or map seam, far too small to re-search the map by accident.
constexpr int   kBridgeMax     = 256;

inline int64_t Key(int x, int z) {
    return static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32 |
                                static_cast<uint32_t>(z));
}
inline int KeyX(int64_t k) { return static_cast<int>(static_cast<uint32_t>(k >> 32)); }
inline int KeyZ(int64_t k) { return static_cast<int>(static_cast<uint32_t>(k & 0xffffffff)); }

// Ring-search outward for the nearest walkable cell, preferring the one whose centre is closest to
// the true world point. Rewrites c/r and returns true on success; on failure the caller keeps the
// original cell so A* still reports its own honest answer.
//
// This was inline in the goal snap and applied to the GOAL ONLY. The start needs it just as badly:
// the fine grid is sampled with GroundAt and has holes, and a player standing on one gets
// `startFloor=0 expands=1 touched=1` -- the search dies at its first expansion because none of the
// start cell's neighbours link back to a cell with no floor. Measured twice on East End
// (2026-07-23 06:36) from (121.50,0,27.20) and (121.50,0,30.47), on open street.
bool SnapToWalkable(int& c, int& r, float wx, float wz, float& outDist) {
    int bc = c, br = r;
    float bestD2 = -1.0f;
    for (int rad = 1; rad <= kSnapMax && bestD2 < 0.0f; ++rad) {
        for (int dz = -rad; dz <= rad; ++dz) {
            for (int dx = -rad; dx <= rad; ++dx) {
                const int adx = dx < 0 ? -dx : dx, adz = dz < 0 ? -dz : dz;
                if ((adx > adz ? adx : adz) != rad) continue;   // ring perimeter only
                const int cc = c + dx, cr = r + dz;
                float cy;
                if (!NavGrid::WalkableAt(cc, cr, cy)) continue;
                float px, pz;
                NavGrid::CellCenter(cc, cr, px, pz);
                const float ddx = px - wx, ddz = pz - wz;
                const float d2 = ddx * ddx + ddz * ddz;
                if (bestD2 < 0.0f || d2 < bestD2) { bestD2 = d2; bc = cc; br = cr; }
            }
        }
    }
    if (bestD2 < 0.0f) return false;
    c = bc; r = br;
    outDist = std::sqrt(bestD2);
    return true;
}

} // namespace

// A* over a FINE walkability grid (NavGrid), lazily sampled with GroundAt against the actual floor mesh
// and cached per map-epoch. `rawPoly` = the raw cell staircase (what the spoken legs are measured from);
// `outPoly` = the smoothed, wall-validated route.
Plan Run(const FVec3& from, const FVec3& to, uint32_t epoch, float bandLo, float bandHi,
         std::vector<FVec3>& rawPoly, std::vector<FVec3>& outPoly, Stats& stats) {
    rawPoly.clear();
    outPoly.clear();
    if (!MapQuery::HasWorld()) return Plan::NoPath;   // walkmap not loaded
    NavGrid::EnsureEpoch(epoch);                       // fresh cache on a new map

    int tc, tr; NavGrid::WorldToCell(to.x, to.z, tc, tr);
    int sc, sr; NavGrid::WorldToCell(from.x, from.z, sc, sr);
    // The target's OWN cell, kept so the end of the route can tell "we finished on the target" from
    // "we finished on an approach cell beside it" -- only the former may append the target's true
    // world position to the polyline. Appending it after routing to an approach cell would put the
    // last leg 0.93 m up onto Montblanc's dais, which is the exact thing this change exists to stop.
    const int origTc = tc, origTr = tr;

    // Target-cell walkability snap: a target standing slightly off-mesh (a ledge, a map edge, an
    // enemy-only tile, or a fine-grid sampling gap) lands on a non-walkable cell, and A* — which only
    // reaches walkable neighbours — would then report a spurious NoPath. Snap the GOAL to its nearest
    // walkable cell. The final poly point becomes that walkable cell, not the off-mesh target, so the
    // last leg lands on ground rather than pointing into a wall.
    bool snapped = false;

    // ---- GOAL SET: every cell you could stand in and still interact ------------------------------
    // NOT "the target's own cell". A target frequently stands where the player cannot: Montblanc on a
    // dais whose floor is 6.93 while the walkway beside it is 6.00. Asking A* to stand ON him is
    // unsatisfiable, and that is exactly what the old code did -- it snapped the goal ONLY when the
    // target's cell had no floor at all, so a walkable dais sailed through and the search was handed
    // an unreachable goal. From the ground floor the fine cell then collapsed target and player
    // together (`expands=0`), which is where "Montblanc. 1 steps" came from.
    //
    // The goal test here is the ENGINE'S OWN predicate, not a heuristic. FUN_0025bad0 accepts an
    // interaction when the player's Y is inside a per-target vertical band and the HORIZONTAL
    // distance is in range -- Y is excluded from distance, so interaction range is a cylinder. A goal
    // cell is therefore any walkable cell near the target whose floor lies inside that band. For
    // Montblanc that admits BOTH the 6.93 dais and the 6.00 walkway; A* never reaches the dais, so it
    // routes to the walkway with no special-casing, and the same rule covers every chest on a ledge
    // and NPC behind a counter without a single map-specific line (feedback_global_not_per_map).
    std::vector<GoalCell> goals;
    if (bandHi >= bandLo) {
        for (int dz = -kApproachCells; dz <= kApproachCells; ++dz) {
            for (int dx = -kApproachCells; dx <= kApproachCells; ++dx) {
                const int cc = tc + dx, cr = tr + dz;
                float cy;
                if (!NavGrid::WalkableAt(cc, cr, cy)) continue;
                if (cy < bandLo || cy > bandHi) continue;       // could not interact from here
                float wx, wz; NavGrid::CellCenter(cc, cr, wx, wz);
                const float ddx = wx - to.x, ddz = wz - to.z;
                const float d2 = ddx * ddx + ddz * ddz;
                if (d2 > kApproachRadius * kApproachRadius) continue;
                goals.push_back(GoalCell{ cc, cr, wx, wz, d2, std::sqrt(d2) });
            }
        }
        std::sort(goals.begin(), goals.end(),
                  [](const GoalCell& a, const GoalCell& b) { return a.d2 < b.d2; });
        // Bound the set: heur() is a min over goals and runs per expansion, so an unbounded set would
        // put the cost in the search's inner loop. The nearest few are the ones worth standing in.
        if (goals.size() > kMaxGoals) goals.resize(kMaxGoals);
    }

    if (!goals.empty()) {
        // Primary = nearest admissible cell. Everything downstream (bridge flood, near-goal
        // fallback, reconstruction) keys off tc/tr, so pointing them at the primary keeps those
        // paths unchanged; the search below may finish on any goal and rewrites tc/tr to that one.
        const bool moved = (goals[0].c != tc || goals[0].r != tr);
        char gm[192];
        snprintf(gm, sizeof(gm),
                 "goal-set: %zu cell(s) in band [%.2f,%.2f], primary (%d,%d)->(%d,%d) d=%.1fm",
                 goals.size(), bandLo, bandHi, tc, tr, goals[0].c, goals[0].r,
                 std::sqrt(goals[0].d2));
        Log::Write("NAV-ROUTE", gm);
        if (moved) snapped = true;   // the poly ends on the approach cell, not on the target itself
        tc = goals[0].c; tr = goals[0].r;
    } else {
        // No band, or nothing admissible near the target: fall back to the original behaviour so a
        // target with no readable band routes exactly as it did before this change.
        float ty;
        if (!NavGrid::WalkableAt(tc, tr, ty)) {
            const int ot = tc, or_ = tr;
            float d = 0.0f;
            if (SnapToWalkable(tc, tr, to.x, to.z, d)) {
                char sm[128];
                snprintf(sm, sizeof(sm), "snap: tgt cell (%d,%d)->(%d,%d) d=%.1fm", ot, or_, tc, tr, d);
                Log::Write("NAV-ROUTE", sm);
                snapped = true;
            }
            // No walkable cell within radius -> leave the goal; A* reports NoPath honestly.
        }
    }
    stats.tx = tc; stats.tz = tr;

    // START-cell snap. Same failure, opposite end: a player standing on a fine-grid hole yields
    // `startFloor=0 expands=1` and the search never begins. Snapping the start costs one ring search
    // and is invisible in the spoken route (the first leg is measured from the player's real
    // position, which `rawPoly` still opens with).
    {
        float sy0;
        if (!NavGrid::WalkableAt(sc, sr, sy0)) {
            const int os = sc, osr = sr;
            float d = 0.0f;
            if (SnapToWalkable(sc, sr, from.x, from.z, d)) {
                char sm[128];
                snprintf(sm, sizeof(sm), "snap-start: cell (%d,%d)->(%d,%d) d=%.1fm", os, osr, sc, sr, d);
                Log::Write("NAV-ROUTE", sm);
            }
        }
    }

    int rays = 0;
    float margin = kMarginStrict;   // set per pass; read by clearWithMargin/passable below

    auto worldOf = [&](int c, int r, float y) -> FVec3 {
        float wx, wz; NavGrid::CellCenter(c, r, wx, wz);
        return FVec3{ wx, y, wz };
    };
    float twx, twz; NavGrid::CellCenter(tc, tr, twx, twz);
    // Multi-goal heuristic: distance to the NEAREST goal. Taking the min keeps it admissible (it can
    // never exceed the true cost to the goal actually used), which is what lets the search finish on
    // whichever approach cell is reachable rather than the one that happened to be closest to the
    // target. With no goal set it degenerates to the single-target distance, i.e. the old behaviour.
    auto heur = [&](int c, int r) -> float {
        float wx, wz; NavGrid::CellCenter(c, r, wx, wz);
        if (goals.empty()) {
            const float dx = wx - twx, dz = wz - twz;
            return std::sqrt(dx * dx + dz * dz);        // admissible Euclidean (meters)
        }
        // Cost to reach goal g and stop there = walk to g, plus the gap g still leaves to the target,
        // weighted. Min over goals keeps it admissible; the weight is what stops the search settling
        // for a far-but-early goal when a closer one is only slightly more walking.
        float best = -1.0f;
        for (const GoalCell& g : goals) {
            const float dx = wx - g.wx, dz = wz - g.wz;
            const float cost = std::sqrt(dx * dx + dz * dz) + kGoalGapWeight * g.d;
            if (best < 0.0f || cost < best) best = cost;
        }
        return best;
    };
    auto isGoal = [&](int c, int r) -> bool {
        if (goals.empty()) return c == tc && r == tr;
        for (const GoalCell& g : goals) if (g.c == c && g.r == r) return true;
        return false;
    };
    // A clear body-height corridor WITH lateral margin: center ray + two rays offset +/-margin
    // perpendicular, so a leg only counts clear with body width on both sides (keeps the route
    // off wall faces / from clipping corners).
    auto clearWithMargin = [&](const FVec3& a, const FVec3& b) -> bool {
        if (!MapQuery::SegmentClear(a, b)) return false;
        const float dx = b.x - a.x, dz = b.z - a.z;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 1e-4f || margin <= 0.0f) return true;
        const float px = -dz / len * margin, pz = dx / len * margin;   // perpendicular * margin
        return MapQuery::SegmentClear(FVec3{ a.x + px, a.y, a.z + pz }, FVec3{ b.x + px, b.y, b.z + pz })
            && MapQuery::SegmentClear(FVec3{ a.x - px, a.y, a.z - pz }, FVec3{ b.x - px, b.y, b.z - pz });
    };
    auto passable = [&](int ac, int ar, float ay, int bc, int br, float by) -> bool {
        if (std::fabs(ay - by) > kMaxStep) return false;                        // coarse cliff gate
        // Step-discontinuity floor test (see kStepDiscont note above). Only edges with a real height
        // change are sub-sampled -- flat/near-level edges are the common case and skip this (fast path,
        // city routing untouched). Walk the floor finely along the edge and reject a sub-step that
        // JUMPS more than kStepDiscont: a ledge/lip the player cannot step over fails, a continuous
        // slope or ramped staircase passes. This is what stops A* routing you across a step the player's
        // own movement rejects (the char jams against it and does not move).
        if (std::fabs(ay - by) > kStepTrigger) {
            float ax, az, bx, bz;
            NavGrid::CellCenter(ac, ar, ax, az);
            NavGrid::CellCenter(bc, br, bx, bz);
            const float ex = bx - ax, ez = bz - az;
            int subN = static_cast<int>(std::ceil(std::sqrt(ex * ex + ez * ez) / kEdgeSubStep));
            if (subN < 1) subN = 1;
            float prevSubY = ay;
            for (int s = 1; s < subN; ++s) {                 // interior sub-samples; endpoints are ay/by
                if (rays >= kMaxRays) return false;
                ++rays;
                const float t = static_cast<float>(s) / static_cast<float>(subN);
                float subY = 0.0f;
                if (!MapQuery::GroundAt(ax + ex * t, az + ez * t, subY)) return false;  // gap in the floor
                if (std::fabs(subY - prevSubY) > kStepDiscont) return false;            // ledge / lip
                prevSubY = subY;
            }
            if (std::fabs(by - prevSubY) > kStepDiscont) return false;                  // last hop onto B
        }
        if (rays >= kMaxRays) return false;
        // Charge what the pass actually spends: three rays with a margin, one without. The relaxed
        // retry therefore costs about a third of the strict pass for the same exploration, which is
        // what keeps a two-pass failure off the game thread's critical path.
        rays += (margin > 0.0f) ? 3 : 1;
        return clearWithMargin(worldOf(ac, ar, ay + kBodyPad), worldOf(bc, br, by + kBodyPad));
    };

    float sy = from.y;
    const bool startWalk = NavGrid::WalkableAt(sc, sr, sy);   // fills sy on hit
    stats.startFloorHit = startWalk;
    if (!startWalk) sy = from.y;                              // stand on the player's own Y

    struct Node { int c, r; float f; };
    struct Cmp { bool operator()(const Node& a, const Node& b) const { return a.f > b.f; } };
    std::priority_queue<Node, std::vector<Node>, Cmp> open;
    std::unordered_map<int64_t, float>   gScore;
    std::unordered_map<int64_t, int64_t> came;

    const int64_t startKey = Key(sc, sr);
    const int64_t goalKey  = Key(tc, tr);
    gScore[startKey] = 0.0f;
    open.push({ sc, sr, heur(sc, sr) });

    const int dirs[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} };
    int expands = 0;
    bool reached = false;
    // Closest cell the search ever REACHED, in cells. Drives both the near-goal fallback and the
    // NoPath diagnostic — "nearest reached was 1.4 cells away" means an islanded doorway, "38 cells"
    // means genuinely walled off, and those want completely different fixes.
    int64_t bestKey   = startKey;
    float   bestCellD = -1.0f;

    while (!open.empty()) {
        Node cur = open.top(); open.pop();
        {
            const float dc = static_cast<float>(cur.c - tc), dr = static_cast<float>(cur.r - tr);
            const float d  = std::sqrt(dc * dc + dr * dr);
            if (bestCellD < 0.0f || d < bestCellD) { bestCellD = d; bestKey = Key(cur.c, cur.r); }
        }
        // Finish on ANY admissible approach cell, and adopt it as the goal so the reconstruction,
        // the `snapped` end-point handling and the stats all describe the cell actually routed to.
        if (isGoal(cur.c, cur.r)) {
            reached = true;
            if (cur.c != tc || cur.r != tr) {
                tc = cur.c; tr = cur.r;
                stats.tx = tc; stats.tz = tr;
                float awx, awz; NavGrid::CellCenter(tc, tr, awx, awz);
                const float adx = awx - to.x, adz = awz - to.z;
                char am[160];
                snprintf(am, sizeof(am),
                         "goal-set: reached alternate approach cell (%d,%d), gap to target %.1fm",
                         tc, tr, std::sqrt(adx * adx + adz * adz));
                Log::Write("NAV-ROUTE", am);
            }
            break;
        }
        if (++expands > kMaxExpand || rays >= kMaxRays) break;

        const int64_t ck = Key(cur.c, cur.r);
        auto cgit = gScore.find(ck);
        if (cgit == gScore.end()) continue;
        const float cg = cgit->second;
        if (cur.f > cg + heur(cur.c, cur.r) + 0.001f) continue;   // stale queue entry

        float cy = sy;
        if (!NavGrid::WalkableAt(cur.c, cur.r, cy) && !(cur.c == sc && cur.r == sr)) continue;
        for (auto& d : dirs) {
            const int nc = cur.c + d[0], nr = cur.r + d[1];
            float ny;
            if (!NavGrid::WalkableAt(nc, nr, ny)) continue;   // off-map / no floor -> not walkable
            if (!passable(cur.c, cur.r, cy, nc, nr, ny)) continue;
            float wx0, wz0, wx1, wz1;
            NavGrid::CellCenter(cur.c, cur.r, wx0, wz0);
            NavGrid::CellCenter(nc, nr, wx1, wz1);
            const float ddx = wx1 - wx0, ddz = wz1 - wz0;
            const float ng = cg + std::sqrt(ddx * ddx + ddz * ddz);
            const int64_t nk = Key(nc, nr);
            auto git = gScore.find(nk);
            if (git == gScore.end() || ng < git->second) {
                gScore[nk] = ng;
                came[nk] = ck;
                open.push({ nc, nr, ng + heur(nc, nr) });
            }
        }
    }

    const int raysStrict = rays;
    stats.expands = expands;
    stats.cells = static_cast<int>(gScore.size());

    // ---- Recovery pass: bridge an ISLANDED goal ------------------------------------------------
    // The strict pass demands a 1 m-wide clear corridor for every edge, which at a narrow archway
    // fails on both offset rays and leaves the doorway cell walkable-but-unreachable. Rather than
    // re-run the whole search at margin 0 (the strict failure already costs ~60 ms of raycasts on the
    // GAME thread — doubling that is exactly the kind of stall this project treats as a bug), flood
    // OUTWARD FROM THE GOAL at margin 0 until it touches a cell the strict pass already reached, then
    // splice the two chains. Hard-bounded at kBridgeMax cells, so the whole recovery is ~2 ms.
    bool bridged = false;
    if (!reached) {
        rays   = 0;                   // bounded by cell count below, so a fresh allowance is safe
        margin = kMarginRelaxed;
        std::unordered_map<int64_t, int64_t> toGoal;   // cell -> the cell one step TOWARD the goal
        std::vector<std::pair<int, int>> q;
        q.reserve(kBridgeMax);
        q.push_back(std::make_pair(tc, tr));
        toGoal[goalKey] = goalKey;
        int64_t meet = 0;
        for (size_t qi = 0; qi < q.size() && !bridged; ++qi) {
            const int cc = q[qi].first, cr = q[qi].second;
            float cy;
            if (!NavGrid::WalkableAt(cc, cr, cy)) continue;
            for (auto& d : dirs) {
                const int nc = cc + d[0], nr = cr + d[1];
                const int64_t nk = Key(nc, nr);
                if (toGoal.find(nk) != toGoal.end()) continue;
                float ny;
                if (!NavGrid::WalkableAt(nc, nr, ny)) continue;
                if (!passable(nc, nr, ny, cc, cr, cy)) continue;
                toGoal[nk] = Key(cc, cr);
                if (gScore.find(nk) != gScore.end()) { meet = nk; bridged = true; break; }
                if (static_cast<int>(q.size()) < kBridgeMax) q.push_back(std::make_pair(nc, nr));
            }
        }
        if (bridged) {
            // `came` maps a cell to its predecessor toward the START; `toGoal` maps toward the GOAL.
            // Walking meet -> goal and writing came[next] = current stitches the bridge onto the
            // strict path, so the existing reconstruction below needs no special case.
            int64_t k = meet;
            while (k != goalKey) {
                const int64_t nxt = toGoal[k];
                came[nxt] = k;
                k = nxt;
            }
            reached = true;
            stats.pass = "bridge";
            char bm[128];
            snprintf(bm, sizeof(bm), "bridge: goal (%d,%d) linked at %d cells via margin=0",
                     tc, tr, static_cast<int>(toGoal.size()));
            Log::Write("NAV-ROUTE", bm);
        }
    }

    // ---- Fallback: route to the closest cell we actually reached --------------------------------
    // A route that stops a few metres short is worth far more than "No path": the player can cover
    // the last stretch, but they cannot cover the whole map on a shrug. Only applied when the search
    // genuinely got near the goal — beyond that tolerance, "No path" is the honest answer.
    stats.nearDist = bestCellD * NavGrid::kFineCell;
    stats.nearC = KeyX(bestKey);
    stats.nearR = KeyZ(bestKey);
    if (!reached && bestCellD >= 0.0f && bestCellD <= static_cast<float>(kGoalNearCells)) {
        tc = KeyX(bestKey);
        tr = KeyZ(bestKey);
        stats.tx = tc; stats.tz = tr;
        stats.pass = "near-goal";
        snapped = true;            // do not append the true (unreached) target to the polyline
        reached = true;
        char nm[128];
        snprintf(nm, sizeof(nm), "near-goal: routing to (%d,%d), %.1fm short of the target",
                 tc, tr, stats.nearDist);
        Log::Write("NAV-ROUTE", nm);
    }

    stats.rays = raysStrict + rays;
    if (!reached) return Plan::NoPath;

    // Finished somewhere other than the target's own cell => the destination is an approach cell, so
    // the route must END there. The target itself may be a metre above it (a dais) or through a
    // counter, and appending its true position would re-introduce the unreachable last leg.
    if (tc != origTc || tr != origTr) snapped = true;

    // Reconstruct target -> start, then emit from -> ... -> to (world coords).
    std::vector<int64_t> rev;
    int64_t k = Key(tc, tr);
    rev.push_back(k);
    while (k != startKey) {
        auto it = came.find(k);
        if (it == came.end()) break;
        k = it->second;
        rev.push_back(k);
    }
    rawPoly.push_back(from);
    for (auto it = rev.rbegin(); it != rev.rend(); ++it) {
        const int cc = KeyX(*it), cr = KeyZ(*it);
        if (cc == sc && cr == sr) continue;                 // start cell == `from` already
        float cy2 = from.y;
        NavGrid::WalkableAt(cc, cr, cy2);
        rawPoly.push_back(worldOf(cc, cr, cy2));
    }
    // End on the exact target only when it sat on a walkable cell; a snapped (off-mesh) target
    // already terminates at its walkable goal-cell centre (last point pushed by the loop), so
    // appending the original off-mesh point would route the final leg into the wall. The size<2
    // guard covers the rare collapse where the snapped goal is the player's own cell.
    if (!snapped) {
        rawPoly.push_back(to);
    } else if (rawPoly.size() < 2) {
        float gy = from.y;
        NavGrid::WalkableAt(tc, tr, gy);
        rawPoly.push_back(worldOf(tc, tr, gy));
    }

    // String-pull: collapse the cell staircase to line-of-sight waypoints, but keep a corner
    // unless the straight span to the next point is DENSELY traversable (floor-continuous, no
    // cliff, no wall along it). Every committed leg therefore passed SegmentTraversable, so a
    // valid detour is never straightened through a wall (the Bug-3 fix).
    outPoly = rawPoly;
    if (outPoly.size() > 2) {
        std::vector<FVec3> smooth;
        smooth.reserve(outPoly.size());
        smooth.push_back(outPoly.front());
        size_t anchor = 0;
        for (size_t probe = 1; probe + 1 < outPoly.size(); ++probe) {
            // Always the STRICT margin here, even when the route itself was bridged at margin 0:
            // the smoother's job is to refuse to straighten a detour through a tight gap, so a
            // relaxed test would undo the very corner the bridge had to keep.
            // Same fine spacing + step-discontinuity cap as passable()'s A* edge test, so the
            // smoother can never straighten a leg back across a ledge the edge test just rejected.
            if (!MapQuery::SegmentTraversable(outPoly[anchor], outPoly[probe + 1],
                                              kEdgeSubStep, kBodyPad, kStepDiscont, kMarginStrict,
                                              rays, kMaxRays)) {
                smooth.push_back(outPoly[probe]);   // needed corner
                anchor = probe;
            }
        }
        smooth.push_back(outPoly.back());
        outPoly.swap(smooth);
    }
    stats.rays = raysStrict + rays;
    return Plan::Route;
}

} // namespace PathSearch

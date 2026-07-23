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
constexpr float kMaxStep      = 1.5f;    // climbable floor-height delta between cells
constexpr float kBodyPad      = 0.9f;    // ray height above floor for wall clearance
constexpr float kValidateStep = 1.0f;    // dense string-pull validator sample spacing (m)


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
Plan Run(const FVec3& from, const FVec3& to, uint32_t epoch,
         std::vector<FVec3>& rawPoly, std::vector<FVec3>& outPoly, Stats& stats) {
    rawPoly.clear();
    outPoly.clear();
    if (!MapQuery::HasWorld()) return Plan::NoPath;   // walkmap not loaded
    NavGrid::EnsureEpoch(epoch);                       // fresh cache on a new map

    int tc, tr; NavGrid::WorldToCell(to.x, to.z, tc, tr);
    int sc, sr; NavGrid::WorldToCell(from.x, from.z, sc, sr);

    // Target-cell walkability snap: a target standing slightly off-mesh (a ledge, a map edge, an
    // enemy-only tile, or a fine-grid sampling gap) lands on a non-walkable cell, and A* — which only
    // reaches walkable neighbours — would then report a spurious NoPath. Snap the GOAL to its nearest
    // walkable cell. The final poly point becomes that walkable cell, not the off-mesh target, so the
    // last leg lands on ground rather than pointing into a wall.
    bool snapped = false;
    {
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
    auto heur = [&](int c, int r) -> float {
        float wx, wz; NavGrid::CellCenter(c, r, wx, wz);
        const float dx = wx - twx, dz = wz - twz;
        return std::sqrt(dx * dx + dz * dz);            // admissible Euclidean (meters)
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
        if (std::fabs(ay - by) > kMaxStep) return false;
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
        if (cur.c == tc && cur.r == tr) { reached = true; break; }
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
            if (!MapQuery::SegmentTraversable(outPoly[anchor], outPoly[probe + 1],
                                              kValidateStep, kBodyPad, kMaxStep, kMarginStrict,
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

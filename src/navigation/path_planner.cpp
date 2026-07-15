#include "navigation/path_planner.h"
#include "navigation/player_state.h"
#include "navigation/map_query.h"
#include "navigation/nav_grid.h"
#include "navigation/path_directions.h"
#include "navigation/nav_common.h"
#include "speech/speech.h"
#include "core/logger.h"

#include <atomic>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <queue>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace PathPlanner {

namespace {

// ---- request state (input thread writes, game thread reads) ------------------
std::atomic<uint32_t> g_epoch{1};          // map generation; bumped on teardown
std::atomic<bool>     g_hasRequest{false};  // cheap O(1) idle check on the game thread
std::mutex            g_mutex;              // guards the fields below
FVec3                 g_target;
std::wstring          g_label;
uint32_t              g_reqEpoch = 0;       // g_epoch captured at Request()
uint64_t              g_reqSeq   = 0;       // distinguishes successive requests
int                   g_framesLeft = 0;     // retry countdown while not yet safe
uint64_t              g_notSafeLoggedSeq = 0; // game-thread only: dedupe the per-frame not-safe log to once/request

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
constexpr float kMargin       = 0.5f;    // lateral clearance (capsule radius) — keeps routes off walls
constexpr int   kWaitFrames   = 90;      // ~1.5 s: retry if the map isn't fully live yet

enum class Plan { Route, NoPath };

inline int64_t Key(int x, int z) {
    return static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32 |
                                static_cast<uint32_t>(z));
}
inline int KeyX(int64_t k) { return static_cast<int>(static_cast<uint32_t>(k >> 32)); }
inline int KeyZ(int64_t k) { return static_cast<int>(static_cast<uint32_t>(k & 0xffffffff)); }

// Diagnostic counters filled by PlanRoute, logged by OnGameFrame (no effect on the
// search). startFloorHit = whether the overlay marks the player's own cell walkable;
// rays = wall rays spent (A* edges + string-pull validation); expands = A* nodes popped.
struct PlanStats {
    int  rays = 0, expands = 0, cells = 0;
    int  tx = 0, tz = 0;
    bool startFloorHit = false;
};

// A* over a FINE walkability grid (NavGrid), lazily sampled with GroundAt against the actual
// floor mesh + cached per map-epoch. Runs on the game thread. `rawPoly` = the raw cell
// staircase (diagnostics); `outPoly` = the smoothed, wall-validated route.
Plan PlanRoute(const FVec3& from, const FVec3& to, uint32_t epoch,
               std::vector<FVec3>& rawPoly, std::vector<FVec3>& outPoly, PlanStats& stats) {
    rawPoly.clear();
    outPoly.clear();
    if (!MapQuery::HasWorld()) return Plan::NoPath;   // walkmap not loaded
    NavGrid::EnsureEpoch(epoch);                       // fresh cache on a new map

    int tc, tr; NavGrid::WorldToCell(to.x, to.z, tc, tr);
    int sc, sr; NavGrid::WorldToCell(from.x, from.z, sc, sr);

    // Target-cell walkability snap: a target standing slightly off-mesh (a ledge, a map edge, an
    // enemy-only tile, or a fine-grid sampling gap) lands on a non-walkable cell, and A* — which only
    // reaches walkable neighbours — would then report a spurious NoPath. Snap the GOAL to its nearest
    // walkable cell (ring search outward; among a ring's walkable cells pick the one whose centre is
    // closest to the true target). The final poly point becomes that walkable cell, not the off-mesh
    // target, so the last leg lands on ground rather than pointing into a wall.
    bool snapped = false;
    {
        float ty;
        if (!NavGrid::WalkableAt(tc, tr, ty)) {
            const int kSnapMax = 6;   // ~9 m at kFineCell (1.5 m)
            int bcx = tc, bcz = tr; float bestD2 = -1.0f;
            for (int rad = 1; rad <= kSnapMax && bestD2 < 0.0f; ++rad) {
                for (int dz = -rad; dz <= rad; ++dz) {
                    for (int dx = -rad; dx <= rad; ++dx) {
                        const int adx = dx < 0 ? -dx : dx, adz = dz < 0 ? -dz : dz;
                        if ((adx > adz ? adx : adz) != rad) continue;   // ring perimeter only
                        const int cc = tc + dx, cr = tr + dz;
                        float cy;
                        if (!NavGrid::WalkableAt(cc, cr, cy)) continue;
                        float wx, wz; NavGrid::CellCenter(cc, cr, wx, wz);
                        const float ddx = wx - to.x, ddz = wz - to.z;
                        const float d2 = ddx * ddx + ddz * ddz;
                        if (bestD2 < 0.0f || d2 < bestD2) { bestD2 = d2; bcx = cc; bcz = cr; }
                    }
                }
            }
            if (bestD2 >= 0.0f) {
                char sm[128];
                snprintf(sm, sizeof(sm), "snap: tgt cell (%d,%d)->(%d,%d) d=%.1fm",
                         tc, tr, bcx, bcz, std::sqrt(bestD2));
                Log::Write("NAV-ROUTE", sm);
                tc = bcx; tr = bcz; snapped = true;
            }
            // No walkable cell within radius -> leave the goal; A* reports NoPath honestly.
        }
    }
    stats.tx = tc; stats.tz = tr;

    int rays = 0;

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
        if (len < 1e-4f || kMargin <= 0.0f) return true;
        const float px = -dz / len * kMargin, pz = dx / len * kMargin;  // perpendicular * margin
        return MapQuery::SegmentClear(FVec3{ a.x + px, a.y, a.z + pz }, FVec3{ b.x + px, b.y, b.z + pz })
            && MapQuery::SegmentClear(FVec3{ a.x - px, a.y, a.z - pz }, FVec3{ b.x - px, b.y, b.z - pz });
    };
    auto passable = [&](int ac, int ar, float ay, int bc, int br, float by) -> bool {
        if (std::fabs(ay - by) > kMaxStep) return false;
        if (rays >= kMaxRays) return false;
        rays += 3;   // center + two offset rays
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
    gScore[startKey] = 0.0f;
    open.push({ sc, sr, heur(sc, sr) });

    const int dirs[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} };
    int expands = 0;
    bool reached = false;

    while (!open.empty()) {
        Node cur = open.top(); open.pop();
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

    stats.rays = rays;
    stats.expands = expands;
    stats.cells = static_cast<int>(gScore.size());
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
            if (!MapQuery::SegmentTraversable(outPoly[anchor], outPoly[probe + 1],
                                              kValidateStep, kBodyPad, kMaxStep, kMargin, rays, kMaxRays)) {
                smooth.push_back(outPoly[probe]);   // needed corner
                anchor = probe;
            }
        }
        smooth.push_back(outPoly.back());
        outPoly.swap(smooth);
    }
    stats.rays = rays;
    return Plan::Route;
}

// Clear the pending flag only if no newer request arrived while we were planning.
void ClearIfSeq(uint64_t seq) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_reqSeq == seq) g_hasRequest.store(false, std::memory_order_release);
}

} // namespace

bool Init()  { return true; }
void Shutdown() { g_hasRequest.store(false, std::memory_order_release); }

void Request(const FVec3& target, const std::wstring& label) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_target     = target;
    g_label      = label;
    g_reqEpoch   = g_epoch.load(std::memory_order_acquire);
    g_framesLeft = kWaitFrames;
    ++g_reqSeq;
    g_hasRequest.store(true, std::memory_order_release);
    char m[128];
    snprintf(m, sizeof(m), "request: target=(%.2f,%.2f,%.2f) epoch=%u seq=%llu (input thread)",
             target.x, target.y, target.z, g_reqEpoch, (unsigned long long)g_reqSeq);
    Log::Write("NAV-ROUTE", m);
}

void OnMapTeardown() {
    g_epoch.fetch_add(1, std::memory_order_acq_rel);   // any pending request is now stale
    NavGrid::Invalidate();                             // drop the whole-map overlay for the dead map
    std::lock_guard<std::mutex> lk(g_mutex);
    g_hasRequest.store(false, std::memory_order_release);
}

void OnGameFrame() {
    if (!g_hasRequest.load(std::memory_order_acquire)) return;   // O(1) common case

    FVec3 target; std::wstring label; uint32_t reqEpoch; uint64_t seq;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_hasRequest.load(std::memory_order_relaxed)) return;
        target = g_target; label = g_label; reqEpoch = g_reqEpoch; seq = g_reqSeq;
    }

    // Map changed since the request was made -> un-revivably stale; drop silently.
    uint32_t curEpoch = g_epoch.load(std::memory_order_acquire);
    if (reqEpoch != curEpoch) {
        char m[112];
        snprintf(m, sizeof(m), "drain seq=%llu: stale epoch (req=%u cur=%u) -> drop",
                 (unsigned long long)seq, reqEpoch, curEpoch);
        Log::Write("NAV-ROUTE", m);
        ClearIfSeq(seq);
        return;
    }

    // Not fully live yet (fade / partial load) OR the player doesn't resolve -> retry
    // for a bounded window before giving up. Never touch map data while unsafe.
    FVec3 from;
    bool navSafe = PlayerState::IsFieldNavSafe();
    bool posOk   = navSafe && PlayerState::ReadPlayerPos(from);
    if (!navSafe || !posOk) {
        bool giveUp = false;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_reqSeq == seq && --g_framesLeft <= 0) {
                g_hasRequest.store(false, std::memory_order_release);
                giveUp = true;
            }
        }
        // This branch can run up to kWaitFrames times; log the not-safe reason ONCE
        // per request (dedupe on seq) so the file isn't spammed per frame.
        if (seq != g_notSafeLoggedSeq) {
            g_notSafeLoggedSeq = seq;
            // Name WHICH gate condition(s) failed (0 == would be safe; then posOk was the
            // blocker). This is what turns a silent route into a diagnosable one.
            uint8_t fm = PlayerState::NavSafeFailMask();
            char names[96];
            PlayerState::FormatNavSafeMask(fm, names, sizeof(names));
            char m[192];
            snprintf(m, sizeof(m),
                     "drain seq=%llu: not nav-safe (fieldSafe=%d posOk=%d failMask=0x%02X[%s]) -> retry up to %d frames",
                     (unsigned long long)seq, navSafe ? 1 : 0, posOk ? 1 : 0, fm, names, kWaitFrames);
            Log::Write("NAV-ROUTE", m);
        }
        if (giveUp) {
            Log::Write("NAV-ROUTE", "drain: gave up (never nav-safe within window) -> Route unavailable");
            Speech::Output(label.empty() ? L"Route unavailable"
                                         : (label + L". Route unavailable"), true);
        }
        return;
    }

    std::vector<FVec3> rawPoly, poly;
    PlanStats st;
    Plan r = PlanRoute(from, target, curEpoch, rawPoly, poly, st);

    const char* planName = (r == Plan::Route) ? "Route" : "NoPath";
    char m[160];
    snprintf(m, sizeof(m), "drain seq=%llu: from=(%.2f,%.2f,%.2f) plan=%s legs=%zu",
             (unsigned long long)seq, from.x, from.y, from.z, planName, poly.size());
    Log::Write("NAV-ROUTE", m);
    // Search stats — diagnoses a NoPath (startFloor=0 => the player's own fine cell has no
    // floor; expands maxed => budget/maze). gridSamples = fine GroundAt samples this map
    // (cache size), fineCell = routing resolution.
    char ms[208];
    snprintf(ms, sizeof(ms),
             "drain seq=%llu: stats plan=%s tgtCell=(%d,%d) rays=%d startFloor=%d expands=%d touched=%d | fineCell=%.1fm gridSamples=%d",
             (unsigned long long)seq, planName, st.tx, st.tz, st.rays, st.startFloorHit ? 1 : 0,
             st.expands, st.cells, NavGrid::kFineCell, NavGrid::SamplesThisMap());
    Log::Write("NAV-ROUTE", ms);

    // Instrumentation: target + raw/smoothed polyline sizes + the first leg, for diagnosing
    // a route. Directions are WORLD-ABSOLUTE (no facing/camera frame).
    {
        const FVec3 s1 = (poly.size() > 1) ? poly[1] : target;
        char mg[192];
        snprintf(mg, sizeof(mg),
                 "drain seq=%llu: tgt=(%.1f,%.1f) rawPts=%zu smPts=%zu firstLeg=(%.1f,%.1f)",
                 (unsigned long long)seq, target.x, target.z, rawPoly.size(), poly.size(), s1.x, s1.z);
        Log::Write("NAV-ROUTE", mg);
    }

    // Egocentric leg directions: "North" = forward = where UP takes you (movement is camera-relative).
    // Reference is the live camera up-direction, NOT the character facing (which is stale when idle
    // and points at the target in combat). 0 fallback if unavailable.
    float facingRad = 0.0f;
    PlayerState::ReadCameraForward(facingRad);

    std::wstring say = label.empty() ? std::wstring() : (label + L". ");
    if (r == Plan::Route) say += PathDirections::Describe(poly, facingRad);
    else                  say += L"No path";

    // Log the spoken directions (ASCII cardinals/digits) so the exact leg text is diagnosable.
    {
        char t[192]; size_t n = 0;
        for (wchar_t wc : say) { if (n + 1 >= sizeof(t)) break; t[n++] = (wc < 128) ? static_cast<char>(wc) : '?'; }
        t[n] = '\0';
        char mt[224];
        snprintf(mt, sizeof(mt), "drain seq=%llu: say=\"%s\"", (unsigned long long)seq, t);
        Log::Write("NAV-ROUTE", mt);
    }

    Speech::Output(say, true);
    ClearIfSeq(seq);
}

} // namespace PathPlanner

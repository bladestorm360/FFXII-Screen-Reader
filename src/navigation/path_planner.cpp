#include "navigation/path_planner.h"
#include "navigation/player_state.h"
#include "navigation/bullet_query.h"
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
constexpr float kCell       = 1.0f;   // grid cell size, meters
constexpr float kMaxRange   = 40.0f;  // straight-line beyond this -> "Too far"
constexpr int   kMaxExpand  = 500;    // A* node-expansion cap
constexpr int   kMaxRays    = 2000;   // total raycast budget (the real limiter)
constexpr float kMaxStep    = 1.5f;   // climbable floor-height delta between cells
constexpr float kBodyPad    = 0.9f;   // ray height above floor for wall clearance
constexpr float kMargin     = 0.5f;   // horizontal clearance margin (~capsule radius)
constexpr int   kWaitFrames = 90;     // ~1.5 s: retry if the map isn't fully live yet
const float     kSqrt2      = 1.41421356f;

enum class Plan { Route, NoPath, TooFar };

inline int64_t Key(int x, int z) {
    return static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32 |
                                static_cast<uint32_t>(z));
}
inline int KeyX(int64_t k) { return static_cast<int>(static_cast<uint32_t>(k >> 32)); }
inline int KeyZ(int64_t k) { return static_cast<int>(static_cast<uint32_t>(k & 0xffffffff)); }

struct CellInfo { bool walkable = false; float floorY = 0.0f; };

// A* over a walkability grid sampled lazily with Bullet rays. Runs on the game
// thread; every ray is SEH-guarded inside BulletQuery. `outPoly` = player -> ... ->
// target on Route.
Plan PlanRoute(const FVec3& from, const FVec3& to, std::vector<FVec3>& outPoly) {
    if (NavCommon::Distance2D(from, to) > kMaxRange) return Plan::TooFar;

    int rays = 0;
    std::unordered_map<int64_t, CellInfo> cells;
    const int box = static_cast<int>(kMaxRange / kCell) + 2;

    auto worldOf = [&](int cx, int cz, float y) -> FVec3 {
        return FVec3{ from.x + cx * kCell, y, from.z + cz * kCell };
    };
    // Lazily test + cache a cell's walkability (floor exists below its center).
    auto cellInfo = [&](int cx, int cz) -> CellInfo& {
        int64_t k = Key(cx, cz);
        auto it = cells.find(k);
        if (it != cells.end()) return it->second;
        CellInfo ci;
        if (rays < kMaxRays) {
            ++rays;
            float fy = 0.0f;
            if (BulletQuery::FloorBelow(worldOf(cx, cz, from.y), 2.0f, 8.0f, fy)) {
                ci.walkable = true;
                ci.floorY = fy;
            }
        }
        return cells.emplace(k, ci).first->second;
    };
    // Edge passability: both walkable, no big step, and a clear body-height corridor.
    auto passable = [&](int ax, int az, const CellInfo& a,
                        int bx, int bz, const CellInfo& b) -> bool {
        if (!a.walkable || !b.walkable) return false;
        if (std::fabs(a.floorY - b.floorY) > kMaxStep) return false;
        if (rays >= kMaxRays) return false;
        ++rays;
        return BulletQuery::HorizontalClear(worldOf(ax, az, a.floorY + kBodyPad),
                                            worldOf(bx, bz, b.floorY + kBodyPad), kMargin);
    };

    const int tx = static_cast<int>(std::lround((to.x - from.x) / kCell));
    const int tz = static_cast<int>(std::lround((to.z - from.z) / kCell));

    auto heur = [&](int x, int z) -> float {
        float ax = std::fabs(static_cast<float>(tx - x));
        float az = std::fabs(static_cast<float>(tz - z));
        return (ax + az) + (kSqrt2 - 2.0f) * std::min(ax, az);   // octile
    };

    struct Node { int x, z; float f; };
    struct Cmp { bool operator()(const Node& a, const Node& b) const { return a.f > b.f; } };
    std::priority_queue<Node, std::vector<Node>, Cmp> open;
    std::unordered_map<int64_t, float>   gScore;
    std::unordered_map<int64_t, int64_t> came;

    // The player stands on valid ground even if the floor probe grazes an edge.
    CellInfo& start = cellInfo(0, 0);
    if (!start.walkable) { start.walkable = true; start.floorY = from.y; }

    gScore[Key(0, 0)] = 0.0f;
    open.push({ 0, 0, heur(0, 0) });

    const int dirs[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} };
    int expands = 0;
    bool reached = false;

    while (!open.empty()) {
        Node cur = open.top(); open.pop();
        if (cur.x == tx && cur.z == tz) { reached = true; break; }
        if (++expands > kMaxExpand || rays >= kMaxRays) break;

        int64_t ck = Key(cur.x, cur.z);
        auto cgit = gScore.find(ck);
        if (cgit == gScore.end()) continue;
        float cg = cgit->second;
        if (cur.f > cg + heur(cur.x, cur.z) + 0.001f) continue;   // stale queue entry

        CellInfo& ci = cellInfo(cur.x, cur.z);
        for (auto& d : dirs) {
            int nx = cur.x + d[0], nz = cur.z + d[1];
            if (std::abs(nx) > box || std::abs(nz) > box) continue;
            CellInfo& ni = cellInfo(nx, nz);
            if (!passable(cur.x, cur.z, ci, nx, nz, ni)) continue;
            float ng = cg + ((d[0] != 0 && d[1] != 0) ? kSqrt2 : 1.0f);
            int64_t nk = Key(nx, nz);
            auto git = gScore.find(nk);
            if (git == gScore.end() || ng < git->second) {
                gScore[nk] = ng;
                came[nk] = ck;
                open.push({ nx, nz, ng + heur(nx, nz) });
            }
        }
    }

    if (!reached) return Plan::NoPath;

    // Reconstruct target -> start, then emit from -> ... -> to.
    std::vector<int64_t> rev;
    int64_t k = Key(tx, tz);
    rev.push_back(k);
    while (k != Key(0, 0)) {
        auto it = came.find(k);
        if (it == came.end()) break;
        k = it->second;
        rev.push_back(k);
    }
    outPoly.clear();
    outPoly.push_back(from);
    for (auto it = rev.rbegin(); it != rev.rend(); ++it) {
        int cx = KeyX(*it), cz = KeyZ(*it);
        if (cx == 0 && cz == 0) continue;                 // start == `from` already
        CellInfo& ci = cellInfo(cx, cz);
        outPoly.push_back(worldOf(cx, cz, ci.walkable ? ci.floorY : from.y));
    }
    outPoly.push_back(to);
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
            char m[128];
            snprintf(m, sizeof(m),
                     "drain seq=%llu: not nav-safe (fieldSafe=%d posOk=%d) -> retry up to %d frames",
                     (unsigned long long)seq, navSafe ? 1 : 0, posOk ? 1 : 0, kWaitFrames);
            Log::Write("NAV-ROUTE", m);
        }
        if (giveUp) {
            Log::Write("NAV-ROUTE", "drain: gave up (never nav-safe within window) -> Route unavailable");
            Speech::Output(label.empty() ? L"Route unavailable"
                                         : (label + L". Route unavailable"), true);
        }
        return;
    }

    std::vector<FVec3> poly;
    Plan r = PlanRoute(from, target, poly);

    const char* planName = (r == Plan::Route) ? "Route" : (r == Plan::TooFar) ? "TooFar" : "NoPath";
    char m[160];
    snprintf(m, sizeof(m), "drain seq=%llu: from=(%.2f,%.2f,%.2f) plan=%s legs=%zu",
             (unsigned long long)seq, from.x, from.y, from.z, planName, poly.size());
    Log::Write("NAV-ROUTE", m);

    std::wstring say = label.empty() ? std::wstring() : (label + L". ");
    if (r == Plan::Route)       say += PathDirections::Describe(poly);
    else if (r == Plan::TooFar) say += L"Too far to route";
    else                        say += L"No path";
    Speech::Output(say, true);
    ClearIfSeq(seq);
}

} // namespace PathPlanner

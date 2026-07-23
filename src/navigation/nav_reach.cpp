#include "navigation/nav_reach.h"
#include "navigation/nav_grid.h"
#include "navigation/map_query.h"
#include "core/logger.h"
#include "core/stall_probe.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace NavReach {

namespace {

// Work per field frame. Each expansion costs up to 8 neighbour tests, and each test is a GroundAt sample
// (first visit only, then cached) plus one body-height clearance ray -- so 60 expansions is roughly 960
// raycasts, on the order of a millisecond. East End's component is ~2,150 cells, so it closes in about
// 36 frames: under a second of barely-loaded frames, then nothing at all.
//
// Deliberately conservative. This runs inside the game's own per-frame field tick, and a mod-side stall
// there is always the mod's fault in this project -- never the game's. If it ever needs to be faster,
// measure it with the STALL_SCOPE below first.
constexpr int   kCellsPerFrame = 60;
// Hard ceiling, so a pathological map cannot make the fill run forever.
constexpr int   kMaxCells      = 40000;
constexpr float kBodyPad       = 0.9f;   // matches the planner: test at body height, not at the feet
constexpr float kMaxStep       = 1.5f;   // matches the planner: climbable floor-height delta

inline int64_t Key(int c, int r) {
    return static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(c)) << 32 |
                                static_cast<uint32_t>(r));
}

// ---- published answer (written on the game thread, read anywhere) ----------------------------
std::mutex                                        g_pubMutex;
std::shared_ptr<const std::unordered_set<int64_t>> g_published;   // null until the first fill closes
std::atomic<bool> g_ready{false};
std::atomic<int>  g_count{0};

// ---- fill state (GAME THREAD ONLY) -----------------------------------------------------------
std::unordered_set<int64_t>     g_seen;
std::vector<std::pair<int,int>> g_queue;
size_t   g_head  = 0;
uint32_t g_epoch = 0xFFFFFFFFu;
bool     g_running = false;
int      g_seedC = 0, g_seedR = 0;

void Publish() {
    auto snap = std::make_shared<const std::unordered_set<int64_t>>(g_seen);
    {
        std::lock_guard<std::mutex> lk(g_pubMutex);
        g_published = snap;
    }
    g_count.store(static_cast<int>(g_seen.size()), std::memory_order_release);
    g_ready.store(true, std::memory_order_release);
}

void Restart(uint32_t epoch, int sc, int sr) {
    g_seen.clear();
    g_queue.clear();
    g_head    = 0;
    g_epoch   = epoch;
    g_seedC   = sc;
    g_seedR   = sr;
    g_running = true;
    g_ready.store(false, std::memory_order_release);
    g_count.store(0, std::memory_order_release);
    g_seen.insert(Key(sc, sr));
    g_queue.push_back(std::make_pair(sc, sr));
}

// Zero-margin corridor test between two adjacent cells — the planner's `passable` without the lateral
// rays. See the header for why zero and not 0.5.
bool Passable(int ac, int ar, float ay, int bc, int br, float by) {
    if (std::fabs(ay - by) > kMaxStep) return false;
    float ax, az, bx, bz;
    NavGrid::CellCenter(ac, ar, ax, az);
    NavGrid::CellCenter(bc, br, bx, bz);
    return MapQuery::SegmentClear(FVec3{ ax, ay + kBodyPad, az }, FVec3{ bx, by + kBodyPad, bz });
}

} // namespace

void Invalidate() {
    g_seen.clear();
    g_queue.clear();
    g_head    = 0;
    g_epoch   = 0xFFFFFFFFu;
    g_running = false;
    g_ready.store(false, std::memory_order_release);
    g_count.store(0, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_pubMutex);
    g_published.reset();
}

void OnGameFrame(uint32_t epoch, const FVec3& playerPos) {
    if (!MapQuery::HasWorld()) return;
    NavGrid::EnsureEpoch(epoch);

    int pc, pr;
    NavGrid::WorldToCell(playerPos.x, playerPos.z, pc, pr);

    if (epoch != g_epoch) {
        Restart(epoch, pc, pr);
    } else if (!g_running) {
        // Completed fill. If the player is no longer inside it they crossed into another component
        // (a door opened, a lift moved, a scripted placement) -- rebuild from where they now stand.
        if (g_ready.load(std::memory_order_acquire) && !g_seen.count(Key(pc, pr))) {
            char m[144];
            snprintf(m, sizeof(m), "reach: player left the reachable set at cell (%d,%d) -- refilling", pc, pr);
            Log::Write("NAV-ROUTE", m);
            Restart(epoch, pc, pr);
        } else {
            return;                       // done and still valid
        }
    }

    STALL_SCOPE("NavReach::fill");
    const int dirs[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} };
    int budget = kCellsPerFrame;
    while (g_head < g_queue.size() && budget-- > 0) {
        const int cc = g_queue[g_head].first, cr = g_queue[g_head].second;
        ++g_head;
        float cy = 0.0f;
        const bool curWalk = NavGrid::WalkableAt(cc, cr, cy);
        // The seed cell is allowed to be off-mesh (the player can stand on a sampling hole); every other
        // cell must have floor, or it would never have been enqueued.
        if (!curWalk && !(cc == g_seedC && cr == g_seedR)) continue;
        for (auto& d : dirs) {
            const int nc = cc + d[0], nr = cr + d[1];
            const int64_t nk = Key(nc, nr);
            if (g_seen.count(nk)) continue;
            float ny = 0.0f;
            if (!NavGrid::WalkableAt(nc, nr, ny)) continue;
            if (!Passable(cc, cr, cy, nc, nr, ny)) continue;
            g_seen.insert(nk);
            if (static_cast<int>(g_seen.size()) < kMaxCells) g_queue.push_back(std::make_pair(nc, nr));
        }
    }

    if (g_head >= g_queue.size() || static_cast<int>(g_seen.size()) >= kMaxCells) {
        g_running = false;
        Publish();
        char m[144];
        snprintf(m, sizeof(m), "reach: fill complete -- %zu cells reachable from (%d,%d)",
                 g_seen.size(), g_seedC, g_seedR);
        Log::Write("NAV-ROUTE", m);
    }
}

bool Ready() { return g_ready.load(std::memory_order_acquire); }
int  CellCount() { return g_count.load(std::memory_order_acquire); }

bool Reachable(const FVec3& p, float tolerance) {
    std::shared_ptr<const std::unordered_set<int64_t>> snap;
    {
        std::lock_guard<std::mutex> lk(g_pubMutex);
        snap = g_published;
    }
    if (!snap || snap->empty()) return true;   // no trustworthy answer -> never filter

    int c, r;
    NavGrid::WorldToCell(p.x, p.z, c, r);
    const int ring = static_cast<int>(std::ceil(tolerance / NavGrid::kFineCell));
    for (int dz = -ring; dz <= ring; ++dz)
        for (int dx = -ring; dx <= ring; ++dx)
            if (snap->count(Key(c + dx, r + dz))) return true;
    return false;
}

} // namespace NavReach

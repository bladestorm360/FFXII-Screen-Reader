#include "navigation/nav_reach.h"
#include "navigation/nav_mesh.h"
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

// Polys expanded per field frame. A poly is far coarser than the 1.5 m cell this used to walk -- a
// corridor is often two triangles -- and each expansion is now three plain memory reads instead of up
// to eight GroundAt samples plus a clearance ray each. The old fill needed ~2,150 cells on East End
// and ~960 raycasts per frame; this needs neither, so the budget can be much larger and still cost
// less.
//
// Still deliberately conservative: it runs inside the game's own per-frame field tick, and a mod-side
// stall there is always the mod's fault in this project -- never the game being slow.
constexpr int kPolysPerFrame = 256;
constexpr int kMaxPolys      = NavMesh::kMaxPolys;   // torn-read backstop

// ---- published answer (written on the game thread, read anywhere) ----------------------------
std::mutex                                         g_pubMutex;
std::shared_ptr<const std::unordered_set<int32_t>> g_published;   // null until the first fill closes
std::atomic<bool> g_ready{false};
std::atomic<int>  g_count{0};

// ---- fill state (GAME THREAD ONLY) -----------------------------------------------------------
std::unordered_set<int32_t>  g_seen;
std::vector<NavMesh::PolyId> g_stack;
uint32_t         g_epoch     = 0xFFFFFFFFu;
NavMesh::PolyId  g_startPoly = NavMesh::kNoPoly;
bool             g_running   = false;

void Publish() {
    auto snap = std::make_shared<const std::unordered_set<int32_t>>(g_seen);
    {
        std::lock_guard<std::mutex> lk(g_pubMutex);
        g_published = snap;
    }
    g_count.store(static_cast<int>(g_seen.size()), std::memory_order_release);
    g_ready.store(true, std::memory_order_release);
}

} // namespace

void OnGameFrame(uint32_t epoch, const FVec3& playerPos) {
    NavMesh::EnsureEpoch(epoch);
    if (!NavMesh::Ready()) return;

    // The poly the player is standing on -- located WITH their Y, so standing under a balcony seeds
    // the flood on the floor rather than on the balcony overhead.
    const NavMesh::PolyId here = NavMesh::FindPolyAt(playerPos.x, playerPos.y, playerPos.z);
    if (here == NavMesh::kNoPoly) return;

    // Restart on a new map, or when the player is somewhere the closed answer does not cover --
    // that means they reached a different component (a lift, a scripted move, a map seam).
    bool restart = (epoch != g_epoch);
    if (!restart && !g_running && g_ready.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(g_pubMutex);
        if (g_published && !g_published->count(here)) restart = true;
    }

    if (restart) {
        char m[144];
        snprintf(m, sizeof(m), "reach: (re)starting flood from poly %d (epoch %u)", here, epoch);
        Log::Write("NAV-ROUTE", m);
        g_epoch     = epoch;
        g_startPoly = here;
        g_seen.clear();
        g_stack.clear();
        g_seen.insert(here);
        g_stack.push_back(here);
        g_running = true;
        g_ready.store(false, std::memory_order_release);
    }

    if (!g_running) return;

    STALL_SCOPE("NavReach::fill");
    int budget = kPolysPerFrame;
    while (!g_stack.empty() && budget-- > 0 && static_cast<int>(g_seen.size()) < kMaxPolys) {
        const NavMesh::PolyId cur = g_stack.back();
        g_stack.pop_back();
        for (int e = 0; e < 3; ++e) {
            const NavMesh::PolyId n = NavMesh::Neighbor(cur, e);
            if (n == NavMesh::kNoPoly || g_seen.count(n)) continue;
            // Deliberately NO volume check here. This decides whether to HIDE an exit from a player
            // who cannot see what was hidden, so it must err toward reachable: a closed door is a
            // reason to route and fail loudly, never a reason to silently drop the destination.
            if (!NavMesh::Walkable(n)) continue;
            g_seen.insert(n);
            g_stack.push_back(n);
        }
    }

    if (g_stack.empty() || static_cast<int>(g_seen.size()) >= kMaxPolys) {
        g_running = false;
        Publish();
        char m[144];
        snprintf(m, sizeof(m), "reach: fill complete -- %zu polys reachable from poly %d",
                 g_seen.size(), g_startPoly);
        Log::Write("NAV-ROUTE", m);
    }
}

void Invalidate() {
    g_running = false;
    g_epoch   = 0xFFFFFFFFu;
    g_seen.clear();
    g_stack.clear();
    g_ready.store(false, std::memory_order_release);
    g_count.store(0, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_pubMutex);
    g_published.reset();
}

bool Ready()     { return g_ready.load(std::memory_order_acquire); }
int  CellCount() { return g_count.load(std::memory_order_acquire); }

bool Reachable(const FVec3& p, float tolerance) {
    std::shared_ptr<const std::unordered_set<int32_t>> snap;
    {
        std::lock_guard<std::mutex> lk(g_pubMutex);
        snap = g_published;
    }
    if (!snap || snap->empty()) return true;   // no trustworthy answer -> never filter

    // A seam sits ON a floor polygon, so the ring search the grid needed is gone: either that
    // polygon is in the component or it genuinely is not. `tolerance` survives only as vertical
    // slack for a point recorded slightly off its own surface.
    NavMesh::PolyId p0 = NavMesh::FindPolyAt(p.x, p.y, p.z);
    if (p0 == NavMesh::kNoPoly && tolerance > 0.0f)
        p0 = NavMesh::FindPolyAt(p.x, p.y + tolerance, p.z);
    if (p0 == NavMesh::kNoPoly) return true;   // cannot place it -> do not filter it out

    return snap->count(p0) != 0;
}

} // namespace NavReach

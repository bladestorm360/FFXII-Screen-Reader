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
std::shared_ptr<const std::unordered_set<int32_t>> g_publishedStrict;   // DIAGNOSTIC ONLY -- see below
std::atomic<bool> g_ready{false};
std::atomic<int>  g_count{0};
std::atomic<int>  g_countStrict{0};

// ---- fill state (GAME THREAD ONLY) -----------------------------------------------------------
std::unordered_set<int32_t>  g_seen;
std::vector<NavMesh::PolyId> g_stack;
// THE STRICT FLOOD -- a MEASUREMENT, and nothing reads it but the log (Session 147).
//
// The permissive flood above expands through `Walkable` alone, which since S96 is the poly TYPE mask
// and nothing else -- bit 23 (the marker on water, lava, bog and out-of-bounds) refuses nothing. So
// the flood crosses the flooded channels of the Garamsythe Waterway, and every exit on the far side
// answers `reach=1`. Our own logs have been printing the contradiction on adjacent lines for weeks:
//
//   raw=0x0FA00000 eff=0x0FA00000 count=3077  *** UNWALKABLE (bit23) ***      <- the census
//   routable? "Exit, ... East Waterway Control" poly=99 eff=0x1FA00000 walk=1 reach=1
//
// and `unreachable=0` in every archived log, because the filter has never dropped anything, ever.
//
// This second flood is the same walk with the leader's own floor test added, so the two answers can
// be COMPARED rather than argued about. It changes NO behaviour: `Reachable` still answers off the
// permissive set, and the exit filter still uses `Reachable`.
//
// WHY NOT JUST RE-ARM BIT 23: because S96 did exactly that and had to revert it -- it refused 399 of
// 690 floor prims on map 311 and cost the tester a working exit, since the Waterway's shallow water
// is ordinary floor the party walks on. And our own archive kills the retry twice over: the SAME
// pattern (`eff=0x0FA00000 walk=1 reach=1`) appears on "Exit, Bhujerba: Travica Way", on a map with
// no water at all, in a log where four routes succeeded. Bit 23 marks the ledge geometry under a
// perfectly good exit seam too. So the question this measures is not "is bit 23 set" -- it is
// whether a terrain-respecting COMPONENT tracks the exits the player genuinely cannot reach while
// still containing the ones they can. If it does, it becomes the filter's second gate. If it refuses
// Travica Way, it is the wrong instrument as well and the log says so on its first outing.
std::unordered_set<int32_t>  g_seenStrict;
std::vector<NavMesh::PolyId> g_stackStrict;
uint32_t         g_epoch     = 0xFFFFFFFFu;
NavMesh::PolyId  g_startPoly = NavMesh::kNoPoly;
bool             g_running   = false;

void Publish() {
    auto snap       = std::make_shared<const std::unordered_set<int32_t>>(g_seen);
    auto snapStrict = std::make_shared<const std::unordered_set<int32_t>>(g_seenStrict);
    {
        std::lock_guard<std::mutex> lk(g_pubMutex);
        g_published       = snap;
        g_publishedStrict = snapStrict;
    }
    g_count.store(static_cast<int>(g_seen.size()), std::memory_order_release);
    g_countStrict.store(static_cast<int>(g_seenStrict.size()), std::memory_order_release);
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
        // The strict flood seeds from the same poly. If the player is somehow STANDING on terrain
        // their class refuses, it seeds anyway -- refusing the seed would make the whole strict set
        // empty and the measurement would read as "nothing is reachable", which is a bug wearing a
        // finding's clothes.
        g_seenStrict.clear();
        g_stackStrict.clear();
        g_seenStrict.insert(here);
        g_stackStrict.push_back(here);
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

    // The strict flood, on the same frame budget. Separate frontier, because a poly reachable ONLY
    // through refused terrain must not go on expanding the strict component -- that is the whole
    // distinction being measured.
    int strictBudget = kPolysPerFrame;
    while (!g_stackStrict.empty() && strictBudget-- > 0 &&
           static_cast<int>(g_seenStrict.size()) < kMaxPolys) {
        const NavMesh::PolyId cur = g_stackStrict.back();
        g_stackStrict.pop_back();
        for (int e = 0; e < 3; ++e) {
            const NavMesh::PolyId n = NavMesh::Neighbor(cur, e);
            if (n == NavMesh::kNoPoly || g_seenStrict.count(n)) continue;
            if (!NavMesh::Walkable(n) || NavMesh::TerrainRefused(n)) continue;
            g_seenStrict.insert(n);
            g_stackStrict.push_back(n);
        }
    }

    if ((g_stack.empty() && g_stackStrict.empty()) ||
        static_cast<int>(g_seen.size()) >= kMaxPolys) {
        g_running = false;
        Publish();
        char m[192];
        snprintf(m, sizeof(m),
                 "reach: fill complete -- %zu polys reachable from poly %d "
                 "(strict, terrain-refusing: %zu -- DIAGNOSTIC, filters nothing)",
                 g_seen.size(), g_startPoly, g_seenStrict.size());
        Log::Write("NAV-ROUTE", m);
    }
}

void Invalidate() {
    g_running = false;
    g_epoch   = 0xFFFFFFFFu;
    g_seen.clear();
    g_stack.clear();
    g_seenStrict.clear();
    g_stackStrict.clear();
    g_ready.store(false, std::memory_order_release);
    g_count.store(0, std::memory_order_release);
    g_countStrict.store(0, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_pubMutex);
    g_published.reset();
    g_publishedStrict.reset();
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

// DIAGNOSTIC TWIN of Reachable, answered off the terrain-refusing component. Same point resolution,
// same fail-open discipline, so any disagreement between the two is about TERRAIN and nothing else.
// Nothing filters on this -- the exit list still lives or dies by Reachable above.
bool ReachableStrict(const FVec3& p, float tolerance) {
    std::shared_ptr<const std::unordered_set<int32_t>> snap;
    {
        std::lock_guard<std::mutex> lk(g_pubMutex);
        snap = g_publishedStrict;
    }
    if (!snap || snap->empty()) return true;

    NavMesh::PolyId p0 = NavMesh::FindPolyAt(p.x, p.y, p.z);
    if (p0 == NavMesh::kNoPoly && tolerance > 0.0f)
        p0 = NavMesh::FindPolyAt(p.x, p.y + tolerance, p.z);
    if (p0 == NavMesh::kNoPoly) return true;

    return snap->count(p0) != 0;
}

} // namespace NavReach

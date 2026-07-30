#include "navigation/nav_blocked.h"
#include "core/logger.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace NavBlocked {
namespace {

struct Spot { FVec3 at; uint32_t epoch; uint64_t stampMs; };

// A tiny ring. An obstruction the player walked into is a point, and a handful of them is all one map
// ever needs -- if a map genuinely has more than this, the routing is wrong in a way a bigger list
// would only hide.
constexpr size_t kMax = 16;

std::vector<Spot> g_spots;

bool NearSpot(const Spot& b, const FVec3& p) {
    const float dx = b.at.x - p.x, dz = b.at.z - p.z;
    return dx * dx + dz * dz <= kRadius * kRadius;
}

} // namespace

void Note(const FVec3& where, uint32_t epoch) {
    const uint64_t now = GetTickCount64();
    // Prune stale and foreign-map entries here rather than on a timer -- this is the only place the
    // set grows, so it is the only place it needs to shrink.
    for (size_t i = 0; i < g_spots.size();) {
        if (g_spots[i].epoch != epoch || now - g_spots[i].stampMs > kTtlMs)
            g_spots.erase(g_spots.begin() + static_cast<long>(i));
        else
            ++i;
    }
    for (Spot& b : g_spots) {                    // already know this spot -> refresh, do not duplicate
        if (NearSpot(b, where)) { b.stampMs = now; return; }
    }
    if (g_spots.size() >= kMax) g_spots.erase(g_spots.begin());
    g_spots.push_back(Spot{ where, epoch, now });
    char m[176];
    snprintf(m, sizeof(m),
             "blocked: recorded (%.1f,%.1f,%.1f) epoch=%u -- %zu spot(s), each held %llus",
             where.x, where.y, where.z, epoch, g_spots.size(),
             (unsigned long long)(kTtlMs / 1000));
    Log::Write("NAV-ROUTE", m);
}

void Clear() {
    if (g_spots.empty()) return;
    g_spots.clear();
    Log::Write("NAV-ROUTE", "blocked: cleared (map change)");
}

bool Any() { return !g_spots.empty(); }

bool Contains(const FVec3& c, uint32_t epoch, uint64_t nowMs) {
    for (const Spot& b : g_spots) {
        if (b.epoch != epoch) continue;
        if (nowMs - b.stampMs > kTtlMs) continue;
        // Y matters: a spot on the floor below must not block the balcony above it.
        if (NearSpot(b, c) && std::fabs(c.y - b.at.y) <= 3.0f) return true;
    }
    return false;
}

} // namespace NavBlocked

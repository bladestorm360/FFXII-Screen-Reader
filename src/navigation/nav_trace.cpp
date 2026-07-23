#include "navigation/nav_trace.h"
#include "navigation/map_names.h"
#include "core/logger.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace NavTrace {

namespace {

// Ring of recent positions. 64 crumbs at ~1 m is ~60 m of trail — several times the distance between a
// doorway and anywhere a player could have come from, so the approach to a transition is always intact.
constexpr size_t kTrail    = 64;
constexpr float  kMinMove  = 1.0f;    // metres before a new crumb is worth recording
constexpr size_t kDumpTail = 12;      // crumbs printed on a transition (the approach, in order)

struct Crumb { float x, y, z; };

std::vector<Crumb> g_trail;
size_t             g_next  = 0;       // ring write cursor once full
int                g_map   = -1;
bool               g_have  = false;
Crumb              g_last{};

void Push(const FVec3& p) {
    if (g_trail.size() < kTrail) {
        g_trail.push_back(Crumb{ p.x, p.y, p.z });
    } else {
        g_trail[g_next] = Crumb{ p.x, p.y, p.z };
        g_next = (g_next + 1) % kTrail;
    }
}

// Oldest-to-newest view of the ring.
void Ordered(std::vector<Crumb>& out) {
    out.clear();
    if (g_trail.size() < kTrail) { out = g_trail; return; }
    out.reserve(kTrail);
    for (size_t i = 0; i < kTrail; ++i) out.push_back(g_trail[(g_next + i) % kTrail]);
}

// The trail, plus the axis-aligned box it spans. The BOX is what answers "is this direction blocked":
// a player who spent a minute trying to walk east and never got past x=48.41 has told us that in data,
// without ever being asked to look at anything.
void DumpOrdered(const char* what, int mapId, size_t tail) {
    std::vector<Crumb> o;
    Ordered(o);
    if (o.empty()) return;

    float minX = o[0].x, maxX = o[0].x, minZ = o[0].z, maxZ = o[0].z, minY = o[0].y, maxY = o[0].y;
    for (const auto& c : o) {
        if (c.x < minX) minX = c.x;   if (c.x > maxX) maxX = c.x;
        if (c.z < minZ) minZ = c.z;   if (c.z > maxZ) maxZ = c.z;
        if (c.y < minY) minY = c.y;   if (c.y > maxY) maxY = c.y;
    }
    char m[224];
    snprintf(m, sizeof(m), "==== %s: mapId=%d crumbs=%zu walked box x[%.1f..%.1f] z[%.1f..%.1f] y[%.1f..%.1f] ====",
             what, mapId, o.size(), minX, maxX, minZ, maxZ, minY, maxY);
    Log::Write("NAV-TRACE", m);

    const size_t first = (o.size() > tail) ? (o.size() - tail) : 0;
    for (size_t i = first; i < o.size(); ++i) {
        snprintf(m, sizeof(m), "  [%zu] (%.2f,%.2f,%.2f)%s", i, o[i].x, o[i].y, o[i].z,
                 (i + 1 == o.size()) ? "   <== LAST POSITION ON THIS MAP" : "");
        Log::Write("NAV-TRACE", m);
    }
}

} // namespace

void OnFieldFrame(int mapId, const FVec3& pos) {
    if (mapId != g_map) {
        // The trail belongs to the map we just LEFT, and its last crumb is within kMinMove of wherever
        // the trigger fired. Dump before resetting, and only when there is something to say (the first
        // map of a session has no predecessor).
        if (g_map >= 0 && !g_trail.empty()) {
            char m[160];
            snprintf(m, sizeof(m), "TRANSITION FIRED: mapId %d -> %d; the trail below ends where the player crossed",
                     g_map, mapId);
            Log::Write("NAV-TRACE", m);
            DumpOrdered("trail of the map just left", g_map, kDumpTail);
        }
        g_map  = mapId;
        g_have = false;
        g_next = 0;
        g_trail.clear();
    }

    if (g_have) {
        const float dx = pos.x - g_last.x, dz = pos.z - g_last.z;
        if (dx * dx + dz * dz < kMinMove * kMinMove) return;   // standing still / shuffling
    }
    g_last = Crumb{ pos.x, pos.y, pos.z };
    g_have = true;
    Push(pos);
}

void DumpTrail() {
    DumpOrdered("trail so far (player has NOT left this map)", g_map, kTrail);
}

} // namespace NavTrace

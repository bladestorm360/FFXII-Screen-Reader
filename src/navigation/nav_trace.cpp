#include "navigation/nav_trace.h"
#include "navigation/map_names.h"
#include "navigation/map_query.h"
#include "navigation/nav_common.h"
#include "navigation/exit_scan.h"   // ClaimedDestForGroup -- the crossing oracle's other half
#include "core/logger.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace NavTrace {

namespace {

// Ring of recent positions. 64 crumbs at ~1 m is ~60 m of trail — several times the distance between a
// doorway and anywhere a player could have come from, so the approach to a transition is always intact.
constexpr size_t kTrail    = 64;
constexpr float  kMinMove  = 1.0f;    // metres before a new crumb is worth recording
constexpr size_t kDumpTail = 12;      // crumbs printed on a transition (the approach, in order)

struct Crumb {
    float x, y, z;
    // Distance from this crumb to the nearest map-jump seam VERTEX, and to that same group's
    // CENTROID -- measured when the crumb is laid down, because at transition time the map has
    // already changed and the old map's walkmap is gone.
    //
    // The last crumb before a transition therefore records, for free and with nothing for the tester
    // to observe, how far the player actually was from the seam when it fired versus how far the mod
    // was TELLING them it was. That difference is the reported "25 steps north for a transition that
    // fires after five", and it is the only ground truth for it.
    int   seamGroup = 0;              // 0 = no seam data for this crumb
    float dNear     = -1.0f;
    float dCentroid = -1.0f;
};

std::vector<Crumb> g_trail;
size_t             g_next  = 0;       // ring write cursor once full
int                g_map   = -1;
bool               g_have  = false;
Crumb              g_last{};

// Seams of the map currently being walked. Filled from the shared per-map cache on the first crumb,
// so the sweep is never paid twice and never runs before the walkmap is up.
std::vector<MapQuery::MapJumpSurface> g_surf;
bool                                  g_haveSurf = false;

// Nearest seam to `p`, by vertex. Fills the crumb's measurement fields.
void MeasureSeams(Crumb& c) {
    if (!g_haveSurf) {
        if (!MapQuery::HasWorld()) return;
        MapQuery::CachedMapJumpSurfaces(g_map, g_surf);
        g_haveSurf = true;
    }
    const FVec3 p{ c.x, c.y, c.z };
    float best = -1.0f;
    for (const auto& s : g_surf) {
        FVec3 nearPt{};                       // NOT `near`: <windef.h> defines it as an empty macro
        if (!MapQuery::NearestPointOnSurface(s, p, nearPt)) continue;
        const float d = NavCommon::Distance2D(p, nearPt);
        if (best < 0.0f || d < best) {
            best = d;
            c.seamGroup = s.group;
            c.dNear     = d;
            c.dCentroid = NavCommon::Distance2D(p, s.centroid);
        }
    }
}

void Push(const Crumb& c) {
    if (g_trail.size() < kTrail) {
        g_trail.push_back(c);
    } else {
        g_trail[g_next] = c;
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
        char seam[112] = {};
        if (o[i].seamGroup != 0)
            snprintf(seam, sizeof(seam), " seam g%d: near=%.1fm/%dst centroid=%.1fm/%dst",
                     o[i].seamGroup, o[i].dNear, NavCommon::DistanceToSteps(o[i].dNear),
                     o[i].dCentroid, NavCommon::DistanceToSteps(o[i].dCentroid));
        snprintf(m, sizeof(m), "  [%zu] (%.2f,%.2f,%.2f)%s%s", i, o[i].x, o[i].y, o[i].z, seam,
                 (i + 1 == o.size()) ? "   <== LAST POSITION ON THIS MAP" : "");
        Log::Write("NAV-TRACE", m);
    }

    // The one line that answers the overshoot question outright: where the transition actually fired
    // versus what the mod would have said the distance was. `centroid` is what an exit aims at today.
    const Crumb& last = o.back();
    if (last.seamGroup != 0) {
        snprintf(m, sizeof(m),
                 "  CROSSED seam g%d at %.1fm (%d steps) from its near edge, but %.1fm (%d steps) from "
                 "its centroid -- the centroid overstates the walk by %.1fm (%d steps)",
                 last.seamGroup, last.dNear, NavCommon::DistanceToSteps(last.dNear),
                 last.dCentroid, NavCommon::DistanceToSteps(last.dCentroid),
                 last.dCentroid - last.dNear,
                 NavCommon::DistanceToSteps(last.dCentroid) - NavCommon::DistanceToSteps(last.dNear));
        Log::Write("NAV-TRACE", m);
    }
}

// THE CROSSING ORACLE -- belief next to outcome, on every transition, for free.
//
// The tester reports exits that are SWAPPED as well as exits that are missing, and nothing the mod
// prints today can tell those apart: every existing line reports what the mod BELIEVES an exit leads
// to, so a wrong belief reads exactly like a right one. This prints the belief and then the fact.
//
// The player walks onto a seam and the game loads a map. The last crumb already records which seam
// group they were standing nearest when it fired (MeasureSeams, above -- measured while the OLD map's
// walkmap was still loaded, which is the only time it can be). ScanExits publishes what that map
// claimed each group leads to. `mapId` is where we actually ended up. If the claim and the arrival
// disagree, the group -> destination binding is wrong, and there is nothing left to infer.
//
// Deliberately NOT another model of the trigger. Session 60's rule stands: five models of the
// transition have been inferred from the blob and refuted in play, and this proposes no sixth. It
// only measures, and it costs one lookup per map change.
void CrossingOracle(int leftMap, int arrivedMap) {
    std::vector<Crumb> o;
    Ordered(o);
    if (o.empty()) return;
    const Crumb& last = o.back();

    char arrived[96] = {};
    {
        const std::wstring n = MapNames::ResolveFullAreaName(arrivedMap);
        for (size_t k = 0; k < n.size() && k < 95; ++k)
            arrived[k] = (n[k] < 128) ? static_cast<char>(n[k]) : '?';
    }

    char m[352];
    if (last.seamGroup == 0) {
        snprintf(m, sizeof(m),
                 "CROSSING ORACLE: left map %d -> arrived %d (\"%s\"), but the last crumb matched NO "
                 "seam group -- this map published no map-jump surface near where the player crossed",
                 leftMap, arrivedMap, arrived);
        Log::Write("NAV-TRACE", m);
        return;
    }

    uint16_t claimed = 0;
    if (!EntityScan::ClaimedDestForGroup(leftMap, last.seamGroup, claimed)) {
        snprintf(m, sizeof(m),
                 "CROSSING ORACLE: left map %d via seam g%d (%.1fm from its near edge) -> arrived %d "
                 "(\"%s\"); the mod claimed NOTHING for that group -- no controller owns it, so this "
                 "exit was never listed",
                 leftMap, last.seamGroup, last.dNear, arrivedMap, arrived);
        Log::Write("NAV-TRACE", m);
        return;
    }

    char claimedName[96] = {};
    {
        const std::wstring n = MapNames::ResolveFullAreaName(static_cast<int>(claimed));
        for (size_t k = 0; k < n.size() && k < 95; ++k)
            claimedName[k] = (n[k] < 128) ? static_cast<char>(n[k]) : '?';
    }
    const bool match = (static_cast<int>(claimed) == arrivedMap);
    snprintf(m, sizeof(m),
             "CROSSING ORACLE: left map %d via seam g%d (%.1fm from its near edge) | mod claimed "
             "%u (\"%s\") | ACTUALLY ARRIVED %d (\"%s\")  <== %s",
             leftMap, last.seamGroup, last.dNear, claimed, claimedName, arrivedMap, arrived,
             match ? "MATCH" : "MISMATCH -- the group->destination binding is WRONG");
    Log::Write("NAV-TRACE", m);
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
            CrossingOracle(g_map, mapId);
        }
        g_map  = mapId;
        g_have = false;
        g_next = 0;
        g_trail.clear();
        g_surf.clear();
        g_haveSurf = false;        // the seams belong to the map we just left
    }

    if (g_have) {
        const float dx = pos.x - g_last.x, dz = pos.z - g_last.z;
        if (dx * dx + dz * dz < kMinMove * kMinMove) return;   // standing still / shuffling
    }
    Crumb c{ pos.x, pos.y, pos.z };
    MeasureSeams(c);
    g_last = c;
    g_have = true;
    Push(c);
}

void DumpTrail() {
    DumpOrdered("trail so far (player has NOT left this map)", g_map, kTrail);
}

} // namespace NavTrace

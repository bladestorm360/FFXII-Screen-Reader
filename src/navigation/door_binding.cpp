#include "navigation/door_binding.h"
#include "navigation/map_script.h"
#include "navigation/map_script_routines.h"
#include "navigation/map_names.h"
#include "navigation/nav_mesh.h"
#include "navigation/nav_rva.h"
#include "core/logger.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace DoorBinding {

namespace {

using EntityList::Category;
using EntityScan::Entity;

// How far from the object the floor it opens may lie. A door's own collision floor is the doorway it
// stands in -- Mirror of the Soul's doors read their material ids on the poly under or beside them --
// while a remote switch opens floor tens of metres away. 3 m is the route reach already used for a
// class-1 target (nav_commands), i.e. "close enough to press".
constexpr float kOpenedFloorReach = 3.0f;
constexpr int   kRingDirs         = 8;
const float     kRingRadii[]      = { 0.0f, 1.0f, 2.0f, kOpenedFloorReach };

// Walkmap material id: flags bits 13-17 -- the index setmapidfloor's bank entry is written at.
constexpr uint32_t kMaterialShift = 13;
constexpr uint32_t kMaterialMask  = 0x1F;

// ---- per-map cache (entity-list lock held by every caller) ------------------------------------
struct Cache {
    int      mapId  = -1;
    uint64_t script = 0;       // MapScript::ScriptFingerprint of the blob the facts were read from
    bool     built  = false;
    bool     ok     = false;
    std::vector<MapScript::RoutineFacts> facts;
    std::vector<int>                     destOf;    // per routine: index into dests, or -1
    std::vector<MapScript::ExitDest>     dests;
    std::vector<void*>                   logged;    // scene objects already reported this map
    int      summaryTotal = 0;                      // high-water of objects the summary line covered
};
Cache g_cache;

void EnsureCache() {
    const int      m  = MapNames::CurrentMapId();
    const uint64_t fp = MapScript::ScriptFingerprint();
    if (g_cache.built && g_cache.mapId == m && g_cache.script == fp) return;
    g_cache = Cache{};
    g_cache.mapId  = m;
    g_cache.script = fp;
    g_cache.built  = true;
    if (fp == 0) return;
    if (!MapScript::ReadRoutineFacts(g_cache.facts)) return;
    MapScript::ReadExitDests(g_cache.dests, false);
    g_cache.destOf.assign(g_cache.facts.size(), -1);
    for (size_t i = 0; i < g_cache.dests.size(); ++i) {
        const int ri = g_cache.dests[i].routineIndex;
        if (ri >= 0 && ri < static_cast<int>(g_cache.destOf.size()) && g_cache.destOf[ri] < 0)
            g_cache.destOf[ri] = static_cast<int>(i);
    }
    g_cache.ok = true;
}

std::string Ascii(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back((c < 128) ? static_cast<char>(c) : '?');
    return s;
}

// Distance to the nearest walkmap poly carrying one of `mask`'s material ids, sampled on rings around
// the object. -1 when none is within kOpenedFloorReach. Material comes from RAW flags: it is static
// map data, so the answer does not flip when the door opens.
float OpenedFloorDistance(const Entity& e, uint32_t mask, int& outMaterial, int& outPoly) {
    outMaterial = -1;
    outPoly     = NavMesh::kNoPoly;
    for (float r : kRingRadii) {
        const int dirs = (r == 0.0f) ? 1 : kRingDirs;
        for (int d = 0; d < dirs; ++d) {
            const float a = static_cast<float>(d) * (6.2831853f / static_cast<float>(kRingDirs));
            const float x = e.pos.x + r * std::cos(a);
            const float z = e.pos.z + r * std::sin(a);
            const NavMesh::PolyId p = NavMesh::FindPolyAt(x, e.pos.y, z);
            if (p == NavMesh::kNoPoly) continue;
            uint32_t raw = 0, eff = 0;
            if (!NavMesh::PolyFlags(p, raw, eff)) continue;
            const uint32_t mat = (raw >> kMaterialShift) & kMaterialMask;
            if (mat == 0 || !((mask >> mat) & 1u)) continue;   // material 0 is every ordinary floor
            outMaterial = static_cast<int>(mat);
            outPoly     = p;
            return r;
        }
    }
    return -1.0f;
}

bool AlreadyLogged(void* obj) {
    for (void* o : g_cache.logged) if (o == obj) return true;
    if (g_cache.logged.size() < 128) g_cache.logged.push_back(obj);
    return false;
}

} // namespace

void PromoteDoors(std::vector<Entity>& out) {
    bool anyCandidate = false;
    for (const auto& e : out)
        if (e.category == Category::Object && e.sceneObj && e.gameNamed) { anyCandidate = true; break; }
    if (!anyCandidate) return;

    EnsureCache();
    if (!g_cache.ok) return;

    int bound = 0, slotAgrees = 0, unbound = 0;
    for (auto& e : out) {
        if (e.category != Category::Object || !e.sceneObj || !e.gameNamed) continue;
        if (MapScript::ObjectContainerId(e.sceneObj) != 0) continue;   // facts describe container 0 only
        const int ri = MapScript::RoutineIndexOfObject(g_cache.facts, e.sceneObj);
        if (ri < 0) { ++unbound; continue; }
        ++bound;
        if (e.slot == static_cast<uint16_t>(ri)) ++slotAgrees;
        const MapScript::RoutineFacts& f = g_cache.facts[static_cast<size_t>(ri)];

        char why[224] = {};
        const int di = g_cache.destOf[static_cast<size_t>(ri)];
        if (di >= 0) {
            const MapScript::ExitDest& d = g_cache.dests[static_cast<size_t>(di)];
            snprintf(why, sizeof(why), "its routine calls mapjump -> dest=%u \"%s\"",
                     d.destMapId, Ascii(d.destName).c_str());
        } else if (f.opensFloorMask != 0) {
            int mat = -1, poly = NavMesh::kNoPoly;
            const float r = OpenedFloorDistance(e, f.opensFloorMask, mat, poly);
            if (r < 0.0f) {
                if (!AlreadyLogged(e.sceneObj)) {
                    char m[256];
                    snprintf(m, sizeof(m),
                             "door-binding: [%u:%u] \"%s\" routine[%d] \"%s\" opens floor mask 0x%X but none of "
                             "that floor lies within %.1fm -- a remote opener, left as Interactables",
                             e.container, e.slot, Ascii(e.label).c_str(), ri, f.name.c_str(),
                             f.opensFloorMask, kOpenedFloorReach);
                    Log::Write("NAV-DIAG", m);
                }
                continue;
            }
            snprintf(why, sizeof(why), "its routine opens floor id %d, found %.1fm from it (poly %d)",
                     mat, r, poly);
        } else {
            continue;
        }

        e.category = Category::Door;
        if (!AlreadyLogged(e.sceneObj)) {
            char m[384];
            snprintf(m, sizeof(m), "door-binding: [%u:%u] \"%s\" at (%.1f,%.1f,%.1f) -> Door: routine[%d] \"%s\", %s",
                     e.container, e.slot, Ascii(e.label).c_str(), e.pos.x, e.pos.y, e.pos.z,
                     ri, f.name.c_str(), why);
            Log::Write("NAV-DIAG", m);
        }
    }

    // Is the object -> routine identity actually holding here? `unbound` above zero on a map is the
    // tell that +0x48 is not the routine's entry table for some objects, and every promotion above
    // rests on that join. Logged on a new HIGH-WATER of objects judged, not once: the handle table
    // streams objects in over the first rescans, so the first line of a map covers almost nothing.
    if ((bound + unbound) > g_cache.summaryTotal) {
        g_cache.summaryTotal = bound + unbound;
        int withDest = 0, opens = 0;
        for (size_t i = 0; i < g_cache.facts.size(); ++i) {
            if (g_cache.destOf[i] >= 0) ++withDest;
            if (g_cache.facts[i].opensFloorMask) ++opens;
        }
        char m[256];
        snprintf(m, sizeof(m),
                 "door-binding: map %d -- %zu routines (%d with a destination, %d open a floor) | "
                 "unclassified objects: %d bound by entry-table identity (slot==routine on %d), %d unbound",
                 g_cache.mapId, g_cache.facts.size(), withDest, opens, bound, slotAgrees, unbound);
        Log::Write("NAV-DIAG", m);
    }
}

const MapScript::RoutineFacts* RoutineOf(void* sceneObj) {
    if (!sceneObj || MapScript::ObjectContainerId(sceneObj) != 0) return nullptr;   // facts are container 0
    EnsureCache();
    if (!g_cache.ok) return nullptr;
    const int ri = MapScript::RoutineIndexOfObject(g_cache.facts, sceneObj);
    return (ri >= 0 && ri < static_cast<int>(g_cache.facts.size())) ? &g_cache.facts[static_cast<size_t>(ri)]
                                                                    : nullptr;
}

} // namespace DoorBinding

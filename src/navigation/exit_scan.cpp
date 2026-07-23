#include "navigation/exit_scan.h"
#include "navigation/entity_classify.h"
#include "navigation/nav_reach.h"
#include "navigation/map_names.h"
#include "navigation/map_script.h"
#include "navigation/map_query.h"
#include "navigation/nav_common.h"
#include "core/logger.h"

#include <cmath>
#include <cstdio>
#include <vector>

// The EXIT half of the field scan, split out of entity_scan.cpp when that file passed the project's
// 500-line ceiling. entity_scan finds scene OBJECTS in the handle table; this finds the map's
// TRANSITIONS, which are not scene objects at all and come from two different engine tables.
namespace EntityScan {

using EntityList::Category;

namespace {

// ---- A transition IS a walkmap surface (Session 64) ---------------------------------------------
//
// The map's floor polygons carry a map-jump tag (`setmapidmj`; NavRva::WALK_POLY_MJ_*) whose value is
// the map-jump GROUP id, and each `__MJ_CTRL` routine declares the group it owns with
// `setmapjumpgroup(K)`. So one routine gives BOTH halves of a transition -- the surface you walk onto
// (from the walkmap) and where it goes (its own `mapjump` literal) -- and they can no longer be
// mismatched, because they are two fields of the same object.
//
// Verified against every crossing NavTrace has ever recorded:
//   Muthru group 3 -> 291 East End; the tester crossed at (55.1,65.4) and (56.4,64.0).
//   East End group 4 -> 290 Muthru;  the tester crossed at (19.7,58.0), walking west from (26,58).
//
// This retires five refuted models (`+0x54[N+1]`, the `+0x54` u `+0x70` union, the `+0x84` edge
// bearing, the boundary/passage marches, blob table order). They all failed the same way: they looked
// for TRANSITIONS in DOOR-shaped places. A transition is a walkmap surface you walk onto; an
// interactable door (a shop, a stair) is a scene object you press Enter on. Separate systems, separate
// readers -- never use one as evidence about the other.
//
// Resolved ONCE per map and cached: the sweep is ~15k guarded reads, its answer cannot change while a
// map is loaded, and the exit scan runs several times a second. Nothing is cached until the walkmap is
// actually up, because the scan starts on the first frame of a new map.
const std::vector<MapQuery::MapJumpSurface>& CachedSurfaces(int mapId) {
    static std::vector<MapQuery::MapJumpSurface> s_surf;
    static int  s_map    = -1;
    static bool s_haveMap = false;

    if (s_map != mapId) { s_map = mapId; s_haveMap = false; s_surf.clear(); }
    if (!s_haveMap && MapQuery::HasWorld()) {
        MapQuery::ReadMapJumpSurfaces(s_surf);
        s_haveMap = true;
    }
    return s_surf;
}

} // namespace

// The map's `+0x70` field-sign records, read ONCE per map and reused.
//
// EnumerateFieldSignRaw calls four game getters, and the scan it feeds runs several times a second from
// the input thread. Re-reading per rescan would multiply our exposure to game code for data that cannot
// change while a map is loaded — the table is built at map load and never rewritten. Re-reads while the
// result is still empty, because the field-sign table streams in after the map id changes, so the first
// rescan on a new map can legitimately see nothing. Caller holds g_mutex.
const std::vector<MapExits::SignRec>& CachedSigns() {
    static std::vector<MapExits::SignRec> s_signs;
    static int s_map = -1;
    const int m = MapNames::CurrentMapId();
    if (m != s_map || s_signs.empty()) {
        s_map = m;
        MapExits::EnumerateFieldSignRaw(s_signs);
    }
    return s_signs;
}

// Append the current map's EXITS = its map-jump TRANSITIONS: the walkmap surfaces the player walks
// onto to change area. Each becomes a fixed-position Category::Exit entity with a synthetic stable
// identity. See the note above CachedSurfaces for the mechanism and the evidence.
//
// NOT interactable doors. Shop entrances, "Stair to Lowtown" and the like are scene objects you press
// Enter on; they come through entity_scan's handle-table walk and their names from `+0x70` field signs.
// Nothing in this file may consult them, and nothing there may consult this.
//
// DROP RULES, both from the game's own data and neither from geometry we invented:
//   * no map-jump surface for the routine's group -> there is nothing to walk onto, so it is not a
//     transition on this map.
//   * destination name does not resolve -> the game's own table calls that map "NOT USED", so the
//     transition leads nowhere a player can go.
// (The name filter existed in S57-S60, was removed in S61 for deleting the real East End corridor, and
// is correct again now -- the bug was never the filter, it was the mislabelling it was applied to.)
//
// Caller holds g_mutex.
void ScanExits(std::vector<Entity>& out) {
    static int s_loggedMap = -1;
    const int  mapId     = MapNames::CurrentMapId();
    const bool logDetail = (mapId != s_loggedMap);   // once per map, not once per rescan
    if (logDetail) s_loggedMap = mapId;

    // Destinations come from the map's OWN script: one `__MJ_CTRL<N>` routine per loader, destination a
    // `mapjump` literal — readable on the first frame of any map, no cross-map data, no cache.
    std::vector<MapScript::ExitDest> dests;
    MapScript::ReadExitDests(dests, logDetail);

    const std::vector<MapQuery::MapJumpSurface>& surfaces = CachedSurfaces(mapId);

    std::vector<Entity> candidates;
    for (const auto& d : dests) {
        // WHERE: the walkmap surface tagged with this routine's map-jump group.
        const MapQuery::MapJumpSurface* surf = nullptr;
        if (d.group > 0)
            for (const auto& sf : surfaces) if (sf.group == d.group) { surf = &sf; break; }
        if (!surf) {
            if (logDetail) {
                char m[192];
                snprintf(m, sizeof(m),
                         "  __MJ_CTRL%03d group=%d dest=%u -> no map-jump surface on this map, dropped",
                         d.ctrlIndex, d.group, d.destMapId);
                Log::Write("NAV-DIAG", m);
            }
            continue;
        }

        // WHERE TO: this same routine's own `mapjump` literal. "NOT USED" means exactly that.
        if (!MapNames::HasRealAreaName(static_cast<int>(d.destMapId))) {
            if (logDetail) {
                char m[192];
                snprintf(m, sizeof(m),
                         "  __MJ_CTRL%03d group=%d surface at (%.1f,%.1f,%.1f) -> dest=%u is NOT USED, dropped",
                         d.ctrlIndex, d.group, surf->centroid.x, surf->centroid.y, surf->centroid.z,
                         d.destMapId);
                Log::Write("NAV-DIAG", m);
            }
            continue;
        }

        Entity e;
        e.sceneObj  = nullptr;
        e.fixed     = true;                                          // fixed world pos, no scene node
        e.flags     = 0;
        e.nameIdx   = static_cast<int16_t>(-(1000 + d.ctrlIndex));   // stable cursor id, one per controller
        e.pos       = surf->centroid;                                // the middle of the seam itself
        e.category  = Category::Exit;
        e.isTransition = true;                                       // the target IS the trigger surface
        e.label     = std::wstring(CategoryWord(Category::Exit)) + L", " + d.destName;
        // Stand the target on the floor: the centroid averages polygon vertices, which on a ramp sits
        // slightly off the walked surface, and the planner measures elevation against it.
        float floorY = 0.0f;
        if (MapQuery::HasWorld() && MapQuery::GroundAt(e.pos.x, e.pos.z, floorY)) e.pos.y = floorY;
        candidates.push_back(e);

        if (logDetail) {
            char n8[96] = {};
            for (size_t k = 0; k < d.destName.size() && k < 95; ++k)
                n8[k] = (d.destName[k] < 128) ? static_cast<char>(d.destName[k]) : '?';
            char m[288];
            snprintf(m, sizeof(m),
                     "  __MJ_CTRL%03d group=%d -> \"%s\" (%u) at (%.1f,%.1f,%.1f) | %d polys, box x[%.1f..%.1f] z[%.1f..%.1f]",
                     d.ctrlIndex, d.group, n8, d.destMapId, e.pos.x, e.pos.y, e.pos.z,
                     surf->polyCount, surf->min.x, surf->max.x, surf->min.z, surf->max.z);
            Log::Write("NAV-DIAG", m);
        }
    }

    // ---- Drop exits with no valid path ---------------------------------------------------------
    // An exit the party cannot walk to is not an exit. Reachability comes from the per-map flood fill
    // (NavReach), never from an A* per exit: a failed search costs ~45,000 raycasts and rescans run
    // several times a second, so the naive version would be a mod-side stall in the game's own frame.
    //
    // FAIL-OPEN, three ways, because this HIDES things from a player who cannot see what was hidden:
    //   1. Nothing is filtered until the fill has closed the component.
    //   2. kExitReachTol of slack, because door triggers sit on the map seam and are routinely a cell
    //      or two past the last walkable sample.
    //   3. If the filter would remove more than HALF a map's exits it removes NOTHING and says so. A
    //      filter eating most of the list is measuring its own predicate, not the map -- and the exits
    //      it would have eaten first are exactly the district doors the tester is trying to reach.
    size_t wouldDrop = 0;
    const bool ready = NavReach::Ready();
    if (ready)
        for (const auto& c : candidates)
            if (!NavReach::Reachable(c.pos, kExitReachTol)) ++wouldDrop;

    const bool tooMany = (wouldDrop * 2 > candidates.size());
    const bool filter  = ready && wouldDrop > 0 && !tooMany;
    if (ready && wouldDrop > 0 && tooMany) {
        char m[176];
        snprintf(m, sizeof(m),
                 "reachability filter disabled: would drop %zu of %zu exits (reachable cells=%d)",
                 wouldDrop, candidates.size(), NavReach::CellCount());
        Log::Write("NAV-DIAG", m);
    }

    for (const auto& c : candidates) {
        if (filter && !NavReach::Reachable(c.pos, kExitReachTol)) {
            if (logDetail) {
                char n8[96] = {};
                for (size_t k = 0; k < c.label.size() && k < 95; ++k)
                    n8[k] = (c.label[k] < 128) ? static_cast<char>(c.label[k]) : '?';
                char m[224];
                snprintf(m, sizeof(m), "  unreachable, dropped: \"%s\" at (%.1f,%.1f,%.1f)",
                         n8, c.pos.x, c.pos.y, c.pos.z);
                Log::Write("NAV-DIAG", m);
            }
            continue;
        }
        out.push_back(c);
    }
}
} // namespace EntityScan

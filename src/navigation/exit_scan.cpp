#include "navigation/exit_scan.h"
#include "navigation/entity_classify.h"
#include "navigation/nav_reach.h"
#include "navigation/map_names.h"
#include "navigation/map_script.h"
#include "navigation/map_query.h"
#include "navigation/nav_common.h"
#include "navigation/player_state.h"
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
// The sweep itself now lives in MapQuery::CachedMapJumpSurfaces, because the breadcrumb trace and the
// '-key probe want the same answer and re-sweeping per subsystem would pay ~15k reads three times a
// map. This keeps a local copy so the several-times-a-second rescan does not copy the vector each
// call; the shared cache is only consulted while this one is still empty for the map.
const std::vector<MapQuery::MapJumpSurface>& CachedSurfaces(int mapId) {
    static std::vector<MapQuery::MapJumpSurface> s_surf;
    static int  s_map    = -1;
    static bool s_haveMap = false;

    if (s_map != mapId) { s_map = mapId; s_haveMap = false; s_surf.clear(); }
    if (!s_haveMap && MapQuery::HasWorld()) {
        MapQuery::CachedMapJumpSurfaces(mapId, s_surf);
        s_haveMap = true;
    }
    return s_surf;
}

// The group -> destination binding, per map, so the crossing oracle can still read it after the map
// has unloaded. Two maps' worth is enough: the one being left and the one being entered.
struct ClaimRow { int mapId = -1; int group = 0; uint16_t dest = 0; };
std::vector<ClaimRow> g_claims;
int                   g_claimMapA = -1;
int                   g_claimMapB = -1;

void PublishClaims(int mapId, const std::vector<MapScript::ExitDest>& dests) {
    if (mapId == g_claimMapA) return;                    // already published for this map
    // Keep the previous map's rows -- the crossing oracle reads them one frame AFTER the map changed.
    if (mapId != g_claimMapB) {
        for (size_t i = 0; i < g_claims.size();) {
            if (g_claims[i].mapId != g_claimMapA) g_claims.erase(g_claims.begin() + static_cast<long long>(i));
            else ++i;
        }
        g_claimMapB = g_claimMapA;
    }
    g_claimMapA = mapId;
    for (const auto& d : dests)
        if (d.group > 0) g_claims.push_back(ClaimRow{ mapId, d.group, static_cast<uint16_t>(d.destMapId) });
}

} // namespace

bool ClaimedDestForGroup(int mapId, int group, uint16_t& destMapId) {
    for (const auto& r : g_claims)
        if (r.mapId == mapId && r.group == group) { destMapId = r.dest; return true; }
    return false;
}

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
    PublishClaims(mapId, dests);

    // Why each candidate was rejected. Counted rather than only logged, because the summary below has
    // to be able to say "3 controllers, 3 surfaces, 3 listed" or "3 controllers, 3 surfaces, 1 listed"
    // -- a missing exit is invisible unless the count that produced it is printed beside it.
    int dropNoGroup = 0, dropNotUsed = 0, dropUnreach = 0;

    std::vector<Entity> candidates;
    for (const auto& d : dests) {
        // WHERE: the walkmap surface tagged with this routine's map-jump group.
        const MapQuery::MapJumpSurface* surf = nullptr;
        if (d.group > 0)
            for (const auto& sf : surfaces) if (sf.group == d.group) { surf = &sf; break; }
        if (!surf) {
            ++dropNoGroup;
            // ALWAYS logged. This used to be gated on `logDetail`, which is once per map id, so a
            // surface that streamed in a moment late -- or one that never arrives at all -- produced
            // exactly one line at the start of the visit and silence afterwards. A missing exit that
            // the log mentioned once, minutes ago, is a missing exit nobody can diagnose.
            char m[192];
            snprintf(m, sizeof(m),
                     "  __MJ_CTRL%03d group=%d dest=%u -> no map-jump surface on this map, dropped "
                     "(map has %zu surface(s))",
                     d.ctrlIndex, d.group, d.destMapId, surfaces.size());
            Log::Write("NAV-DIAG", m);
            continue;
        }

        // WHERE TO: this same routine's own `mapjump` literal. "NOT USED" means exactly that.
        if (!MapNames::HasRealAreaName(static_cast<int>(d.destMapId))) {
            ++dropNotUsed;
            char m[192];
            snprintf(m, sizeof(m),
                     "  __MJ_CTRL%03d group=%d surface at (%.1f,%.1f,%.1f) -> dest=%u is NOT USED, dropped",
                     d.ctrlIndex, d.group, surf->centroid.x, surf->centroid.y, surf->centroid.z,
                     d.destMapId);
            Log::Write("NAV-DIAG", m);
            continue;
        }

        Entity e;
        e.sceneObj  = nullptr;
        e.fixed     = true;                                          // fixed world pos, no scene node
        e.flags     = 0;
        e.nameIdx   = static_cast<int16_t>(-(1000 + d.ctrlIndex));   // stable cursor id, one per controller
        e.category  = Category::Exit;
        e.isTransition = true;                                       // the target IS the trigger surface
        e.label     = std::wstring(CategoryWord(Category::Exit)) + L", " + d.destName;

        // AIM AT THE NEAR EDGE, not the middle of the seam. A seam is a strip -- Southern Plaza's is
        // 28 polys spanning z[132.0..140.0] -- so its centroid is metres past the point the player
        // actually crosses, which is where "25 steps north" for a transition that fires after five
        // came from. The nearest tagged vertex is the point you reach first, and using it fixes the
        // spoken distance and the route goal together, so `/` and `\` agree by construction.
        //
        // Recomputed on every scan (which is every command) from the live player position; that read
        // is memory-only, so it is safe on the input thread.
        e.pos = surf->centroid;
        FVec3 pp{};
        if (PlayerState::ReadPlayerPos(pp)) {
            FVec3 nearPt{};   // NOT `near`: <windef.h> defines it as an empty macro
            if (MapQuery::NearestPointOnSurface(*surf, pp, nearPt)) e.pos = nearPt;
        }
        // The Y comes from the seam's OWN vertex. It used to come from MapQuery::GroundAt, which is
        // not a floor query at all: FUN_0026e3c0 takes the topmost floor and THEN climbs up to 30
        // units and casts back down with mask 0xFFFF, returning whatever it hits. On Upper
        // Apartments that could not tell the Highhall seam from the floor 7.8 m beneath it.
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

    size_t listed = 0;
    for (const auto& c : candidates) {
        if (filter && !NavReach::Reachable(c.pos, kExitReachTol)) {
            ++dropUnreach;
            char n8[96] = {};
            for (size_t k = 0; k < c.label.size() && k < 95; ++k)
                n8[k] = (c.label[k] < 128) ? static_cast<char>(c.label[k]) : '?';
            char m[224];
            snprintf(m, sizeof(m), "  unreachable, dropped: \"%s\" at (%.1f,%.1f,%.1f)",
                     n8, c.pos.x, c.pos.y, c.pos.z);
            Log::Write("NAV-DIAG", m);
            continue;
        }
        out.push_back(c);
        ++listed;
    }

    // ---- THE EXIT INVENTORY -------------------------------------------------------------------
    // One line that says how many exits this map HAS and how many the player is being told about.
    //
    // Re-logged whenever the numbers change, NOT once per map id. That distinction is the whole
    // reason it exists: the walkmap and the field script stream in after the map id does, so the
    // first scan of a visit legitimately sees zero surfaces, and a "once per map" latch prints that
    // zero and then goes quiet for the rest of the visit. The tester reports missing exits and the
    // log has been agreeing that everything is fine, because it only ever spoke at the one moment
    // when nothing was loaded yet.
    static int    s_invMap = -1;
    static size_t s_invSig = 0;
    const size_t  sig = (dests.size() * 1000003u) ^ (surfaces.size() * 10007u) ^ (listed * 101u) ^
                        static_cast<size_t>(dropNoGroup * 31 + dropNotUsed * 7 + dropUnreach);
    if (mapId != s_invMap || sig != s_invSig) {
        s_invMap = mapId;
        s_invSig = sig;
        char m[240];
        snprintf(m, sizeof(m),
                 "exits: controllers=%zu surfaces=%zu listed=%zu | dropped: nogroup=%d notused=%d "
                 "unreachable=%d",
                 dests.size(), surfaces.size(), listed, dropNoGroup, dropNotUsed, dropUnreach);
        Log::Write("NAV-DIAG", m);

        // EVERY walkmap map-jump group, including the ones no controller claims. An unclaimed group
        // is a transition surface the player can walk onto with no destination attached to it -- a
        // MISSING EXIT, and until now completely invisible, because the loop above only ever iterates
        // controllers and so can only report the failures it happens to walk past.
        for (const auto& sf : surfaces) {
            bool claimed = false;
            for (const auto& d : dests) if (d.group == sf.group) { claimed = true; break; }
            snprintf(m, sizeof(m),
                     "  surface g%d: %d polys at (%.1f,%.1f,%.1f) box x[%.1f..%.1f] z[%.1f..%.1f]%s",
                     sf.group, sf.polyCount, sf.centroid.x, sf.centroid.y, sf.centroid.z,
                     sf.min.x, sf.max.x, sf.min.z, sf.max.z,
                     claimed ? "" : "   <== NO CONTROLLER CLAIMS THIS GROUP -- unreachable exit");
            Log::Write("NAV-DIAG", m);
        }
    }
}
} // namespace EntityScan

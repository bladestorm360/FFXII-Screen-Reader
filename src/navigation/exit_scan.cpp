#include "navigation/exit_scan.h"
#include "navigation/entity_classify.h"
#include "navigation/nav_reach.h"
#include "navigation/nav_mesh.h"
#include "navigation/map_names.h"
#include "navigation/map_script.h"
#include "navigation/map_script_census.h"
#include "navigation/map_query.h"
#include "navigation/map_seams.h"
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
// Resolved ONCE per map: the sweep is ~15k guarded reads, its answer cannot change while a map is
// loaded, and the exit scan runs several times a second. The sweep AND the cache both live in
// MapQuery now (PrimeMapJumpSurfaces / CachedMapJumpSurfaces), driven once per map from
// PathPlanner's nav-safe frame, so this file only ever READS.
//
// There used to be a second cache here, on the theory that it saved the several-times-a-second
// rescan a vector copy. It saved nothing -- the shared cache returns a copy by design -- and it
// carried its own copy of the bug that shared cache had: both latched the first sweep that found
// MapQuery::HasWorld() true, which on the first frame of a new map is the PREVIOUS map's walkmap.
// Two layers of latch meant fixing one still left the exits a map behind. Do not reintroduce a
// local seam cache; ask MapQuery, and take "nothing yet" for an answer.

// The group -> destination binding, per map, so the crossing oracle can still read it after the map
// has unloaded. Two maps' worth is enough: the one being left and the one being entered.
struct ClaimRow { int mapId = -1; int group = 0; uint16_t dest = 0; };
std::vector<ClaimRow> g_claims;
int                   g_claimMapA = -1;   // current map
int                   g_claimMapB = -1;   // the one before it, kept for the oracle

// IDEMPOTENT, not latched. Two bugs lived in the latched version, and an A -> B -> A round trip --
// which is what walking between two waterway maps is -- hit both:
//
//   * `if (mapId == g_claimMapA) return;` fired on the FIRST call for a map. If the field script
//     was not readable yet that call carried zero rows, so the map was published empty and could
//     never be republished; the oracle then reported "the mod claimed NOTHING for that group" for
//     the rest of the visit.
//   * Re-entering the map still held in g_claimMapB skipped BOTH the prune and the B = A update, so
//     that map's old rows survived and the new ones were appended beside them. ClaimedDestForGroup
//     returns the FIRST match, i.e. the stale one, and the store grew on every round trip.
//
// So: republish whenever the map changed OR the row count is not yet what this scan says it should
// be, and always clear the map's own rows before appending.
void PublishClaims(int mapId, const std::vector<MapScript::ExitDest>& dests) {
    size_t want = 0;
    for (const auto& d : dests) if (d.group > 0) ++want;
    size_t held = 0;
    for (const auto& r : g_claims) if (r.mapId == mapId) ++held;
    if (mapId == g_claimMapA && held == want) return;   // already current and complete

    if (mapId != g_claimMapA) { g_claimMapB = g_claimMapA; g_claimMapA = mapId; }
    // Drop this map's own rows (a re-entry re-publishes them fresh) plus anything that is neither of
    // the two maps we keep.
    for (size_t i = 0; i < g_claims.size();) {
        const int m = g_claims[i].mapId;
        if (m == mapId || (m != g_claimMapA && m != g_claimMapB))
            g_claims.erase(g_claims.begin() + static_cast<long long>(i));
        else ++i;
    }
    for (const auto& d : dests)
        if (d.group > 0) g_claims.push_back(ClaimRow{ mapId, d.group, static_cast<uint16_t>(d.destMapId) });
}

// PAIR THE ONE UNCLAIMED SURFACE WITH THE ONE UNBOUND DESTINATION — Session 105.
//
// An event-fired transition supplies only half of S64's binding. Map 313's staircase into the Royal
// Palace Cellar Stores is a real walkmap surface tagged map-jump group 1 that the seam sweep has
// always found, and routine[4] (the `イベント…` routine) holds its `mapjump(567, 1, flags=0x1)` — but
// NOTHING arms group 1. The container census proved that across all five script containers, so there
// is no call anywhere that says "group 1 is this routine's". The two halves exist; the join does not.
//
// So the join is made by ELIMINATION, and only when elimination is the whole answer:
//
//   exactly ONE swept surface that no routine's group claims
//   AND exactly ONE group-less candidate with a real destination
//   => that surface is that candidate's. Any other count binds NOTHING.
//
// This is not a proximity match and it invents no geometry — the failure mode of every refuted exit
// model (S57-S64), and of the field-sign group-1 record this session tested and threw away when its
// `areaId` resolved to "Pharos at Ridorana" while the script said Royal Palace. It is arithmetic over
// the game's own two lists, and it is *unreachable* on a map that is already correct: such a map has
// zero unclaimed surfaces, so the first count is 0 and the function returns before deciding anything.
//
// The falsifier is already shipped and needs no new code: the binding is published to `g_claims`, so
// walking through it makes NavTrace's `CROSSING ORACLE` compare what the mod claimed against where
// the party actually arrived, and print `MATCH` or `MISMATCH` by itself.
//
// Caller holds g_mutex. Mutates `dests` — that is the point; the loop below then treats the pair
// exactly like any other group, with `groupInferred` carrying the provenance into the log.
void BindUnclaimedSurface(std::vector<MapScript::ExitDest>& dests,
                          const std::vector<MapQuery::MapJumpSurface>& surfaces, bool haveSurfaces) {
    if (!haveSurfaces) return;   // nothing swept yet; this is not the frame to decide on

    int unclaimedGroup = -1, unclaimedCount = 0;
    for (const auto& sf : surfaces) {
        bool claimed = false;
        for (const auto& d : dests) if (d.group > 0 && d.group == sf.group) { claimed = true; break; }
        if (!claimed) { ++unclaimedCount; unclaimedGroup = sf.group; }
    }

    int candIdx = -1, candCount = 0;
    for (size_t i = 0; i < dests.size(); ++i) {
        const MapScript::ExitDest& d = dests[i];
        if (d.viaController || d.group > 0) continue;
        if (!MapNames::HasRealAreaName(static_cast<int>(d.destMapId))) continue;
        ++candCount;
        candIdx = static_cast<int>(i);
    }

    // The ordinary map: every surface claimed, no group-less candidate. Say nothing.
    if (unclaimedCount == 0 && candCount == 0) return;

    // ALWAYS logged, never gated on logDetail. Both outcomes are about an exit the player either
    // gains or does not, and "we declined to bind" must be as visible as "we bound".
    if (unclaimedCount != 1 || candCount != 1) {
        char m[256];
        snprintf(m, sizeof(m),
                 "  elimination binding: %d unclaimed surface(s) vs %d group-less destination(s) -- "
                 "not 1:1, nothing bound%s",
                 unclaimedCount, candCount,
                 (unclaimedCount > 0 && candCount == 0) ? " (surface with no destination anywhere)"
                                                        : "");
        Log::Write("NAV-DIAG", m);
        return;
    }

    dests[candIdx].group         = unclaimedGroup;
    dests[candIdx].groupInferred = true;

    char n8[96] = {};
    for (size_t k = 0; k < dests[candIdx].destName.size() && k < 95; ++k)
        n8[k] = (dests[candIdx].destName[k] < 128) ? static_cast<char>(dests[candIdx].destName[k]) : '?';
    char m[288];
    snprintf(m, sizeof(m),
             "  elimination binding: the ONE unclaimed surface (group %d) is routine[%d] \"%s\"'s "
             "-> dest=%u (\"%s\") -- INFERRED, no script arms this group",
             unclaimedGroup, dests[candIdx].routineIndex, dests[candIdx].routineName.c_str(),
             dests[candIdx].destMapId, n8);
    Log::Write("NAV-DIAG", m);
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
// identity. See MapQuery::PrimeMapJumpSurfaces for the mechanism and the evidence.
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

    // ALWAYS-PRINTING, LOG-ONLY: the same scan across ALL FIVE script containers, not just the map
    // blob `ReadExitDests` reads. Nothing below consumes it. Its own latch decides when to print,
    // and its `MAP-JUMP GROUPS ARMED ANYWHERE` line is what the `NO CONTROLLER CLAIMS THIS GROUP`
    // line further down has to be read against -- a swept group that appears in the seams and in no
    // container is bound outside every loaded script, which is the answer S103 could not get.
    MapScript::LogContainerCensus();

    // WHERE: the map's transition seams, swept once per map on a nav-safe game frame. Before that
    // frame arrives this is legitimately empty, and an empty answer means WE LIST NO EXITS -- never
    // that we fall back on whatever was cached last, which is how every exit on this map ended up
    // wearing the previous map's geometry.
    std::vector<MapQuery::MapJumpSurface> surfaces;
    const bool haveSurfaces = MapQuery::CachedMapJumpSurfaces(mapId, surfaces);

    // BEFORE the claims are published, so the inferred pair is published with the rest and the
    // crossing oracle can check it, and so the "NO CONTROLLER CLAIMS THIS GROUP" seam line stops
    // firing for a surface that now has an owner.
    BindUnclaimedSurface(dests, surfaces, haveSurfaces);

    PublishClaims(mapId, dests);

    // One line, not one per controller: on the first frames of every map the seams are simply not
    // swept yet, and printing "dropped" for each controller there would bury the real drops.
    if (!haveSurfaces && !dests.empty()) {
        static int s_notSweptMap = -1;
        if (mapId != s_notSweptMap) {
            s_notSweptMap = mapId;
            char m[176];
            snprintf(m, sizeof(m),
                     "map-jump seams not swept for map %d yet (%zu controller(s) waiting) -- no exits "
                     "listed this scan",
                     mapId, dests.size());
            Log::Write("NAV-DIAG", m);
        }
    }

    // Why each candidate was rejected. Counted rather than only logged, because the summary below has
    // to be able to say "3 controllers, 3 surfaces, 3 listed" or "3 controllers, 3 surfaces, 1 listed"
    // -- a missing exit is invisible unless the count that produced it is printed beside it.
    int dropNoGroup = 0, dropNotUsed = 0, dropUnreach = 0;

    std::vector<Entity> candidates;
    for (const auto& d : dests) {
        // WHO BOUND THIS GROUP, for every log line below. A door controller is named by its index; an
        // event-bound transition has no controller index at all and is named by its routine, which is
        // the only thing that identifies it (Session 102).
        char who[96];
        if (d.viaController) snprintf(who, sizeof(who), "__MJ_CTRL%03d", d.ctrlIndex);
        else                 snprintf(who, sizeof(who), "routine[%d] \"%s\"%s", d.routineIndex,
                                      d.routineName.c_str(),
                                      d.groupInferred ? " [group INFERRED]" : "");

        // WHERE: the walkmap surface tagged with this routine's map-jump group.
        const MapQuery::MapJumpSurface* surf = nullptr;
        if (d.group > 0)
            for (const auto& sf : surfaces) if (sf.group == d.group) { surf = &sf; break; }
        if (!surf) {
            ++dropNoGroup;
            // ALWAYS logged -- but only once the seams for THIS map actually exist. This used to be
            // gated on `logDetail`, which is once per map id, so a surface that streamed in a moment
            // late -- or one that never arrives at all -- produced exactly one line at the start of
            // the visit and silence afterwards. A missing exit that the log mentioned once, minutes
            // ago, is a missing exit nobody can diagnose. The `haveSurfaces` gate is not a step back
            // towards that: with no seams swept yet EVERY controller misses, and the one-line notice
            // above says so; here we want the case where the seams are known and this group is not
            // among them, which is a genuinely missing exit.
            if (haveSurfaces) {
                char m[192];
                snprintf(m, sizeof(m),
                         "  %s group=%d dest=%u -> no map-jump surface on this map, dropped "
                         "(map has %zu surface(s))",
                         who, d.group, d.destMapId, surfaces.size());
                Log::Write("NAV-DIAG", m);
            }
            continue;
        }

        // WHERE TO: this same routine's own `mapjump` literal. "NOT USED" means exactly that.
        if (!MapNames::HasRealAreaName(static_cast<int>(d.destMapId))) {
            ++dropNotUsed;
            char m[192];
            snprintf(m, sizeof(m),
                     "  %s group=%d surface at (%.1f,%.1f,%.1f) -> dest=%u is NOT USED, dropped",
                     who, d.group, surf->centroid.x, surf->centroid.y, surf->centroid.z,
                     d.destMapId);
            Log::Write("NAV-DIAG", m);
            continue;
        }

        Entity e;
        e.sceneObj  = nullptr;
        e.fixed     = true;                                          // fixed world pos, no scene node
        e.flags     = 0;
        // Stable cursor id, one per BINDING. Controllers keep the exact ids they have always had;
        // event-bound transitions have no controller index (-1, which would collide the moment a map
        // had two of them) and take a disjoint band keyed on their routine slot instead.
        e.nameIdx   = static_cast<int16_t>(d.viaController ? -(1000 + d.ctrlIndex)
                                                           : -(2000 + d.routineIndex));
        e.category  = Category::Exit;
        e.isTransition = true;                                       // the target IS the trigger surface
        // The handle back to the WHOLE surface, for a route that needs to end on any part of it
        // rather than on the one vertex `pos` names below. See Entity::seamGroup.
        e.seamGroup = d.group;
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
                     "  %s group=%d -> \"%s\" (%u) at (%.1f,%.1f,%.1f) | %d polys, box x[%.1f..%.1f] z[%.1f..%.1f]",
                     who, d.group, n8, d.destMapId, e.pos.x, e.pos.y, e.pos.z,
                     surf->polyCount, surf->min.x, surf->max.x, surf->min.z, surf->max.z);
            Log::Write("NAV-DIAG", m);
        }
    }

    // ---- EVENT-BOUND EXITS WITH NO SURFACE (Session 122, exit_event_bind.cpp) ------------------
    // Map 572's class: the map's one transition is an event routine and the walkmap has ZERO
    // map-jump surfaces, so the loop above dropped everything and `candidates` is empty -- which is
    // this pass's admission gate. It joins each dropped group-less dest to the trigger RECT whose
    // event table names its routine (`nameOff`, the S119 join) and lists the rect's position as the
    // exit. A map that listed even one exit above never runs it: unreachable, not skipped. Runs
    // BEFORE the reachability filter and the routability table so its entries face both, same as
    // every other exit.
    AppendEventBoundExits(dests, out, haveSurfaces, candidates, dropNoGroup);

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

    // ---- THE ROUTABILITY TABLE ------------------------------------------------------------------
    // One line per exit, every scan, whether or not it is filtered: the poly it stands on, that poly's
    // EFFECTIVE flags, whether the party may walk it, and whether the flood reaches it.
    //
    // WHY THIS EXISTS (Session 96). A blind player cannot see what was hidden from them, so "is this
    // exit reachable" has to be answerable from DATA rather than from a search that failed. The tester
    // walked to an exit in Central Spur Waterway that the mod was answering "No path" for -- a listed
    // destination that would not route -- and nothing in the log could say which of the two was wrong.
    //
    // It also makes the two predicates' disagreement visible. `NavReach` floods on Walkable alone;
    // A* additionally requires EdgePassable. `reach=1` beside a route that says No path is exactly that
    // divergence, and it is the shape of a false negative.
    if (NavMesh::Ready()) {
        for (const auto& c : candidates) {
            const NavMesh::PolyId ep = NavMesh::FindPolyAt(c.pos.x, c.pos.y, c.pos.z);
            uint32_t er = 0, ee = 0;
            const bool haveFlags = (ep != NavMesh::kNoPoly) && NavMesh::PolyFlags(ep, er, ee);
            char n8[80] = {};
            for (size_t k = 0; k < c.label.size() && k < 79; ++k)
                n8[k] = (c.label[k] < 128) ? static_cast<char>(c.label[k]) : '?';
            char m[240];
            snprintf(m, sizeof(m),
                     "  routable? \"%s\" at (%.1f,%.1f,%.1f) poly=%d eff=0x%08X walk=%d reach=%d",
                     n8, c.pos.x, c.pos.y, c.pos.z, ep, haveFlags ? ee : 0u,
                     (ep != NavMesh::kNoPoly && NavMesh::Walkable(ep)) ? 1 : 0,
                     (ready && NavReach::Reachable(c.pos, kExitReachTol)) ? 1 : 0);
            Log::Write("NAV-DIAG", m);
        }
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

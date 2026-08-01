#include "navigation/exit_scan.h"
#include "navigation/map_script.h"
#include "navigation/map_names.h"
#include "navigation/nav_common.h"
#include "navigation/nav_rva.h"
#include "navigation/player_state.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/logger.h"

#include <cmath>
#include <cstdio>
#include <vector>

// AN EVENT-BOUND EXIT WITH NO SURFACE AT ALL (Session 122) -- map 572's one way out.
//
// Map 572 (Royal Palace: The Garden Stairs) closed the S121 play with `exits: controllers=1
// surfaces=0 listed=0 | nogroup=1`. Its single transition is routine[7] "ムービー開始位置" ("movie
// start position"): walk to the stairs, a cutscene fires, the party arrives on 314 (Garamsythe
// Waterway: East Spur Stairs). The routine arms NO group -- the ordinary S64 binding has no first
// half -- and the walkmap carries ZERO map-jump surfaces, so S105's elimination rule has nothing to
// pair either: that rule needs exactly one unclaimed surface, and here there are none to claim.
// `Exit=0` for a blind player on a map whose one way out is invisible.
//
// TWO BINDING SOURCES, tried in order, both from the map's own data:
//
// 1. The S119 event-table join, object-side: an object whose event table (`object+0x48`) carries
//    `ExitDest::nameOff` is wired to that routine, and its position becomes the route target.
//    ⚠ REFUTED ON 572 BY ITS OWN FALSIFIER (S123, first play of this pass): `nameOff=0x39A
//    matches 0 container-0 object(s)` on every scan of the visit. An event table names the
//    object's OWN handlers (`init|touch|touchon|SET_RECT|…`); a routine those handlers FIRE never
//    appears in it. That is S120's door lesson (`[0:57]` ran the field-sign template; the jump
//    lived inside the machinery) repeating for rects -- the join has now missed for BOTH object
//    classes it was proposed for. It stays as the first source because it is one integer compare
//    from certainty when it does hit, and because its match-count line is the measurement that
//    settles the question per map -- but expect source 2 to be the one that binds.
//
// 2. FIELD-SIGN ELIMINATION (S123, from the same play's log): the map's `+0x70` table carried the
//    answer all along -- five records, and `g0[1] pos=(85.95,32.00,61.08) usable=1 shown=1` is a
//    LIVE group-0 doorway placard at the top of the stairs that NO object claims (nearest is the
//    back door, 20.75 m -- far outside kSignMatchDist). The game draws its exit arrow from that
//    record this instant (`shown=1`) while the mod lists nothing. A `g1[2]` record sits at the
//    SAME position -- the doubled-placard pattern S105 measured at 313's event staircase. So:
//    exactly ONE unclaimed live group-0 record and exactly ONE still-unbound group-less event
//    dest ⇒ they are each other's, and the record's position is the exit. Any other count binds
//    NOTHING and prints both counts -- S105's elimination, over the field-sign table instead of
//    the walkmap. On a working map every live group-0 record is claimed by its door object (that
//    is what a group-0 record IS, S92), so the unclaimed count is 0 and the rule cannot fire.
//
// No surface, no seam group, no geometry invented. Walking to the placard IS the exit, which is
// exactly what `Entity::isTransition` already means.
//
// WHY THIS CANNOT TOUCH A WORKING MAP (the tester's rule, structural, not promised):
//   1. It runs only when `candidates` is EMPTY -- the map is about to list NOTHING. Any map that
//      lists even one exit never reaches the join at all; the block is unreachable from a working
//      map, not merely skipped (the S101 safety shape).
//   2. It binds only a dest the surface path has already DROPPED (`!viaController && group <= 0`),
//      so no existing exit can be relabelled, moved, or duplicated.
//   3. Exactly ONE container-0 object may match the routine's nameOff. Two matches, or none, bind
//      nothing and say so -- the same 1:1-or-nothing honesty as S105's elimination.
// No map id appears anywhere in this file (the global-not-per-map rule); the gate is the MECHANISM
// -- an event transition on a map whose exit list would otherwise be empty. Map 569's backlog
// (`controllers=3 surfaces=3 listed=0 | nogroup=3`) is the same family and passes the same gate.
//
// WHAT IS DELIBERATELY NOT TESTED: the rect's wake flag (`+0xC` bit 5, S120's ENTER-scene handoff
// bit). Requiring it would be a MODEL of what a movie rect must look like, and five rounds on 569
// were lost to models the census then refuted -- so the flag is LOGGED as a measurement and gates
// nothing. If a play shows a bound rect that never fires, the logged flags are the diagnosis.
//
// FALSIFIER, shipped with the fix: the binding line prints the rect's handle-table identity,
// position, flag bytes, and its distance to the nearest Door/Shop object the scan found -- the
// tester's open question ("is the movie rect the same place as door [0:8]?") is answered by the
// first log this produces. There is no CROSSING ORACLE line for this class (the oracle keys on
// map-jump groups and this exit has none), so the play itself is the oracle: route to the rect,
// the cutscene either fires or the log says exactly what was bound and why.
namespace EntityScan {

using EntityList::Category;

namespace {

// One container-0 scene object with a non-empty event table: the join's entire candidate universe.
// The offsets are read ONCE per pass, not once per dest -- the table is static while the map is
// loaded, and the pass may run once per command on a gated map.
struct EvtObj {
    void*    obj   = nullptr;
    uint8_t  table = 0xFF;      // handle table the object was found in (log identity, [table:slot])
    uint16_t slot  = 0xFFFF;
    int      n     = 0;
    uint32_t offs[16] = {};
};

// Bound on the candidate universe, and LOGGED when hit -- a silent cap reads as "covered
// everything" exactly when it didn't. 569's whole census was ~70 trigger objects; 512 is headroom,
// not a working figure.
constexpr size_t kMaxEvtObjs = 512;

// The same guarded walk BuildLocked does (entity_scan.cpp), collecting only what the join can use:
// container-0 objects that HAVE an event table. BuildLocked's own loop is play-confirmed, hot, and
// interleaves a dozen concerns -- it keeps its inline walk deliberately; this one exists because
// the movie rect is nameless and non-interactive, so THE RULE drops it from the entity list and
// the join can never find it there. Memory-only, SEH-guarded reads throughout.
void CollectContainer0EventObjects(std::vector<EvtObj>& out, bool& overflow) {
    out.clear();
    overflow = false;
    void* base = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!base) return;
    for (uint32_t c = 0; c < NavRva::HANDLE_TABLE_CONTAINERS; ++c) {
        void* table = static_cast<char*>(base) + static_cast<size_t>(c) * NavRva::HANDLE_TABLE_STRIDE;
        uint8_t active = 0;
        if (!MemRead::SafeReadU8(table, NavRva::TBL_ACTIVE_OFF, &active) || (active & 1) == 0)
            continue;
        void* entries = MemRead::PtrAt(table, NavRva::TBL_ENTRIES_OFF);
        if (!entries) continue;
        uint32_t count = 0;
        if (!MemRead::SafeReadU32(entries, NavRva::ENTRIES_COUNT_OFF, &count) || count == 0 ||
            count > 4096)
            continue;
        for (uint32_t i = 0; i < count; ++i) {
            void* obj = MemRead::PtrAt(entries, NavRva::ENTRIES_SLOT0_OFF + i * 8);
            if (!obj) continue;
            // The join is an offset compare inside ONE name pool -- the map-global script's, which
            // is what ReadExitDests parses. An object owned by another container resolves its
            // events against a different pool, where an equal integer means nothing (map_script.h).
            if (MapScript::ObjectContainerId(obj) != 0) continue;
            EvtObj rec;
            rec.n = MapScript::ObjectEventNameOffsets(obj, rec.offs, 16);
            if (rec.n <= 0) continue;
            if (out.size() >= kMaxEvtObjs) {
                overflow = true;
                Log::Write("NAV-DIAG",
                           "event-exit: candidate universe hit its 512-object cap -- later objects "
                           "were NOT considered; if a binding is missing, raise kMaxEvtObjs");
                return;
            }
            rec.obj   = obj;
            rec.table = static_cast<uint8_t>(c);
            rec.slot  = static_cast<uint16_t>(i);
            out.push_back(rec);
        }
    }
}

} // namespace

void AppendEventBoundExits(const std::vector<MapScript::ExitDest>& dests,
                           const std::vector<Entity>& scanned,
                           bool haveSurfaces,
                           std::vector<Entity>& candidates,
                           int& dropNoGroup) {
    // Gate 1: the seams for THIS map have actually been swept. Before that frame every controller
    // misses its surface and `candidates` is empty on EVERY map -- deciding then would open this
    // pass map-wide for the first frames of every visit. Same rule as BindUnclaimedSurface.
    if (!haveSurfaces) return;
    // Gate 2: the map is about to list NOTHING. One listed exit anywhere and this pass is
    // unreachable -- which is the entire never-widen guarantee, so it must stay the FIRST test
    // after the sweep gate.
    if (!candidates.empty()) return;

    // Gate 3: there is something of this class to bind. Controllers and group-armed (or
    // elimination-inferred) dests belong to the surface path, dropped or not.
    bool anyQualifying = false;
    for (const auto& d : dests)
        if (!d.viaController && d.group <= 0) { anyQualifying = true; break; }
    if (!anyQualifying) return;

    std::vector<EvtObj> evtObjs;
    bool overflow = false;
    CollectContainer0EventObjects(evtObjs, overflow);

    // Detail latch: the DECISION lines below print every pass (a bind or a refusal is about an
    // exit the player gains or does not -- same rule as the elimination binding), but the
    // measurement lines (flag bytes, nearest door) re-print only when the outcome actually
    // changes, because on a gated map this pass runs on every command.
    static int    s_detailMap = -1;
    static size_t s_detailSig = 0;
    size_t sig = evtObjs.size() * 131u;

    struct Bound { const EvtObj* rec; FVec3 pos; };
    std::vector<std::pair<int, Bound>> boundLog;   // routineIndex -> what got bound, for the detail
    // Qualifying dests source 1 could not bind, in dest order -- source 2's candidate side.
    std::vector<const MapScript::ExitDest*> unbound;

    // One construction for both sources, so the two kinds of event exit cannot drift apart.
    auto listExit = [&candidates, &dropNoGroup](const MapScript::ExitDest& d, const FVec3& pos) {
        Entity e;
        e.sceneObj = nullptr;   // fixed world pos, exactly like every other exit entry: the target
        e.fixed    = true;      // does not move, and a scene pointer here would drag this entry
        e.flags    = 0;         // through passes built for interactables.
        // Same disjoint id band the surface path gives an event-bound transition: stable per
        // routine slot, collision-free against controller exits by construction.
        e.nameIdx      = static_cast<int16_t>(-(2000 + d.routineIndex));
        e.category     = Category::Exit;
        e.isTransition = true;  // arriving at the target IS crossing it
        e.seamGroup    = 0;     // NO surface behind this exit; the seam machinery must stay out
        e.label        = std::wstring(CategoryWord(Category::Exit)) + L", " + d.destName;
        e.pos          = pos;
        candidates.push_back(e);
        --dropNoGroup;          // the inventory line reports outcomes, and this dest is now LISTED
    };

    for (const auto& d : dests) {
        if (d.viaController || d.group > 0) continue;
        // ReadExitDests already enforced the class's admission gate (real area name, not the
        // world-map teleport flags) before this dest ever existed -- not re-tested here.

        char n8[96] = {};
        for (size_t k = 0; k < d.destName.size() && k < 95; ++k)
            n8[k] = (d.destName[k] < 128) ? static_cast<char>(d.destName[k]) : '?';

        if (d.nameOff == 0) {
            char m[224];
            snprintf(m, sizeof(m),
                     "event-exit: routine[%d] \"%s\" -> dest=%u (\"%s\") has NO readable name-pool "
                     "offset -- the join has no key, nothing bound",
                     d.routineIndex, d.routineName.c_str(), d.destMapId, n8);
            Log::Write("NAV-DIAG", m);
            sig ^= static_cast<size_t>(d.routineIndex) * 2654435761u;
            // No key, no join -- but the dest is still real; source 2 may bind it by count.
            unbound.push_back(&d);
            continue;
        }

        int           matchCount = 0;
        const EvtObj* hit        = nullptr;
        const EvtObj* shows[4]   = {};
        for (const auto& e : evtObjs) {
            bool m = false;
            for (int k = 0; k < e.n && !m; ++k) m = (e.offs[k] == d.nameOff);
            if (!m) continue;
            if (matchCount < 4) shows[matchCount] = &e;
            ++matchCount;
            hit = &e;
        }
        sig ^= (static_cast<size_t>(d.routineIndex) * 2654435761u) ^
               (static_cast<size_t>(matchCount) * 97u) ^
               (reinterpret_cast<uintptr_t>(matchCount == 1 ? hit->obj : nullptr) >> 4);

        // 1:1 OR NOTHING, the S105 shape. Zero matches means the trigger is not visible in any
        // loaded event table (or the table streamed in late -- the pass re-runs and self-heals);
        // two means the map has two objects wired to one transition routine and picking one would
        // be a guess. Either way both counts are printed, because "we declined to bind" must be
        // as visible as "we bound".
        if (matchCount != 1) {
            char m[288];
            snprintf(m, sizeof(m),
                     "event-exit: routine[%d] \"%s\" -> dest=%u (\"%s\") nameOff=0x%X matches %d "
                     "container-0 object(s)%s -- not exactly one, nothing bound",
                     d.routineIndex, d.routineName.c_str(), d.destMapId, n8, d.nameOff, matchCount,
                     overflow ? " (universe CAPPED -- count untrustworthy)" : "");
            Log::Write("NAV-DIAG", m);
            for (int s = 0; s < matchCount && s < 4; ++s) {
                FVec3 sp{};
                const bool ok = PlayerState::ReadSceneObjectPos(shows[s]->obj, sp);
                char l[176];
                snprintf(l, sizeof(l), "    match %d: obj [%u:%u] evt=%d pos=(%.1f,%.1f,%.1f)%s",
                         s + 1, shows[s]->table, shows[s]->slot, shows[s]->n,
                         ok ? sp.x : 0.0f, ok ? sp.y : 0.0f, ok ? sp.z : 0.0f,
                         ok ? "" : " (unreadable)");
                Log::Write("NAV-DIAG", l);
            }
            unbound.push_back(&d);
            continue;
        }

        FVec3 pos{};
        if (!PlayerState::ReadSceneObjectPos(hit->obj, pos) ||
            (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f)) {
            char m[224];
            snprintf(m, sizeof(m),
                     "event-exit: routine[%d] \"%s\" bound to obj [%u:%u] but its position is "
                     "unreadable/unplaced -- nothing listed this scan",
                     d.routineIndex, d.routineName.c_str(), hit->table, hit->slot);
            Log::Write("NAV-DIAG", m);
            unbound.push_back(&d);
            continue;
        }

        listExit(d, pos);

        char m[320];
        snprintf(m, sizeof(m),
                 "event-exit: routine[%d] \"%s\" -> dest=%u (\"%s\") bound to trigger obj [%u:%u] "
                 "at (%.1f,%.1f,%.1f) via event-table join (nameOff=0x%X) -- EVENT transition, "
                 "route ends ON the rect",
                 d.routineIndex, d.routineName.c_str(), d.destMapId, n8, hit->table, hit->slot,
                 pos.x, pos.y, pos.z, d.nameOff);
        Log::Write("NAV-DIAG", m);
        boundLog.push_back({ d.routineIndex, Bound{ hit, pos } });
    }

    // ---- Source 2: FIELD-SIGN ELIMINATION (S123) ------------------------------------------------
    //
    // The `+0x70` table is the game's own placard registry, and a live group-0 record is a doorway
    // placard by S92's measurement (only group 0 holds press-Enter doorways; the record marks the
    // "-> area" arrow). On a correct map every live group-0 record is claimed by a door object
    // within kSignMatchDist -- that pairing is exactly what TagDoorwaysAndDropSignTwins ships on.
    // On 572 one live record is claimed by nothing: `g0[1] (85.95,32.00,61.08) usable=1 shown=1`,
    // nearest object the back door 20.75 m away -- a placard whose "door" is the EVENT rect at the
    // top of the stairs, which the entity scan can never list (nameless, non-interactive). The
    // game draws an exit arrow from that record while the mod says the room has no exits.
    //
    // So: exactly ONE unclaimed live group-0 record AND exactly ONE still-unbound group-less event
    // dest ⇒ they are each other's, and the record's position is the exit. Any other count binds
    // nothing and prints both counts -- S105's elimination, arithmetic over two of the game's own
    // lists. The claim test reuses the S92 rule verbatim (non-NPC scene object within
    // kSignMatchDist), so this pass and the doorway tagger cannot disagree about what "claimed"
    // means. Early in a visit, before the handle table streams in, even the BACK door's record
    // reads unclaimed -- the count is then 2, and the rule declines until the map is actually
    // loaded. Fail closed, self-healing.
    //
    // KNOWN LIMIT, printed rather than hidden: the dest-side count is a LOWER BOUND while the 0x40
    // scan hole stands (572 reads `spans read=9, unreadable=6` -- six routines this reader never
    // read). A hidden second dest would make 1:1 a coincidence; the falsifier is the CROSSING
    // itself, and the decline path already handles the day the hole is fixed and a second dest
    // appears.
    if (!unbound.empty()) {
        const std::vector<MapExits::SignRec>& signs = CachedSigns();
        int                      unclaimedCount = 0;
        const MapExits::SignRec* rec            = nullptr;
        for (const auto& s : signs) {
            if (s.group != kSignDoorwayGroup) continue;
            if (s.pos.x == 0.0f && s.pos.y == 0.0f && s.pos.z == 0.0f) continue;   // unused slot
            bool claimed = false;
            for (const auto& o : scanned) {
                if (!o.sceneObj || o.category == Category::NPC) continue;
                if (NavCommon::Distance2D(s.pos, o.pos) <= kSignMatchDist) { claimed = true; break; }
            }
            if (!claimed) { ++unclaimedCount; rec = &s; }
        }
        sig ^= static_cast<size_t>(unclaimedCount) * 8191u + unbound.size() * 127u;

        if (unbound.size() != 1 || unclaimedCount != 1) {
            char m[224];
            snprintf(m, sizeof(m),
                     "field-sign elimination: %d unclaimed live g0 record(s) vs %zu unbound event "
                     "dest(s) -- not 1:1, nothing bound",
                     unclaimedCount, unbound.size());
            Log::Write("NAV-DIAG", m);
        } else {
            const MapScript::ExitDest& d = *unbound[0];
            listExit(d, rec->pos);

            char n8[96] = {};
            for (size_t k = 0; k < d.destName.size() && k < 95; ++k)
                n8[k] = (d.destName[k] < 128) ? static_cast<char>(d.destName[k]) : '?';
            char m[352];
            snprintf(m, sizeof(m),
                     "field-sign elimination: the ONE unclaimed live g0 record [%d] at "
                     "(%.2f,%.2f,%.2f) usable=%d shown=%d is routine[%d] \"%s\"'s -> dest=%u "
                     "(\"%s\") -- INFERRED, position from the map's own placard",
                     rec->index, rec->pos.x, rec->pos.y, rec->pos.z, rec->usable ? 1 : 0,
                     rec->shown ? 1 : 0, d.routineIndex, d.routineName.c_str(), d.destMapId, n8);
            Log::Write("NAV-DIAG", m);

            // The 313 corroboration, measured here too: S105 found the event staircase carrying a
            // doubled placard (a live record in a HIGHER group at the same spot). Log-only.
            for (const auto& s : signs) {
                if (&s == rec || s.group == kSignDoorwayGroup) continue;
                if (NavCommon::Distance2D(s.pos, rec->pos) > 0.25f ||
                    std::fabs(s.pos.y - rec->pos.y) > 0.25f) continue;
                snprintf(m, sizeof(m),
                         "  corroboration: g%d[%d] sits at the same position -- the doubled-placard "
                         "pattern S105 measured at 313's event staircase",
                         s.group, s.index);
                Log::Write("NAV-DIAG", m);
                break;
            }
        }
    }

    const int mapId = MapNames::CurrentMapId();
    if (mapId == s_detailMap && sig == s_detailSig) return;
    s_detailMap = mapId;
    s_detailSig = sig;

    // The measurements, once per outcome change: the rect's own flag bytes (wake = `+0xC` bit 5,
    // S120's ENTER-scene handoff bit -- logged, never gated on), and the tester's open question --
    // how far the rect stands from the nearest Door/Shop object the scan admitted. On 572 that is
    // door [0:8] "Door": same place or not is decided by this line, not by a design assumption.
    for (const auto& b : boundLog) {
        uint8_t f8 = 0, fB = 0, fC = 0;
        MemRead::SafeReadU8(b.second.rec->obj, 0x08, &f8);
        MemRead::SafeReadU8(b.second.rec->obj, 0x0B, &fB);
        MemRead::SafeReadU8(b.second.rec->obj, 0x0C, &fC);
        char m[200];
        snprintf(m, sizeof(m),
                 "  event-exit rect [%u:%u]: f8=0x%02X fB=0x%02X fC=0x%02X wake=%d evt=%d",
                 b.second.rec->table, b.second.rec->slot, f8, fB, fC, (fC & 0x20) ? 1 : 0,
                 b.second.rec->n);
        Log::Write("NAV-DIAG", m);

        const Entity* door  = nullptr;
        float         doorD = 0.0f;
        for (const auto& s : scanned) {
            if (s.category != Category::Door && s.category != Category::Shop) continue;
            const float dd = NavCommon::Distance2D(s.pos, b.second.pos);
            if (!door || dd < doorD) { door = &s; doorD = dd; }
        }
        if (door) {
            char n8[64] = {};
            for (size_t k = 0; k < door->label.size() && k < 63; ++k)
                n8[k] = (door->label[k] < 128) ? static_cast<char>(door->label[k]) : '?';
            snprintf(m, sizeof(m),
                     "  event-exit rect [%u:%u]: nearest Door/Shop is \"%s\" [%u:%u] at "
                     "(%.1f,%.1f,%.1f), %.2fm from the rect",
                     b.second.rec->table, b.second.rec->slot, n8, door->container, door->slot,
                     door->pos.x, door->pos.y, door->pos.z, doorD);
            Log::Write("NAV-DIAG", m);
        } else {
            snprintf(m, sizeof(m),
                     "  event-exit rect [%u:%u]: no Door/Shop object in this scan to measure against",
                     b.second.rec->table, b.second.rec->slot);
            Log::Write("NAV-DIAG", m);
        }
    }
}

} // namespace EntityScan

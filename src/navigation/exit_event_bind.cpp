#include "navigation/exit_scan.h"
#include "navigation/map_script.h"
#include "navigation/map_names.h"
#include "navigation/nav_common.h"
#include "navigation/nav_rva.h"
#include "navigation/player_state.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/logger.h"

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
// WHAT BINDS IT INSTEAD: the S119 event-table join, object-side. The rect the player walks into to
// fire the event carries that routine in its OWN event table (`object+0x48`, entries are name-pool
// offsets), and `ExitDest::nameOff` is the same routine's offset in the same pool. Offset == offset
// is the map's own data saying "entering this object's volume runs that transition routine" -- the
// join that already binds doors to transition routines in entity_postscan.cpp, and the first
// object<->routine binding this project ever measured (S119). The rect's POSITION then becomes the
// route target: no surface, no seam group, no geometry invented. Walking into the rect IS the exit,
// which is exactly what `Entity::isTransition` already means.
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
            continue;
        }

        Entity e;
        e.sceneObj = nullptr;   // fixed world pos, exactly like every other exit entry: the rect
        e.fixed    = true;      // does not move, and a scene pointer here would drag this entry
        e.flags    = 0;         // through passes built for interactables.
        // Same disjoint id band the surface path gives an event-bound transition: stable per
        // routine slot, collision-free against controller exits by construction.
        e.nameIdx      = static_cast<int16_t>(-(2000 + d.routineIndex));
        e.category     = Category::Exit;
        e.isTransition = true;  // arriving at the rect IS crossing it -- the trigger fires on entry
        e.seamGroup    = 0;     // NO surface behind this exit; the seam machinery must stay out of it
        e.label        = std::wstring(CategoryWord(Category::Exit)) + L", " + d.destName;
        e.pos          = pos;
        candidates.push_back(e);
        --dropNoGroup;          // the inventory line reports outcomes, and this dest is now LISTED

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

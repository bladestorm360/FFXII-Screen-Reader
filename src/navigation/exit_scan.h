#pragma once

#include <vector>

#include "navigation/entity_scan.h"   // Entity
#include "navigation/map_exits.h"     // SignRec
#include "navigation/map_script.h"    // ExitDest (the event-exit join's input)

// The map's TRANSITIONS — the doorways that move the party to another area. Split out of
// entity_scan.cpp when that file passed the project's 500-line ceiling: that file walks the scene-object
// handle table, and a transition is not a scene object at all. It is assembled from two engine tables
// that DISAGREE about which doorways exist, which is the whole reason this is its own unit now.
namespace EntityScan {

// The map's `+0x70` field-sign records, read once per map and cached. Shared with the object scanner,
// which uses the same records to tell a doorway from a decorative sign of the same name. Caller holds
// the entity-list mutex.
const std::vector<MapExits::SignRec>& CachedSigns();

// Append this map's exits to `out` as fixed-position Category::Exit entities. Caller holds the mutex.
void ScanExits(std::vector<Entity>& out);

// AN EVENT-BOUND EXIT WITH NO SURFACE (S122/S123, exit_event_bind.cpp) -- map 572's class: the
// map's one transition is an event routine (`mapjump`, no group armed) and the walkmap carries no
// map-jump surfaces, so both S64's binding and S105's elimination have nothing to pair. Two
// binding sources, in order: (1) the S119 event-table join (`ExitDest::nameOff` == an entry of a
// container-0 object's event table -- REFUTED on 572, 0 matches: event tables name an object's
// own handlers, not the routines they fire; kept as the per-map measurement); (2) FIELD-SIGN
// ELIMINATION -- exactly one unclaimed live group-0 `+0x70` record and exactly one unbound event
// dest pair up, and the record's position (the game's own exit placard) is the route target.
//
// Reachable ONLY when `candidates` is empty -- a map that lists any exit never runs either source
// -- and both bind 1:1 or nothing. `scanned` is the object list ScanExits was handed: the sign
// claim test runs against it (the S92 rule, kSignMatchDist), and the bound target is measured
// against its nearest Door/Shop for the log. Decrements `dropNoGroup` for each dest it lists, so
// the exit inventory reports outcomes, not intermediate states. Caller holds the mutex.
void AppendEventBoundExits(const std::vector<MapScript::ExitDest>& dests,
                           const std::vector<Entity>& scanned,
                           bool haveSurfaces,
                           std::vector<Entity>& candidates,
                           int& dropNoGroup);

// WHAT THIS MAP CLAIMED A SEAM LEADS TO -- the group -> destination binding, kept per map so it can
// still be read after the map has changed.
//
// This exists for the CROSSING ORACLE in nav_trace. The tester reports exits that are swapped as well
// as exits that are missing, and neither can be diagnosed from the mod's own output today, because the
// mod only ever prints what it BELIEVES. The oracle prints belief next to outcome: the player walks
// onto a seam, the game loads a map, and if that map is not the one this table names for that seam,
// the binding is wrong and there is nothing left to argue about. The script the claim came from is
// gone by then -- it lives in the map that just unloaded -- so the answer has to be cached while the
// map is still up. Returns false when this map never published a claim for that group.
bool ClaimedDestForGroup(int mapId, int group, uint16_t& destMapId);

} // namespace EntityScan

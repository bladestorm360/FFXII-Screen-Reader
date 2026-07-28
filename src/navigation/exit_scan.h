#pragma once

#include <vector>

#include "navigation/entity_scan.h"   // Entity
#include "navigation/map_exits.h"     // SignRec

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

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

} // namespace EntityScan

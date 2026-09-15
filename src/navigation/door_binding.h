#pragma once

#include <vector>
#include "navigation/entity_scan.h"

// DOORS BY WHAT THE MAP SCRIPT SAYS THE OBJECT DOES -- Session 179.
//
// The user's rule: an interactable with destination data is a Door, period. Before this, `Door` came
// only from geometry -- nearest `+0x70` field-sign arrow, or a template sibling of such an object --
// so a door whose arrow sat further away, or a door with no arrow at all, listed as Interactables.
//
// Two promotions, both keyed on the object's OWN routine (pointer identity, map_script_routines.h):
//
//  1. DESTINATION. The routine calls `mapjump` to a real place (ReadExitDests' own acceptance: not
//     the world-map teleport menu, a real area name). The Acolyte's Burden's three Ancient Doors are
//     `big_door_01..03`, each jumping to a named map.
//  2. IN-MAP DOOR. The routine OPENS a closed floor id (`setmapidfloor(N, 0, 1)`) and floor carrying
//     that id lies within reach of the object itself. Mirror of the Soul's `gim_door01..04` have no
//     destination -- they open the corridor they stand in. The PROXIMITY half is what keeps a remote
//     switch, or the NPC who opens a gate somewhere else, out of Doors: a census of the 769 map
//     scripts found both of those calling the same native. Known residual risk, logged on every
//     promotion: a lift or cart that opens its own platform would pass both tests.
//
// ONLY EVER REFINES `Category::Object`, the bucket for "recognised nothing". A positive identification
// (Save Crystal, Treasure, Shop, NPC...) is never overwritten. Runs on every map, with or without a
// `+0x70` table -- TagDoorwaysAndDropSignTwins returns early on maps that have none.
namespace DoorBinding {

// Caller holds the entity-list lock (it is part of EntityScan::Build).
void PromoteDoors(std::vector<EntityScan::Entity>& out);

} // namespace DoorBinding

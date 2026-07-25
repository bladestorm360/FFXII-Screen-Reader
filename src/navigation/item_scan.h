#pragma once

#include <vector>

#include "navigation/entity_scan.h"   // Entity

// Ground loot — the items an enemy leaves behind when it dies.
//
// These are NOT in the scene-object handle table entity_scan.cpp walks, which is why they were
// invisible to the pathfinder: the engine keeps dropped loot in its own 10-slot global pool
// (DAT_02ec0fa0), with world positions in a parallel marker table (DAT_022be7f0). A drop is
// therefore closer to an exit than to a field object — fixed position, no scene node — so this
// module is modelled on exit_scan.cpp.
//
// Two threads, two jobs, and the split is the whole design:
//   * The HOOKS run on the game thread. They are the only place an item id may be turned into a
//     name, because that is a game call (core/item_names.h). They render the label once, into a
//     cache, exactly as the combat log renders its text at append time.
//   * ScanDrops() is pure memory reads and runs on whichever thread rebuilt the list — the input
//     thread for a keypress, the game thread for the field tick. It never resolves a name; it only
//     reads the cache.
namespace ItemScan {

// Installs the three loot-pool hooks (spawn / collected / discarded). Safe to call when they fail:
// the category simply stays empty rather than showing stale drops.
bool Init();
void Shutdown();

// Consume the "the loot pool changed" edge, set by any of the three hooks. Returns true at most
// once per change. This is what lets a drop appear in the list without the player pressing rescan.
bool TakeDirty();

// Append every live ground drop to `out` as fixed-position Category::Items entities.
// Caller holds the entity-list mutex.
void ScanDrops(std::vector<EntityScan::Entity>& out);

} // namespace ItemScan

#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "navigation/entity_list.h"
#include "navigation/entity_scan.h"
#include "navigation/nav_types.h"

// The list module's PRIVATE state and helpers, shared between `entity_list.cpp` (which owns the list
// and the per-frame tick) and `entity_commands.cpp` (which owns the hotkey commands over it). Split
// when entity_list.cpp passed the project's 500-line ceiling; the two halves are genuinely different
// jobs -- maintaining a set, versus driving a cursor across it -- but they cannot be separated without
// sharing the state, so it is declared here rather than duplicated.
//
// Not a public interface: `entity_list.h` is. Nothing outside those two translation units includes this.
// Every function here expects the caller to already hold `g_mutex`, except where noted.
namespace EntityList {
namespace Internal {

using EntityScan::Entity;

// The [ / ] focus. Tracked across rescans by the OBJECT, never by its spoken label -- see CursorMatch
// in entity_list.cpp for what that cost when it was otherwise.
struct CursorId {
    void*        obj     = nullptr;
    int16_t      nameIdx = 0;
    std::wstring label;
    Category     cat     = Category::All;
    bool         valid   = false;
};

extern std::mutex          g_mutex;
extern std::vector<Entity> g_entities;
extern Category            g_currentCategory;
extern Availability        g_availability;
extern CursorId            g_cursor;

// Rebuild the list, keeping anything that has only just stopped being reported (see kEntityGraceMs).
int  RescanLocked();

// Re-read live positions and ranges for the current set.
void RefreshPositionsLocked(const FVec3& playerPos);

// Does this entity pass the current category + availability filters?
bool PassesFiltersLocked(const Entity& e);

// Indices into g_entities that pass the category + availability filters, nearest first.
std::vector<size_t> FilteredSortedLocked();

// The player's world position. Lock-free; false when the field is not live.
bool ReadPlayer(FVec3& pos);

// 2 = this IS the focused object, 1 = an equivalent replacement, 0 = no match.
int  CursorMatch(const Entity& e);

// Index within `view` of the focused entity, or -1. Prefers an exact match over a re-lock.
int  FindFocusInViewLocked(const std::vector<size_t>& view);

void SetFocusLocked(const Entity& e);
void ClearFocusLocked();

// Speak an entity: its label, then bearing + distance in the shipped relative frame.
void SpeakEntityLocked(const Entity& e, const FVec3& playerPos);
void SpeakNoTargets();

} // namespace Internal
} // namespace EntityList

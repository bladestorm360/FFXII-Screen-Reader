#pragma once

#include <cstdint>
#include "navigation/nav_types.h"

// PLACES THE PLAYER PHYSICALLY FAILED TO GET PAST.
//
// Not a guess about the map, and not a learned label: every entry is an OBSERVATION of the live world,
// recorded only when the navigation stuck detector sees the player holding a movement key and making
// no progress along the leg they were told to walk. The map data is never consulted to create one.
//
// WHY IT EXISTS (Session 96). `PathSearch`'s ban list is scoped to a single request, deliberately --
// "the obstacle may be a door that opens". But that also means the very next `\` offered the same
// impassable way again, which is exactly what the tester reported: *"often when the pathfinder does
// get unblocked, it routes back to the blocked section."* Nothing in the mod remembered that they had
// just proved a route unwalkable with their own feet.
//
// AND THE MEMORY MATTERS MORE THAN THE AUTOMATIC RE-PLAN. The tester's own observation was that they
// will press `\` long before any timer fires -- so the value here is not that the mod silently re-plans
// (it does), it is that their next manual route request already knows.
//
// IT MUST EXPIRE, and that is what keeps it a measurement rather than the permanent label this project
// forbids. Entries die on a map change (epoch) and after kTtlMs. A door that opens, a story flag that
// flips, a platform that moves -- all get retried.
//
// GAME THREAD ONLY, like the rest of navigation.
namespace NavBlocked {

// How long a measured obstruction is believed. Long enough to survive a few route presses and a walk
// back round, short enough that a gate opening is retried within a minute or so.
constexpr uint64_t kTtlMs = 90000;   // 90 s

// A poly centroid within this of a recorded spot is refused. Generous on purpose: the player stops one
// body radius from whatever stopped them, and the corridor they could not pass is wider than the exact
// point where they jammed.
constexpr float kRadius = 3.0f;

// Record an obstruction at the player's position. Refreshes rather than duplicates a nearby entry,
// and prunes stale/foreign-map ones as it goes, so the set never needs a tick of its own.
void Note(const FVec3& where, uint32_t epoch);

// Drop everything. Called on map teardown: these are coordinates on the map being torn down.
void Clear();

// Is anything recorded at all? The O(1) check A* uses once per request so the common case costs
// nothing per edge.
bool Any();

// Does `c` fall inside a live recorded obstruction for this map? A* consults this exactly like its own
// ban list, so a measured blockage is indistinguishable from a wall to the search -- which is the
// point: it routes AROUND rather than failing.
bool Contains(const FVec3& c, uint32_t epoch, uint64_t nowMs);

} // namespace NavBlocked

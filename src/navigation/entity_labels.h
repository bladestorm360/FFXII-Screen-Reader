#pragma once

#include "navigation/nav_types.h"   // FVec3

#include <cstdint>
#include <string>

// PLAYER LABELS and STABLE NUMBERS for field entities.
//
// Two problems, one store:
//
//  * **The player's own words.** The game leaves a lot of things anonymous -- fifteen townsfolk all
//    called "Rabanastran", signs with no name at all -- and the one that opens the east gate is not
//    distinguishable from the other fourteen. The player names it "gate guard" and it stays named.
//  * **A number that does not move.** Duplicate labels get a " 1" / " 2" suffix. That suffix used to be
//    an ordinal within whatever set the last scan happened to see, and the handle table streams objects
//    in and out (measured: NPC=14 <-> 15 across 118 rescans on one map), so everyone was renumbered
//    constantly. Assign each object a number ONCE and keep it.
//
// **This is not the Session 62 mistake.** That store had the mod DISCOVER game facts by playing and then
// present them as truth. This one holds the player's own text, and which ordinal we already gave an
// object -- presentation state we authored, never a claim about the game. Every game fact (position,
// name, category, availability) is still read fresh from the handle table on every scan.
//
// IDENTITY (Session 79): `mapId . baseLabel . nameIdx`, disambiguated by POSITION when that triple is
// not unique on the map.
//
// **STRUCK: `mapId . container . slot`.** The slot is assigned at map load in script order, so it is not
// stable across loads -- a re-slotted object looked new and took a fresh number, which is exactly the
// drift this store exists to prevent (map 243 held five Nomads numbered 1, 3, 5, 6, 7). `nameIdx` is the
// object's npcdic id and comes from the map's own data, identical every load.
//
// **The position anchor is not optional.** Every object the game leaves anonymous carries `nameIdx = -1`
// and falls back to one category word, so `{map, "NPC", -1}` is the SAME key for all of them. Without a
// position tie-breaker they would collide onto one record -- and since the include-by-KIND widening in
// entity_scan.cpp admits precisely those objects, the fix would break the people it just added.
//
// Position is a tie-breaker, never the primary key: an object with a unique `{map, label, nameIdx}` is
// matched on that alone, so a WANDERING NPC with an id of its own stays stable no matter where it walks.
// Only when the triple repeats does the nearest anchor decide, which is stable for stationary NPCs and
// admittedly is not for a roaming group that also shares one id (the six `nameIdx=238` Cockatrices).
// They have no distinguishing identity in the game's own data and inventing one would be a fabrication.
//
// Not thread-safe by design: every caller is the input thread holding the entity-list mutex.
namespace EntityLabels {

// Load the store from disk. Safe to call repeatedly; failure is silent (an empty store still works).
void Init();

// Start a matching pass. Records already handed out during this pass are not handed out again, so two
// live objects sharing a key can never collapse onto one record (and one number). Call once per pass.
void BeginScan();

// The player's label for this entity, or empty when they have not named it. Overrides EVERYTHING --
// game name, duplicate number, category word.
std::wstring LabelFor(int mapId, int16_t nameIdx, const std::wstring& baseLabel, const FVec3& pos);

// Assign (or, with an empty string, clear) the player's label. Persists immediately: a crash must not
// cost the player the naming work they just did. `container`/`slot` are stored for diagnostics only --
// they are NOT part of the identity (see the strike above).
void SetLabel(int mapId, int16_t nameIdx, const std::wstring& baseLabel, const FVec3& pos,
              uint8_t container, uint16_t slot, const std::wstring& label);

// The duplicate-suffix number for this entity, assigning the lowest not yet used under `baseLabel` on
// this map if it has none. Stable across rescans, streaming, reloads and sessions.
int NumberFor(int mapId, int16_t nameIdx, const std::wstring& baseLabel, const FVec3& pos,
              uint8_t container, uint16_t slot);

// Drop the in-memory view and reload from disk. Called on map change so a hand-edit of the file takes
// effect without restarting the game.
void Reload();

} // namespace EntityLabels

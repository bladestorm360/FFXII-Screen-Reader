#pragma once

#include "navigation/nav_types.h"   // FVec3

#include <cstdint>
#include <string>

// PLAYER LABELS for field entities. **Only** the player's own words.
//
// The game leaves a lot of things anonymous -- fifteen townsfolk all called "Rabanastran", signs with
// no name at all -- and the one that opens the east gate is not distinguishable from the other
// fourteen. The player names it "gate guard" and it stays named.
//
// **NUMBERS USED TO LIVE HERE TOO, AND THAT WAS THE BUG (Session 81).** The duplicate " 1" / " 2"
// suffix was persisted so it would never move. For stationary objects that worked; for anything that
// ROAMS it was unbounded growth, because a record's anchor is deliberately never refreshed -- so a
// moving object outran its own record and `NumberFor` minted a new one, with the free-number search
// counting every leaked record as taken, so the number could only climb. Measured on the live store:
// **39 records labelled "Cockatrice", numbered 1..39, for six real animals**, and the tester heard
// "Cockatrice 37". Numbering now happens within a single scan, in EntityScan::NumberDuplicateLabels,
// and is never written down. A store cannot leak numbers it does not hold.
//
// **This is not the Session 62 mistake.** That store had the mod DISCOVER game facts by playing and then
// present them as truth. This one holds nothing but text the player typed. Every game fact (position,
// name, category, availability) is still read fresh from the handle table on every scan.
//
// IDENTITY (Session 79): `mapId . baseLabel . nameIdx`, disambiguated by POSITION when that triple is
// not unique on the map.
//
// **STRUCK as the PERSISTENT key: `mapId . container . slot`.** The slot is assigned at map load in
// script order, so it is not stable across loads -- a re-slotted object looked new and took a fresh
// number. `nameIdx` is the object's npcdic id, from the map's own data, identical every load.
//
// That strike is about PERSISTENCE and does NOT forbid within-scan numbering on the same pair.
// `{container, slot}` is the engine's own name for a handle-table object -- the interaction scorer
// writes exactly that pair into the globals the confirm handler dereferences to act on what the player
// is facing -- and it is perfectly stable for as long as the map is loaded. Unstable across loads is
// why it may not be stored; stable while loaded is why it is the right key for a number recomputed
// every scan.
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
// The roaming limitation above now costs only a player LABEL on a roamer. It can no longer leak
// records, because the roaming case is exactly what stopped being written to this file.
//
// KNOWN CONSEQUENCE of the odd-slot name read: `baseLabel` is part of the key, so a label filed under
// `{map, "Nomad", 223}` will not match once that object resolves to "Dania". Since Session 81 the name
// no longer flips DURING play -- the personal name is preferred from the first scan rather than from
// the story beat that introduces the character -- so this is a one-time consequence of upgrading, and
// the format bump discards the file anyway. If player labels ever accumulate, the fix is to re-home a
// record when its `nameIdx` matches and only the words moved, never to stop reading the name the game
// is showing.
//
// Not thread-safe by design: every caller is the input thread holding the entity-list mutex.
namespace EntityLabels {

// Load the store from disk. Safe to call repeatedly; failure is silent (an empty store still works).
void Init();

// Start a matching pass. Records already handed out during this pass are not handed out again, so two
// live objects sharing a key can never collapse onto one record -- which would put one person's chosen
// name onto a body they never named. Call once per pass.
void BeginScan();

// The player's label for this entity, or empty when they have not named it. Overrides EVERYTHING --
// game name, duplicate number, category word.
std::wstring LabelFor(int mapId, int16_t nameIdx, const std::wstring& baseLabel, const FVec3& pos);

// Assign (or, with an empty string, clear) the player's label. Persists immediately: a crash must not
// cost the player the naming work they just did. `container`/`slot` are stored for diagnostics only --
// they are NOT part of the identity (see the strike above).
void SetLabel(int mapId, int16_t nameIdx, const std::wstring& baseLabel, const FVec3& pos,
              uint8_t container, uint16_t slot, const std::wstring& label);

// Drop the in-memory view and reload from disk. Called on map change so a hand-edit of the file takes
// effect without restarting the game.
void Reload();

} // namespace EntityLabels

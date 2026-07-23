#pragma once

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
// IDENTITY: `mapId . container . slot`, the object's place in the scene-object handle table, which is
// assigned at map load in script order. `nameIdx` is stored alongside as a VALIDATION field: if the key
// resolves but the npcdic id has changed, the slot has been reused and the entry is stale -- that is
// logged and ignored rather than mislabelling a stranger.
//
// Not thread-safe by design: every caller is the input thread holding the entity-list mutex.
namespace EntityLabels {

// Load the store from disk. Safe to call repeatedly; failure is silent (an empty store still works).
void Init();

// The player's label for this entity, or empty when they have not named it. Overrides EVERYTHING --
// game name, duplicate number, category word.
std::wstring LabelFor(int mapId, uint8_t container, uint16_t slot, int16_t nameIdx);

// Assign (or, with an empty string, clear) the player's label. Persists immediately: a crash must not
// cost the player the naming work they just did.
void SetLabel(int mapId, uint8_t container, uint16_t slot, int16_t nameIdx, const std::wstring& label);

// The duplicate-suffix number for this entity, assigning the lowest not yet used under `baseLabel` on
// this map if it has none. Stable across rescans, streaming, reloads and sessions.
int NumberFor(int mapId, uint8_t container, uint16_t slot, int16_t nameIdx,
              const std::wstring& baseLabel);

// Drop the in-memory view and reload from disk. Called on map change so a hand-edit of the file takes
// effect without restarting the game.
void Reload();

} // namespace EntityLabels

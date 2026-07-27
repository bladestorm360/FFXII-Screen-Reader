#pragma once

#include <cstdint>
#include <string>

#include "navigation/entity_list.h"   // Category

// What a field object IS and what it is CALLED — the per-object judgement layer, split out of
// entity_scan.cpp when that file crossed the 500-line limit. Finding objects (the handle-table walk,
// the combatant pool, the exit table) and deciding what each one is are separate jobs; only the
// second one is here, and none of it needs the Entity record, so this header stays free of it.
//
// Everything is a memory-only, SEH-guarded read of the game's own data. The engine's own functions
// are REPLICATED, never called: `ResolveObjectName` mirrors FUN_00263990, `IsInteractionAvailable`
// mirrors FUN_002675c0. Every spoken LABEL is the game's own text; the ids and kind nibbles here
// only drive the category FILTER.
namespace EntityScan {

// Category -> the word used as a label fallback and for the category announcement.
const wchar_t* CategoryWord(EntityList::Category c);

// The game's own display name for a field object, read memory-only from its scene object. Empty
// when unresolvable -- callers fall back to a category word rather than inventing one.
//
// It resolves to the PERSONAL name whenever the npcdic carries a distinct one: slot id*2+1 beats
// id*2, so a "Nomad" reads "Dania". Not gated on whether the game has introduced the character --
// the name is in the map's own dictionary either way, and speaking it removes five invented numbers
// per map. See the definition for why the pointer pre-check is equivalent to comparing the strings.
std::wstring ResolveObjectName(void* sceneObj);

// Has the player been introduced to this npcdic character? A live per-id bit in the game's own state
// block (replica of FUN_0032a930, written by the `settalknpcname` / `releasetalknpcname` natives).
//
// DIAGNOSTIC ONLY since Session 81 -- it no longer chooses the slot the mod speaks. Its one caller is
// the `inclusion:` tally, where it reports how many of the personal names we speak belong to
// characters the player has actually met.
bool TalkNameKnown(int id);

// How many objects spoke the ODD npcdic slot during the last scan -- the names this build reveals
// that earlier ones did not. Reset at the top of every scan.
void ResetNameStats();
int  OddSlotWins();

// True when an npcdic name key falls in the field gimmick-object band (433-469).
bool InGimmickBand(int16_t nameIdx);

// Category from the npcdic id band + the engine's own object KIND. `flags` is accepted but
// deliberately unused -- see the definition for why classifying on it was wrong.
EntityList::Category ClassifyByNameKey(uint32_t flags, int16_t nameIdx, bool isCharacter, uint8_t kind);

// "Would the game let the player interact with this object right now?" -- a memory-only replica of
// the engine's own predicate FUN_002675c0. False means story-gated / not yet usable; the object is
// still listed and still routable, so the player can walk to it and find whoever gates it.
bool IsInteractionAvailable(void* sceneObj, uint8_t kind, uint32_t flags);

} // namespace EntityScan

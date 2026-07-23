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
std::wstring ResolveObjectName(void* sceneObj);

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

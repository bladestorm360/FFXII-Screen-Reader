#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "navigation/entity_list.h"   // Category
#include "navigation/nav_types.h"

// Builds the list of live interactive field objects: the handle-table walk, the combatant pool, the
// map exits, and the npcdic name/category classification that labels them.
//
// Split out of entity_list.cpp (883 lines). Finding things and TRACKING a cursor over them are
// different jobs; they were fused only because the scanner wrote the module's file-scope vector
// directly. Build() takes its destination by reference instead, so this module holds no state and
// the caller keeps ownership of the lock.
//
// Pure, SEH-guarded memory reads. Every LABEL is the game's own text (npcdic, or the map's own
// fieldsignmes string); the npcdic id bands only drive the category FILTER, never the spoken words.
namespace EntityScan {

// A live interactive field object. Identity is the SCENE OBJECT pointer, which is stable
// while the map is loaded (the handle table holds it from load to teardown).
struct Entity {
    void*        sceneObj = nullptr;   // handle-table scene object (identity; nullptr for fixed exits)
    uint32_t     flags    = 0;         // *(sceneObj+0x1C): FLAG_TALK (NPC) / FLAG_ACTION
    int16_t      nameIdx  = 0;         // *(sceneObj+0x102): npcdic id (>=0) / custom (<0)
    EntityList::Category category = EntityList::Category::Object;
    std::wstring label;
    FVec3        pos;
    float        dist2D   = 0.0f;      // to player, refreshed per command
    bool         fixed    = false;     // exit/map-jump: fixed world pos, no scene node (don't refresh via +0xB8)
    bool         noBearing = false;    // pos is NOT world-space (connection-DB exits carry map-atlas offsets)
                                       // -> speak the label only, never a fabricated direction
};
// Sanity bound on an exit's distance from the player -- rejects garbage positions. Shared with the
// per-area diagnostic dump so both use one number.
constexpr float kExitMaxDist = 2000.0f;

// Rebuild `out` from scratch: handle-table objects, then combatants, then map exits. Returns the
// count; empty and 0 when the field isn't active. The caller holds its own list lock.
int Build(std::vector<Entity>& out);

// Bitmask of currently-active handle-table containers (bit c set iff container c is active).
// Memory-only; touches only the handle table, so the per-frame tick can detect a container-set
// change without paying for a full rescan.
uint32_t ActiveContainerMask();

// The game's own display name for a field object, read memory-only from its scene object. Empty
// when unresolvable -- callers fall back to a category word rather than inventing one.
std::wstring ResolveObjectName(void* sceneObj);

// True when an npcdic name key falls in the field gimmick-object band (433-469).
bool InGimmickBand(int16_t nameIdx);

// Category -> the word used as a label fallback and for the category announcement.
const wchar_t* CategoryWord(EntityList::Category c);

} // namespace EntityScan

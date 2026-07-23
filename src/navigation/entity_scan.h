#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "navigation/entity_list.h"      // Category
#include "navigation/entity_classify.h"  // ResolveObjectName / InGimmickBand / CategoryWord / …
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
    uint32_t     flags    = 0;         // *(sceneObj+0x1C): FLAG_TALK / FLAG_ACTION -- MODE, not identity
    int16_t      nameIdx  = 0;         // *(sceneObj+0x102): npcdic id (>=0) / custom (<0)
    uint8_t      kind     = 0;         // *(sceneObj+0x0E) & 0xF: the engine's object kind (1 person, 5 gimmick)
    EntityList::Category category = EntityList::Category::Object;
    std::wstring label;
    FVec3        pos;
    float        dist2D   = 0.0f;      // to player, refreshed per command
    bool         fixed    = false;     // exit/map-jump: fixed world pos, no scene node (don't refresh via +0xB8)
    bool         noBearing = false;    // pos is NOT world-space (connection-DB exits carry map-atlas offsets)
                                       // -> speak the label only, never a fabricated direction
    bool         available = true;     // the game would let the player interact with it RIGHT NOW
                                       // (memory-only replica of FUN_002675c0). False = story-gated /
                                       // not yet usable. Non-interaction entries (exits, combatants)
                                       // are always true so no filter can hide them.
    // A `+0x70` field-sign record sits on this object, i.e. the map script bound it to a location jump
    // (`setfieldsignlocationjumpinfo`) -- it is a DOORWAY, not a decorative sign. The engine treats both
    // as action targets with identical flags, so this is the only sound way to tell them apart.
    bool         doorway   = false;
    // `label` is the game's own string (npcdic or the map's fieldsignmes text), not the category-word
    // fallback. The sign-twin drop compares labels, and an unnamed object whose label is merely the word
    // "Interactables" must never match another unnamed object.
    bool         gameNamed = false;
    // This entry is a map-jump TRANSITION whose position is the trigger surface itself (exit_scan.cpp),
    // so arriving at it IS crossing it. The planner uses this to say "At the exit" instead of grinding
    // out two-metre legs when the player is already standing on the seam.
    //
    // (It replaces a `crossRad` heading that was spoken as "walk east" in S59 and refuted in play. There
    // is no crossing direction to derive any more: the route ends ON the trigger.)
    bool         isTransition = false;
    // Where the object sits in the scene-object handle table, and the interaction payload ids the engine
    // would run. Diagnostics only -- these are what identify an object the game gives no name to.
    uint8_t      container = 0xFF;
    uint16_t     slot      = 0xFFFF;
    uint16_t     actionId  = 0xFFFF;   // sceneObj+0xCC (0xFFFF = inherit from the map's object record)
    uint16_t     talkId    = 0xFFFF;   // sceneObj+0xDC
};
// Sanity bound on an exit's distance from the player -- rejects garbage positions. Shared with the
// per-area diagnostic dump so both use one number.
constexpr float kExitMaxDist = 2000.0f;

// How near a `+0x70` field sign must be to a `+0x54` jump slot to be describing the same doorway. A
// sign marks the "-> area" arrow and the slot is the volume you step into, so they are never coincident:
// measured 3.6-6.4 m apart on every East End district door, against ~25 m to the next-nearest door.
constexpr float kSignMatchDist = 8.0f;

// How near a field-sign record must be to a scene object for that object to BE the doorway rather than a
// decorative sign of the same name. Every East End shop doorway matched inside ~2 m; its same-named twin,
// which carries no jump info at all, is 6-15 m away.
constexpr float kSignObjectDist = 2.5f;

// Slack allowed when testing an exit against the reachable set. Door triggers sit ON the map seam and are
// routinely a cell or two past the last cell with a floor sample, so a zero-tolerance test would report
// perfectly ordinary district doors as unreachable and hide them.
constexpr float kExitReachTol = 4.5f;

// Rebuild `out` from scratch: handle-table objects, then combatants, then map exits. Returns the
// count; empty and 0 when the field isn't active. The caller holds its own list lock.
int Build(std::vector<Entity>& out);

// Bitmask of currently-active handle-table containers (bit c set iff container c is active).
// Memory-only; touches only the handle table, so the per-frame tick can detect a container-set
// change without paying for a full rescan.
uint32_t ActiveContainerMask();

// (ResolveObjectName / InGimmickBand / CategoryWord / ClassifyByNameKey / IsInteractionAvailable
//  moved to navigation/entity_classify.h, included above so existing callers are unaffected.)

} // namespace EntityScan

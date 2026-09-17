#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "navigation/nav_types.h"

// Proactive interactive-object list, built by walking the game's own scene-object
// handle table (DAT_02098e10, 5 containers) — the registry every field object (NPC,
// gate, door, switch, treasure, crystal) is allocated into at map load, and the same
// set the game itself scans (FUN_0025b820) to decide what the player is near. Each live
// talk/action object yields {world pos (sceneObj+0xB8), interaction flags, npcdic name};
// categories come from the flags + npcdic id band. READ-ONLY and SEH-guarded throughout.
namespace EntityList {

enum class Category {
    All = 0,      // filter pseudo-category
    Exit,         // a map-jump SURFACE you walk onto; arriving at it IS crossing it
    Door,         // a press-Enter OBJECT that carries map-transition data (Entity::doorway)
    Shop,         // a Door with its own name sign beside it (Entity::hasNameSign)
    SaveCrystal,
    GateCrystal,
    Treasure,
    NPC,
    Object,       // unclassified gimmick — signs, levers, switches: NO map transition
    Enemy,        // live battle combatant (BtlWork pool), read separately from the handle table
    Items,        // ground loot an enemy dropped (DAT_02ec0fa0 pool), read separately again
    Trap,         // floor trap: NOT a scene object at all -- a model instance enumerated from the
                  // game's own per-map trap table, and only while the game is showing them
    Count
};
// Door and Shop sit IMMEDIATELY after Exit on purpose (tester's instruction): all three are ways off
// this map, so cycling between them is one `=` press. They are three genuinely different things and
// exit_diag.h already called the middle one out as "a third class again" -- the five East End shops,
// The Sandsea, the Stair to Lowtown. Exit is a floor you step on; Door and Shop are objects you press
// Enter at; Object is everything interactable that leads nowhere.
// Items sits IMMEDIATELY after Enemy on purpose: the cycle is a plain modulo over [0, Count), so
// one `=` press flips between the enemies you are fighting and the loot they left. Requested by the
// tester -- checking for drops is what you do the moment a fight ends. Do not reorder.
// Trap is APPENDED, deliberately disturbing none of the adjacencies above. It is also the one
// category the `=` cycle can SKIP entirely: the game hides traps until a party member has Libra up,
// and the mod hides the category on the same condition (ChangeCategoryLocked).
// (Category::Event is RETIRED. It was created in Session 43 to hold the mapData+0x54 table after that
//  table was wrongly demoted from Exit — see map_query.h. The +0x54 entries are map-jump exits and are
//  back under Category::Exit; nothing else ever produced an Event, so the category had no source left.
//  The naviicon "markers" that were its other intended source were disproven and removed in Session 44.)

// AVAILABILITY filter — orthogonal to Category, toggled with F5. Many field interactables are story-
// gated: the object exists and can be walked to, but the game will not act on it yet. Finding a gate
// you cannot use is still what tells you where to look for the NPC who gates it, so nothing is ever
// hidden by default; Gated narrows the list to exactly the blocked things when you want to find one.
enum class Availability {
    All = 0,      // everything (default — the filter never starts out hiding anything)
    Gated,        // only entries the game currently refuses to let you interact with
    Count
};

bool Init();
void Shutdown();

// Full rebuild from the scene-object handle table (rescan key, or freshly on every
// cycle/describe command). Returns the entity count. Excludes the leader.
int Rescan();

// Called once per field frame (GAME THREAD, from the nav field-frame hook). Auto-rescans
// when a handle-table container streams in — the object containers register lazily AFTER
// the field-active bit is set (especially on a SAVE-LOAD), so without this the list stays
// empty until the user happens to press rescan at the right moment. Edge-triggered on the
// active-container mask (5 byte reads/frame when quiescent); never wipes a populated list
// on a transient empty while a container is still live.
void OnFieldFrame();

// The game's current-area name (localized), or empty if not loaded. Lock-free.
std::wstring CurrentAreaName();

// Hotkey commands (called from nav_commands on the input thread). Each refreshes
// live positions first, then acts + speaks.
void CmdNext();               // ]
void CmdPrev();               // [
void CmdNextCategory();       // =
void CmdPrevCategory();       // -
void CmdToggleAvailability(); // F5  (All <-> Story-gated)
void CmdDescribeCurrent();    // /  (cardinal bearing + distance to the selection)
void CmdRescan();             // `
void CmdLabelFromClipboard(); // F6 name the focused entity with whatever is on the clipboard

// Live world position + label of the current route target (the focused object, else
// the nearest in the active filter). False if not on the field or nothing is listed.
// Used by the `/` route command to hand a fixed world target to the game-thread A*
// planner. Reads only the persistent handle table — input-thread safe.
//
// `outIsTransition` (optional) tells the planner the target is a map-jump surface, so reaching it means
// crossing it. Only exits set it; everything else reports false.
// `outSceneObj` optionally returns the target's scene-object pointer, which the route path needs to
// read the target's INTERACTION BAND (InteractTarget::ReadBandFor) -- routing has to know where you
// could STAND to interact, not just where the object is. Null for entries with no scene object
// (exits, combatant-pool entries).
// `outSeamGroup` (optional) returns the target's map-jump group, 0 when it is not a walk-onto
// surface. It is the handle the planner needs to ask for the WHOLE seam rather than the single
// vertex `outPos` names -- see EntityScan::Entity::seamGroup for why a point is not enough.
bool GetCurrentTarget(FVec3& outPos, std::wstring& outLabel, bool* outIsTransition = nullptr,
                      void** outSceneObj = nullptr, int* outSeamGroup = nullptr);

// PUT THE FOCUS ON ONE SPECIFIC ENTITY, chosen by a caller-supplied test (S181, the Sochen puzzle
// guide). The nearest passing entity wins, the `[` / `]` cursor lands on it, and its spoken label is
// returned so the caller can name it in one sentence. False when nothing passes.
//
// The point of focusing rather than routing: `GetCurrentTarget` prefers the focused object over the
// nearest, so the route key then leads there with no second path through the planner -- the guide
// names a target, the player's own key still does the routing.
//
// GAME THREAD (it rescans and reads live transforms). `sceneObj` is null for fixed exits, and
// `seamGroup` is 0 for everything that is not a walk-onto surface.
typedef bool (*EntityTest)(void* sceneObj, int seamGroup, void* ctx);
bool FocusWhere(EntityTest test, void* ctx, std::wstring* outLabel);

// Dump the raw handle table (tag NAV-DIAG): every named/interactive scene object per
// container with its category byte, interaction flags, npcdic key, name, and world
// position — the data that confirms where a given object (e.g. the tutorial gate) lives.
void LogDiagnostic();

// LIVE positions of every listed entity whose npcdic name index equals `nameIdx` (fresh transform
// read per call; falls back to the last known position when a single read fails, matching
// RefreshPositionsLocked's streaming-noise rule). Returns the match count. GAME THREAD callers
// only — added for PathDanger's per-request zone build (Session 106), where the danger actors move
// during the very window the route must avoid them in.
int CollectPositionsByNameIdx(int16_t nameIdx, std::vector<FVec3>& out);

// SCENE-OBJECT POINTERS for every listed entity whose npcdic name index equals `nameIdx`, written
// into `out` (at most `cap`); returns how many were written. The identity a hook can compare
// against — `Entity::sceneObj` is stable for the life of the map.
//
// Added for sneak assist (S113), which must answer "is THIS object one of the map's guards?" from
// inside the game's own trigger test, tens of times per frame. Pointers, not positions, because the
// question is identity and identity must not be re-derived from geometry.
int CollectSceneObjectsByNameIdx(int16_t nameIdx, void** out, int cap);

// The label the scan gave a scene object, or empty when it is not listed (unnamed rect actors, and
// anything the handle-table walk filtered). Diagnostic use only — it is what lets a log line name
// the object that reported a touch instead of printing a bare pointer.
std::wstring LabelForSceneObject(void* sceneObj);

// One NPC near the player, as the shout-minigame guard key reports them.
struct NearbyNPC {
    std::wstring label;              // the game's own npcdic name — never a mod-authored one
    FVec3        pos;
    int16_t      nameIdx = 0;        // npcdic id, so the log can census WHICH NPCs a map carries
    float        dist2D  = 0.0f;
};

// The nearest `maxOut` Category::NPC entries to `from`, nearest first, positions refreshed.
// Returns how many were written. GAME THREAD callers only (it reads live transforms).
//
// Added for the shout minigame's guard key (Bhujerba): the identity of the Imperials whose earshot
// costs the player points is not derivable from the map scripts, so the honest answer is the
// game's own names with a bearing and a distance rather than an invented "guard" verdict. Sits
// beside CollectPositionsByNameIdx / CollectSceneObjectsByNameIdx because it answers the same
// class of question against the same locked list.
int CollectNearestNPCs(const FVec3& from, int maxOut, std::vector<NearbyNPC>& out);

} // namespace EntityList

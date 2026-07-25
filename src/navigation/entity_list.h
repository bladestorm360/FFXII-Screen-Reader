#pragma once

#include <cstdint>
#include <string>
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
    Exit,
    SaveCrystal,
    GateCrystal,
    Treasure,
    NPC,
    Object,       // unclassified gimmick
    Enemy,        // live battle combatant (BtlWork pool), read separately from the handle table
    Items,        // ground loot an enemy dropped (DAT_02ec0fa0 pool), read separately again
    Count
};
// Items sits IMMEDIATELY after Enemy on purpose: the cycle is a plain modulo over [0, Count), so
// one `=` press flips between the enemies you are fighting and the loot they left. Requested by the
// tester -- checking for drops is what you do the moment a fight ends. Do not reorder.
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
bool GetCurrentTarget(FVec3& outPos, std::wstring& outLabel, bool* outIsTransition = nullptr,
                      void** outSceneObj = nullptr);

// Dump the raw handle table (tag NAV-DIAG): every named/interactive scene object per
// container with its category byte, interaction flags, npcdic key, name, and world
// position — the data that confirms where a given object (e.g. the tutorial gate) lives.
void LogDiagnostic();

} // namespace EntityList

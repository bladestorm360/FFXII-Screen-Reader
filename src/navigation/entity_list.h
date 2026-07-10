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
void CmdNextCategory();       // Shift+]
void CmdPrevCategory();       // Shift+[
void CmdDescribeCurrent();    // \  (cardinal bearing + distance to the selection)
void CmdRescan();             // `

// Live world position + label of the current route target (the focused object, else
// the nearest in the active filter). False if not on the field or nothing is listed.
// Used by the `/` route command to hand a fixed world target to the game-thread A*
// planner. Reads only the persistent handle table — input-thread safe.
bool GetCurrentTarget(FVec3& outPos, std::wstring& outLabel);

// Cardinal (default) vs egocentric direction mode for spoken bearings.
void SetEgocentric(bool on);
bool IsEgocentric();

// Dump the raw handle table (tag NAV-DIAG): every named/interactive scene object per
// container with its category byte, interaction flags, npcdic key, name, and world
// position — the data that confirms where a given object (e.g. the tutorial gate) lives.
void LogDiagnostic();

} // namespace EntityList

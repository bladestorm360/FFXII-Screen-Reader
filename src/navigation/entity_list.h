#pragma once

#include <cstdint>
#include <string>
#include "navigation/nav_types.h"

// Proactive field-object list, built by walking the game's own field-actor pool
// (DAT_0208e688). Each live actor yields {world pos, yaw, kind, def id}; labels
// and fine categories come from the observe-hooks / field-sign block (Layer 2).
// Everything is READ-ONLY and SEH-guarded; the pool walk never mutates game state.
namespace EntityList {

enum class Category {
    All = 0,      // filter pseudo-category
    Exit,
    SaveCrystal,
    GateCrystal,
    Treasure,
    NPC,
    Object,       // unclassified gimmick
    Count
};

bool Init();
void Shutdown();

// Full rebuild from the live actor pool (map-load hook, rescan key, or lazily on
// first command). Returns the entity count. Excludes the leader.
int Rescan();

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

// Cardinal (default) vs egocentric direction mode for spoken bearings.
void SetEgocentric(bool on);
bool IsEgocentric();

// Log every entity's raw coords + def bytes + distance (tag NAV-DIAG) for the
// single confirmation pass: tune units-per-step from real coordinates and pin the
// save-crystal / treasure gimmick-def discriminator.
void LogDiagnostic();

} // namespace EntityList

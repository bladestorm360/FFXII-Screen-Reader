#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "navigation/nav_types.h"

// Read-only reader for the LOADED FIELD SCRIPT that ships inside every map's control blob.
//
// Why this exists: a map's exit destination is not stored on the exit. The `+0x54` jump table holds
// x/y/z/angle and a run of zero bytes; the `+0x70` field-sign records hold positions with an empty
// destination slot on interior maps; and no engine getter returns "destination for door N" (every
// mapData accessor was traced). The destination lives in the map's own compiled script: the map
// toolchain emits one routine per map-jump door named `__MJ_CTRL<NNN>`, and that routine calls the
// `mapjump` native with its destination as a literal.
//
// Confirmed on three different maps in one session (Nalbina 275 Inner Ward / 279 Lower Apartments /
// 280 Upper Apartments — routine tables of 31 / 24 / 37 entries). The destinations recovered this way
// reconstruct the Nalbina floor chain exactly (274 <-> 275 <-> 279 <-> 280 <-> 282), so the read is
// generic rather than tuned to any one map.
//
// GENERALITY IS THE POINT: nothing here may key off a map id, a routine index, or a code offset. The
// only constants are engine-wide blob/VM facts that are identical for every map because they belong to
// the format, not the content. Prologue and non-prologue maps use the same mechanism.
namespace MapScript {

// One map-jump controller routine recovered from the loaded script, resolved to its physical door.
//
// DOOR RULE (walk-tested on five maps): **`__MJ_CTRL<N>` owns `+0x54` slot `N + 1`.** Slot 0 is the
// default/cutscene arrival and is never an exit; a slot with no controller is an arrival point, not a
// door. Confirmed by walking exits and observing where the game actually landed, and independently by
// the arrival relation (if M jumps to D with entrance E, D's door back to M is D's slot E) — both agree
// on every measured pair.
struct ExitDest {
    int          ctrlIndex = -1;   // NNN parsed from the `__MJ_CTRL<NNN>` routine name
    int          slot      = -1;   // owning `+0x54` door slot == ctrlIndex + 1
    FVec3        pos{};            // that slot's RAW world position (the match key -- see below)
    bool         posOk     = false;
    uint16_t     destMapId = 0;    // destination map id (the flags==0 `mapjump` literal)
    uint16_t     entrance  = 0;    // arrival slot on the DESTINATION map (not a local door index)
    uint32_t     codeOff   = 0;    // routine entry offset in the blob (diagnostics only)
    std::wstring destName;         // "<region>: <sub-area>", resolved via MapNames::ResolveFullAreaName
};

// Parse the currently loaded map's field script and return every `__MJ_CTRL<NNN>` controller with its
// destination and owning door, ordered by ctrlIndex. Memory-only and SEH-guarded: a torn blob (mid-load /
// mid-teardown) yields an empty list rather than a fault. Returns false when no field script is loaded.
//
// Callers should match a door to its destination BY POSITION (`pos`), not by slot number: the `+0x54`
// table repeats records (one map's slot 1 is byte-identical to slot 0) and `MapExits::EnumerateMapJumps`
// de-duplicates them, so the surviving entry's index may differ from the owning slot while naming the same
// physical doorway. Position matching also fails safe — a mismatch yields no label rather than a wrong one.
//
// `logDetail` dumps what was found to NAV-DIAG. It is expensive and noisy, so callers pass true only when
// the map has actually changed.
bool ReadExitDests(std::vector<ExitDest>& out, bool logDetail);

} // namespace MapScript

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
// DOOR RULE (Session 58, decoded from the `'` capture on East End + Muthru Bazaar): controller `i` owns
// the `+0x84` record immediately AFTER the i-th "edge" record — an edge being a `+0x84` entry that
// appears in no `+0x54` entry. See map_script.cpp's ResolveControllerArrivals for the full derivation.
//
// **STRUCK: the S46 "`__MJ_CTRL<N>` owns `+0x54` slot `N + 1`" rule.** It paired two structurally
// independent tables (the exit routines and the party ARRIVAL table) and only ever held on Nalbina's
// 2-3-loader maps. On Rabanastre East End it put Muthru (a west doorway) mid-map and Southern Plaza (a
// south doorway) at the far west — the reported "Southern Plaza loads the Bazaar". It survives only as
// the fallback when the `+0x84` shape is unexpected, so an odd map degrades instead of emitting garbage.
struct ExitDest {
    int          ctrlIndex = -1;   // NNN parsed from the `__MJ_CTRL<NNN>` routine name; -1 when the
                                   // binding came from a routine that is not a controller
    // WHICH ROUTINE BOUND THIS GROUP, and whether it was a door controller.
    //
    // A transition's two halves are `setmapjumpgroup(K)` -- the walkmap tag that says WHERE -- and
    // the same routine's `mapjump` literal, which says WHERE TO (S64). That rule is about the CALLS
    // a routine makes; the `__MJ_CTRL<NNN>` NAME was only ever how they were found. A map's stairway
    // into a dungeon is bound by its EVENT routine instead (map 313's group 1: a real 2-poly surface
    // at Y=17 that the seam sweep finds and no controller claimed, so it was dropped as "leads
    // nowhere" and the staircase was invisible to a blind player).
    //
    // `routineIndex` is the identity for those -- stable within a map, and distinct from ctrlIndex,
    // which is -1 for them. Callers keying a cursor on the exit must use whichever applies.
    int          routineIndex  = -1;
    bool         viaController = true;
    // TRUE when `group` was not read from the script at all but INFERRED downstream, by the
    // elimination rule in exit_scan.cpp: exactly one swept map-jump surface that no routine claims,
    // and exactly one group-less candidate with a real destination, so there is only one way to pair
    // them. Map 313's dungeon staircase is that case -- routine[4] (the `イベント…` routine) holds
    // `mapjump(567 "Royal Palace: Cellar Stores", entrance=1, flags=0x1)` and arms NO group, and the
    // container census proves NOTHING on that map arms group 1. The walkmap carries the tag; no
    // runtime call ever sets it, because an event-fired transition does not use the group system to
    // decide -- the event does. Recorded so a log line can never present an inference as a reading.
    bool         groupInferred = false;
    std::string  routineName;      // sanitised, for the log only (most are Shift-JIS)
    // The routine's NAME-POOL OFFSET -- the join key that binds a door OBJECT to its transition
    // (Session 119). Scene objects carry an event table (`object+0x48`) whose entries are name-pool
    // offsets into the same pool, so `entry == nameOff` is the map's own data saying "this object's
    // events run that routine": no authoring order (banned S46/S58), no label text, no locale.
    // Only comparable within one container's pool; see ObjectEventNameOffsets.
    uint32_t     nameOff = 0;
    // The `mapjump` call's third literal. NOT a transition kind: the decompile chain
    // FUN_00355350 -> FUN_00314440 -> FUN_003145e0 uses it as a PRESENTATION bitfield (bit 0 picks
    // the no-fade path, bit 1 feeds FUN_002efa70). `0x0A` is the world-map teleport menu's
    // combination and is the one value excluded. Recorded so the log can show what real maps use.
    uint16_t     jumpFlags = 0;
    int          slot      = -1;   // authoring-order id (== ctrlIndex + 1; also the routine's 0x011E arg)
    FVec3        pos{};            // the doorway's ARRIVAL point: walkable, a couple of steps inside the map
    bool         posOk     = false;
    // The transition TRIGGER's reference point: off the walkable mesh, out past the map boundary, at a
    // variable distance (~120 units on one East End exit). Useless as a route target for exactly that
    // reason — but the DIRECTION from `pos` to `edge` is the direction the player crosses the seam, which
    // is what lets the route target be pushed out to the boundary instead of stopping at the arrival.
    FVec3        edge{};
    bool         edgeOk    = false;
    // Which `+0x54` slot this doorway's arrival IS -- a plain fact about the blob, recovered by exact
    // match against `+0x84`. Diagnostics only.
    int          arrivalSlot = -1;
    // **THE BINDING.** The routine's `setmapjumpgroup(K)` argument: the id the WALKMAP tags this
    // transition's floor polygons with. Position comes from those polygons, the destination from this
    // same routine's `mapjump` literal -- one object, both halves, so they can never be mismatched.
    // -1 when the routine has no such call (then nothing about it is established and it is dropped).
    int          group       = -1;
    uint16_t     destMapId = 0;    // destination map id (the flags==0 `mapjump` literal)
    uint16_t     entrance  = 0;    // arrival slot on the DESTINATION map (not a local door index)
    uint32_t     codeOff   = 0;    // routine entry offset in the blob (diagnostics only)
    std::wstring destName;         // "<region>: <sub-area>", resolved via MapNames::ResolveFullAreaName
};

// Parse the currently loaded map's field script and return every `__MJ_CTRL<NNN>` controller with its
// destination and owning door, ordered by ctrlIndex. Memory-only and SEH-guarded: a torn blob (mid-load /
// mid-teardown) yields an empty list rather than a fault. Returns false when no field script is loaded.
//
// Each returned record is already resolved to its own doorway — `pos` (walkable arrival) and `edge`
// (off-mesh trigger reference). Callers do NOT cross-reference `MapExits::EnumerateMapJumps`; that path
// matched doors to destinations by position against a de-duplicated `+0x54` copy, and it is gone. The
// only thing outside this file that reads `slot` is diagnostics.
//
// `logDetail` dumps what was found to NAV-DIAG. It is expensive and noisy, so callers pass true only when
// the map has actually changed.
bool ReadExitDests(std::vector<ExitDest>& out, bool logDetail);

// The NAME of the routine an event fire on `object` would start, raw from the name pool.
//
// `eventIdx` is the index `FUN_003dbb60` carries -- and it is OBJECT-LOCAL: `FUN_003dbcf0` bounds it
// against the object's own event table at `object+0x48` ([count:u32][8-byte records]) whose entries
// hold NAME-POOL OFFSETS, resolved against the object's own container's blob. It is NOT a routine-
// table index. (S117 shipped `RoutineNameAt(index)` on that wrong reading; the S118 play log refuted
// it -- objects fired consecutive small indices resolving to `setup` and the map director, names no
// trigger volume could be starting -- and this replaced it. No other caller ever existed.)
//
// Names are mostly Shift-JIS, so the bytes are returned untranscoded and callers compare bytes;
// nothing here is user-facing text. False on a torn/absent blob, table, or an out-of-range index --
// callers must treat that as "unknown", never as "not a match".
bool FiredRoutineName(void* object, uint32_t eventIdx, std::string& out);

// The RAW name-pool offsets in `object`'s event table (`object+0x48`, [count:u32][8-byte records]),
// up to `cap`. Returns how many were written; 0 on no/unreadable table. The offset form exists for
// exact joins against `ExitDest::nameOff` -- an integer compare in the map's own pool, immune to the
// Shift-JIS names that `AsciiSafe` mangles. Offsets are pool-relative, so a join is only valid when
// both sides live in the SAME container: check ObjectContainerId first.
int ObjectEventNameOffsets(void* object, uint32_t* out, int cap);

// Which script container owns `object` (`object+0x15`, the id `FUN_00263ff0` indexes the handle
// table with). -1 when unreadable. The map-global script -- the one ReadExitDests parses -- is
// container 0.
int ObjectContainerId(void* object);

// Session 57 capture (file-only, `'`-triggered): dumps BOTH parallel position tables (+0x54 and +0x84)
// raw + un-deduped, and every `__MJ_CTRL` routine's full bytecode with its CALLACTPOPA native calls
// annotated (mapjump 0x008d / zone-test 0x202d + operands). This is the data the offline decode uses to
// find each exit's true transition-tile position (currently we use the +0x54 arrival point). Read-only.
void DumpCaptureDiag();

} // namespace MapScript

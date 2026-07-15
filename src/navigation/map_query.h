#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include "navigation/nav_types.h"

// Read-only queries against the game's SQEX FIELD-COLLISION walkmap — the floor/wall
// mesh the field (and the AI NPCs) actually walk on. This is the walkability oracle for
// field navigation. Unlike Bullet (which the Nalbina prologue never builds), this mesh
// is loaded with every field map, so it is live wherever the player can walk.
//
// The collision ctx is a GLOBAL (DAT_0209a678, gated by DAT_0209a670) — read directly,
// no capture hook needed. The two query functions (FUN_003208c0 ground, FUN_00230b60
// segment) are reentrant and touch no shared writable state, so they are safe to call
// thousands of times per route. Every game call is SEH-guarded; a fault degrades to
// "no floor" / "clear" rather than crashing.
namespace MapQuery {

// True when the field-collision manager + ctx0 are present (the walkmap is loaded).
// Memory-only. This is the "world" liveness signal for IsFieldNavSafe.
bool HasWorld();

// Walkable-floor test at an arbitrary world (X,Z): true + ground height on success.
// The single best call for the A* grid's cell walkability. Gated on HasWorld().
bool GroundAt(float x, float z, float& outY);

// True if the straight segment from -> to is unobstructed by a WALKING wall (walk query
// class). Both endpoints are tested at `from.y` (equal-Y, so a slope-climbing segment can't
// clip a rising floor). Returns true (clear) when there is no world — callers gate on
// HasWorld() first.
bool SegmentClear(const FVec3& from, const FVec3& to);

// Raw segment test with an explicit query class + flags — returns the hit index (>=0
// BLOCKED, <0 CLEAR; <0 also when there is no world). Used by SegmentClear (walk class) and
// by the '-key self-test to log the walk (mask=4) vs camera (mask=0xffff) contrast.
int SegmentHit(const FVec3& from, const FVec3& to, uint16_t mask, uint32_t flags);

// ---- Direct walkmap-grid read (the whole-map overlay source; no raycasts) ----
// The walkmap is itself a uniform staggered grid over a floor/wall mesh. These read its
// header + per-cell floor polys directly (see NavRva WALK_* offsets), so a whole map's
// walkability + height can be baked in one O(cells) memory pass. All SEH-guarded; a torn
// read degrades to invalid / not-walkable.

// Grid geometry + cached array bases for the CURRENT map (populated by GetGridInfo).
struct WalkGridInfo {
    int  nCols = 0, nRows = 0;
    int  cellSizeX = 0, cellSizeZ = 0;
    int  originX = 0, originZ = 0;
    bool valid = false;
    // Cached ctx0 sub-array bases (internal use by ReadCellFloor; not for callers).
    void* header = nullptr, *vertArr = nullptr, *polyArr = nullptr;
    void* csrTable = nullptr, *primList = nullptr;
};

// Read the live walkmap grid header + origin + array bases. False if no walkmap / garbage.
bool GetGridInfo(WalkGridInfo& out);

// World XZ -> cell (col,row). Returns false (and fills the raw indices) when out of bounds.
// Replicates FUN_00233050 (integer division + odd-column brick stagger).
bool WorldToCell(const WalkGridInfo& g, float wx, float wz, int& col, int& row);

// Cell (col,row) -> world XZ of its center (inverse of WorldToCell, incl. the stagger).
void CellCenter(const WalkGridInfo& g, int col, int row, float& wx, float& wz);

// Direct per-cell walkability + floor height from the grid: true iff the cell holds a
// walkable floor poly (type 0), writing the topmost such floor's height (plane eval at the
// cell center) to outY. No game call, no raycast.
bool ReadCellFloor(const WalkGridInfo& g, int col, int row, float& outY);

// Dense traversability of a straight segment (the string-pull validator). Samples every
// `step` m; at each sample requires floor present (GroundAt), |dFloorY| <= maxStep vs the
// previous sample, and SegmentClear (walk class) on the ~step sub-segment at the local
// floor+bodyPad height — plus, when `margin` > 0, two rays offset +/-margin perpendicular so
// the leg only counts clear with body width on both sides (keeps routes off wall faces).
// Counts every ray into `rays`; returns false (conservative) on the first failing sample or
// when `rayCap` is exhausted. Returns true when there is no world.
bool SegmentTraversable(const FVec3& a, const FVec3& b, float step, float bodyPad,
                        float maxStep, float margin, int& rays, int rayCap);

// ---- Map EXITS (map-jump points + field-sign destinations) -------------------
// A map exit: fixed world position, plus a destination area when one can be resolved.
struct ExitRec {
    FVec3        pos;
    float        angle = 0.0f;
    int          index = 0;       // exit slot index
    uint16_t     areaId = 0xFFFF; // destination planmapname area id (0xffff = none)
    std::wstring destName;        // resolved destination area name (empty if unresolved)
    bool         usable = false;  // story gate satisfied (FUN_002648f0 buf[0])
};

// Enumerate the current map's MAP-JUMP POINTS — the mapData+0x54 table behind getmapjumpposbyindex.
// These are the intra-map "Mapjump" transitions (e.g. Inner Ward -> Upper Apartments): walking into one
// moves the party to another area. Tester-confirmed.
//
// CORRECTION (this session): Session 43 demoted this table to "party arrival/spawn, not exits" on the
// claim that FUN_00353490 places the party from it. That is false — FUN_00353490 is abs 0x353490 = RVA
// 0x233490 = NavRva::GETMAPJUMPANGLEBYINDEX, the script native `getmapjumpanglebyindex`; it returns a
// jump's angle and places nothing. The demotion had no basis and is reverted here.
//
// Records carry x/y/z/angle ONLY: FUN_00264b90 is the sole reader of this table in the whole binary and
// touches just word[i*8+1..4]; record bytes +0x10..+0x1f are read by NOTHING, so there is no destination
// id in them (the old destIdx@+0x1d -> +0x8c chain read dead bytes and always yielded areaId 0xffff).
// Destination names come from EnumerateExits (the +0x70 field-sign array) instead.
// Memory-only + SEH-guarded; `logRaw` writes per-candidate diagnostics to NAV-DIAG. Clears `out` first.
void EnumerateMapJumps(const FVec3* playerPos, float maxDist, std::vector<ExitRec>& out, bool logRaw);

// Enumerate the field-sign array at mapData+0x70 — the curated list the game draws as radar blips /
// "→ <area>" arrows. Each record carries a destIdx (+0x1d) into the mapData+0x8c area table, which is how
// the game resolves the destination NAME (chain confirmed in the game's own sign renderer FUN_003f9720).
// This is the only source of exit destination names.
//
// NOTE: +0x70 is a GROUP-offset table ([u32 groupCount][u32 groupOff...]); each group's sub-table is
// [u32 count][12B hdr][rec x 0x20]. FUN_00264ac0/FUN_00264ae0 take a GROUP index in RCX — the mod used to
// call the count getter with no argument at all, so it read whatever garbage was in the register and
// returned 0 on every map. That is why "+0x70 is empty" was concluded without ever measuring it.
// Memory-only + SEH-guarded. Clears `out` first.
void EnumerateExits(const FVec3* playerPos, float maxDist, std::vector<ExitRec>& out, bool logRaw);

// (The naviicon minimap "markers" enumerator was REMOVED in Session 44 — two decompile traces proved that
// array holds only character/unit dots that duplicate the combatant scan; no objective/crystal source.)

} // namespace MapQuery

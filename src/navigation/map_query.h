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

// ---- Area-name resolvers (the game's own planmapname table; SEH-guarded, memory-only) ----
// Current field map id (gameState+0x1044 via FUN_003148f0; 0 when no field map). Each resolver returns
// empty unless the id resolves to an in-range printable name, so a stray id is never spoken.
// NOTE the two are NOT interchangeable — planmapname is indexed BY MAP ID, so FUN_00377870(mapId) is the
// SUB-AREA; the FUN_00264f90 hop maps a map id to its REGION's index (proven live: every Nalbina sub-area
// id came back "Nalbina Fortress" through that path). Getting these backwards is what made every exit
// announce as the region.
int          CurrentMapId();
std::wstring ResolveAreaName(int mapId);     // SUB-AREA, e.g. 279 -> "Lower Apartments"
std::wstring ResolveRegionName(int mapId);   // REGION,   e.g. 279 -> "Nalbina Fortress"
std::wstring ResolveFullAreaName(int mapId); // "<region>: <sub-area>"; degrades to whichever half resolves

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
// The four floats (x/y/z/angle) are the only bytes FUN_00264b90 reads; the 16-byte trailer at record
// +0x10..+0x1f is read by NO game code. The OLD destIdx@+0x1d -> mapData+0x8c chain (struck) mis-read
// that trailer through the +0x70 machinery and always yielded areaId 0xffff. This function now probes
// the trailer DIRECTLY through the game's own resolver (mapId -> FUN_00264f90 -> FUN_00377870): each
// trailer word (and its u16 halves) is fed to the resolver and the first that returns an in-range
// printable area name becomes ExitRec.destName — the DQ7R idiom, resolved on the current map with no
// traversal. Empty destName -> caller labels the exit "Exit".
// Memory-only + SEH-guarded; `logRaw` writes the raw trailer + the winning candidate to NAV-DIAG so
// the exact destination field is pinned across a run or two. Clears `out` first.
void EnumerateMapJumps(const FVec3* playerPos, float maxDist, std::vector<ExitRec>& out, bool logRaw);

// Enumerate the current map's EXITS from the field-sign array at mapData+0x70, through the game's own
// getters (FUN_00264ac0(group) / FUN_002649b0(group,i) / FUN_002648f0(rec,buf)). THE exit source: the only
// records carrying a WORLD position AND a destination together, so `pos` is a real world point (bearing and
// steps are honest) and `destName` is the game's own "<region>: <sub-area>" for the map you'd arrive in.
// `usable` = story gate satisfied; records the game says aren't shown right now are skipped, as are exits
// whose destination doesn't resolve (silence beats a bare "Exit") and any leading back into this same area.
//
// NOTE: the long-standing "+0x70 is empty on every map" verdict was a CALLING-CONVENTION bug, not data —
// the group getters take their index in ECX and were being called with no argument, so the bounds check
// failed and every count read 0. See the ABI note in nav_rva.h. Clears `out`; `logRaw` dumps each record.
void EnumerateFieldSignExits(std::vector<ExitRec>& out, bool logRaw);

// DIAGNOSTIC (log-only): scan the loaded map's field-script bytecode for `mapjump(dest, entrance, flags)`
// literals (the game's own per-map exit destinations) and log each with its resolved name + the preceding
// dispatch bytecode. Used to design the source-door <-> destination pairing; no user-facing output.
void DiagScanScriptMapjumps();

// Enumerate the region's SUB-AREA LIST from the in-game map's connection database — the source the
// map screen uses to draw the region's floors. NOT AN EXIT LIST — do not feed Category::Exit from it.
//
// CORRECTED (this session): these records are the sub-areas OF THE CURRENT REGION, not the doorways out of
// the current room. PROVEN by FUN_003c0380, which searches a group's type-0 records for `rec+0x0c == the
// CURRENT map id` and returns the group — so rec+0x0c is a map id and the current map is itself one of the
// records. FUN_003c0340 returns byte[+1]+byte[+0] = (type-0 sub-areas in region) + (type-1 inter-region
// doors). Nalbina's five records are its five floors: their +0x04..+0x07 bytes form a prev/next chain
// (i=0 next=1 prev=ff … i=4 next=ff prev=3) and their coords are ATLAS paste offsets for the map image
// (FUN_003bed70 bboxes them as rec.x + destTexture.width) — hence `pos` is MAP-SPACE, never world.
// Listing these as exits announced five floors of the fortress, most unreachable from the room you're in.
//
// Retained (unused) as the basis for a possible "areas in this region" overview readout. Fills `out` per
// record: `destName` = "<region>: <sub-area>", `areaId` = that map id, `pos` = MAP-SPACE (x,0,y).
// Clears `out` first; `logRaw` dumps each record to NAV-DIAG.
void EnumerateMapConnections(std::vector<ExitRec>& out, bool logRaw);

// (EnumerateExitDestinations was REMOVED: the +0x8c dest table is indexed by a field-sign record's +0x1d
//  byte, not the jump index — nothing pairs +0x8c[i] with +0x54[i]. Exit destination names will come from
//  the in-game map's own resolver, DAT_02b457e0 → FUN_003c0340/FUN_003bf430/FUN_003c2320, pending the probe.)

// (EnumerateExits — the mapData+0x70 field-sign walker — was REMOVED this session: the +0x70 array is
//  confirmed EMPTY on every map by two testers, so it never yielded a destination name.)

// (The naviicon minimap "markers" enumerator was REMOVED in Session 44 — two decompile traces proved that
// array holds only character/unit dots that duplicate the combatant scan; no objective/crystal source.)

} // namespace MapQuery

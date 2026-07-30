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
    // CSR holds FOUR layers: index = layer * (cellCount + 1) + cell (FUN_0022f830). Layer 0 is the
    // floor polys, which is the only one we read -- and layer 0's index is just `cell`, so this is
    // needed only if a caller ever reaches for volumes in layers 1-2.
    int  cellCount = 0;
    bool valid = false;
    // Cached ctx0 sub-array bases (internal use by ReadCellFloor; not for callers).
    void* header = nullptr, *vertArr = nullptr, *polyArr = nullptr;
    void* csrTable = nullptr, *primList = nullptr;
};

// Point-in-triangle in the XZ plane for the poly at byte offset `polyBase` in the poly array --
// the engine's FUN_002324f0 test, without which a poly's infinite PLANE extrapolates to nonsense
// (a Clan Hall column once reported floors at -2089 and +2537). Exposed because the navmesh needs
// exactly the same predicate to find which triangle you are standing on.
bool PolyContainsXZ(const WalkGridInfo& g, uint32_t polyBase, float px, float pz);

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
//
// CAUTION -- "topmost" is why navigation was elevation-blind (Session 73). In any column with
// stacked geometry (plinth, balcony, bridge, upper storey) this describes the surface ABOVE the
// player's head, not the floor they are standing on. Prefer AllFloorsAt below for anything that
// has to know WHICH level it is talking about; this stays for callers that genuinely want the roof.
bool ReadCellFloor(const WalkGridInfo& g, int col, int row, float& outY);

// ---- Stacked floors: every walkable level in one column ------------------------------------------
// The mod's whole navigation stack asks f(x,z) -> y, which is not a well-defined question in a game
// with balconies and bridges: MapQuery::GroundAt is a GAME function taking only (x,z), and our own
// ScanTopFloorAt resolved the ambiguity by keeping the maximum. It already visited every floor poly
// in the cell -- it just discarded all but the highest. AllFloorsAt keeps them.
struct FloorLayer {
    float y        = 0.0f;   // plane-evaluated height at the requested (evalX, evalZ)
    float cosSlope = 1.0f;   // |B| / |(A,B,C)|; 1.0 = flat, smaller = steeper
};

// Ceiling on levels reported per column. Far above any real cell; purely a stack bound.
constexpr size_t kMaxFloorLayers = 16;

// Two floors closer together than this are ONE walkable surface, not two levels -- co-planar polys
// meeting inside a cell must not read as a step. Deliberately the same magnitude as PathSearch's
// kStepDiscont (0.35): the height at which the engine stops letting you walk across a change IS the
// height at which two surfaces become different levels.
constexpr float kLayerMerge = 0.35f;

// Every walkable floor in cell (col,row), evaluated at (evalX,evalZ), ASCENDING by height with
// near-coplanar polys merged. Returns the number of layers written to `out` (0 = no floor).
// `outRawCount` optionally receives the pre-merge poly count, so a caller can see how much
// geometry collapsed -- and can tell when a column hit the kMaxFloorLayers bound.
// Memory-only, no game call, no raycast. Same cost as ReadCellFloor.
size_t AllFloorsAt(const WalkGridInfo& g, int col, int row, float evalX, float evalZ,
                   FloorLayer* out, size_t maxOut, int* outRawCount = nullptr);

// DIAGNOSTIC: topmost walkable floor at an arbitrary world XZ (not just a cell centre), returning the
// floor height AND the poly's slope cosine (B / |(A,B,C)| from the plane normal; 1.0 = flat, smaller =
// steeper). Shares ReadCellFloor's CSR->prim->poly traversal. Used by the NAV-ROUTE route-profile dump
// to distinguish a step-discontinuity (isolated big dY) from a smooth-but-steep slope (small dY per
// step, low cosine). NOT a walkability gate -- the field engine imposes no slope limit. Reads the live
// grid each call, so it is for log-only diagnostics, not the hot A* path. Handles its own GetGridInfo.
bool GroundInfoAt(float x, float z, float& outY, float& outCosSlope);

// ---- The engine's own body sweep: "if I ask to move A -> B, where do I actually end up?" ----------
//
// This is the mod's HARD passability test, and it is the engine's own — `FUN_00230c10`, the routine the
// character controller runs on every step. It answers with a DISPLACEMENT, not a flag, which is what
// makes it the right instrument: the tester's description of the game's refusal ("you can still walk
// against a cliff, you just make no progress") is a statement about achieved distance.
//
// Prefer this over SegmentClear for anything that decides whether a route leg is walkable.
// SegmentClear is one hairline ray with no body radius, no depenetration and no answer to "how far";
// it cannot see an obstacle the character's 0.27 m body hits but a zero-width line misses.
//
// PURE: the whole call tree writes no game memory. See the block comment on the definition.
struct BodyMove {
    bool  valid    = false;   // false = no world / unresolvable RVA; treated as clear, never as blocked
    bool  blocked  = false;   // the engine refused some part of the requested displacement
    float requested = 0.0f;   // |to - from| on the XZ plane
    float achieved  = 0.0f;   // |reached - from| on the XZ plane
    float fraction  = 1.0f;   // achieved / requested, the engine's own ctrl+0x120 quantity
    FVec3 reached{};          // where the character actually ends up (== `to` when not blocked)
};

// True when the full displacement is legal. `out` is filled either way, so a caller can ask how far it
// would have got. GAME THREAD, on a nav-safe frame — same contract as SegmentClear.
bool BodySweep(const FVec3& from, const FVec3& to, BodyMove& out);

} // namespace MapQuery

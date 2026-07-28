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

// ---- Map-jump surfaces: WHERE a transition physically is -----------------------------------------
// The floor polygons the player walks onto to fire a map transition, tagged by the script's
// `setmapidmj` (see NavRva::WALK_POLY_MJ_*). `group` is the map-jump group id, which is also the
// argument the owning `__MJ_CTRL` routine passes to `setmapjumpgroup` -- so this is the half of a
// transition that says WHERE, and the routine is the half that says WHERE TO.
//
// This is the thing five earlier models tried and failed to infer from the map-control blob. It was
// never in the blob: transitions live in the WALKMAP, and interactable doors (shops, stairs) live in
// the scene-object table. Two separate systems; do not use either as evidence about the other.
constexpr size_t kMaxSurfaceVerts = 256;   // torn-read bound; a real seam is 2-28 polys

struct MapJumpSurface {
    int   group     = 0;
    FVec3 centroid{};        // mean of the tagged polys' base vertices -- the middle of the seam
    FVec3 min{}, max{};      // bounding box, so a recorded crossing can be checked against it
    int   polyCount = 0;
    // All three vertices of every tagged triangle, so a caller can aim at the seam's NEAR EDGE
    // rather than its middle. Southern Plaza's seam is 28 polys spanning z[132.0..140.0], so its
    // centroid overstates the walk by several steps and the route drives through the transition
    // instead of to it.
    std::vector<FVec3> verts;
    // The tagged polys themselves -- navmesh node ids. A route to this exit is a search whose goal
    // set is exactly these, and a reachability answer is whether any of them is in the player's
    // component. Both questions are meaningless against a grid and exact against the mesh.
    std::vector<int>   polys;
};

// One full sweep of the walkmap grid (~15k guarded reads). Prefer the cache below. Empty when there
// is no walkmap.
bool ReadMapJumpSurfaces(std::vector<MapJumpSurface>& out);

// ---- The per-map seam cache: ONE gated writer, many pure readers ---------------------------------
//
// HasWorld() IS A LIVENESS SIGNAL, NOT AN IDENTITY SIGNAL. It says a walkmap is resident; it does
// NOT say the walkmap belongs to the map id you are holding. The map id flips BEFORE the engine
// swaps the walkmap, so a sweep taken the moment the id changed reads the PREVIOUS map's polygons --
// and the old cache then latched that answer for the whole visit. Garamsythe Waterway served map
// 311's three seams to map 315 and 315's two seams back to 311, which mislabelled every exit,
// dropped the one whose group did not exist on the wrong map, and put another 199 steps away.
//
// So the sweep is no longer something a reader can trigger. PrimeMapJumpSurfaces is the only writer
// and runs on the GAME THREAD behind PlayerState::IsFieldNavSafe() -- the same gate that makes
// NavMesh/NavReach correct across transitions -- and PathPlanner::OnMapTeardown drops the answer on
// the way out, exactly as it drops the navmesh and the reachable set.

// GAME THREAD ONLY, and only from inside a nav-safe frame. Sweeps when `mapId` is not the map
// already cached; otherwise a no-op. See PathPlanner::OnGameFrame for the single call site.
void PrimeMapJumpSurfaces(int mapId);

// Any thread. Serves a COPY of the seams IF AND ONLY IF they were swept for `mapId` -- a caller can
// never be handed another map's geometry. NEVER sweeps: before the first primed frame of a map this
// returns false and an empty vector, and the caller says nothing. Silence, not wrong speech.
// (A copy rather than a reference because the writer thread may rebuild the vector underneath.)
//
// The return is SWEPT-ness, not emptiness: "not looked yet" and "looked, and this map has no seams"
// are different answers, and only the second makes a controller with no surface a MISSING EXIT.
bool CachedMapJumpSurfaces(int mapId, std::vector<MapJumpSurface>& out);

// GAME THREAD. Drop the cached seams — called from PathPlanner::OnMapTeardown beside
// NavMesh::Invalidate / NavReach::Invalidate, so a map reloaded onto its own id re-sweeps too.
void InvalidateMapJumpSurfaces();

// Nearest point of `s` to `from` on the XZ plane, over the seam's own vertices. This is what the
// player reaches first, and what both the spoken distance and the route goal should aim at.
bool NearestPointOnSurface(const MapJumpSurface& s, const FVec3& from, FVec3& out);

// Dense traversability of a straight segment (the string-pull validator). Samples every
// `step` m; at each sample requires floor present (GroundAt), |dFloorY| <= maxStep vs the
// previous sample, and SegmentClear (walk class) on the ~step sub-segment at the local
// floor+bodyPad height — plus, when `margin` > 0, two rays offset +/-margin perpendicular so
// the leg only counts clear with body width on both sides (keeps routes off wall faces).
// Counts every ray into `rays`; returns false (conservative) on the first failing sample or
// when `rayCap` is exhausted. Returns true when there is no world.
bool SegmentTraversable(const FVec3& a, const FVec3& b, float step, float bodyPad,
                        float maxStep, float margin, int& rays, int rayCap);

} // namespace MapQuery

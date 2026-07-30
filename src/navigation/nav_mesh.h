#pragma once

#include <cstdint>
#include <vector>
#include "navigation/nav_types.h"

// THE GAME'S OWN NAVMESH (Session 74).
//
// FFXII has no navigation grid. The field-collision walkmap is a connected mesh of triangles, and
// every triangle carries the index of its neighbour across each of its three edges at `+0x16/+0x18/
// +0x1A`. The character mover `FUN_002327d0` keeps a CURRENT POLY INDEX across frames and steps onto
// the neighbour when the position leaves the triangle, gated by `FUN_00230a40`. That graph is the
// authoritative answer to "can you walk from here to there", and this module reads it.
//
// WHY THIS REPLACED THE GRID. The mod used to sample its own uniform 1.5 m grid, store ONE floor
// height per cell, and invent edges between neighbouring cells with a step-height gate. Every part of
// that was wrong:
//   * one height per cell cannot hold a balcony over a walkway, so stacked destinations collapsed --
//     Upper Apartments announced its Highhall and Lower Apartments exits one step apart;
//   * the step gates (`kMaxStep` 1.5, `kStepDiscont` 0.35) refuse edges the engine has NO gate on,
//     so any staircase with a riser over 0.35 m was unroutable and A* reported "unreachable" for
//     places the player walks daily;
//   * the heights came from `MAP_GROUND_AT`, which is not a floor query at all -- `FUN_0026e3c0`
//     takes the topmost floor and THEN climbs up to 30 units and casts back down with mask 0xFFFF,
//     returning whatever it hits, so cells could hold a wall or a rooftop.
// The mesh has none of those problems because it is what the game itself walks.
//
// ELEVATION IS FREE. A balcony and the floor beneath it are two disconnected components of the same
// graph that happen to share a grid cell. Nothing here special-cases height; there is no layer index,
// no stacking test, and no Y tolerance anywhere in the connectivity.
//
// GAME THREAD ONLY -- `EdgePassable` casts a walk-class segment, which is a game call.
namespace NavMesh {

// A poly index is a s16 in the engine; -1 means none.
using PolyId = int32_t;
constexpr PolyId kNoPoly = -1;

// Bound on a single graph walk. Far above any real route (East End's reachable component was ~2,150
// GRID cells and a poly is coarser than a cell), and it is the only thing standing between a torn
// read and an unbounded loop.
constexpr int kMaxPolys = 20000;

bool Ready();                       // walkmap loaded and the arrays look sane

// Cached per map epoch. Invalidate drops it; EnsureEpoch rebuilds lazily on next use.
void EnsureEpoch(uint32_t epoch);
void Invalidate();

// ---- Raw mesh reads (memory-only, SEH-guarded) ---------------------------------------------------

// The poly's three vertices in world space. False on a torn read or a negative vertex index.
bool PolyVerts(PolyId p, FVec3 out[3]);

bool PolyCentroid(PolyId p, FVec3& out);

// Midpoint of edge `e` (0..2, the edge from vert e to vert (e+1)%3).
//
// NO LONGER USED BY ROUTING, and must not go back: threading a polyline through the midpoint of every
// shared edge is what made routes double back on themselves ("South 2, North 7, ..."). On a mesh whose
// triangles are often whole corridors, successive midpoints sit at opposite ends of their portals.
// The route uses EdgePortal below plus the funnel string-pull instead. Kept for diagnostics.
bool EdgeMidpoint(PolyId p, int e, FVec3& out);

// The two ENDPOINTS of edge `e` -- the portal the player crosses to leave `p` through that edge.
// A funnel string-pull needs the portal's extents, not its middle: the shortest line through a
// corridor touches portal ENDS, and only the endpoints can tell it where the corridor pinches.
bool EdgePortal(PolyId p, int e, FVec3& a, FVec3& b);

// Neighbour across edge `e`, or kNoPoly. A boundary edge (map edge, wall) has no neighbour.
PolyId Neighbor(PolyId p, int e);

// Raw and effective flags. `Effective` applies the runtime override table (FUN_00232020) -- a script
// that opens a gate changes these without touching geometry.
bool PolyFlags(PolyId p, uint32_t& raw, uint32_t& effective);

// Walkable for the PARTY (movement class 4), i.e. `(effectiveFlags & 7) == 0`. See
// NavRva::WALK_CLASS_PARTY for why the class collapses to just the type test.
//
// This is the engine's PERMISSIVE floor test and a faithful replica of FUN_00230a40 -- keep it that
// way. It is deliberately NOT the mod's passability predicate: the engine's own hard refusal is a
// body-versus-boundary test (NavFootprint::Clears) layered on top of this, not a stricter version of
// it. Anything that makes this function stricter than FUN_00230a40 is a bug.
bool Walkable(PolyId p);

// Is `p` a legitimate floor-poly index? A poly index is an s16 in the engine and the prim encoding
// reserves >= 0x4000 for volumes, so a real floor poly is always in [0, 0x4000). Exposed because
// NavFootprint needs the same notion of validity and a second copy of the bound would be a second
// place to get it wrong.
bool ValidPolyId(PolyId p);

// Map-jump group tag, or 0. `(effectiveFlags >> 3) & 0xF` -- four bits, see NavRva.
int MapJumpGroup(PolyId p);

// ---- Locating yourself on the mesh ---------------------------------------------------------------

// The poly containing (x,z), preferring the one whose plane is nearest `y`. Y MATTERS: under a
// balcony, the topmost containing poly is the balcony, and routing from it would be routing from a
// surface the player is not standing on. Pass the player's own Y.
// Returns kNoPoly when no triangle in the cell contains the point.
PolyId FindPolyAt(float x, float y, float z);

// Height of `p`'s plane at (x,z). Only meaningful inside the triangle.
bool PolyHeightAt(PolyId p, float x, float z, float& outY);

// The point of `p`'s triangle closest to (x,z) in the XZ plane, with Y taken from the poly's own
// plane. Inside the triangle this is just (x,z) projected onto the surface; outside it is the
// nearest point on the nearest edge.
//
// This is the piece DQ7R gets from Unreal's `ProjectPointToNavigation` and which we have to supply
// ourselves. It exists so a route to a target the player cannot stand on can end at a point that is
// BOTH on the walkable mesh AND provably inside the engine's interaction range -- the point tested
// and the point walked to being the same is what keeps the two direction keys honest.
bool ClosestPointOnPoly(PolyId p, float x, float z, FVec3& out);

// ---- Connectivity --------------------------------------------------------------------------------

// Can the party cross from `p` into `Neighbor(p,e)`?
//
// Two gates, and only two. The mesh's own adjacency already encodes every height relationship the
// engine honours, so there is deliberately NO step gate and NO slope gate here -- adding either is
// what made stairs unroutable before.
//   1. the neighbour exists and is walkable for class 4;
//   2. the edge is not blocked by a VOLUME.
// Gate 2 is why a raycast survives at all: static volumes (prims 0x4000-0x5000) and dynamic
// obstacles (>= 0x5000, i.e. doors and moving platforms) block movement WITHOUT appearing in floor
// adjacency, so the floor under a closed gate is still adjacent to the floor before it. One
// walk-class segment per expanded edge catches them.
bool EdgePassable(PolyId p, int e, PolyId neighbor);

// The sub-span of the shared edge the party can ACTUALLY cross, as a portal for the string-pull.
// `outA`/`outB` are always filled with something usable -- the full edge when nothing is blocked or
// nothing could be read. Returns false only when NO part of the edge is passable.
//
// This is the difference between "these triangles are adjacent" and "here is the opening". A shared
// edge on this mesh runs 8-16 m; certifying its midpoint and then letting the funnel thread the path
// through its end is how routes came out crossing terrain the party cannot walk. GAME THREAD ONLY --
// it casts walk-class segments.
bool EdgeClearSpan(PolyId p, int e, PolyId neighbor, FVec3& outA, FVec3& outB);

// Flood the walkable component containing `start`. Diagnostic + reachability; `out` is unordered.
// Stops at kMaxPolys. Volume blocking is NOT applied (a flood is about the mesh, not about doors).
int FloodFrom(PolyId start, std::vector<PolyId>& out);

} // namespace NavMesh

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "navigation/nav_types.h"

// ---- Map-jump surfaces: WHERE a transition physically is -----------------------------------------
// The floor polygons the player walks onto to fire a map transition, tagged by the script's
// `setmapidmj` (see NavRva::WALK_POLY_MJ_*). `group` is the map-jump group id, which is also the
// argument the owning `__MJ_CTRL` routine passes to `setmapjumpgroup` -- so this is the half of a
// transition that says WHERE, and the routine is the half that says WHERE TO.
//
// This is the thing five earlier models tried and failed to infer from the map-control blob. It was
// never in the blob: transitions live in the WALKMAP, and interactable doors (shops, stairs) live in
// the scene-object table. Two separate systems; do not use either as evidence about the other.
//
// Split out of map_query.h (Session 93) alongside map_seams.cpp -- one concern, one header, and it took
// map_query.h back under the 150-line cap. The namespace is still MapQuery, so nothing but the include
// list changed for callers.
namespace MapQuery {

constexpr size_t kMaxSurfaceVerts = 256;   // torn-read bound; a real seam is 2-28 polys

struct MapJumpSurface {
    int   group     = 0;
    FVec3 centroid{};        // mean of the tagged polys' base vertices -- the middle of the seam
    FVec3 min{}, max{};      // bounding box, so a recorded crossing can be checked against it
    int   polyCount = 0;
    // All three vertices of every tagged triangle, so a caller can aim at the seam's NEAR EDGE rather
    // than its middle. Southern Plaza's seam is 28 polys spanning z[132.0..140.0], so its centroid
    // overstates the walk by several steps and the route drives through the transition instead of to it.
    std::vector<FVec3> verts;
    // The tagged polys themselves -- navmesh node ids. A route to this exit is a search whose goal set
    // is exactly these, and a reachability answer is whether any of them is in the player's component.
    // Both questions are meaningless against a grid and exact against the mesh.
    std::vector<int>   polys;
};

// One full sweep of the walkmap grid (~15k guarded reads). Prefer the cache below. Empty when there
// is no walkmap.
bool ReadMapJumpSurfaces(std::vector<MapJumpSurface>& out);

// ---- The per-map seam cache: ONE gated writer, many pure readers ---------------------------------
//
// HasWorld() IS A LIVENESS SIGNAL, NOT AN IDENTITY SIGNAL. It says a walkmap is resident; it does NOT
// say the walkmap belongs to the map id you are holding. The map id flips BEFORE the engine swaps the
// walkmap, so a sweep taken the moment the id changed reads the PREVIOUS map's polygons -- and the old
// cache then latched that answer for the whole visit. Garamsythe Waterway served map 311's three seams
// to map 315 and 315's two seams back to 311, which mislabelled every exit, dropped the one whose group
// did not exist on the wrong map, and put another 199 steps away.
//
// So the sweep is not something a reader can trigger. PrimeMapJumpSurfaces is the only writer and runs
// on the GAME THREAD behind PlayerState::IsFieldNavSafe(), and PathPlanner::OnMapTeardown drops the
// answer on the way out, exactly as it drops the navmesh and the reachable set.

// GAME THREAD ONLY, and only from inside a nav-safe frame. Sweeps ONCE PER `epoch`; otherwise a no-op.
// See PathPlanner::OnGameFrame for the single call site, and map_seams.cpp for why the key is the
// teardown epoch and not the map id -- being nav-safe does NOT prove the resident walkmap belongs to the
// id you are holding, which is a correction to this header's own earlier claim (Session 93).
void PrimeMapJumpSurfaces(int mapId, uint32_t epoch);

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

} // namespace MapQuery

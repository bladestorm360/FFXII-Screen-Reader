#pragma once

#include <cstdint>

// Fine walkability grid for routing: a uniform grid (kFineCell) lazily sampled with
// MapQuery::GroundAt against the actual floor MESH and cached per map-epoch. This replaces
// the walkmap's coarse 8 m native cells (a spatial index that false-fails short routes).
// GAME-THREAD ONLY (PlanRoute + OnGameFrame/OnMapTeardown); no locking; never touches the
// EntityList scanner (so it can't cross-acquire that subsystem's mutex).
namespace NavGrid {

constexpr float kFineCell = 1.5f;   // routing cell size, meters

// Clear the cache when the map changes (call before planning). Invalidate frees it on teardown.
void EnsureEpoch(uint32_t epoch);
void Invalidate();

// Uniform world<->cell mapping (kFineCell cells, origin at world 0).
void CellCenter(int col, int row, float& wx, float& wz);
void WorldToCell(float wx, float wz, int& col, int& row);

// Per-cell walkability + floor height, lazily sampled (GroundAt at the cell center) and cached
// per map-epoch. Fills floorY. Cache hits are free; a miss costs one GroundAt.
bool WalkableAt(int col, int row, float& floorY);

// Diagnostics.
int CachedCells();
int SamplesThisMap();

} // namespace NavGrid

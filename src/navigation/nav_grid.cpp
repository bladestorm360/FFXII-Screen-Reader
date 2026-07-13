#include "navigation/nav_grid.h"
#include "navigation/map_query.h"

#include <unordered_map>
#include <cmath>

namespace NavGrid {

namespace {

struct Cell { bool walk; float y; };
std::unordered_map<int64_t, Cell> g_cache;   // fine cell -> walkability (per map-epoch)
uint32_t g_epoch   = 0xFFFFFFFFu;
int      g_samples = 0;

inline int64_t Key(int c, int r) {
    return static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(c)) << 32 |
                                static_cast<uint32_t>(r));
}

} // namespace

void EnsureEpoch(uint32_t epoch) {
    if (epoch != g_epoch) { g_cache.clear(); g_epoch = epoch; g_samples = 0; }
}

void Invalidate() { g_cache.clear(); g_epoch = 0xFFFFFFFFu; g_samples = 0; }

void CellCenter(int col, int row, float& wx, float& wz) {
    wx = (static_cast<float>(col) + 0.5f) * kFineCell;
    wz = (static_cast<float>(row) + 0.5f) * kFineCell;
}

void WorldToCell(float wx, float wz, int& col, int& row) {
    col = static_cast<int>(std::floor(wx / kFineCell));
    row = static_cast<int>(std::floor(wz / kFineCell));
}

bool WalkableAt(int col, int row, float& floorY) {
    const int64_t k = Key(col, row);
    auto it = g_cache.find(k);
    if (it != g_cache.end()) { floorY = it->second.y; return it->second.walk; }
    float wx, wz;
    CellCenter(col, row, wx, wz);
    float y = 0.0f;
    const bool w = MapQuery::GroundAt(wx, wz, y);   // fine floor-mesh sample (raycast)
    g_cache.emplace(k, Cell{ w, y });
    ++g_samples;
    floorY = y;
    return w;
}

int CachedCells() { return static_cast<int>(g_cache.size()); }
int SamplesThisMap() { return g_samples; }

} // namespace NavGrid

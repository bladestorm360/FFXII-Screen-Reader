#include "navigation/map_route_rules.h"

namespace MapRouteRules {
namespace {

struct Row {
    uint32_t mapId;
    // Class-refused ground is a CUT on this map, not a price, and the march grazes nothing across it.
    // Evidence for 184: the 09-16 log's routes paid terrain=4000 / 12000 through the falls and the player
    // recorded three blocked spots walking into the water; the user ruled any water crossing there invalid.
    bool     refuseTerrain;
};

constexpr Row kRows[] = {
    { 184u, true },   // Sochen Cave Palace: Falls of Time (rui_a01) -- user ruling 2026-09-17
};

} // namespace

bool MapRefusesTerrain(uint32_t mapId) {
    for (const Row& r : kRows)
        if (r.mapId == mapId) return r.refuseTerrain;
    return false;
}

} // namespace MapRouteRules

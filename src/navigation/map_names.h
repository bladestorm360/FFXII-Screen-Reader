#pragma once

#include <cstdint>
#include <string>

// The game's own planmapname tables: map id -> localized area / region name.
//
// Split out of map_query.h. These are NAME lookups, entirely independent of the walkmap geometry
// map_query owns -- they were only neighbours because both start from a map id.
//
// All SEH-guarded; every resolver returns EMPTY unless the id resolves to an in-range printable
// name, so a stray id is never spoken.
namespace MapNames {

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

} // namespace MapNames

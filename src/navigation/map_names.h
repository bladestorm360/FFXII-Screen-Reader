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

// True when `mapId` has a REAL name, false when its planmapname slot holds the developers' PLACEHOLDER
// text. Some shipped slots are filler, and resolving one produces confident nonsense -- Rabanastre East
// End has two doors whose destinations (293, 294) are correct map ids with filler names, which announced
// as "Pharos at Ridorana: NOT USED" because the placeholder's MapRef record resolves to an unrelated
// region as well.
//
// Detected from the table itself: any name claimed by >= 3 distinct map ids is filler (genuine names
// repeat at most twice in the shipped table). No hardcoded string -- other locales ship other filler --
// and no hardcoded map ids. Scans the table once per session on first call, then answers from a set.
//
// Callers should use this to decide whether to SPEAK a destination, not to hide the thing itself: a door
// whose destination will not resolve is still a door, and hiding it removes the only handle the player
// has on it.
bool HasRealAreaName(int mapId);

} // namespace MapNames

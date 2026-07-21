#include "navigation/map_names.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/logger.h"
#include "core/game_text.h"

#include <Windows.h>
#include <cstdint>
#include <string>

namespace {

// planmapname name getters. BOTH take a plain index and return a codec ptr, but they read DIFFERENT
// tables out of one blob (see NavRva's table-A/B block): FUN_00377b60 = table A (AREA, idx = map id),
// FUN_00377870 = table B (REGION, idx = region index). Pure getters, POD in/out, SEH-guarded.
typedef const uint8_t* (__fastcall* Pfn_AreaName)(unsigned int);
static const uint8_t* CallAreaNameById(Pfn_AreaName fn, unsigned int areaId) {
    __try { return fn(areaId); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Shared decode: run `idx` through the getter at `rva` and return a printable string, else empty.
// Both tables return the SAME empty sentinel (DAT_01ceb638) on an out-of-range index, so an id that
// belongs to the other table degrades to "" rather than to a wrong name.
static std::wstring PlanmapNameVia(uint32_t rva, uint16_t idx) {
    if (idx == NavRva::AREAID_NONE) return std::wstring();
    Pfn_AreaName fn = reinterpret_cast<Pfn_AreaName>(Hooks::ResolveRva(rva));
    if (!fn) return std::wstring();
    const uint8_t* codec = CallAreaNameById(fn, idx);
    if (!codec || reinterpret_cast<const void*>(codec) == Hooks::ResolveRva(NavRva::EMPTY_STRING))
        return std::wstring();
    std::wstring s = GameText::Decode(codec, 128);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// FUN_00377b60(mapId) -> SUB-AREA name (table A, bounds countA=1312). This is the getter the game's own
// map resolver FUN_003c2320 uses for a connection record's dest.
static std::wstring PlanmapAreaName(uint16_t mapId) {
    return PlanmapNameVia(NavRva::MAPAREA_NAME_BY_ID, mapId);
}

// FUN_00377870(regionIdx) -> REGION name (table B, bounds countB=59). Feed it CallMapNameIdx's output,
// never a map id: a map id (279) fails `279 < 59` and yields the empty sentinel.
static std::wstring PlanmapRegionName(uint16_t regionIdx) {
    return PlanmapNameVia(NavRva::MAPREGION_NAME_BY_IDX, regionIdx);
}

// FUN_00264f90(mapId) -> planmapname REGION INDEX (map-master DAT_02099d88, record+6). Pure getter;
// POD in/out; SEH-guarded. The game's own region resolver — the one FUN_003145e0 (the mapjump executor)
// and the HUD use — is FUN_00377870(FUN_00264f90(mapId)).
typedef unsigned short (__fastcall* Pfn_MapNameIdx)(int mapId);
static int CallMapNameIdx(int mapId) {
    Pfn_MapNameIdx fn = reinterpret_cast<Pfn_MapNameIdx>(Hooks::ResolveRva(NavRva::MAP_NAME_INDEX_BY_ID));
    if (!fn) return 0;
    __try { return static_cast<int>(fn(mapId)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
} // namespace

namespace MapNames {

// Localized SUB-AREA name for a map id, e.g. 279 -> "Lower Apartments". This is FUN_00377b60(mapId) —
// planmapname table A, which IS indexed by map id. (It is NOT FUN_00377870: that reads table B and
// bounds-checks against the 59 REGIONS, so a map id always missed and returned "" — the bug that made
// every exit and the map-entry announcement speak the region alone. See NavRva's table-A/B block.)
// Empty unless the id is plausible AND resolves to a printable name, so a stray id is rejected.
std::wstring ResolveAreaName(int mapId) {
    if (mapId <= 0 || mapId > NavRva::MAP_ID_MAX) return std::wstring();
    return PlanmapAreaName(static_cast<uint16_t>(mapId));
}

// Localized REGION name for a map id, e.g. 279 -> "Nalbina Fortress". FUN_00264f90 maps a map id to its
// REGION's planmapname index (PROVEN by the live log: every Nalbina sub-area id resolved to "Nalbina
// Fortress" through this path), then FUN_00377870 renders it from table B.
std::wstring ResolveRegionName(int mapId) {
    if (mapId <= 0 || mapId > NavRva::MAP_ID_MAX) return std::wstring();
    const int nameIdx = CallMapNameIdx(mapId);
    if (nameIdx <= 0) return std::wstring();
    return PlanmapRegionName(static_cast<uint16_t>(nameIdx));
}

// "<region>: <sub-area>" for a map id — e.g. "Nalbina Fortress: Lower Apartments". Falls back to whichever
// half resolves; empty if neither does.
std::wstring ResolveFullAreaName(int mapId) {
    const std::wstring sub = ResolveAreaName(mapId);
    const std::wstring region = ResolveRegionName(mapId);
    if (region.empty()) return sub;
    if (sub.empty() || sub == region) return region;
    return region + L": " + sub;
}

// Current field map id = gameState+0x1044 via FUN_003148f0() (neg -> 0). 0 when no field map is loaded.
int CurrentMapId() {
    typedef int (__fastcall* Pfn_GetMapId)();
    Pfn_GetMapId fn = reinterpret_cast<Pfn_GetMapId>(Hooks::ResolveRva(NavRva::GETMAPID_FIELD));
    if (!fn) return 0;
    __try { int id = fn(); return id > 0 ? id : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
} // namespace MapNames

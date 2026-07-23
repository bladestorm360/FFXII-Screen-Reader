#include "navigation/map_names.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/logger.h"
#include "core/game_text.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

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

// The developers' placeholder string for an unshipped planmapname slot, in the US build.
//
// LOCALE CAVEAT, stated plainly: this is the ONE locale-specific point in the exit feature. The mod
// supports 12 locales and this token is the US one; a JP/FR/DE report of a wrongly-named or wrongly-
// silenced exit should suspect this constant first. It is not an invented label -- it is the game's own
// text, matched exactly -- but it is a per-build string, so the log below prints every shared-name group
// on startup precisely so the filler token for another locale can be read straight off the log (a run of
// otherwise-unrelated map ids sharing one string is the filler) and added here.
//
// WHY NOT DETECT IT BY MULTIPLICITY (the Session 55 attempt, struck): counting how many map ids share a
// name and calling the popular ones filler was refuted by its own first run -- "NOT USED" is shared by
// only 2 ids (293, 294) while the genuine "Aerodrome" is shared by 5 (788/791/794/797/800) and
// "No. 10 Channel" by 3. Multiplicity flags the real names and misses the real placeholder. The earlier
// "filler is shared by 4 ids" evidence came from notes/planmapname_areas.csv, which CONCATENATES the
// area and region tables; within table A the token appears twice.
constexpr wchar_t kPlaceholderNameUS[] = L"NOT USED";

// True when `mapId` has a REAL name in planmapname, as opposed to that placeholder text. Callers use it
// to decide whether to SPEAK a destination -- a door whose destination is a placeholder is still listed
// and routable, it just loses the destination clause (see ScanExits).
bool HasRealAreaName(int mapId) {
    if (mapId <= 0 || mapId > NavRva::MAP_ID_MAX) return false;

    // One-pass shared-name report, log-only, latched once the table actually answers. It no longer DRIVES
    // the decision (see the struck multiplicity note above); it survives because it is what names the
    // filler token on any locale we have not measured.
    static bool s_reported = false;
    if (!s_reported) {
        std::map<std::wstring, std::vector<int>> byName;
        for (int id = 1; id <= NavRva::MAP_ID_MAX; ++id) {
            std::wstring n = PlanmapAreaName(static_cast<uint16_t>(id));
            if (n.empty()) continue;
            byName[n].push_back(id);
        }
        if (byName.empty()) {   // called mid-transition; the getter returned the empty sentinel for all
            const std::wstring n = ResolveAreaName(mapId);
            return !n.empty() && n != kPlaceholderNameUS;
        }
        s_reported = true;
        int groups = 0;
        for (const auto& kv : byName) {
            if (kv.second.size() < 2) continue;
            ++groups;
            const bool filler = (kv.first == kPlaceholderNameUS);
            char n8[64] = {};
            for (size_t k = 0; k < kv.first.size() && k < 63; ++k)
                n8[k] = (kv.first[k] > 0 && kv.first[k] < 128) ? static_cast<char>(kv.first[k]) : '?';
            std::string ids;
            for (int id : kv.second) { ids += " "; ids += std::to_string(id); }
            char m[224];
            snprintf(m, sizeof(m), "planmapname: \"%s\" claimed by %zu ids:%s%s",
                     n8, kv.second.size(), ids.c_str(), filler ? "   <== PLACEHOLDER (dropped from speech)" : "");
            Log::Write("NAV-DIAG", m);
        }
        char sum[144];
        snprintf(sum, sizeof(sum),
                 "planmapname: %zu named ids, %d shared-name groups (placeholder token = \"NOT USED\")",
                 byName.size(), groups);
        Log::Write("NAV-DIAG", sum);
    }

    const std::wstring name = ResolveAreaName(mapId);
    return !name.empty() && name != kPlaceholderNameUS;
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

#include "navigation/map_query.h"
#include "navigation/nav_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "core/logger.h"
#include "core/game_text.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

namespace {

// FUN_003208c0(x, z, outY) -> bool (AL). MS x64: x=XMM0, z=XMM1, outY=R8.
typedef uint8_t(__fastcall* Pfn_GroundAt)(float x, float z, float* outY);
// FUN_00230b60(ctx0, outHit16, from[4], to[4], mask, flags) -> int (>=0 blocked, <0 clear).
typedef int(__fastcall* Pfn_SegTest)(void* ctx, void* out, const float* from,
                                     const float* to, uint16_t mask, uint32_t flags);

// ctx0 = the field-collision world pointer at DAT_0209a678, valid only when the manager
// gate DAT_0209a670 is set. Memory-only; both reads SEH-guarded via MemRead.
void* Ctx0() {
    if (!MemRead::PtrAt(Hooks::ResolveRva(NavRva::MAP_COLL_GATE), 0)) return nullptr;
    return MemRead::PtrAt(Hooks::ResolveRva(NavRva::MAP_COLL_CTX0), 0);
}

// POD-only SEH scopes: each call dereferences the walkmap; a fault (torn map mid-load /
// mid-teardown) degrades to a safe default rather than crashing.
static bool CallGroundAt(Pfn_GroundAt fn, float x, float z, float* outY) {
    __try { return fn(x, z, outY) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static int CallSegTest(Pfn_SegTest fn, void* ctx, void* out, const float* from,
                       const float* to, uint16_t mask, uint32_t flags) {
    __try { return fn(ctx, out, from, to, mask, flags); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }   // fault -> treat as clear
}

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

// (ResolveAreaName / ResolveRegionName / CurrentMapId are PUBLIC — defined below in namespace MapQuery.
//  They use PlanmapAreaName / PlanmapRegionName + CallMapNameIdx above.)

} // namespace

namespace MapQuery {

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

bool HasWorld() { return Ctx0() != nullptr; }

bool GroundAt(float x, float z, float& outY) {
    if (!HasWorld()) return false;
    Pfn_GroundAt fn = reinterpret_cast<Pfn_GroundAt>(Hooks::ResolveRva(NavRva::MAP_GROUND_AT));
    if (!fn) return false;
    float y = 0.0f;
    if (!CallGroundAt(fn, x, z, &y)) return false;
    outY = y;
    return true;
}

int SegmentHit(const FVec3& from, const FVec3& to, uint16_t mask, uint32_t flags) {
    void* ctx = Ctx0();
    if (!ctx) return -1;   // no world -> clear
    Pfn_SegTest fn = reinterpret_cast<Pfn_SegTest>(Hooks::ResolveRva(NavRva::MAP_SEG_TEST));
    if (!fn) return -1;
    // Equal-Y endpoints (both at from.y): for a vertical wall the query Y is irrelevant,
    // and holding Y flat stops a slope-climbing segment from clipping a rising floor poly.
    const float f[4] = { from.x, from.y, from.z, 1.0f };
    const float t[4] = { to.x,   from.y, to.z,   1.0f };
    float out[4] = {};
    return CallSegTest(fn, ctx, out, f, t, mask, flags);   // >=0 blocked, <0 clear
}

bool SegmentClear(const FVec3& from, const FVec3& to) {
    // Walk query class — matches exactly what the player's/NPCs' own wall feelers block on.
    return SegmentHit(from, to, NavRva::MAP_MASK_WALK, NavRva::MAP_SEG_FLAGS) < 0;
}

// ---- Direct walkmap-grid read -----------------------------------------------

namespace {
inline bool ReadHdrInt(void* base, uint32_t off, int& out) {
    uint32_t u = 0;
    if (!MemRead::SafeReadU32(base, off, &u)) return false;
    out = static_cast<int>(u);
    return true;
}
} // namespace

bool GetGridInfo(WalkGridInfo& out) {
    out = WalkGridInfo{};
    void* ctx = Ctx0();
    if (!ctx) return false;
    void* header = MemRead::PtrAt(ctx, NavRva::WALK_CTX_HEADER);
    if (!header) return false;

    int nCols, nRows, csx, csz, ox, oz;
    if (!ReadHdrInt(header, NavRva::WALK_HDR_NCOLS, nCols)) return false;
    if (!ReadHdrInt(header, NavRva::WALK_HDR_NROWS, nRows)) return false;
    if (!ReadHdrInt(header, NavRva::WALK_HDR_CELL_X, csx)) return false;
    if (!ReadHdrInt(header, NavRva::WALK_HDR_CELL_Z, csz)) return false;
    if (!ReadHdrInt(ctx, NavRva::WALK_CTX_ORIGIN_X, ox)) return false;
    if (!ReadHdrInt(ctx, NavRva::WALK_CTX_ORIGIN_Z, oz)) return false;

    // Sanity-gate against a torn/garbage read: positive dims, integer cell sizes, and the
    // engine's own 16-bit cell-index ceiling. Anything else -> treat as no grid.
    if (nCols <= 0 || nRows <= 0 || csx <= 0 || csz <= 0) return false;
    if (static_cast<int64_t>(nCols) * nRows > static_cast<int64_t>(NavRva::WALK_MAX_CELLS)) return false;

    out.vertArr  = MemRead::PtrAt(ctx, NavRva::WALK_CTX_VERTS);
    out.polyArr  = MemRead::PtrAt(ctx, NavRva::WALK_CTX_POLYS);
    out.csrTable = MemRead::PtrAt(ctx, NavRva::WALK_CTX_CSR);
    out.primList = MemRead::PtrAt(ctx, NavRva::WALK_CTX_PRIMS);
    if (!out.vertArr || !out.polyArr || !out.csrTable || !out.primList) return false;

    out.nCols = nCols; out.nRows = nRows;
    out.cellSizeX = csx; out.cellSizeZ = csz;
    out.originX = ox; out.originZ = oz;
    out.header = header;
    out.valid = true;
    return true;
}

bool WorldToCell(const WalkGridInfo& g, float wx, float wz, int& col, int& row) {
    if (!g.valid || g.cellSizeX <= 0 || g.cellSizeZ <= 0) return false;
    const int gx = static_cast<int>(static_cast<float>(g.originX) + wx);
    int c = gx / g.cellSizeX;
    float gz = static_cast<float>(g.originZ) + wz;
    if (c & 1) gz -= static_cast<float>(g.cellSizeZ / 2);      // odd-column brick stagger (integer, matches game)
    int r = static_cast<int>(gz) / g.cellSizeZ;
    col = c; row = r;
    return (c >= 0 && r >= 0 && c < g.nCols && r < g.nRows);
}

void CellCenter(const WalkGridInfo& g, int col, int row, float& wx, float& wz) {
    const float halfX = static_cast<float>(g.cellSizeX) * 0.5f;
    const float halfZ = static_cast<float>(g.cellSizeZ) * 0.5f;
    wx = static_cast<float>(col * g.cellSizeX) + halfX - static_cast<float>(g.originX);
    float z = static_cast<float>(row * g.cellSizeZ) + halfZ - static_cast<float>(g.originZ);
    if (col & 1) z += static_cast<float>(g.cellSizeZ / 2);     // undo the stagger (integer, matches game)
    wz = z;
}

bool ReadCellFloor(const WalkGridInfo& g, int col, int row, float& outY) {
    if (!g.valid) return false;
    if (col < 0 || row < 0 || col >= g.nCols || row >= g.nRows) return false;
    const int cell = g.nCols * row + col;
    const int cellCount = g.nCols * g.nRows;
    if (cell < 0 || cell + 1 > cellCount) return false;

    uint16_t start = 0, end = 0;
    if (!MemRead::SafeReadU16(g.csrTable, static_cast<uint32_t>(cell) * 2u, &start)) return false;
    if (!MemRead::SafeReadU16(g.csrTable, static_cast<uint32_t>(cell + 1) * 2u, &end)) return false;
    if (end < start) return false;

    float cx, cz;
    CellCenter(g, col, row, cx, cz);

    bool found = false;
    float bestY = 0.0f;
    // Bound the per-cell scan: a torn/garbage CSR range could otherwise spin for ~65k reads
    // per cell x 32k cells. No real cell holds anywhere near this many primitives.
    for (uint32_t k = start; k < end && (k - start) < 256u; ++k) {
        uint16_t prim = 0;
        if (!MemRead::SafeReadU16(g.primList, k * 2u, &prim)) break;
        if (prim >= NavRva::WALK_PRIM_FLOOR_MAX) continue;      // wall / empty -> not a floor
        const uint32_t pbase = static_cast<uint32_t>(prim) * NavRva::WALK_POLY_STRIDE;
        uint32_t flags = 0;
        if (!MemRead::SafeReadU32(g.polyArr, pbase + NavRva::WALK_POLY_FLAGS, &flags)) continue;
        if ((flags & NavRva::WALK_POLY_TYPE_MASK) != 0) continue;   // non-walkable poly type
        float A, B, C; int16_t vi = -1;
        if (!MemRead::SafeReadF32(g.polyArr, pbase + NavRva::WALK_POLY_PLANE_A, &A)) continue;
        if (!MemRead::SafeReadF32(g.polyArr, pbase + NavRva::WALK_POLY_PLANE_B, &B)) continue;
        if (!MemRead::SafeReadF32(g.polyArr, pbase + NavRva::WALK_POLY_PLANE_C, &C)) continue;
        if (!MemRead::SafeReadS16(g.polyArr, pbase + NavRva::WALK_POLY_BASEVERT, &vi)) continue;
        if (vi < 0) continue;
        if (B > -0.001f && B < 0.001f) continue;               // near-vertical: no valid height
        float vx, vy, vz;
        const uint32_t vbase = static_cast<uint32_t>(vi) * NavRva::WALK_VERT_STRIDE;
        if (!MemRead::SafeReadF32(g.vertArr, vbase + 0x00, &vx)) continue;
        if (!MemRead::SafeReadF32(g.vertArr, vbase + 0x04, &vy)) continue;
        if (!MemRead::SafeReadF32(g.vertArr, vbase + 0x08, &vz)) continue;
        const float y = vy + ((vx - cx) * A + (vz - cz) * C) / B;
        if (!found || y > bestY) { bestY = y; found = true; }  // topmost walkable floor
    }
    if (!found) return false;
    outY = bestY;
    return true;
}

bool SegmentTraversable(const FVec3& a, const FVec3& b,
                        float step, float bodyPad, float maxStep, float margin, int& rays, int rayCap) {
    if (!HasWorld()) return true;                              // no world -> treat as clear
    const float dx = b.x - a.x, dz = b.z - a.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    const float s = (step > 0.01f) ? step : 1.0f;
    int n = static_cast<int>(std::ceil(len / s));
    if (n < 1) n = 1;
    // Perpendicular unit * margin (for the lateral clearance rays).
    float pmx = 0.0f, pmz = 0.0f;
    const bool useMargin = margin > 0.0f && len > 1e-4f;
    if (useMargin) { pmx = -dz / len * margin; pmz = dx / len * margin; }

    float prevY = 0.0f, px = a.x, pz = a.z;
    bool havePrev = false;
    for (int i = 0; i <= n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        const float sx = a.x + dx * t, sz = a.z + dz * t;
        if (rays >= rayCap) return false;                     // budget exhausted -> conservative
        ++rays;
        float sy = 0.0f;
        if (!GroundAt(sx, sz, sy)) return false;              // gap / no floor
        if (havePrev && std::fabs(sy - prevY) > maxStep) return false;  // cliff / ledge
        if (havePrev) {
            if (rays >= rayCap) return false;
            ++rays;
            // Wall test on the ~step sub-segment at local floor+bodyPad (short span keeps the
            // SegmentHit Y-flatten harmless); plus +/-margin offset rays for body width.
            const FVec3 f{ px, prevY + bodyPad, pz };
            const FVec3 t2{ sx, sy + bodyPad, sz };
            if (!SegmentClear(f, t2)) return false;
            if (useMargin) {
                if (!SegmentClear(FVec3{ f.x + pmx, f.y, f.z + pmz }, FVec3{ t2.x + pmx, t2.y, t2.z + pmz })) return false;
                if (!SegmentClear(FVec3{ f.x - pmx, f.y, f.z - pmz }, FVec3{ t2.x - pmx, t2.y, t2.z - pmz })) return false;
            }
        }
        prevY = sy; px = sx; pz = sz; havePrev = true;
    }
    return true;
}

// The mapData+0x54 table = the MAP-JUMP POINT table (getmapjumpposbyindex) — the intra-map "Mapjump"
// transitions that move the party between areas. Tester-confirmed by walking into them. See the header
// for why Session 43's "arrival/spawn, not exits" demotion was wrong (FUN_00353490 is
// getmapjumpanglebyindex, not a party-placement call).
// DIAGNOSTIC (log-only): dump a record's raw bytes, then report every u16 field the game's OWN area-name
// resolver (the same FUN_00377b60/FUN_00264f90 chain the "Entering <area>" announcement uses) turns into a
// real area name. The DQ7R idiom: the record stores the destination as a map id; we don't know which field,
// so we resolve them all. The offset that resolves to a NEIGHBOUR area (never the current map, and the same
// offset on every record) is the destination — then we read it directly and speak it. No new probe needed.
static void DiagScanRecordForDest(const char* tag, int idx, void* rec, int nbytes, int here) {
    char hex[3 * 0x20 + 1] = {};
    int p = 0;
    for (int b = 0; b < nbytes && b < 0x20; ++b) {
        uint8_t v = 0; MemRead::SafeReadU8(rec, static_cast<uint32_t>(b), &v);
        p += snprintf(hex + p, sizeof(hex) - static_cast<size_t>(p), "%02x ", v);
    }
    char m[160];
    snprintf(m, sizeof(m), "    %s[%d] raw: %s", tag, idx, hex);
    Log::Write("NAV-DIAG", m);
    for (int o = 0; o + 2 <= nbytes; o += 2) {
        uint16_t v = 0;
        if (!MemRead::SafeReadU16(rec, static_cast<uint32_t>(o), &v)) continue;
        if (v == 0 || v > NavRva::MAP_ID_MAX) continue;
        if (ResolveAreaName(v).empty()) continue;   // resolver rejects non-printable / out-of-range ids
        const std::wstring full = ResolveFullAreaName(v);
        char nm[96] = {};
        for (size_t k = 0; k < full.size() && k < 95; ++k) nm[k] = (full[k] < 128) ? static_cast<char>(full[k]) : '?';
        char line[176];
        snprintf(line, sizeof(line), "      +0x%02x = %u -> \"%s\"%s",
                 o, v, nm, (static_cast<int>(v) == here) ? "  <== CURRENT MAP (self)" : "");
        Log::Write("NAV-DIAG", line);
    }
}

// SEH-guarded bulk copy, isolated so callers can use C++ objects (a function with __try can't also unwind).
static bool SehReadBytes(const void* src, void* dst, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// DIAGNOSTIC (log-only): the field-door DESTINATION is a LITERAL in the map's loaded field script — the
// game runs `mapjump(destMapId, entrance, flags)` encoded as three push-immediates then the native call:
//   4f <destU16> 4f <entU16> 4f <flagsU16> 5d 8d 00   (0x4f = push u16; 0x5d = CALLACTPOPA; native 0x8d).
// Runtime-confirmed (hook_mapjump_trace): Lower Apartments 279 -> mapjump(280,1,0). This scans the loaded
// blob for that pattern and logs each mapjump's dest + resolved name + entrance + preceding bytecode (the
// dispatch that ties a source trigger to a mapjump) so the source-door<->dest pairing can be finalized.
void DiagScanScriptMapjumps() {
    void* containerBase = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!containerBase) return;
    void* mapData = MemRead::PtrAt(containerBase, NavRva::TBL_GUARD_OFF);   // *(u64*)(containerBase+0)
    if (!mapData) return;

    // Read the blob into a local buffer, page by page, stopping at the first unmapped page.
    // WIN_START is 0 (was 0x400): the routine table shows the map's FIRST routines live BELOW +0x400,
    // and the init/setup routine that BINDS each +0x54 trigger to its destination handler is down there.
    // Every prior dump started at +0x400, so we only ever captured the transition EXECUTION, never the
    // wiring that decides which door runs which handler — the one fact the door<->dest pairing needs.
    constexpr size_t WIN_START = 0x0, MAX = 0x18000, PAGE = 0x1000;
    std::vector<uint8_t> buf;
    buf.reserve(MAX);
    uint8_t page[PAGE];
    for (size_t off = WIN_START; off < WIN_START + MAX; off += PAGE) {
        if (!SehReadBytes(static_cast<const char*>(mapData) + off, page, PAGE)) break;
        buf.insert(buf.end(), page, page + PAGE);
    }
    const int here = CurrentMapId();
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "==== field-script mapjump literals  (curMap=%d, scanned 0x%zX bytes) ====",
             here, buf.size());
    Log::Write("NAV-DIAG", hdr);

    int found = 0;
    std::vector<size_t> field0;   // buffer offsets of flags=0 (field-door) mapjump calls -> dispatch dump range
    for (size_t i = 9; i + 3 <= buf.size(); ++i) {
        if (buf[i] != 0x5D || buf[i + 1] != 0x8D || buf[i + 2] != 0x00) continue;   // CALLACTPOPA mapjump(0x8d)
        const bool lit = (buf[i - 9] == 0x4F && buf[i - 6] == 0x4F && buf[i - 3] == 0x4F);
        const uint16_t dest  = static_cast<uint16_t>(buf[i - 8] | (buf[i - 7] << 8));
        const uint16_t ent   = static_cast<uint16_t>(buf[i - 5] | (buf[i - 4] << 8));
        const uint16_t flags = static_cast<uint16_t>(buf[i - 2] | (buf[i - 1] << 8));
        if (lit && flags == 0) field0.push_back(i);
        const uint32_t blobOff = static_cast<uint32_t>(WIN_START + i);
        const std::wstring name = lit ? ResolveFullAreaName(dest) : std::wstring();
        char n8[80] = {};
        for (size_t k = 0; k < name.size() && k < 79; ++k) n8[k] = (name[k] < 128) ? static_cast<char>(name[k]) : '?';
        char m[192];
        snprintf(m, sizeof(m), "  mapjump @+0x%X lit=%d dest=%u (\"%s\") entrance=%u flags=%u%s",
                 blobOff, lit ? 1 : 0, dest, n8, ent, flags,
                 (lit && static_cast<int>(dest) == here) ? "  [self]" : "");
        Log::Write("NAV-DIAG", m);
        // ~0x30 bytes of bytecode preceding the first push — the dispatch that selects this call.
        const size_t cs = (i >= 9 + 0x30) ? i - 9 - 0x30 : 0;
        char ctx[3 * 0x30 + 24] = {};
        int p = snprintf(ctx, sizeof(ctx), "     pre[+0x%zX]: ", WIN_START + cs);
        for (size_t k = cs; k < i - 9 && p < static_cast<int>(sizeof(ctx)) - 3; ++k)
            p += snprintf(ctx + p, sizeof(ctx) - static_cast<size_t>(p), "%02x ", buf[k]);
        Log::Write("NAV-DIAG", ctx);
        if (++found >= 64) { Log::Write("NAV-DIAG", "  (cap 64)"); break; }
    }
    char s[64];
    snprintf(s, sizeof(s), "  mapjump literals found: %d", found);
    Log::Write("NAV-DIAG", s);

    (void)field0;
    // ROUTINE TABLE (blob header[0x18] -> array of (flag<<16 | codeOffset)): identifies each routine's entry
    // point, so the init/main routine that binds a +0x54 trigger to a destination routine can be located.
    // The previous parse reported only TWO entries and that was a BUG, not the data: the first u32 at
    // rtOff is an ENTRY COUNT (a live blob read showed 0x18 = 24 there, then 0x254, 0x00ff0000, ...),
    // so index 0 was really the count, and index 2 (0x00ff0000, low word 0) tripped the "coff == 0 ->
    // stop" guard and truncated the table. Entry layout is not yet pinned, so dump RAW u32s instead of
    // pretending to split flag/offset — the format gets settled offline against the hex dump below.
    uint32_t rtOff = 0;
    MemRead::SafeReadU32(mapData, 0x18, &rtOff);
    if (rtOff > 0x10 && rtOff < 0x18000) {
        uint32_t rtCount = 0;
        MemRead::SafeReadU32(mapData, rtOff, &rtCount);
        char rh[112];
        snprintf(rh, sizeof(rh), "==== routine table @+0x%X  count(word0)=%u  raw u32s ====", rtOff, rtCount);
        Log::Write("NAV-DIAG", rh);
        const int n = (rtCount > 0 && rtCount <= 256) ? static_cast<int>(rtCount) + 8 : 72;
        char rt[256]; int rp = 0;
        for (int i = 0; i < n; ++i) {
            uint32_t e = 0;
            if (!MemRead::SafeReadU32(mapData, rtOff + i * 4, &e)) break;
            rp += snprintf(rt + rp, sizeof(rt) - static_cast<size_t>(rp), "[%d]=%x ", i, e);
            if (rp > 200) { Log::Write("NAV-DIAG", rt); rp = 0; rt[0] = 0; }
        }
        if (rp) Log::Write("NAV-DIAG", rt);
    }
    // WHOLE blob as hex so the ENTIRE field script disassembles offline. Deliberately NOT capped at the
    // routine table / +0x3C00 any more: that old window cut off BOTH ends of what we need — the init
    // routines that wire trigger->handler sit below +0x400, and the per-exit "gateway" records (which
    // carry destMapId + entrance + a pointer into that door's handler) sit around +0x5E00..+0x8100.
    // 0x9000 covers init, all handlers, the routine table, and the gateway records in one pass.
    {
        size_t de = buf.size();
        if (de > 0x9000) de = 0x9000;
        char dh[80];
        snprintf(dh, sizeof(dh), "==== field-script code [+0x%zX .. +0x%zX] ====", WIN_START, WIN_START + de);
        Log::Write("NAV-DIAG", dh);
        for (size_t o = 0; o < de; o += 0x20) {
            char line[3 * 0x20 + 24] = {};
            int p = snprintf(line, sizeof(line), "  +0x%zX: ", WIN_START + o);
            for (size_t k = o; k < o + 0x20 && k < de && p < static_cast<int>(sizeof(line)) - 3; ++k)
                p += snprintf(line + p, sizeof(line) - static_cast<size_t>(p), "%02x ", buf[k]);
            Log::Write("NAV-DIAG", line);
        }
    }
}

void EnumerateMapJumps(const FVec3* playerPos, float maxDist, std::vector<ExitRec>& out, bool logRaw) {
    out.clear();
    using MemRead::PtrAt; using MemRead::SafeReadU16;
    using MemRead::SafeReadU32; using MemRead::SafeReadF32;
    const int here = logRaw ? CurrentMapId() : 0;

    // mapData = *(u64*)(containerBase+0); jump table offset at mapData+0x54, base = mapData + off +
    // reloc (~0). Precondition: *(u16)(mapData)>2.
    void* containerBase = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!containerBase) return;
    uint32_t reloc = 0;
    SafeReadU32(Hooks::ResolveRva(PhyreTypes::MASTERDATA_RELOC_BASE), 0, &reloc);   // _DAT_01f83530 (~0)

    void* mapData = PtrAt(containerBase, NavRva::TBL_GUARD_OFF);   // *(u64*)(containerBase+0)
    uint16_t tag = 0;
    uint32_t tableOff = 0;
    if (mapData) { SafeReadU16(mapData, 0, &tag); SafeReadU32(mapData, NavRva::TBL_EXIT_OFF, &tableOff); }
    const uint32_t exitOff = tableOff + reloc;
    char* exitBase = (mapData && tag > 2 && exitOff != 0)
                         ? static_cast<char*>(mapData) + exitOff : nullptr;
    uint32_t count = 0;
    if (exitBase) SafeReadU32(exitBase, 0, &count);
    const bool countOk = (count >= 1 && count <= NavRva::EXIT_COUNT_MAX);

    if (logRaw) {
        char m[192];
        snprintf(m, sizeof(m),
                 "map-jumps(+0x54) slot0: mapData=%p tag=%u tableOff=0x%X base=%p count=%u ok=%d",
                 mapData, tag, tableOff, static_cast<void*>(exitBase), count, countOk ? 1 : 0);
        Log::Write("NAV-DIAG", m);
    }
    if (!countOk) return;

    for (uint32_t i = 0; i < count; ++i) {
        // float x/y/z/angle at uint-word [i*8 + 1..4] -> byte i*0x20 + {4,8,0xC,0x10}.
        const uint32_t rec = i * 8u;
        float x = 0, y = 0, z = 0, ang = 0;
        bool ok = SafeReadF32(exitBase, (rec + 1) * 4, &x)
               && SafeReadF32(exitBase, (rec + 2) * 4, &y)
               && SafeReadF32(exitBase, (rec + 3) * 4, &z)
               && SafeReadF32(exitBase, (rec + 4) * 4, &ang);

        // Position-only. Destination NAMES come from the in-game map's own resolver (map-screen manager
        // DAT_02b457e0 / FUN_003c0340+FUN_003bf430+FUN_003c2320), pending probe confirmation — NOT from
        // this +0x54 table (its trailer is all-zero) nor the +0x8c table (indexed by a field-sign byte,
        // not the jump index — the 0.5 guess, reverted).
        const bool finite = std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(ang);
        const bool nonZero = !(x == 0.0f && y == 0.0f && z == 0.0f);
        float dist = -1.0f;
        bool nearOk = true;
        if (playerPos && ok && finite) {
            const float dx = x - playerPos->x, dz = z - playerPos->z;
            dist = std::sqrt(dx * dx + dz * dz);
            nearOk = dist <= maxDist;
        }
        // Root-cause de-double: the +0x54 table literally repeats records (runtime: count=3 with records
        // 0 and 1 byte-identical). Drop a record whose pos+angle exactly equals one already emitted — the
        // same physical exit, not a proximity dedup (equality is exact because the bytes are identical).
        bool dupe = false;
        for (const auto& p : out)
            if (p.pos.x == x && p.pos.y == y && p.pos.z == z && p.angle == ang) { dupe = true; break; }
        const bool pass = ok && finite && nonZero && nearOk && !dupe;
        if (logRaw) {
            char m[160];
            snprintf(m, sizeof(m), "  jump[%u] pos=(%.1f,%.1f,%.1f) ang=%.2f d=%.1f dupe=%d pass=%d",
                     i, x, y, z, ang, dist, dupe ? 1 : 0, pass ? 1 : 0);
            Log::Write("NAV-DIAG", m);
            // Record = 0x20 bytes starting at the x float (exitBase + 4 + i*0x20). Scan its fields for the dest.
            DiagScanRecordForDest("jump", static_cast<int>(i),
                                  exitBase + 4 + static_cast<size_t>(i) * NavRva::EXIT_REC_STRIDE,
                                  NavRva::EXIT_REC_STRIDE, here);
        }
        if (!pass) continue;
        ExitRec e;
        e.pos = FVec3{ x, y, z };
        e.angle = ang;
        e.index = static_cast<int>(i);
        out.push_back(e);
    }
}

// ---- The in-game map's OWN exit -> destination resolver (the DQ7R equivalent) ------------------------
// We CALL the game's getters and let the GAME do the lookup on its own live data — it resolves this on
// area entry and keeps it live to draw the field minimap, so there is no "is it loaded?" branch: each
// call is POD-only + SEH-guarded, and a fault mid-load simply yields nothing this pass.
namespace {
typedef int   (__fastcall* Pfn_SubMapIdx)();
typedef int   (__fastcall* Pfn_ConnCount)(unsigned int subMap);
typedef void* (__fastcall* Pfn_ConnRec)(unsigned int subMap, unsigned int i, int* typeOut);

static int CallSubMapIndex() {   // FUN_003bfdf0() — current sub-map index (does map-id -> index itself)
    Pfn_SubMapIdx fn = reinterpret_cast<Pfn_SubMapIdx>(Hooks::ResolveRva(NavRva::MAP_SUBMAP_INDEX));
    if (!fn) return -1;
    __try { return fn(); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static int CallConnCount(unsigned int subMap) {          // FUN_003c0340(subMap) -> exit count
    Pfn_ConnCount fn = reinterpret_cast<Pfn_ConnCount>(Hooks::ResolveRva(NavRva::MAP_CONN_COUNT));
    if (!fn) return 0;
    __try { return fn(subMap); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void* CallConnRec(unsigned int subMap, unsigned int i, int* typeOut) {  // FUN_003bf430 -> record*
    Pfn_ConnRec fn = reinterpret_cast<Pfn_ConnRec>(Hooks::ResolveRva(NavRva::MAP_CONN_RECORD));
    if (!fn) return nullptr;
    __try { return fn(subMap, i, typeOut); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
} // namespace

void EnumerateMapConnections(std::vector<ExitRec>& out, bool logRaw) {
    out.clear();
    const int subMap = CallSubMapIndex();
    if (subMap < 0 || static_cast<uint32_t>(subMap) == NavRva::MAP_SUBMAP_NONE) {
        if (logRaw) Log::Write("NAV-DIAG", "map-conn: no sub-map index (not on a mapped field area)");
        return;
    }
    const int n = CallConnCount(static_cast<unsigned int>(subMap));
    if (logRaw) {
        char m[80];
        snprintf(m, sizeof(m), "map-conn: subMap=%d count=%d", subMap, n);
        Log::Write("NAV-DIAG", m);
    }
    if (n <= 0 || n > 64) return;

    const int here = CurrentMapId();   // the record pointing at the area we're standing in isn't an exit
    for (int i = 0; i < n; ++i) {
        int type = 3;                  // 3 = invalid (the getter's own sentinel)
        void* rec = CallConnRec(static_cast<unsigned int>(subMap), static_cast<unsigned int>(i), &type);
        if (!rec || type == 3) continue;

        uint16_t dest = 0; int16_t mx = 0, my = 0;
        MemRead::SafeReadU16(rec, NavRva::CONNREC_DEST_OFF, &dest);
        MemRead::SafeReadS16(rec, NavRva::CONNREC_MAPX_OFF, &mx);
        MemRead::SafeReadS16(rec, NavRva::CONNREC_MAPY_OFF, &my);
        // dest = destination planmapname MAP ID (PROVEN live). "<region>: <sub-area>".
        std::wstring name = ResolveFullAreaName(static_cast<int>(dest));

        if (logRaw) {
            // Log BOTH halves so the sub-area/region split stays verifiable from the log alone (getting
            // them backwards is what made every exit announce as "Nalbina Fortress").
            const std::wstring sub = ResolveAreaName(static_cast<int>(dest));
            const std::wstring reg = ResolveRegionName(static_cast<int>(dest));
            char s8[48] = {}, r8[48] = {};
            for (size_t k = 0; k < sub.size() && k < 47; ++k) s8[k] = (sub[k] < 128) ? static_cast<char>(sub[k]) : '?';
            for (size_t k = 0; k < reg.size() && k < 47; ++k) r8[k] = (reg[k] < 128) ? static_cast<char>(reg[k]) : '?';
            char m[208];
            snprintf(m, sizeof(m), "  conn[%d] type=%d atlasPos=(%d,%d) dest=%u sub=\"%s\" region=\"%s\"",
                     i, type, mx, my, dest, s8, r8);
            Log::Write("NAV-DIAG", m);
        }
        if (name.empty()) continue;                                   // unresolvable -> stay silent
        if (here > 0 && static_cast<int>(dest) == here) continue;     // that's the area we're in

        ExitRec e;
        e.pos = FVec3{ static_cast<float>(mx), 0.0f, static_cast<float>(my) };  // MAP-SPACE, not world
        e.index = i;
        e.areaId = dest;
        e.destName = name;
        e.usable = (type != NavRva::CONNREC_TYPE_DOOR);
        out.push_back(e);
    }
}

// (EnumerateExitDestinations / the +0x8c-by-jump-index dest resolver were REMOVED: that table is indexed
//  by a field-sign record's +0x1d byte, NOT the jump index — no code pairs +0x8c[i] with +0x54[i] (0.5,
//  wrong). The +0x70 field-sign enumerator below is that key, taken from the game's own getters.)

// ---- Map EXITS = the field-sign array at mapData+0x70, via the game's own getters --------------------
// The ONLY structure carrying a world position AND a destination on ONE record, so it is the only source
// that can speak "Exit, <region>: <sub-area>, <steps> <bearing>" without joining unrelated tables.
//
// ABI — the bug that hid this table for four sessions: FUN_00264ac0 / FUN_00264ae0 take a GROUP index in
// ECX. Ghidra prints them "(void)" because FUN_00264ac0 never WRITES ecx, it forwards its own incoming
// ecx to FUN_00264ae0. Declaring the count getter as int(*)() passed junk in rcx, FUN_00264ae0's
// `param_1 < groupCount` bounds check failed, and the count came back 0 on EVERY map — which is what
// "+0x70 is empty on interiors" actually measured. These typedefs take the group explicitly.
namespace {
typedef int   (__fastcall* Pfn_ExitCount)(unsigned int group);          // FUN_00264ac0(group)
typedef void* (__fastcall* Pfn_ExitTable)(unsigned int group);          // FUN_00264ae0(group)
typedef void* (__fastcall* Pfn_ExitObj)(unsigned int group, int i);     // FUN_002649b0(group, i)
typedef int   (__fastcall* Pfn_ExitDestInfo)(void* rec, void* buf);     // FUN_002648f0(rec, buf)

static int CallExitCount(unsigned int group) {
    Pfn_ExitCount fn = reinterpret_cast<Pfn_ExitCount>(Hooks::ResolveRva(NavRva::MAPEXIT_COUNT));
    if (!fn) return 0;
    __try { return fn(group); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void* CallExitTable(unsigned int group) {        // group sub-table base ([0]=count), null if none
    Pfn_ExitTable fn = reinterpret_cast<Pfn_ExitTable>(Hooks::ResolveRva(NavRva::MAPEXIT_TABLE_BY_GROUP));
    if (!fn) return nullptr;
    __try { return fn(group); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static void* CallExitObj(unsigned int group, int i) {   // null = not DRAWN right now (render gate) — diag only
    Pfn_ExitObj fn = reinterpret_cast<Pfn_ExitObj>(Hooks::ResolveRva(NavRva::MAPEXIT_OBJ_BY_INDEX));
    if (!fn) return nullptr;
    __try { return fn(group, i); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static bool CallExitDestInfo(void* rec, void* buf) {    // buf: b0 usable, u16 areaId @+4
    Pfn_ExitDestInfo fn = reinterpret_cast<Pfn_ExitDestInfo>(Hooks::ResolveRva(NavRva::MAPEXIT_DESTINFO));
    if (!fn) return false;
    __try { return fn(rec, buf) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

void EnumerateFieldSignExits(std::vector<ExitRec>& out, bool logRaw) {
    out.clear();
    const int here = CurrentMapId();

    for (unsigned int g = 0; g < NavRva::MAPEXIT_GROUP_MAX; ++g) {
        const int n = CallExitCount(g);
        void* tbl = CallExitTable(g);
        if (logRaw && (n != 0 || tbl)) {
            char m[96];
            snprintf(m, sizeof(m), "map-exits(+0x70) group=%u count=%d tbl=%p", g, n, tbl);
            Log::Write("NAV-DIAG", m);
        }
        if (!tbl || n <= 0 || n > static_cast<int>(NavRva::EXIT_COUNT_MAX)) continue;

        for (int i = 0; i < n; ++i) {
            // Walk the record ourselves (FUN_002649b0's own arithmetic) so the RENDER gate can't hide an
            // exit that exists: for group<2 that getter returns null unless the "-> area" arrow is being
            // drawn this instant. We keep its answer only as a diagnostic ("shown").
            void* rec = static_cast<char*>(tbl) + NavRva::MAPEXIT_TBL_HDR +
                        static_cast<size_t>(i) * NavRva::EXIT_REC_STRIDE;
            const bool shown = (CallExitObj(g, i) != nullptr);

            // World position straight off the record — the same floats the game draws the "→ area" marker at.
            float x = 0.0f, y = 0.0f, z = 0.0f;
            if (!MemRead::SafeReadF32(rec, NavRva::EXITREC_X_OFF, &x) ||
                !MemRead::SafeReadF32(rec, NavRva::EXITREC_Y_OFF, &y) ||
                !MemRead::SafeReadF32(rec, NavRva::EXITREC_Z_OFF, &z)) continue;

            // The game fills the destination: rec+0x1d -> FUN_00264870 -> +0x8c record word[5] = map id.
            // This IS the game's own destination resolver — we only hand it the record it needs.
            uint8_t buf[16] = {};
            const bool gotDest = CallExitDestInfo(rec, buf);
            uint16_t areaId = NavRva::AREAID_NONE;
            memcpy(&areaId, buf + NavRva::EXITBUF_AREAID_OFF, sizeof(areaId));
            const bool usable = (buf[NavRva::EXITBUF_USABLE_OFF] != 0);
            uint8_t destIdx = 0;
            MemRead::SafeReadU8(rec, NavRva::EXITREC_DESTGRP_OFF, &destIdx);

            std::wstring name = gotDest ? ResolveFullAreaName(static_cast<int>(areaId)) : std::wstring();

            if (logRaw) {
                // Log EVERY record and why it drops — a silent `continue` is what hid all 3 exits last run.
                const std::wstring sub = ResolveAreaName(static_cast<int>(areaId));
                const std::wstring reg = ResolveRegionName(static_cast<int>(areaId));
                char s8[48] = {}, r8[48] = {};
                for (size_t k = 0; k < sub.size() && k < 47; ++k) s8[k] = (sub[k] < 128) ? static_cast<char>(sub[k]) : '?';
                for (size_t k = 0; k < reg.size() && k < 47; ++k) r8[k] = (reg[k] < 128) ? static_cast<char>(reg[k]) : '?';
                char m[256];
                snprintf(m, sizeof(m),
                         "  exit[g%u.%d] world=(%.1f,%.1f,%.1f) destIdx=%u gotDest=%d areaId=%u usable=%d shown=%d sub=\"%s\" region=\"%s\"",
                         g, i, x, y, z, destIdx, gotDest ? 1 : 0, areaId, usable ? 1 : 0, shown ? 1 : 0, s8, r8);
                Log::Write("NAV-DIAG", m);
                // Scan the full 0x20-byte record for a field that resolves to a neighbour area (the dest).
                DiagScanRecordForDest("sign", static_cast<int>(g) * 100 + i, rec, NavRva::EXIT_REC_STRIDE, here);
            }

            if (areaId == NavRva::AREAID_NONE || name.empty()) continue;   // no honest name -> stay silent
            if (here > 0 && static_cast<int>(areaId) == here) continue;     // leads back into this same area

            ExitRec e;
            e.pos      = FVec3{ x, y, z };            // WORLD — bearing/steps are honest for these
            e.index    = static_cast<int>(g) * 100 + i;
            e.areaId   = areaId;
            e.destName = name;
            e.usable   = usable;
            out.push_back(e);
        }
    }
}

// RETIRED (this session): the mapData+0x70 field-sign array (EnumerateExits + LogExitGroupTableRaw +
// the FUN_00264ac0/002649b0/002648f0 getters) is confirmed EMPTY on every map by two testers, so it
// never produced a destination name. Exit destination names now come from the game's own resolver
// applied to the +0x54 record's trailer inside EnumerateMapJumps above (see that function). The dead
// +0x70 walker and its getters are removed; NavRva keeps the struck offsets on record.

// (EnumerateMarkers removed in Session 44 — the naviicon array DAT_02b45a80 holds only character/unit dots
//  (party/ally/enemy) that duplicate the combatant scan, with no objective/crystal/label source. See
//  nav_rva.h for the retired NAVIICON_*/MARK_* note.)

} // namespace MapQuery

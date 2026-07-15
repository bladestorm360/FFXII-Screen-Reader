#include "navigation/map_query.h"
#include "navigation/nav_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/logger.h"
#include "core/game_text.h"

#include <cstdint>
#include <cstdio>
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

// FUN_00377870(areaId) -> planmapname area-name codec ptr (same getter family as FUN_003778b0, the
// current-area name the mod already calls). Pure getter, POD in/out, SEH-guarded.
typedef const uint8_t* (__fastcall* Pfn_AreaName)(unsigned int);
static const uint8_t* CallAreaNameById(Pfn_AreaName fn, unsigned int areaId) {
    __try { return fn(areaId); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Destination area name for a planmapname area id (0xffff -> none). Empty on any failure.
static std::wstring ResolveAreaName(uint16_t areaId) {
    if (areaId == NavRva::AREAID_NONE) return std::wstring();
    Pfn_AreaName fn = reinterpret_cast<Pfn_AreaName>(Hooks::ResolveRva(NavRva::MAPAREA_NAME_BY_ID));
    if (!fn) return std::wstring();
    const uint8_t* codec = CallAreaNameById(fn, areaId);
    if (!codec || reinterpret_cast<const void*>(codec) == Hooks::ResolveRva(NavRva::EMPTY_STRING))
        return std::wstring();
    std::wstring s = GameText::Decode(codec, 128);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// ---- Real map-exit getters (FUN_00264ac0 / 002649b0 / 002648f0) — pure getters, SEH-guarded.
// They self-gate on "field map loaded", so they are safe to call anytime (return 0/null if no map).
//
// ABI FIX: FUN_00264ac0 takes a GROUP index. Ghidra decompiles it as `FUN_00264ac0(void)` calling
// `FUN_00264ae0()` because it never WRITES ecx — it passes its own incoming ecx straight through, and
// FUN_00264ae0 uses that as a real array index (`if (param_1 < *(int*)groupTable) return
// groupTable[param_1 + 1] + blob;`). The old typedef here took no parameters, so every call left
// whatever junk happened to be in rcx, the bounds check failed, and the count came back 0 on EVERY
// map — which is what "the +0x70 path is dead" was actually measuring. The game's own sign renderer
// FUN_003f9720 passes the same group to the count getter and to FUN_002649b0.
typedef int   (__fastcall* Pfn_ExitCount)(unsigned int group);
typedef void* (__fastcall* Pfn_ExitObj)(unsigned int group, int index);
typedef unsigned long long (__fastcall* Pfn_ExitDestInfo)(void* obj, void* buf);

static int CallExitCount(unsigned int group) {
    Pfn_ExitCount fn = reinterpret_cast<Pfn_ExitCount>(Hooks::ResolveRva(NavRva::MAPEXIT_COUNT));
    if (!fn) return 0;
    __try { return fn(group); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void* CallExitObj(unsigned int group, int index) {
    Pfn_ExitObj fn = reinterpret_cast<Pfn_ExitObj>(Hooks::ResolveRva(NavRva::MAPEXIT_OBJ_BY_INDEX));
    if (!fn) return nullptr;
    __try { return fn(group, index); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static bool CallExitDestInfo(void* obj, void* buf) {
    Pfn_ExitDestInfo fn = reinterpret_cast<Pfn_ExitDestInfo>(Hooks::ResolveRva(NavRva::MAPEXIT_DESTINFO));
    if (!fn) return false;
    __try { fn(obj, buf); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

namespace MapQuery {

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
void EnumerateMapJumps(const FVec3* playerPos, float maxDist, std::vector<ExitRec>& out, bool logRaw) {
    out.clear();
    using MemRead::PtrAt; using MemRead::SafeReadU16;
    using MemRead::SafeReadU32; using MemRead::SafeReadF32;

    // mapData = *(u64*)(containerBase+0); jump table offset at mapData+0x54, base = mapData + off +
    // reloc (~0). Precondition: *(u16)(mapData)>2.
    void* containerBase = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!containerBase) return;
    uint32_t reloc = 0;
    SafeReadU32(Hooks::ResolveRva(NavRva::MAPJUMP_RELOC_BASE), 0, &reloc);   // _DAT_01f83530 (~0)

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
        const bool finite = std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(ang);
        const bool nonZero = !(x == 0.0f && y == 0.0f && z == 0.0f);
        float dist = -1.0f;
        bool nearOk = true;
        if (playerPos && ok && finite) {
            const float dx = x - playerPos->x, dz = z - playerPos->z;
            dist = std::sqrt(dx * dx + dz * dz);
            nearOk = dist <= maxDist;
        }

        // NO destination id here. FUN_00264b90 is the only reader of this table in the entire binary
        // and it touches x/y/z/angle only — record bytes +0x10..+0x1f are read by nothing, so the old
        // destIdx@+0x1d -> mapData+0x8c chain was reading dead bytes (it returned destIdx=0 / areaId
        // 0xffff on every record of every map). Names come from the +0x70 field-sign array instead and
        // are attached by the caller.
        const bool pass = ok && finite && nonZero && nearOk;
        if (logRaw) {
            char m[160];
            snprintf(m, sizeof(m), "  jump[%u] pos=(%.1f,%.1f,%.1f) ang=%.2f dist=%.1f pass=%d",
                     i, x, y, z, ang, dist, pass ? 1 : 0);
            Log::Write("NAV-DIAG", m);
        }
        if (!pass) continue;
        ExitRec e;
        e.pos = FVec3{ x, y, z };
        e.angle = ang;
        e.index = static_cast<int>(i);
        out.push_back(e);
    }
}

// Log the +0x70 group table by DIRECT MEMORY READ — no call, no calling-convention assumption. This is
// the honest answer to "is +0x70 populated?", which has never actually been measured: the only prior
// measurement went through a count getter that was invoked with no group argument (see the ABI note
// above), so it was guaranteed to read garbage and return 0 regardless of the data.
//   blob+0x70 -> u32 rel-offset; groupTable = blob + off + reloc
//   groupTable = [u32 groupCount][u32 groupOff_0][u32 groupOff_1]...   (FUN_00264ae0)
//   group g sub-table = blob + groupOff_g = [u32 count][12B hdr][rec x 0x20]   (FUN_002649b0)
static void LogExitGroupTableRaw(void* mapData, uint32_t reloc) {
    using MemRead::SafeReadU32;
    uint32_t off = 0;
    if (!mapData || !SafeReadU32(mapData, NavRva::TBL_FIELDSIGN_OFF, &off)) {
        Log::Write("NAV-DIAG", "map-exits(+0x70) raw: no mapData");
        return;
    }
    char m[192];
    if (off == 0) {
        Log::Write("NAV-DIAG", "map-exits(+0x70) raw: blob+0x70 == 0 (no field-sign table on this map)");
        return;
    }
    char* groupTable = static_cast<char*>(mapData) + off + reloc;
    uint32_t groupCount = 0;
    SafeReadU32(groupTable, 0, &groupCount);
    snprintf(m, sizeof(m), "map-exits(+0x70) raw: off=0x%X table=%p groupCount=%u",
             off, static_cast<void*>(groupTable), groupCount);
    Log::Write("NAV-DIAG", m);
    if (groupCount == 0 || groupCount > 32) return;
    for (uint32_t g = 0; g < groupCount; ++g) {
        uint32_t groupOff = 0;
        if (!SafeReadU32(groupTable, 4 + g * 4, &groupOff) || groupOff == 0) continue;
        char* sub = static_cast<char*>(mapData) + groupOff;
        uint32_t n = 0;
        SafeReadU32(sub, 0, &n);
        snprintf(m, sizeof(m), "  group[%u] off=0x%X sub=%p count=%u", g, groupOff, static_cast<void*>(sub), n);
        Log::Write("NAV-DIAG", m);
    }
}

// Map exits with DESTINATIONS — the field-sign array at mapData+0x70 (the radar / "→ <area>" gate list).
// Read via the game's own getters, which apply the leader-visibility filter + the ETB indirection; the
// story-gate usability + destination area id come from FUN_002648f0. Each SHOWN exit -> world pos + name.
void EnumerateExits(const FVec3* /*playerPos*/, float /*maxDist*/, std::vector<ExitRec>& out, bool logRaw) {
    out.clear();
    using MemRead::PtrAt; using MemRead::SafeReadU8; using MemRead::SafeReadU32; using MemRead::SafeReadF32;

    // Only when a field map is loaded (slot-0 active bit). The getters self-gate too.
    void* containerBase = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    uint8_t active = 0;
    if (!containerBase || !SafeReadU8(containerBase, NavRva::TBL_ACTIVE_OFF, &active) || (active & 1) == 0) {
        if (logRaw) Log::Write("NAV-DIAG", "map-exits: no field map loaded");
        return;
    }

    if (logRaw) {
        uint32_t reloc = 0;
        SafeReadU32(Hooks::ResolveRva(NavRva::MAPJUMP_RELOC_BASE), 0, &reloc);
        LogExitGroupTableRaw(PtrAt(containerBase, NavRva::TBL_GUARD_OFF), reloc);
    }

    // Walk every group. Group 0 is the one the sign renderer uses for the leader-visibility-filtered
    // list, but the table is group-indexed and higher groups skip the story mask (FUN_002649b0 only
    // applies it for group < 2), so enumerating all of them is what "every exit on this map" means.
    for (unsigned int group = 0; group < NavRva::MAPEXIT_GROUP_MAX; ++group) {
        const int count = CallExitCount(group);
        if (logRaw && count != 0) {
            char m[80];
            snprintf(m, sizeof(m), "map-exits(+0x70): group=%u count=%d", group, count);
            Log::Write("NAV-DIAG", m);
        }
        if (count <= 0 || count > 256) continue;

        for (int i = 0; i < count; ++i) {
            void* obj = CallExitObj(group, i);   // null = not shown now (story/visibility filtered)
            if (!obj) continue;

            float x = 0, y = 0, z = 0, enable = 0;
            SafeReadF32(obj, NavRva::EXITREC_X_OFF, &x);
            SafeReadF32(obj, NavRva::EXITREC_Y_OFF, &y);
            SafeReadF32(obj, NavRva::EXITREC_Z_OFF, &z);
            SafeReadF32(obj, NavRva::EXITREC_ENABLE_OFF, &enable);

            // FUN_002648f0(record, buf) -> FUN_00264920(record[+0x1d] = destIdx, buf).
            // buf: b0 usable, b1..b3 story flags, u16 @+4 = destination area id (signed <0 => none).
            // Chain confirmed in the game's own sign renderer FUN_003f9720.
            unsigned char buf[16] = {};
            const bool infoOk = CallExitDestInfo(obj, buf);
            const uint8_t usable = buf[NavRva::EXITBUF_USABLE_OFF];
            const uint16_t areaId = static_cast<uint16_t>(buf[NavRva::EXITBUF_AREAID_OFF] |
                                                          (buf[NavRva::EXITBUF_AREAID_OFF + 1] << 8));
            std::wstring destName = infoOk ? ResolveAreaName(areaId) : std::wstring();

            const bool finite  = std::isfinite(x) && std::isfinite(z);
            const bool nonZero = !(x == 0.0f && z == 0.0f);
            const bool shown   = enable != 0.0f;
            const bool pass    = infoOk && finite && nonZero;   // surface all SHOWN exits (usable or story-locked)

            if (logRaw) {
                char nm[48] = {};
                for (size_t k = 0; k < destName.size() && k < 47; ++k)
                    nm[k] = (destName[k] < 128) ? static_cast<char>(destName[k]) : '?';
                char m[208];
                snprintf(m, sizeof(m),
                         "  exit[g%u:%d] obj=%p pos=(%.1f,%.1f,%.1f) enable=%.1f usable=%u areaId=%u \"%s\" pass=%d",
                         group, i, obj, x, y, z, enable, usable, areaId, nm, pass ? 1 : 0);
                Log::Write("NAV-DIAG", m);
            }
            if (!pass) continue;
            ExitRec e;
            e.pos = FVec3{ x, y, z };
            e.index = i;
            e.areaId = areaId;
            e.destName = destName;
            e.usable = (usable != 0) && shown;
            out.push_back(e);
        }
    }
}

// (EnumerateMarkers removed in Session 44 — the naviicon array DAT_02b45a80 holds only character/unit dots
//  (party/ally/enemy) that duplicate the combatant scan, with no objective/crystal/label source. See
//  nav_rva.h for the retired NAVIICON_*/MARK_* note.)

} // namespace MapQuery

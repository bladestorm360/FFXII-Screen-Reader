#include "navigation/map_query.h"
#include "navigation/nav_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"

#include <cstdint>
#include <cmath>

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

} // namespace MapQuery

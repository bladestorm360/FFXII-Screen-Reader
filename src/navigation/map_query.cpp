#include "navigation/map_query.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "core/logger.h"
#include "core/game_text.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <mutex>
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


// (ResolveAreaName / ResolveRegionName / CurrentMapId are PUBLIC — defined below in namespace MapQuery.
//  They use PlanmapAreaName / PlanmapRegionName + CallMapNameIdx above.)

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
    // The engine's own cell count, which is the CSR LAYER STRIDE minus one. Falls back to the
    // product when the field reads as garbage; layer 0 (all we read) is unaffected either way.
    int16_t cc = 0;
    out.cellCount = (MemRead::SafeReadS16(ctx, NavRva::WALK_CTX_CELLCOUNT, &cc) && cc > 0)
                        ? static_cast<int>(cc) : (nCols * nRows);
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

namespace {

// Point-in-triangle in the XZ plane -- the test the cell scan was missing, and the reason a single
// Clan Hall column reported "floors" at -2089.09 and +2537.15 (Session 73). A poly's PLANE is
// infinite: evaluating it for a point outside the poly's own triangle extrapolates without limit,
// and a nearly-flat poly (B just over the 0.001 gate) extrapolates fastest of all. The engine never
// does this -- its own cell scan FUN_00231900 calls FUN_002324f0 to reject non-containing polys
// BEFORE it will take a height from one.
//
// Replicated from FUN_002324f0: for each of the three edges (verts at poly +0x10/+0x12/+0x14, next
// = (i+1)%3) build the edge and the vertex->point vector **with Y zeroed**, normalise the edge, and
// take cross(edge, toPoint).y, which FUN_00202e10 shows is `edge.z*toPoint.x - edge.x*toPoint.z`.
// An edge rejects the point when that is <= -0.0001; a degenerate edge cannot reject. Inside == no
// edge rejects.
bool PolyContainsXZDetail(const WalkGridInfo& g, uint32_t pbase, float px, float pz) {
    float vx[3], vz[3];
    for (int i = 0; i < 3; ++i) {
        int16_t vi = -1;
        const uint32_t off = NavRva::WALK_POLY_VERT0 + static_cast<uint32_t>(i) * 2u;
        if (!MemRead::SafeReadS16(g.polyArr, pbase + off, &vi)) return false;
        if (vi < 0) return false;
        const uint32_t vb = static_cast<uint32_t>(vi) * NavRva::WALK_VERT_STRIDE;
        if (!MemRead::SafeReadF32(g.vertArr, vb + 0x00, &vx[i])) return false;
        if (!MemRead::SafeReadF32(g.vertArr, vb + 0x08, &vz[i])) return false;
    }
    for (int i = 0; i < 3; ++i) {
        const int j = (i + 1) % 3;
        float ex = vx[j] - vx[i], ez = vz[j] - vz[i];
        const float len = std::sqrt(ex * ex + ez * ez);
        if (len <= NavRva::WALK_POLY_EDGE_EPS) continue;    // degenerate edge cannot reject
        ex /= len; ez /= len;                               // engine normalises before the cross
        const float crossY = ez * (px - vx[i]) - ex * (pz - vz[i]);
        if (crossY <= -NavRva::WALK_POLY_EDGE_EPS) return false;   // outside this edge
    }
    return true;
}

// Scan a walkmap cell's floor prims and evaluate EVERY walkable (type-0) floor at (evalX,evalZ).
// This is the primitive; ScanTopFloorAt and AllFloorsAt are both thin wrappers over it.
//
// The poly-decode loop is unchanged from the original topmost-only scan -- only what happens with
// each hit differs. `outTopY`/`outTopCos` are tracked unconditionally and independently of the
// layer array, so ScanTopFloorAt's answer is byte-identical to what it was before this split even
// if the layer array overflows. `out`/`maxOut` may be null/0 when only the top is wanted.
// Returns the number of merged layers written; `outRawCount` gets the pre-merge poly count.
size_t ScanFloorsAt(const WalkGridInfo& g, int col, int row, float evalX, float evalZ,
                    FloorLayer* out, size_t maxOut,
                    float* outTopY, float* outTopCos, int* outRawCount) {
    if (outRawCount) *outRawCount = 0;
    if (!g.valid) return 0;
    if (col < 0 || row < 0 || col >= g.nCols || row >= g.nRows) return 0;
    const int cell = g.nCols * row + col;
    const int cellCount = g.nCols * g.nRows;
    if (cell < 0 || cell + 1 > cellCount) return 0;

    uint16_t start = 0, end = 0;
    if (!MemRead::SafeReadU16(g.csrTable, static_cast<uint32_t>(cell) * 2u, &start)) return 0;
    if (!MemRead::SafeReadU16(g.csrTable, static_cast<uint32_t>(cell + 1) * 2u, &end)) return 0;
    if (end < start) return 0;

    bool found = false;
    float bestY = 0.0f, bestCos = 1.0f;
    size_t n = 0;
    int raw = 0;
    // Bound the per-cell scan: a torn/garbage CSR range could otherwise spin for ~65k reads
    // per cell x 32k cells. No real cell holds anywhere near this many primitives.
    for (uint32_t k = start; k < end && (k - start) < 256u; ++k) {
        uint16_t prim = 0;
        if (!MemRead::SafeReadU16(g.primList, k * 2u, &prim)) break;
        if (prim >= NavRva::WALK_PRIM_FLOOR_MAX) continue;      // wall / empty -> not a floor
        const uint32_t pbase = static_cast<uint32_t>(prim) * NavRva::WALK_POLY_STRIDE;
        uint32_t flags = 0;
        if (!MemRead::SafeReadU32(g.polyArr, pbase + NavRva::WALK_POLY_FLAGS, &flags)) continue;
        // Engine equivalent is a BITSET test, `mask >> (flags & 7) & 1` (FUN_00231900); this is
        // that test with mask == 1, i.e. walkable-floor polys only.
        if ((flags & NavRva::WALK_POLY_TYPE_MASK) != 0) continue;   // non-walkable poly type
        float A, B, C; int16_t vi = -1;
        if (!MemRead::SafeReadF32(g.polyArr, pbase + NavRva::WALK_POLY_PLANE_A, &A)) continue;
        if (!MemRead::SafeReadF32(g.polyArr, pbase + NavRva::WALK_POLY_PLANE_B, &B)) continue;
        if (!MemRead::SafeReadF32(g.polyArr, pbase + NavRva::WALK_POLY_PLANE_C, &C)) continue;
        if (!MemRead::SafeReadS16(g.polyArr, pbase + NavRva::WALK_POLY_VERT0, &vi)) continue;
        if (vi < 0) continue;
        // STRICTLY positive, matching FUN_00231890's `0.001 < B`. The old `|B| > 0.001` also let
        // through downward-facing (ceiling) polys, which the engine never treats as ground.
        if (B <= NavRva::WALK_POLY_MIN_B) continue;
        // The point must actually lie INSIDE this poly -- without it the plane extrapolates and the
        // cell reports floors hundreds of units away. Ordered after the cheap gates, as the engine does.
        if (!PolyContainsXZDetail(g, pbase, evalX, evalZ)) continue;
        float vx, vy, vz;
        const uint32_t vbase = static_cast<uint32_t>(vi) * NavRva::WALK_VERT_STRIDE;
        if (!MemRead::SafeReadF32(g.vertArr, vbase + 0x00, &vx)) continue;
        if (!MemRead::SafeReadF32(g.vertArr, vbase + 0x04, &vy)) continue;
        if (!MemRead::SafeReadF32(g.vertArr, vbase + 0x08, &vz)) continue;
        const float y = vy + ((vx - evalX) * A + (vz - evalZ) * C) / B;
        const float nrm = std::sqrt(A * A + B * B + C * C);
        const float cosv = (nrm > 1e-6f) ? ((B < 0.0f ? -B : B) / nrm) : 1.0f;  // |B|/|normal| in [0,1]
        ++raw;

        if (!found || y > bestY) {                             // topmost walkable floor (unchanged)
            bestY = y;
            bestCos = cosv;
            found = true;
        }
        if (!out || maxOut == 0) continue;

        // Merge into an existing level when within kLayerMerge, keeping the highest poly's plane so
        // the top layer always carries exactly the plane ScanTopFloorAt would have chosen. (A later
        // poly can bridge two levels that were kept apart; harmless -- it only ever over-splits.)
        bool merged = false;
        for (size_t i = 0; i < n; ++i) {
            const float d = y - out[i].y;
            if (d > -kLayerMerge && d < kLayerMerge) {
                if (y > out[i].y) { out[i].y = y; out[i].cosSlope = cosv; }
                merged = true;
                break;
            }
        }
        if (merged || n >= maxOut) continue;   // overflow is visible to callers as raw >> n

        size_t ins = n;                        // insertion sort, ascending by height
        while (ins > 0 && out[ins - 1].y > y) { out[ins] = out[ins - 1]; --ins; }
        out[ins].y = y;
        out[ins].cosSlope = cosv;
        ++n;
    }
    if (outRawCount) *outRawCount = raw;
    if (found) {
        if (outTopY)   *outTopY   = bestY;
        if (outTopCos) *outTopCos = bestCos;
    }
    return n;
}

// Evaluate the TOPMOST walkable floor at (evalX,evalZ). Shared by ReadCellFloor (cell centre) and
// GroundInfoAt (arbitrary XZ). When outCosSlope != nullptr it also returns that poly's slope cosine.
// Returns false if the cell holds no walkable floor.
bool ScanTopFloorAt(const WalkGridInfo& g, int col, int row, float evalX, float evalZ,
                    float& outY, float* outCosSlope) {
    float topY = 0.0f, topCos = 1.0f;
    int raw = 0;
    ScanFloorsAt(g, col, row, evalX, evalZ, nullptr, 0, &topY, &topCos, &raw);
    if (raw == 0) return false;
    outY = topY;
    if (outCosSlope) *outCosSlope = topCos;
    return true;
}
} // namespace

size_t AllFloorsAt(const WalkGridInfo& g, int col, int row, float evalX, float evalZ,
                   FloorLayer* out, size_t maxOut, int* outRawCount) {
    return ScanFloorsAt(g, col, row, evalX, evalZ, out, maxOut, nullptr, nullptr, outRawCount);
}

bool ReadCellFloor(const WalkGridInfo& g, int col, int row, float& outY) {
    float cx, cz;
    CellCenter(g, col, row, cx, cz);
    return ScanTopFloorAt(g, col, row, cx, cz, outY, nullptr);
}

bool GroundInfoAt(float x, float z, float& outY, float& outCosSlope) {
    WalkGridInfo g;
    if (!GetGridInfo(g) || !g.valid) return false;
    int col = 0, row = 0;
    WorldToCell(g, x, z, col, row);   // fills col/row even when out of bounds; ScanTopFloorAt bounds-checks
    return ScanTopFloorAt(g, col, row, x, z, outY, &outCosSlope);
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

// ---- Map-jump surfaces (Session 64) --------------------------------------------------------------
// Sweep the walkmap once and group every floor poly by its `setmapidmj` tag. Same CSR -> prim -> poly
// traversal ReadCellFloor uses, with the same 256-prims-per-cell guard; a poly contributes its BASE
// VERTEX position, which is a real point on the surface rather than a grid-cell approximation (East
// End's cells are 8 m, far too coarse to aim at a doorway with).
bool ReadMapJumpSurfaces(std::vector<MapJumpSurface>& out) {
    out.clear();
    WalkGridInfo g;
    if (!GetGridInfo(g) || !g.valid) return false;

    // Polys are shared between cells, so the same one is reached many times; count each once.
    std::vector<uint16_t> seen;
    seen.reserve(256);

    for (int row = 0; row < g.nRows; ++row) {
        for (int col = 0; col < g.nCols; ++col) {
            const int cell = g.nCols * row + col;
            uint16_t start = 0, end = 0;
            if (!MemRead::SafeReadU16(g.csrTable, static_cast<uint32_t>(cell) * 2u, &start)) continue;
            if (!MemRead::SafeReadU16(g.csrTable, static_cast<uint32_t>(cell + 1) * 2u, &end)) continue;
            if (end < start) continue;

            for (uint32_t k = start; k < end && (k - start) < 256u; ++k) {
                uint16_t prim = 0;
                if (!MemRead::SafeReadU16(g.primList, k * 2u, &prim)) break;
                if (prim >= NavRva::WALK_PRIM_FLOOR_MAX) continue;      // wall / empty
                const uint32_t pbase = static_cast<uint32_t>(prim) * NavRva::WALK_POLY_STRIDE;

                uint32_t flags = 0;
                if (!MemRead::SafeReadU32(g.polyArr, pbase + NavRva::WALK_POLY_FLAGS, &flags)) continue;
                if ((flags & NavRva::WALK_POLY_TYPE_MASK) != 0) continue;   // not a walkable floor
                const int group =
                    static_cast<int>((flags >> NavRva::WALK_POLY_MJ_SHIFT) & NavRva::WALK_POLY_MJ_MASK);
                if (group == 0) continue;                                   // ordinary floor

                bool dup = false;
                for (uint16_t sp : seen) if (sp == prim) { dup = true; break; }
                if (dup) continue;
                seen.push_back(prim);

                // ALL THREE vertices, not just vert0. Reading only the base vertex gave every seam a
                // third of its real geometry: Upper Apartments' Highhall came back as 2 polys
                // spanning a 0.3 m depth with a 1.9 m rise, which is not a surface anyone can stand
                // on. Both the spoken distance and the route goal were aimed at that fragment.
                FVec3 v[3];
                bool haveAll = true;
                for (int k = 0; k < 3 && haveAll; ++k) {
                    int16_t vi = -1;
                    const uint32_t voff = NavRva::WALK_POLY_VERT0 + static_cast<uint32_t>(k) * 2u;
                    if (!MemRead::SafeReadS16(g.polyArr, pbase + voff, &vi) || vi < 0) { haveAll = false; break; }
                    const uint32_t vbase = static_cast<uint32_t>(vi) * NavRva::WALK_VERT_STRIDE;
                    haveAll = MemRead::SafeReadF32(g.vertArr, vbase + 0x00, &v[k].x)
                           && MemRead::SafeReadF32(g.vertArr, vbase + 0x04, &v[k].y)
                           && MemRead::SafeReadF32(g.vertArr, vbase + 0x08, &v[k].z);
                }
                if (!haveAll) continue;

                MapJumpSurface* surf = nullptr;
                for (auto& e : out) if (e.group == group) { surf = &e; break; }
                if (!surf) {
                    out.push_back(MapJumpSurface{});
                    surf = &out.back();
                    surf->group = group;
                    surf->min = surf->max = v[0];
                }
                surf->polys.push_back(static_cast<int>(prim));
                for (int k = 0; k < 3; ++k) {
                    if (surf->verts.size() < kMaxSurfaceVerts) surf->verts.push_back(v[k]);
                    surf->centroid.x += v[k].x; surf->centroid.y += v[k].y; surf->centroid.z += v[k].z;
                    if (v[k].x < surf->min.x) surf->min.x = v[k].x;  if (v[k].x > surf->max.x) surf->max.x = v[k].x;
                    if (v[k].y < surf->min.y) surf->min.y = v[k].y;  if (v[k].y > surf->max.y) surf->max.y = v[k].y;
                    if (v[k].z < surf->min.z) surf->min.z = v[k].z;  if (v[k].z > surf->max.z) surf->max.z = v[k].z;
                }
                ++surf->polyCount;
            }
        }
    }

    for (auto& e : out) {
        if (e.polyCount <= 0) continue;
        const float n = static_cast<float>(e.polyCount) * 3.0f;   // three vertices per triangle
        e.centroid.x /= n; e.centroid.y /= n; e.centroid.z /= n;
    }
    return !out.empty();
}

bool CachedMapJumpSurfaces(int mapId, std::vector<MapJumpSurface>& out) {
    static std::mutex                   s_mutex;
    static std::vector<MapJumpSurface>  s_surf;
    static int                          s_map     = -1;
    static bool                         s_haveMap = false;

    std::lock_guard<std::mutex> lk(s_mutex);
    if (s_map != mapId) { s_map = mapId; s_haveMap = false; s_surf.clear(); }
    // Nothing is cached until the walkmap is actually up: the first scan of a new map runs on its
    // first frame, before the collision context exists, and caching an empty answer there would pin
    // the map to "no exits" for as long as it stays loaded.
    if (!s_haveMap && HasWorld()) {
        ReadMapJumpSurfaces(s_surf);
        s_haveMap = true;
    }
    out = s_surf;
    return !out.empty();
}

bool PolyContainsXZ(const WalkGridInfo& g, uint32_t polyBase, float px, float pz) {
    return PolyContainsXZDetail(g, polyBase, px, pz);
}

bool NearestPointOnSurface(const MapJumpSurface& s, const FVec3& from, FVec3& out) {
    if (s.verts.empty()) return false;
    float bestD2 = -1.0f;
    for (const FVec3& v : s.verts) {
        const float dx = v.x - from.x, dz = v.z - from.z;   // XZ only: a seam's Y is its own floor
        const float d2 = dx * dx + dz * dz;
        if (bestD2 < 0.0f || d2 < bestD2) { bestD2 = d2; out = v; }
    }
    return bestD2 >= 0.0f;
}

} // namespace MapQuery

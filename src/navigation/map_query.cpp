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
// FUN_00232490(ctx0, pos[4], flag) -> int volume-hit count. Layer mask 6 = CSR layers 1 and 2, which
// is what finally lets routing see WALLS. See nav_rva.h MAP_POINT_IN_VOLUME.
typedef int(__fastcall* Pfn_PointInVolume)(void* ctx, const float* pos, int flag);
// FUN_00230a40(ctx0, s16 polyIdx, s16 moveClass) -> 1 walkable / 0 refused. See nav_rva.h
// MAP_FLOOR_WALKABLE -- this is the test that knows about water.
typedef int(__fastcall* Pfn_FloorWalkable)(void* ctx, int16_t poly, int16_t cls);
// FUN_00232020(rawFlags) -> effective flags. Takes the WORD, not a poly -- no walkmap pointer needed.
typedef uint32_t(__fastcall* Pfn_EffFlags)(uint32_t raw);
// FUN_00230c10(ctx0, outPos[4], from[4], to[4], queryClass, bodyRadius) -> int (0 clear, else blocked).
// Args 5 and 6 land on the stack under MS x64 regardless of type, which is exactly how the engine's
// own call sites lay them out (`local_a8 = class` then `local_a0 = 0x3e8a3d71` at FUN_0032bcc0:52-53).
typedef int(__fastcall* Pfn_BodySweep)(void* ctx, float* outPos, const float* from,
                                       const float* to, uint16_t cls, float radius);

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
static int CallBodySweep(Pfn_BodySweep fn, void* ctx, float* outPos, const float* from,
                         const float* to, uint16_t cls, float radius) {
    __try { return fn(ctx, outPos, from, to, cls, radius); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }    // fault -> treat as clear (never invent a block)
}
static int CallFloorWalkable(Pfn_FloorWalkable fn, void* ctx, int16_t poly, int16_t cls) {
    __try { return fn(ctx, poly, cls); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }    // fault -> walkable (never invent a block)
}
static int CallPointInVolume(Pfn_PointInVolume fn, void* ctx, const float* pos) {
    __try { return fn(ctx, pos, 1); }                     // 1 matches FUN_00231400's own flag slot
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }    // fault -> no volume (never invent a block)
}


// (ResolveAreaName / ResolveRegionName / CurrentMapId are PUBLIC — defined below in namespace MapQuery.
//  They use PlanmapAreaName / PlanmapRegionName + CallMapNameIdx above.)

} // namespace

namespace MapQuery {


bool HasWorld() { return Ctx0() != nullptr; }

// S100, DIAGNOSTIC ONLY -- the `'` probe's dynamic-obstacle diagnostic needs the raw ctx to hand to
// FUN_00231690. Routing code never touches this; every routing entry point resolves it internally.
void* DebugCollisionCtx() { return Ctx0(); }

uint32_t EffectiveFlags(uint32_t raw) {
    Pfn_EffFlags fn = reinterpret_cast<Pfn_EffFlags>(Hooks::ResolveRva(NavRva::MAP_EFFECTIVE_FLAGS));
    if (!fn) return raw;
    __try { return fn(raw); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return raw; }
}

bool FloorWalkable(int polyIdx, uint16_t cls, bool* answered) {
    if (answered) *answered = false;
    if (polyIdx < 0 || polyIdx > 0x7FFF) return false;
    void* ctx = Ctx0();
    if (!ctx) return true;                              // no world -> never invent a block
    Pfn_FloorWalkable fn =
        reinterpret_cast<Pfn_FloorWalkable>(Hooks::ResolveRva(NavRva::MAP_FLOOR_WALKABLE));
    if (!fn) return true;
    if (answered) *answered = true;
    return CallFloorWalkable(fn, ctx, static_cast<int16_t>(polyIdx),
                             static_cast<int16_t>(cls)) != 0;
}

bool PointInVolume(const FVec3& pos, bool* answered) {
    if (answered) *answered = false;
    void* ctx = Ctx0();
    if (!ctx) return false;                             // no world -> never invent a block
    Pfn_PointInVolume fn =
        reinterpret_cast<Pfn_PointInVolume>(Hooks::ResolveRva(NavRva::MAP_POINT_IN_VOLUME));
    if (!fn) return false;
    const float p[4] = { pos.x, pos.y, pos.z, 1.0f };
    if (answered) *answered = true;
    return CallPointInVolume(fn, ctx, p) > 0;
}

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
    // HONOUR to.y. This used to force `t[1] = from.y`, on the theory that a vertical wall does not
    // care about the query Y and that flattening stopped a slope-climbing segment from clipping a
    // rising floor. Both halves were wrong in the same direction, and it was costing real edges.
    //
    // `FUN_0022cc50` -- the per-prim callback this cast drives -- tests type-0 FLOOR triangles
    // unconditionally, so the floor is a blocker like any other. Flattening a segment that should rise
    // therefore runs it UNDER the destination floor, and the cast reports the floor as a wall. That is
    // a false BLOCK on exactly the geometry the mod most needs to cross: NavMesh::StraddleAt computes
    // a proper b.y from the neighbour's centroid plus the body pad, and this line threw it away.
    // (GameArchitecture.md:1651-1656 asserted at 0.98 that class 4 "skips floors (poly records)",
    // which is what licensed the flatten. STRUCK -- it does not.)
    //
    // The engine never flattens either: FUN_0032bcc0:20-23 LIFTS the endpoints instead. So the fix is
    // to pass the Y the caller computed, and the failure mode moves in the PERMISSIVE direction --
    // edges that were wrongly refused become passable, which is the opposite of the islanding risk.
    const float f[4] = { from.x, from.y, from.z, 1.0f };
    const float t[4] = { to.x,   to.y,   to.z,   1.0f };
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

// ---- The engine's OWN body sweep -----------------------------------------------------------------
//
// `FUN_00230c10` (RVA 0x110C10) is the routine the character controller itself uses to answer "may I
// move from here to there", and it is the hard passability check the tester described: push into a
// wall and you make no progress, push obliquely and you slide. Three phases, read line by line:
//   1. a clipped DDA segment walk from -> to (FUN_0022f430 + FUN_0022cc50) with the query class;
//   2. an ellipsoid DEPENETRATION at the resolved point over CSR layers 0|1|2, body = a sphere of
//      radius `arg6`, which records the deepest penetration;
//   3. two probes rotated +/-30 degrees about the travel axis, keeping the SHORTEST reach -- a
//      conservative capsule approximation -- then a pull-back out of the surface along the travel
//      axis by the measured penetration depth.
// Return 0 means the requested displacement is fully legal and `to` may be taken verbatim; non-zero
// means obstructed and `outPos` is where the character actually ends up. All three of the engine's own
// call sites (FUN_0032bcc0:55, FUN_0032beb0:38/:68, FUN_0032ca70:70) pass class 4 and radius
// 0x3e8a3d71 = 0.27f, and use the return exactly that way. Confidence 0.99 on the signature and the
// two constants; they are read off the call sites, not inferred.
//
// WHY THE MOD MAY CALL IT: its complete 29-function call tree writes NOTHING to game memory -- every
// write is to a caller-supplied out buffer or the function's own stack, and every DAT_ reference in the
// tree is an rvalue. That makes it a pure getter in the strict sense the read-only rule requires, on
// the same footing as the MAP_SEG_TEST call SegmentClear already makes. The footprint-aware border test
// (FUN_0022f9b0) is NOT callable -- the footprint only reaches it through globals FUN_0022ef20 writes
// -- so that half is REPLICATED in nav_footprint.cpp instead. Do not "simplify" by calling it.
bool BodySweep(const FVec3& from, const FVec3& to, BodyMove& out) {
    out = BodyMove{};
    out.requested = std::sqrt((to.x - from.x) * (to.x - from.x) + (to.z - from.z) * (to.z - from.z));
    out.reached   = to;
    void* ctx = Ctx0();
    if (!ctx) { out.valid = false; out.achieved = out.requested; out.fraction = 1.0f; return true; }
    Pfn_BodySweep fn = reinterpret_cast<Pfn_BodySweep>(Hooks::ResolveRva(NavRva::MAP_BODY_SWEEP));
    if (!fn) { out.valid = false; out.achieved = out.requested; out.fraction = 1.0f; return true; }

    const float f[4] = { from.x, from.y, from.z, 1.0f };
    const float t[4] = { to.x,   to.y,   to.z,   1.0f };
    float res[4] = {};
    const int blocked = CallBodySweep(fn, ctx, res, f, t, NavRva::MAP_CLASS_PARTY_SEG,
                                      NavRva::MAP_BODY_RADIUS);
    out.valid   = true;
    out.blocked = (blocked != 0);
    if (!out.blocked) {
        out.achieved = out.requested;
        out.fraction = 1.0f;
        return true;
    }
    out.reached = FVec3{ res[0], res[1], res[2] };
    // XZ ONLY, which is the ENGINE's own formula: FUN_0032beb0:46-55 divides |out-from| by |to-from|
    // with the Y term excluded and stores the result at ctrl+0x120 as the fraction of the requested
    // move the character actually got. Including Y here would make a legal step up a ramp read as a
    // partial block.
    const float dx = out.reached.x - from.x, dz = out.reached.z - from.z;
    out.achieved = std::sqrt(dx * dx + dz * dz);
    out.fraction = (out.requested > 1e-4f) ? (out.achieved / out.requested) : 1.0f;
    return false;
}

bool PolyContainsXZ(const WalkGridInfo& g, uint32_t polyBase, float px, float pz) {
    return PolyContainsXZDetail(g, polyBase, px, pz);
}

} // namespace MapQuery

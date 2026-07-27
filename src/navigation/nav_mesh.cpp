#include "navigation/nav_mesh.h"
#include "navigation/map_query.h"
#include "navigation/nav_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"

#include <cmath>
#include <unordered_set>

namespace NavMesh {

namespace {

// The walkmap arrays for the current map, refreshed per epoch. Every read below is a plain memory
// read through MemRead's SEH guards, so a torn structure degrades to "no poly" rather than a fault.
MapQuery::WalkGridInfo g_grid;
bool     g_have  = false;
uint32_t g_epoch = 0xFFFFFFFFu;

bool Grid(MapQuery::WalkGridInfo*& out) {
    if (!g_have) {
        if (!MapQuery::HasWorld()) return false;
        if (!MapQuery::GetGridInfo(g_grid) || !g_grid.valid) return false;
        g_have = true;
    }
    out = &g_grid;
    return true;
}

inline uint32_t PolyBase(PolyId p) {
    return static_cast<uint32_t>(p) * NavRva::WALK_POLY_STRIDE;
}

// A poly index is a s16 in the engine, and the prim encoding reserves >= 0x4000 for volumes, so a
// legitimate floor poly is always in [0, 0x4000).
inline bool ValidPoly(PolyId p) {
    return p >= 0 && p < static_cast<PolyId>(NavRva::WALK_PRIM_FLOOR_MAX);
}

// FUN_00232020: two banks of {mask, value} rewrite a poly's flags. Read straight from the live table
// so a script that opens a gate is reflected the moment it does.
uint32_t EffectiveFlags(uint32_t raw) {
    void* tbl = Hooks::ResolveRva(NavRva::WALK_FLAG_TABLE);
    if (!tbl) return raw;

    const uint32_t ia = (raw >> 13) & 0x1F;
    const uint32_t ic = ((raw >> 3) & 0xF) + NavRva::WALK_FLAG_GROUP_BASE;
    if (ia >= NavRva::WALK_FLAG_ENTRIES || ic >= NavRva::WALK_FLAG_ENTRIES) return raw;

    uint32_t ma = 0, va = 0, mc = 0, vc = 0;
    if (!MemRead::SafeReadU32(tbl, ia * 8u + 0u, &ma)) return raw;
    if (!MemRead::SafeReadU32(tbl, ia * 8u + 4u, &va)) return raw;
    if (!MemRead::SafeReadU32(tbl, ic * 8u + 0u, &mc)) return raw;
    if (!MemRead::SafeReadU32(tbl, ic * 8u + 4u, &vc)) return raw;

    const uint32_t inner = (raw & ~ma) | (va & ma);
    return (vc & mc) | (inner & ~mc);
}

} // namespace

bool Ready() {
    MapQuery::WalkGridInfo* g = nullptr;
    return Grid(g);
}

void EnsureEpoch(uint32_t epoch) {
    if (epoch != g_epoch) { g_epoch = epoch; g_have = false; g_grid = MapQuery::WalkGridInfo{}; }
}

void Invalidate() {
    g_have = false;
    g_epoch = 0xFFFFFFFFu;
    g_grid = MapQuery::WalkGridInfo{};
}

bool PolyVerts(PolyId p, FVec3 out[3]) {
    MapQuery::WalkGridInfo* g = nullptr;
    if (!Grid(g) || !ValidPoly(p)) return false;
    const uint32_t pb = PolyBase(p);
    for (int i = 0; i < 3; ++i) {
        int16_t vi = -1;
        if (!MemRead::SafeReadS16(g->polyArr, pb + NavRva::WALK_POLY_VERT0 + static_cast<uint32_t>(i) * 2u, &vi))
            return false;
        if (vi < 0) return false;
        const uint32_t vb = static_cast<uint32_t>(vi) * NavRva::WALK_VERT_STRIDE;
        if (!MemRead::SafeReadF32(g->vertArr, vb + 0x00, &out[i].x)) return false;
        if (!MemRead::SafeReadF32(g->vertArr, vb + 0x04, &out[i].y)) return false;
        if (!MemRead::SafeReadF32(g->vertArr, vb + 0x08, &out[i].z)) return false;
    }
    return true;
}

bool PolyCentroid(PolyId p, FVec3& out) {
    FVec3 v[3];
    if (!PolyVerts(p, v)) return false;
    out.x = (v[0].x + v[1].x + v[2].x) / 3.0f;
    out.y = (v[0].y + v[1].y + v[2].y) / 3.0f;
    out.z = (v[0].z + v[1].z + v[2].z) / 3.0f;
    return true;
}

bool EdgeMidpoint(PolyId p, int e, FVec3& out) {
    if (e < 0 || e > 2) return false;
    FVec3 v[3];
    if (!PolyVerts(p, v)) return false;
    const int j = (e + 1) % 3;             // edge e runs vert e -> vert (e+1)%3, per FUN_002324f0
    out.x = (v[e].x + v[j].x) * 0.5f;
    out.y = (v[e].y + v[j].y) * 0.5f;
    out.z = (v[e].z + v[j].z) * 0.5f;
    return true;
}

bool EdgePortal(PolyId p, int e, FVec3& a, FVec3& b) {
    if (e < 0 || e > 2) return false;
    FVec3 v[3];
    if (!PolyVerts(p, v)) return false;
    a = v[e];                 // edge e runs vert e -> vert (e+1)%3, per FUN_002324f0
    b = v[(e + 1) % 3];
    return true;
}

PolyId Neighbor(PolyId p, int e) {
    MapQuery::WalkGridInfo* g = nullptr;
    if (!Grid(g) || !ValidPoly(p) || e < 0 || e > 2) return kNoPoly;
    int16_t n = -1;
    const uint32_t off = NavRva::WALK_POLY_NEIGHBOR0 + static_cast<uint32_t>(e) * 2u;
    if (!MemRead::SafeReadS16(g->polyArr, PolyBase(p) + off, &n)) return kNoPoly;
    return ValidPoly(n) ? static_cast<PolyId>(n) : kNoPoly;
}

bool PolyFlags(PolyId p, uint32_t& raw, uint32_t& effective) {
    MapQuery::WalkGridInfo* g = nullptr;
    if (!Grid(g) || !ValidPoly(p)) return false;
    raw = 0;
    if (!MemRead::SafeReadU32(g->polyArr, PolyBase(p) + NavRva::WALK_POLY_FLAGS, &raw)) return false;
    effective = EffectiveFlags(raw);
    return true;
}

bool Walkable(PolyId p) {
    uint32_t raw = 0, eff = 0;
    if (!PolyFlags(p, raw, eff)) return false;
    // Movement class 4 (the party) hits none of FUN_00230a40's per-class branches, so the whole
    // predicate collapses to the type test. See NavRva::WALK_CLASS_PARTY.
    return (eff & NavRva::WALK_POLY_TYPE_MASK) == 0;
}

int MapJumpGroup(PolyId p) {
    uint32_t raw = 0, eff = 0;
    if (!PolyFlags(p, raw, eff)) return 0;
    if ((eff & NavRva::WALK_POLY_TYPE_MASK) != 0) return 0;
    return static_cast<int>((eff >> NavRva::WALK_POLY_MJ_SHIFT) & NavRva::WALK_POLY_MJ_MASK);
}

bool PolyHeightAt(PolyId p, float x, float z, float& outY) {
    MapQuery::WalkGridInfo* g = nullptr;
    if (!Grid(g) || !ValidPoly(p)) return false;
    const uint32_t pb = PolyBase(p);
    float A, B, C;
    if (!MemRead::SafeReadF32(g->polyArr, pb + NavRva::WALK_POLY_PLANE_A, &A)) return false;
    if (!MemRead::SafeReadF32(g->polyArr, pb + NavRva::WALK_POLY_PLANE_B, &B)) return false;
    if (!MemRead::SafeReadF32(g->polyArr, pb + NavRva::WALK_POLY_PLANE_C, &C)) return false;
    if (B <= NavRva::WALK_POLY_MIN_B) return false;          // ceiling; never ground
    int16_t vi = -1;
    if (!MemRead::SafeReadS16(g->polyArr, pb + NavRva::WALK_POLY_VERT0, &vi) || vi < 0) return false;
    const uint32_t vb = static_cast<uint32_t>(vi) * NavRva::WALK_VERT_STRIDE;
    float vx, vy, vz;
    if (!MemRead::SafeReadF32(g->vertArr, vb + 0x00, &vx)) return false;
    if (!MemRead::SafeReadF32(g->vertArr, vb + 0x04, &vy)) return false;
    if (!MemRead::SafeReadF32(g->vertArr, vb + 0x08, &vz)) return false;
    // FUN_00231890 exactly.
    outY = vy + ((vx - x) * A + (vz - z) * C) / B;
    return true;
}

bool ClosestPointOnPoly(PolyId p, float x, float z, FVec3& out) {
    MapQuery::WalkGridInfo* g = nullptr;
    if (!Grid(g) || !ValidPoly(p)) return false;

    float bx = x, bz = z;
    if (!MapQuery::PolyContainsXZ(*g, PolyBase(p), x, z)) {
        // Outside: clamp to the nearest point of the nearest edge. Plain segment projection, three
        // times -- a triangle has no other candidates once the interior is ruled out.
        FVec3 v[3];
        if (!PolyVerts(p, v)) return false;
        float best = -1.0f;
        for (int i = 0; i < 3; ++i) {
            const int j = (i + 1) % 3;
            const float ex = v[j].x - v[i].x, ez = v[j].z - v[i].z;
            const float len2 = ex * ex + ez * ez;
            float t = 0.0f;
            if (len2 > 1e-8f) {
                t = ((x - v[i].x) * ex + (z - v[i].z) * ez) / len2;
                t = (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
            }
            const float px = v[i].x + ex * t, pz = v[i].z + ez * t;
            const float dx = px - x, dz = pz - z;
            const float d2 = dx * dx + dz * dz;
            if (best < 0.0f || d2 < best) { best = d2; bx = px; bz = pz; }
        }
        if (best < 0.0f) return false;
    }

    float py = 0.0f;
    if (!PolyHeightAt(p, bx, bz, py)) return false;
    out = FVec3{ bx, py, bz };
    return true;
}

PolyId FindPolyAt(float x, float y, float z) {
    MapQuery::WalkGridInfo* g = nullptr;
    if (!Grid(g)) return kNoPoly;

    int col = 0, row = 0;
    if (!MapQuery::WorldToCell(*g, x, z, col, row)) return kNoPoly;
    const int cell = g->nCols * row + col;
    if (cell < 0 || cell >= g->nCols * g->nRows) return kNoPoly;

    // CSR layer 0 -- index is just `cell`, because layer 0's stride offset is zero.
    uint16_t start = 0, end = 0;
    if (!MemRead::SafeReadU16(g->csrTable, static_cast<uint32_t>(cell) * 2u, &start)) return kNoPoly;
    if (!MemRead::SafeReadU16(g->csrTable, static_cast<uint32_t>(cell + 1) * 2u, &end)) return kNoPoly;
    if (end < start) return kNoPoly;

    PolyId best = kNoPoly;
    float  bestDy = 0.0f;
    for (uint32_t k = start; k < end && (k - start) < 256u; ++k) {
        uint16_t prim = 0;
        if (!MemRead::SafeReadU16(g->primList, k * 2u, &prim)) break;
        if (prim >= NavRva::WALK_PRIM_FLOOR_MAX) continue;     // volume, not a floor
        const PolyId p = static_cast<PolyId>(prim);

        // Floor FINDING uses RAW flags -- FUN_00231900 deliberately bypasses the override table.
        uint32_t raw = 0;
        if (!MemRead::SafeReadU32(g->polyArr, PolyBase(p) + NavRva::WALK_POLY_FLAGS, &raw)) continue;
        if ((raw & NavRva::WALK_POLY_TYPE_MASK) != 0) continue;
        if (!MapQuery::PolyContainsXZ(*g, PolyBase(p), x, z)) continue;

        float py = 0.0f;
        if (!PolyHeightAt(p, x, z, py)) continue;

        // NEAREST to the query Y, not topmost. Standing under a balcony, the topmost containing poly
        // IS the balcony, and starting a route from it would be starting from a surface the player
        // is not on. This is the whole reason FindPolyAt takes a Y at all.
        const float dy = std::fabs(py - y);
        if (best == kNoPoly || dy < bestDy) { best = p; bestDy = dy; }
    }
    return best;
}

bool EdgePassable(PolyId p, int e, PolyId neighbor) {
    if (!ValidPoly(neighbor)) return false;
    if (!Walkable(neighbor)) return false;

    // Volume check. The mesh's adjacency knows nothing about walls, doors or moving platforms, so
    // the floor under a CLOSED GATE is still adjacent to the floor before it. One walk-class segment
    // is what catches that -- and it is the only raycast left in routing.
    //
    // The segment STRADDLES THE SHARED EDGE rather than running centroid to centroid. Two adjacent
    // triangles can be large, and a long diagonal between their centres passes close to whatever
    // else is nearby -- which is how the old grid's clearance rays islanded doorway cells and made
    // narrow archways report NoPath. A short span across the actual crossing point tests the thing
    // we care about and nothing else.
    FVec3 mid{}, ca{}, cb{};
    if (!EdgeMidpoint(p, e, mid) || !PolyCentroid(p, ca) || !PolyCentroid(neighbor, cb))
        return true;                                    // unreadable -> do not block

    constexpr float kBodyPad = 0.9f;                    // test at body height, not at the feet
    constexpr float kStraddle = 0.25f;                  // fraction of the way toward each centre
    const FVec3 a{ mid.x + (ca.x - mid.x) * kStraddle,
                   mid.y + (ca.y - mid.y) * kStraddle + kBodyPad,
                   mid.z + (ca.z - mid.z) * kStraddle };
    const FVec3 b{ mid.x + (cb.x - mid.x) * kStraddle,
                   mid.y + (cb.y - mid.y) * kStraddle + kBodyPad,
                   mid.z + (cb.z - mid.z) * kStraddle };
    return MapQuery::SegmentClear(a, b);
}

int FloodFrom(PolyId start, std::vector<PolyId>& out) {
    out.clear();
    if (!ValidPoly(start)) return 0;

    std::unordered_set<PolyId> seen;
    std::vector<PolyId> stack;
    seen.insert(start);
    stack.push_back(start);

    while (!stack.empty() && static_cast<int>(out.size()) < kMaxPolys) {
        const PolyId cur = stack.back();
        stack.pop_back();
        out.push_back(cur);
        for (int e = 0; e < 3; ++e) {
            const PolyId n = Neighbor(cur, e);
            if (n == kNoPoly || seen.count(n)) continue;
            if (!Walkable(n)) continue;
            seen.insert(n);
            stack.push_back(n);
        }
    }
    return static_cast<int>(out.size());
}

} // namespace NavMesh

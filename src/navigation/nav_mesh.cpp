#include "navigation/nav_mesh.h"
#include "navigation/map_query.h"
#include "navigation/player_state.h"
#include "navigation/nav_footprint.h"
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

bool ValidPolyId(PolyId p) { return ValidPoly(p); }

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

bool NeighborChecked(PolyId p, int e, PolyId& n) {
    // `Neighbor` conflates "the read failed" with "there is genuinely no neighbour", and for most
    // callers that is fine -- both mean "do not cross". The adjacency MARCH cannot afford it: a torn
    // read reported as a boundary would turn into a breach verdict, i.e. an invented wall. The march
    // must fail OPEN, so it needs the two cases separated.
    n = kNoPoly;
    MapQuery::WalkGridInfo* g = nullptr;
    if (!Grid(g) || !ValidPoly(p) || e < 0 || e > 2) return false;
    int16_t raw = -1;
    const uint32_t off = NavRva::WALK_POLY_NEIGHBOR0 + static_cast<uint32_t>(e) * 2u;
    if (!MemRead::SafeReadS16(g->polyArr, PolyBase(p) + off, &raw)) return false;
    n = ValidPoly(raw) ? static_cast<PolyId>(raw) : kNoPoly;
    return true;
}

bool PolyContains(PolyId p, float x, float z) {
    MapQuery::WalkGridInfo* g = nullptr;
    if (!Grid(g) || !ValidPoly(p)) return false;
    return MapQuery::PolyContainsXZ(*g, PolyBase(p), x, z);
}

bool PolyFlags(PolyId p, uint32_t& raw, uint32_t& effective) {
    MapQuery::WalkGridInfo* g = nullptr;
    if (!Grid(g) || !ValidPoly(p)) return false;
    raw = 0;
    if (!MemRead::SafeReadU32(g->polyArr, PolyBase(p) + NavRva::WALK_POLY_FLAGS, &raw)) return false;
    effective = EffectiveFlags(raw);
    return true;
}

// IS THIS A FLOOR THE ROUTER MAY USE? The type test, and deliberately nothing else.
//
// SESSION 96 ADDED A TERRAIN REFUSAL HERE AND IT IS NOW STRUCK. The addition routed every walkability
// question through `FloorWalkable(poly, class 0)`, whose bit-23 branch was described as "the marker the
// level designer puts on water, lava, bog and out-of-bounds". On map 311 that refused 399 of 690 floor
// prims and cost the tester an exit that had routed for the whole game to that point.
//
// **THE TESTER WALKS THROUGH THAT WATER.** It is ankle-deep -- the game has no swimming, so shallow
// water is ordinary floor with a puddle on it, and a check that refuses it is refusing ground the
// player is demonstrably standing on. Whatever bit 23 marks, "the party cannot go here" is not it, at
// least not for the class we are asking about.
//
// AND THE EVIDENCE IT WAS ADDED ON DID NOT SURVIVE EITHER. It was justified by "three sessions of
// routes through impassable terrain", but [[project-unvalidated-frontier-session95]] had ALREADY found
// and fixed that: `Plan::Frontier` shipped a straight line to a point several polys away with no
// validation at all. A second explanation was stacked on top of a solved problem and only the second
// one broke anything.
//
// The question the router actually needs is "can the character get there", and the instrument for that
// is the engine's own body walk at character scale -- which `path_validate` now runs in 0.5 m steps and
// which is what will refuse deep water, since the engine has to stop the player walking into it. Terrain
// TYPE is a hypothesis about that question; the walk is a measurement of it. `TerrainRefused` below
// keeps the hypothesis under observation without letting it decide anything.
bool Walkable(PolyId p) {
    uint32_t raw = 0, eff = 0;
    if (!PolyFlags(p, raw, eff)) return false;
    return (eff & NavRva::WALK_POLY_TYPE_MASK) == 0;
}

// The engine's per-class floor test -- PROMOTED in Session 100 from hypothesis to validated
// predicate, with both gates closed:
//   * the class chain is proven in the decompile (FUN_002681d0 writes the LEADER class 0 to
//     walkObj+0x80, which IS moveCtx+0x50 via FUN_003db140's +0x30, and FUN_00230a40's class-0
//     branch requires bit 23 CLEAR);
//   * play agrees everywhere it has been asked: the 315 walk stops at the exact poly-23|224 flag
//     boundary, the waded 321 shallows are bit-23-clear, and the NavTrace STANDING-ON-REFUSED
//     tripwire has never fired once.
// CONSUMERS: A*'s terrain PRICE (never a cut -- the S96 lesson stands), the march's accept rule,
// and the frontier's bestNear guard. ONE SUBSET IS CUT, and it is not this test's doing: a floor a
// SCRIPT has closed (raw class bit clear, effective bit set -- ReachGate::ScriptClosedFlags) is the
// engine's own runtime refusal rather than our reading of terrain type, and A* cuts it since S182.
// `NavMesh::Walkable` deliberately stays the permissive TYPE test: wiring the class test into it is the over-refusal lever S96 pulled (it gates flood, goal
// acceptance, EdgePassable and GroundY at once) and must never be pulled again.
bool TerrainRefused(PolyId p) {
    if (!ValidPoly(p)) return false;
    return !MapQuery::FloorWalkable(p, PlayerState::PartyMovementClass());
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

namespace {

// A PORTAL IS AN OPENING, NOT AN EDGE (Session 86).
//
// The mesh's adjacency knows nothing about walls, doors or moving platforms: the floor under a
// CLOSED GATE is still adjacent to the floor before it. A walk-class segment is what catches that.
// But a single probe at the edge's MIDPOINT only certifies the middle of the edge, and on this mesh
// a triangle is often an entire corridor -- the shared edges run 8 to 16 metres. The string-pull
// then threads the taut path through wherever it likes along that edge, which in the field was up to
// **4.8 m** from the one point that had been tested. Certified the middle, walked through the end.
//
// So the edge is sampled along its length, and callers get the sub-span that is actually clear.
//
// The probe STRADDLES the edge rather than running centroid to centroid: two adjacent triangles can
// be large, and a long diagonal between their centres passes close to whatever else is nearby --
// which is how the old grid's clearance rays islanded doorway cells and made narrow archways report
// NoPath. A short span across the crossing point tests the thing we care about and nothing else.
constexpr int kEdgeSamples = 7;

// The probe segment across the shared edge at parameter `t` along it (0 = v[e], 1 = v[e+1]).
bool StraddleAt(PolyId p, int e, PolyId neighbor, float t, FVec3& a, FVec3& b) {
    FVec3 l{}, r{}, ca{}, cb{};
    if (!EdgePortal(p, e, l, r) || !PolyCentroid(p, ca) || !PolyCentroid(neighbor, cb)) return false;
    const FVec3 pt{ l.x + (r.x - l.x) * t, l.y + (r.y - l.y) * t, l.z + (r.z - l.z) * t };
    constexpr float kBodyPad  = 0.9f;                   // test at body height, not at the feet
    constexpr float kStraddle = 0.25f;                  // fraction of the way toward each centre
    a = FVec3{ pt.x + (ca.x - pt.x) * kStraddle,
               pt.y + (ca.y - pt.y) * kStraddle + kBodyPad,
               pt.z + (ca.z - pt.z) * kStraddle };
    b = FVec3{ pt.x + (cb.x - pt.x) * kStraddle,
               pt.y + (cb.y - pt.y) * kStraddle + kBodyPad,
               pt.z + (cb.z - pt.z) * kStraddle };
    return true;
}

inline float SampleT(int i) {
    // Half-offset so a sample never lands exactly on a vertex, where the straddle degenerates
    // against whatever wall meets the triangle there. t runs 0.071 .. 0.929, never 0 or 1.
    //
    // KNOWN GAP, RECORDED SO IT IS NOT RE-DISCOVERED (Session 96). A taut funnel corner is by
    // construction a portal ENDPOINT -- so the string-pull's preferred points are exactly the two
    // positions per portal this deliberately never measures. `EdgeClearSpan` compounds it by handing
    // back the FULL edge whenever every sample is clear.
    //
    // The fix for that is NOT to inset every span here. That is a global geometry change on every
    // route on every map, and this session already spent three of those on plausible stories that
    // broke maps which worked. `path_search`'s repair ladder handles a bad corner on the FAILURE PATH
    // instead, where a mistake costs one route rather than all of them. Revisit only with a measured
    // answer to what the corner is actually hitting -- `PathValidate::Diagnose` prints it.
    return (static_cast<float>(i) + 0.5f) / static_cast<float>(kEdgeSamples);
}

} // namespace

// Crossing points where the body would be pushed off the boundary, and ones sitting inside a collision
// volume, that we let through anyway. Purely counters for the diagnostic; nothing gates on either.
// Declared in the header and PRINTED on the `refused:` line -- see the note there.
int g_tightCrossings  = 0;
int g_volumeCrossings = 0;

void ResetCrossingCounters() { g_tightCrossings = 0; g_volumeCrossings = 0; }

// Does the party's BODY fit across this edge at parameter `t`? TWO questions decide it, and one is
// merely counted:
//   1. can the body move from one side to the other (MapQuery::BodySweep -- the engine's own 0.27 m
//      radius sweep with depenetration, which sees obstacles a zero-width line passes beside, AND
//      which iterates the volume layers itself -- see below);
//   2. does the body FIT at the crossing point without overlapping a boundary of the walkable region
//      (NavFootprint::Clears -- the replica of the engine's own refusal).
//   -- counted only: MapQuery::PointInVolume, which decides nothing here and no longer decides
//      anything anywhere (Session 97).
//
// ~~STRUCK (Session 97): "TEST 1 IS NEW AND IT IS THE ONE THAT MATTERED (Session 96). Tests 2 and 3
// both reason about the FLOOR ... so a wall standing inside a floor triangle passed every check the
// router had."~~
//
// **TEST 2 ALREADY SEES VOLUMES, AND ALWAYS DID.** `FUN_00230c10`'s two ellipsoid push-out passes
// iterate with CSR layer mask **7** -- layers 0, 1 AND 2 -- through `FUN_0022de60`, over the same
// `0x4000`-tagged, `0x90`-stride volume primitives `FUN_00232490` reads. Conf 0.97. The body sweep is
// not a floor-only instrument; the premise above simply was not checked against the decompile.
//
// And test 1 is the COARSER of the two. `FUN_00232490` installs `FUN_0022f8b0`, which tests exactly one
// bit (31) of the merged flags word -- no class, no query class -- where the engine's own movement
// collision reads `merged_flags & 7` against the mover's class: 0 always solid, 1 conditional on bit 30,
// **4 solid only when queryClass != 4, and the party's movers pass 4**, 2/3/5/6/7 never colliding, bit
// 23 a hit that is recorded and does not block. It also hard-excludes the `>= 0x5000` range -- the doors
// and moving platforms. So it counts volumes the party walks through and misses ones that shut.
//
// It stayed a COUNTER here, which is why this file was never the problem. The same predicate was fatal
// in `path_validate.cpp` and refused 16 of 16 routes on map 315 while the sweep objected to none. That
// veto is gone; see WallSuspect there. Keep this one a counter.
bool BodyFitsAt(PolyId p, int e, PolyId neighbor, float t) {
    FVec3 a{}, b{};
    if (!StraddleAt(p, e, neighbor, t, a, b)) return true;      // unreadable -> never invent a block
    // The crossing point itself. **THIS Y IS AT BODY HEIGHT, NOT AT THE SURFACE** -- `StraddleAt` adds
    // `kBodyPad` to both endpoints, so their mean carries it too. The comment here used to claim the
    // opposite ("at the surface rather than at body height ... the pad would only mislead a reader"),
    // which is a statement about code that was never written. Corrected, not changed: `Clears` ignores Y
    // outright (SegDist2XZ is XZ-only), so the only consumer of this Y is the volume counter, and body
    // height is the height that counter WANTS. Noted because `volXing = 0` on map 311 was read as
    // evidence about a probe taken at a different height (Session 96).
    const FVec3 mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };
    // THE VOLUME TEST IS COUNTED, NOT FATAL (Session 96). It was added earlier this session on the
    // premise that walls were the routing problem; the tester then established that walls were never
    // failing -- water was. An unproven test that can only ever SUBTRACT edges has no business gating a
    // hot path when false negatives are the live complaint, and a listed-but-unroutable destination is
    // the worst outcome there is for a player who cannot see what was hidden. Kept as a measurement so
    // we can tell whether it would ever have blocked anything.
    if (MapQuery::PointInVolume(mid)) ++g_volumeCrossings;
    MapQuery::BodyMove mv;
    if (!MapQuery::BodySweep(a, b, mv)) return false;

    // NavFootprint::Clears IS FATAL AGAIN (Session 96, second correction -- and this is a REVERT, not a
    // new idea). It was demoted to a counter earlier this session because with bit-23 newly refusing
    // water, every walkway in a sewer gained a hard border on both sides and map 315 produced 618
    // reachable polys and not one completed route.
    //
    // THE DEMOTION WAS THE WRONG HALF OF THE FIX. It cost A* the only thing that kept it out of gaps
    // the body cannot pass, and the tester lost an exit that had routed fine for the whole game up to
    // that map: A* proposed a corridor through a pinch, the string-pull's chord died in it
    // (`why=sweep stopPoly=97 walk=1`, on good ground with `volXing=0` -- a border push-back, exactly
    // what this test measures), and the breach then banned the only opening.
    //
    // What was actually wrong was the LEVEL, not the test. A `false` here used to DELETE the edge, and
    // deleting edges is what emptied 315. It now makes the edge EXPENSIVE instead -- see
    // kTightPenalty in path_search.cpp. A* avoids a pinch whenever any alternative exists, which is
    // the pre-Session-96 behaviour that worked, and still has a corridor when the pinch is the only
    // way through, which is what the deletion took away.
    if (!NavFootprint::Clears(mid, neighbor, nullptr)) { ++g_tightCrossings; return false; }
    return true;
}

bool EdgePassable(PolyId p, int e, PolyId neighbor) {
    if (!ValidPoly(neighbor)) return false;
    if (!Walkable(neighbor)) return false;

    // ANY clear sample means the party can get through somewhere along this edge -- which is what
    // adjacency should mean. Short-circuits on the first hit, so the common (unobstructed) case still
    // costs one probe. WHERE it is clear is EdgeClearSpan's job; the search only needs to know the
    // crossing exists at all, and answering "is the midpoint clear" instead used to reject a doorway
    // whose middle happened to be blocked.
    //
    // KEEPING "any sample" is what stops the stricter body test from islanding narrow-but-legal
    // geometry -- an archway needs exactly one parameter along its edge where the character fits, which
    // is also the engine's own standard. Making this "every sample" would reintroduce S68/S75's
    // unroutable staircases by a different route.
    for (int i = 0; i < kEdgeSamples; ++i) {
        if (BodyFitsAt(p, e, neighbor, SampleT(i))) return true;
    }
    return false;
}

bool EdgeClearSpan(PolyId p, int e, PolyId neighbor, FVec3& outA, FVec3& outB) {
    // Always leave the caller with a usable portal: the full edge unless we learn better.
    if (!EdgePortal(p, e, outA, outB)) return false;
    if (!ValidPoly(neighbor) || !Walkable(neighbor)) return false;

    bool clear[kEdgeSamples] = {};
    int nClear = 0;
    for (int i = 0; i < kEdgeSamples; ++i) {
        // Same predicate as EdgePassable, deliberately: if the two disagreed, the search would certify a
        // crossing the string-pull then clipped away, or clip to a span the search never validated.
        clear[i] = BodyFitsAt(p, e, neighbor, SampleT(i));
        if (clear[i]) ++nClear;
    }
    if (nClear == 0) return false;                       // nothing gets through here at all
    if (nClear == kEdgeSamples) return true;             // nothing blocked -> hand back the full edge

    // Clip to the LONGEST RUN of clear samples. A run rather than "every clear sample" because a
    // pillar mid-edge leaves two separate gaps, and a portal spanning both would let the funnel
    // thread straight through the pillar -- the very failure this function exists to stop.
    int bestStart = 0, bestLen = 0, curStart = 0, curLen = 0;
    for (int i = 0; i < kEdgeSamples; ++i) {
        if (!clear[i]) { curLen = 0; continue; }
        if (curLen == 0) curStart = i;
        ++curLen;
        if (curLen > bestLen) { bestLen = curLen; bestStart = curStart; }
    }
    // Endpoints are the outermost SAMPLED points of that run, never the interpolated boundary: the
    // obstacle's true edge lies somewhere between the last clear sample and the first blocked one,
    // and this is the conservative end of that interval.
    const FVec3 l = outA, r = outB;
    const float tLo = SampleT(bestStart);
    const float tHi = SampleT(bestStart + bestLen - 1);
    outA = FVec3{ l.x + (r.x - l.x) * tLo, l.y + (r.y - l.y) * tLo, l.z + (r.z - l.z) * tLo };
    outB = FVec3{ l.x + (r.x - l.x) * tHi, l.y + (r.y - l.y) * tHi, l.z + (r.z - l.z) * tHi };
    return true;
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

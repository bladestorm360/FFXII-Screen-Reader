// Map-jump surfaces: the walkmap sweep and its per-map cache.
//
// SPLIT OUT OF map_query.cpp (Session 93), which had reached the 500-line cap with the pathfinder
// work still to land. Declarations moved to map_seams.h beside it, which also took map_query.h back
// under the 150-line cap. The namespace is still MapQuery, so callers only gained an include. The
// seam sweep is a self-contained concern -- one writer, three readers, its own mutex -- which is what
// makes it the natural seam to cut on.

#include "navigation/map_seams.h"
#include "navigation/map_query.h"
#include "navigation/nav_rva.h"
#include "core/mem_read.h"
#include "core/logger.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace MapQuery {

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

// The one cache, and the three entry points that reach it. See the block comment in map_seams.h for
// why the sweep is gated on the caller's frame rather than on HasWorld().
namespace {
std::mutex                   g_seamMutex;
std::vector<MapJumpSurface>  g_seams;
int                          g_seamMap = -1;             // the map these seams were swept FOR
uint32_t                     g_seamEpoch = 0xFFFFFFFFu;  // the teardown epoch they were swept IN
} // namespace

// KEYED ON THE EPOCH, NOT THE MAP ID (Session 93). This used to be `if (g_seamMap == mapId) return;`,
// which made the FIRST nav-safe frame that reported a given id the one that swept -- and at the
// LEADING EDGE of a transition the id has already flipped while the previous map's walkmap is still
// resident. Measured on the tester's Ridorana log: the sweep ran on map 306's polygons and tagged
// them 1101, and the crossing oracle then attributed a crossing to a seam 48-49 m away and printed
// `MISMATCH -- the group->destination binding is WRONG` about a binding that was fine.
//
// That STRIKES the justification this cache shipped with (GameArchitecture.md:405 and the header
// comment in map_seams.h): `IsFieldNavSafe()` is NOT false for the whole of a transition. It is false
// for the middle of one. The epoch, which PathPlanner::OnMapTeardown bumps, is the only signal that
// actually brackets a map -- and map_seams.h's own header already recorded the phenomenon ("the map id
// flips BEFORE the engine swaps the walkmap") without anyone connecting it to this line.
//
// Sweeping once per epoch also means a map reloaded onto its own id re-sweeps, which the id-keyed
// version only got right because OnMapTeardown happened to clear the cache.
void PrimeMapJumpSurfaces(int mapId, uint32_t epoch) {
    if (mapId <= 0) return;                       // 0 = mid-transition, no map to attribute a sweep to
    std::lock_guard<std::mutex> lk(g_seamMutex);
    if (g_seamEpoch == epoch) return;             // already swept in this epoch
    g_seams.clear();
    g_seamMap = -1;
    if (!HasWorld()) return;                      // caller's gate should preclude this; costs nothing if not
    ReadMapJumpSurfaces(g_seams);
    // Tagged with the map it was swept FOR *and* the epoch it was swept IN. Every reader matches on
    // the map id, so the only two answers a reader can get are "this map's seams" and "nothing yet".
    g_seamMap   = mapId;
    g_seamEpoch = epoch;
    char m[144];
    snprintf(m, sizeof(m), "seams: swept %zu group(s) for map %d in epoch %u",
             g_seams.size(), mapId, epoch);
    Log::Write("NAV-ROUTE", m);
}

bool CachedMapJumpSurfaces(int mapId, std::vector<MapJumpSurface>& out) {
    std::lock_guard<std::mutex> lk(g_seamMutex);
    if (g_seamMap != mapId) { out.clear(); return false; }
    out = g_seams;
    // SWEPT-ness, not emptiness. "We have not looked yet" and "we looked and this map has no seams"
    // are different answers and only the second one means a controller with no surface is a genuinely
    // MISSING EXIT worth logging as one.
    return true;
}

void InvalidateMapJumpSurfaces() {
    std::lock_guard<std::mutex> lk(g_seamMutex);
    g_seams.clear();
    g_seamMap   = -1;
    g_seamEpoch = 0xFFFFFFFFu;
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

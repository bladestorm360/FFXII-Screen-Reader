#include "navigation/nav_footprint.h"
#include "navigation/nav_rva.h"

#include <cmath>

namespace NavFootprint {

namespace {

using NavMesh::kNoPoly;

// The engine caps its own visited set at 0x80 polys (FUN_0022f9b0:152) and its deferred-vertex list at
// 0x10 (:144). A body of radius 0.27 m can only overlap a handful of triangles even on a fine mesh, so
// this is a torn-read bound rather than a real limit -- but it is kept at the engine's number so the
// two cannot disagree about a pathological case.
constexpr int kMaxVisit = 128;

// Squared XZ distance from `p` to the segment a->b, and the closest point. Y is ignored throughout:
// FUN_0022f9b0 projects to the ground plane (it multiplies through a matrix that zeroes Y and then
// writes 0.0 into the Y slot outright at :71), because the actor is pinned to the poly plane anyway.
float SegDist2XZ(const FVec3& p, const FVec3& a, const FVec3& b) {
    const float ex = b.x - a.x, ez = b.z - a.z;
    const float len2 = ex * ex + ez * ez;
    float t = 0.0f;
    if (len2 > 1e-8f) {
        t = ((p.x - a.x) * ex + (p.z - a.z) * ez) / len2;
        t = (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
    const float cx = a.x + ex * t, cz = a.z + ez * t;
    const float dx = cx - p.x, dz = cz - p.z;
    return dx * dx + dz * dz;
}

} // namespace

float BodyRadius() { return NavRva::MAP_BODY_RADIUS; }

bool Clears(const FVec3& pos, PolyId poly, float* outMargin) {
    const float r = BodyRadius();
    const float r2 = r * r;
    float worstMargin = 1e9f;
    bool  clear = true;

    // Iterative rather than recursive: same traversal as the engine's tail recursion, but a fixed
    // array cannot blow the game thread's stack on a torn mesh.
    PolyId visit[kMaxVisit];
    int    nVisit = 0;
    if (!NavMesh::ValidPolyId(poly)) { if (outMargin) *outMargin = 0.0f; return true; }
    visit[nVisit++] = poly;

    for (int vi = 0; vi < nVisit; ++vi) {
        const PolyId cur = visit[vi];
        FVec3 v[3];
        if (!NavMesh::PolyVerts(cur, v)) continue;      // torn read -> this triangle contributes nothing

        for (int e = 0; e < 3; ++e) {
            const FVec3& a = v[e];
            const FVec3& b = v[(e + 1) % 3];
            const float d2 = SegDist2XZ(pos, a, b);

            // THE DEMOTION, and it is the whole check. `FUN_0022f9b0:85-88`: a neighbour that exists but
            // fails the class walkability test is set to -1, i.e. made indistinguishable from a map
            // edge. So "there is no floor beyond this edge" and "the floor beyond this edge is not
            // something I may stand on" are ONE condition, which is why walls, cliffs, water and
            // unwalkable ground all come out of a single branch.
            const PolyId n = NavMesh::Neighbor(cur, e);
            const bool hardBorder = (n == kNoPoly) || !NavMesh::Walkable(n);

            if (hardBorder) {
                const float margin = std::sqrt(d2) - r;
                if (margin < worstMargin) worstMargin = margin;
                if (d2 < r2) clear = false;             // body overlaps a boundary -> the engine pushes us off
                continue;
            }

            // Walkable neighbour: the body may extend into it, so its borders count too. Only descend
            // when the body actually reaches this edge -- otherwise a large triangle would drag in the
            // whole component.
            if (d2 >= r2) continue;
            bool seen = false;
            for (int k = 0; k < nVisit; ++k) if (visit[k] == n) { seen = true; break; }
            if (!seen && nVisit < kMaxVisit) visit[nVisit++] = n;
        }
    }

    if (outMargin) *outMargin = (worstMargin > 1e8f) ? 1e9f : worstMargin;
    return clear;
}

} // namespace NavFootprint

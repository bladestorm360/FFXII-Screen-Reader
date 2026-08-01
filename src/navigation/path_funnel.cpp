#include "navigation/path_funnel.h"
#include "navigation/nav_footprint.h"
#include "navigation/nav_mesh.h"

#include <cmath>

namespace PathFunnel {

namespace {

// 2D cross product in the ground plane: > 0 means `c` is counter-clockwise of a->b, i.e. to its LEFT.
inline float TriArea2(const FVec3& a, const FVec3& b, const FVec3& c) {
    return (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
}

// `out` receives start, the corners, and end. Y rides along on the portal vertices, which are real
// mesh vertices sitting on their own surfaces -- so a route up stairs still describes correctly without
// the funnel itself ever reasoning about height.
//
// THE SIGN CONVENTION, AND WHY IT LOOKS "WRONG" AGAINST THE REFERENCE (Session 86).
// The branch structure below is the reference implementation's, but every comparison is NEGATED
// relative to it -- deliberately. `TriArea2` above is the exact negation of the reference's
// `dtTriArea2D`; expand both and the terms cancel:
//     TriArea2(a,b,c)     = (b.x-a.x)(c.z-a.z) - (c.x-a.x)(b.z-a.z)
//     dtTriArea2D(a,b,c)  = (c.x-a.x)(b.z-a.z) - (b.x-a.x)(c.z-a.z)   ==  -TriArea2(a,b,c)
// The reference's `<=0 / >0 / >=0 / <0` had been transcribed VERBATIM onto a helper of the opposite
// sign, which inverted the funnel's entire notion of left and right. That inversion is the thing
// BestPolarity was built to paper over, and it is why the log showed a corner at nearly every portal
// (`corners=11/15`) with the polarity reporting FLIPPED on 75 routes out of 75.
//
// Do NOT "restore" these to match a reference without also negating TriArea2 -- the two have to agree,
// and it is TriArea2's doc comment ("> 0 == LEFT") that is correct for this codebase's frame.
void Funnel(const FVec3& start, const FVec3& end, const std::vector<Portal>& portals,
            std::vector<FVec3>& out, std::vector<int>& outIdx) {
    out.clear();
    outIdx.clear();
    out.push_back(start);
    outIdx.push_back(-1);                 // the start belongs to no portal

    // A corner is only ever a portal endpoint, and adjacent portals SHARE vertices -- so the raw funnel
    // emits exact duplicates. Collapsing them is not cosmetic: a duplicate at index 1/2 gives the
    // passed-waypoint drop a zero-length segment, which trips its `len2 < 1e-6f` guard on the first
    // iteration and silently disables the whole Session 78 leg-0 reversal fix.
    //
    // The index rides in the SAME branch as the point, so a collapsed duplicate cannot desynchronise
    // the two vectors -- which would silently mis-address the corridor when a leg is un-pulled.
    auto pushCorner = [&out, &outIdx](const FVec3& p, size_t i) {
        if (out.empty() || !SameXZ(out.back(), p)) { out.push_back(p); outIdx.push_back(static_cast<int>(i)); }
    };

    FVec3 apex = start, pLeft = start, pRight = start;
    size_t apexIdx = 0, leftIdx = 0, rightIdx = 0;

    for (size_t i = 0; i <= portals.size(); ++i) {
        // The terminal "portal" is the destination collapsed to a point, so the last leg is pulled taut
        // against the real end rather than against the final edge.
        const FVec3 left  = (i < portals.size()) ? portals[i].left  : end;
        const FVec3 right = (i < portals.size()) ? portals[i].right : end;

        // Tighten the RIGHT bound.
        if (TriArea2(apex, pRight, right) >= 0.0f) {
            if (SameXZ(apex, pRight) || TriArea2(apex, pLeft, right) < 0.0f) {
                pRight = right; rightIdx = i;
            } else {
                // Right crossed left: the left bound is a corner. Emit it and restart from there.
                pushCorner(pLeft, leftIdx);
                apex = pLeft; apexIdx = leftIdx;
                pLeft = apex; pRight = apex;
                leftIdx = rightIdx = apexIdx;
                i = apexIdx;                 // ++i makes this apexIdx+1
                continue;
            }
        }
        // Tighten the LEFT bound.
        if (TriArea2(apex, pLeft, left) <= 0.0f) {
            if (SameXZ(apex, pLeft) || TriArea2(apex, pRight, left) > 0.0f) {
                pLeft = left; leftIdx = i;
            } else {
                pushCorner(pRight, rightIdx);
                apex = pRight; apexIdx = rightIdx;
                pLeft = apex; pRight = apex;
                leftIdx = rightIdx = apexIdx;
                i = apexIdx;
                continue;
            }
        }
    }

    // ALWAYS finish on the target itself. `to` is the exact same FVec3 the `/` describe key measures to,
    // so the two keys can never name different destinations (Session 76).
    if (out.empty() || !SameXZ(out.back(), end)) { out.push_back(end); outIdx.push_back(static_cast<int>(portals.size())); }
}

} // namespace

bool SameXZ(const FVec3& a, const FVec3& b) {
    return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.z - b.z) < 1e-4f;
}

float PathLenXZ(const std::vector<FVec3>& pts) {
    float len = 0.0f;
    for (size_t i = 1; i < pts.size(); ++i) {
        const float dx = pts[i].x - pts[i - 1].x, dz = pts[i].z - pts[i - 1].z;
        len += std::sqrt(dx * dx + dz * dz);
    }
    return len;
}

void BestPolarity(const FVec3& start, const FVec3& end, const std::vector<Portal>& portals,
                  std::vector<FVec3>& out, bool& flipped, float& lenKept, float& lenOther,
                  std::vector<int>* outIdx) {
    std::vector<Portal> mirrored;
    mirrored.reserve(portals.size());
    for (const Portal& p : portals) mirrored.push_back(Portal{ p.right, p.left });

    std::vector<FVec3> a, b;
    std::vector<int>   ia, ib;
    // Mirroring swaps left/right WITHIN each portal; it does not reorder them. So a corner's portal
    // index means the same thing in both polarities and the winner's indices travel with it.
    Funnel(start, end, portals,  a, ia);
    Funnel(start, end, mirrored, b, ib);
    const float la = PathLenXZ(a), lb = PathLenXZ(b);

    flipped  = (lb < la);
    out      = flipped ? b : a;
    lenKept  = flipped ? lb : la;
    lenOther = flipped ? la : lb;
    if (outIdx) *outIdx = flipped ? ib : ia;
}

// The midpoint of a portal's CLIPPED span, not of the whole edge: EdgeClearSpan already narrowed each
// portal to the run of samples where the body was measured to fit, so this point is one the body has
// been shown to pass through rather than one we hope it can.
FVec3 SpanMid(const Portal& q) {
    return FVec3{ (q.left.x + q.right.x) * 0.5f,
                  (q.left.y + q.right.y) * 0.5f,
                  (q.left.z + q.right.z) * 0.5f };
}

int Unpull(const std::vector<FVec3>& poly, const std::vector<int>& idx,
           const std::vector<Portal>& portals, size_t badLeg, std::vector<FVec3>& out) {
    out.clear();
    // The mapping has to be intact and the leg has to be a real interior leg. Anything else and we
    // would be splicing against indices that do not describe this polyline.
    if (badLeg == 0 || badLeg >= poly.size() || idx.size() != poly.size()) return 0;

    const int lo = idx[badLeg - 1];
    const int hi = idx[badLeg];

    out.assign(poly.begin(), poly.begin() + static_cast<ptrdiff_t>(badLeg));

    int added = 0;
    for (int k = lo + 1; k < hi && k < static_cast<int>(portals.size()); ++k) {
        if (k < 0) continue;
        const FVec3 m = SpanMid(portals[k]);
        if (out.empty() || !SameXZ(out.back(), m)) { out.push_back(m); ++added; }
    }

    // AND THE CORNER ITSELF (Session 96, second correction). Splicing waypoints only into the APPROACH
    // is a no-op when the unreachable thing IS the corner -- which is what the log showed nine times
    // running: `1 corridor waypoint spliced -> still breaching`, then five, then still breaching. A
    // taut corner is a portal ENDPOINT, and the crossing test deliberately never samples endpoints
    // (`SampleT = (i+0.5)/7`, never 0 or 1), so it is one of exactly two points per portal that were
    // never measured. Its own span midpoint WAS measured.
    //
    // The FINAL point is exempt: that is the caller's destination, the same FVec3 the `/` key
    // describes, and moving it would make the two keys name different places (S76).
    const bool interior = (badLeg + 1 < poly.size());
    if (interior && hi >= 0 && hi < static_cast<int>(portals.size())) {
        const FVec3 m = SpanMid(portals[hi]);
        if (out.empty() || !SameXZ(out.back(), m)) {
            out.push_back(m);
            ++added;
            out.insert(out.end(), poly.begin() + static_cast<ptrdiff_t>(badLeg) + 1, poly.end());
            return added;
        }
    }

    if (added == 0) { out.clear(); return 0; }
    out.insert(out.end(), poly.begin() + static_cast<ptrdiff_t>(badLeg), poly.end());
    return added;
}

int UnpullDeparture(const std::vector<FVec3>& poly, const std::vector<int>& idx,
                    const std::vector<Portal>& portals, size_t badLeg, std::vector<FVec3>& out) {
    out.clear();
    // Same integrity bar as Unpull: the mapping has to describe THIS polyline or we would be splicing
    // against indices that belong to another one.
    if (badLeg == 0 || badLeg >= poly.size() || idx.size() != poly.size()) return 0;

    // The departure corner must be a real corner. `badLeg - 1 == 0` is the player's own position, which
    // is not on any portal (`idx[0] == -1`) and is not ours to move -- the route has to start where the
    // player is standing.
    const size_t at = badLeg - 1;
    if (at == 0) return 0;

    const int pi = idx[at];
    if (pi < 0 || pi >= static_cast<int>(portals.size())) return 0;

    const FVec3 m = SpanMid(portals[pi]);
    // A replacement that lands on either neighbour would collapse a leg to zero length, which
    // `DropPassedWaypoints` and the direction pass both have guards for and neither should have to use.
    if (SameXZ(m, poly[at - 1]) || SameXZ(m, poly[at + 1])) return 0;
    // And if it IS where the corner already is, this rung has nothing to offer -- say so rather than
    // spending a full re-validation on an identical polyline.
    if (SameXZ(m, poly[at])) return 0;

    out = poly;
    out[at] = m;
    return 1;
}

int FullCorridor(const FVec3& from, const FVec3& to, const std::vector<Portal>& portals,
                 std::vector<FVec3>& out) {
    out.clear();
    out.push_back(from);
    int added = 0;
    for (const Portal& q : portals) {
        const FVec3 m = SpanMid(q);
        if (!SameXZ(out.back(), m)) { out.push_back(m); ++added; }
    }
    if (!SameXZ(out.back(), to)) out.push_back(to);
    return added;
}

int DropPassedWaypoints(const FVec3& from, std::vector<FVec3>& poly, std::vector<int>* idx) {
    int dropped = 0;
    while (poly.size() >= 3) {
        const FVec3 c1 = poly[1], c2 = poly[2];
        const float ex = c2.x - c1.x, ez = c2.z - c1.z;
        const float len2 = ex * ex + ez * ez;
        if (len2 < 1e-6f) break;
        const float t = ((from.x - c1.x) * ex + (from.z - c1.z) * ez) / len2;
        if (t <= 0.0f) break;                       // still ahead of the player -- a real waypoint
        poly.erase(poly.begin() + 1);
        // The portal mapping erases in step, or it stops describing this polyline at all -- and a
        // mapping that is silently off by one addresses the wrong stretch of corridor when a leg is
        // un-pulled, which is worse than having no mapping.
        if (idx && idx->size() > 1) idx->erase(idx->begin() + 1);
        ++dropped;
    }
    return dropped;
}

// HOW FAR A CORNER IS PULLED OFF THE BOUNDARY IT SITS ON.
//
// One body radius (0.27 m) is the minimum that clears the engine's own refusal, and it was what this
// used -- which left every taut corner 27 cm from a wall. That is fine for a path walked exactly, and
// hopeless for one walked from an eight-way spoken heading: the tester was routed into a wall on a leg
// whose corners were legal. Widening is the tester's own choice of fix ("keep the words, widen the
// route"). The improvement test below still gates every move, so this is a preference for the middle
// of a corridor, never a licence to push a corner somewhere worse.
// Reduced from 0.45 (Session 96). The larger margin was chosen for a drift theory that the tester
// later established was not what was failing, and a wider move is exactly what pushed corners off the
// walkway and into the water above. The walkability check below is the real fix; this keeps a little
// clearance without reaching for ground that is not there.
constexpr float kClearanceMargin = 0.15f;

int InsetCorners(std::vector<FVec3>& poly, InsetStats* stats) {
    if (stats) *stats = InsetStats{};
    if (poly.size() < 3) return 0;
    const float r = NavFootprint::BodyRadius() + kClearanceMargin;
    int moved = 0;

    for (size_t i = 1; i + 1 < poly.size(); ++i) {
        if (stats) ++stats->corners;
        const FVec3 prev = poly[i - 1], cur = poly[i], next = poly[i + 1];

        // Unit vectors along the two legs, away from the corner. Their sum bisects the interior angle,
        // so stepping along it moves INTO the corridor rather than across either leg.
        float ax = prev.x - cur.x, az = prev.z - cur.z;
        float bx = next.x - cur.x, bz = next.z - cur.z;
        const float la = std::sqrt(ax * ax + az * az), lb = std::sqrt(bx * bx + bz * bz);
        if (la < 1e-4f || lb < 1e-4f) continue;            // degenerate leg; nothing to bisect
        ax /= la; az /= la; bx /= lb; bz /= lb;
        float sx = ax + bx, sz = az + bz;
        const float ls = std::sqrt(sx * sx + sz * sz);
        // A straight-through corner has no interior to move into (the legs are antiparallel and cancel).
        // It is also not a corner in any meaningful sense, so leave it exactly where it is.
        if (ls < 1e-3f) continue;
        sx /= ls; sz /= ls;

        const PolyId home = NavMesh::FindPolyAt(cur.x, cur.y, cur.z);
        if (home == NavMesh::kNoPoly) continue;            // off-mesh corner: not ours to move

        float before = 0.0f;
        NavFootprint::Clears(cur, home, &before);

        // THE DIRECTION IS MEASURED, NOT ASSUMED (Session 100). The bisector is the right move for
        // a corner pinched between its own two legs -- and the WRONG one for the pinned class that
        // killed 315's bank route: a portal-endpoint corner whose wall runs PARALLEL to one leg.
        // There the clearance gradient is the wall's NORMAL; a bisector step slides along the wall,
        // gains nothing, `after > before` never passes, and this mechanism sat inert (`inset=0` on
        // every funnel line of the whole saga) while validation died 0.5-1.0 m short of corner
        // after corner. So: try the bisector AND both perpendiculars of each leg, keep whichever
        // the footprint MEASURES best. Still a measurement, never a nudge.
        //
        // Every candidate must land on a floor poly, be walkable, and -- since S100 -- be ground
        // the LEADER'S class can stand on: FindPolyAt answers on RAW flags and happily accepts
        // water (S96 measured that failure: `inset=3` moved corners into the channel and an
        // 8.52 m leg died 4.57 m along), and TerrainRefused now closes the class half of it.
        const float dirs[5][2] = {
            { sx, sz },          // interior-angle bisector (the original candidate)
            { -az, ax },         // perpendiculars of the incoming leg...
            { az, -ax },
            { -bz, bx },         // ...and of the outgoing leg
            { bz, -bx },
        };
        float  bestAfter = before;
        FVec3  bestCand{};
        PolyId bestPoly  = NavMesh::kNoPoly;   // where the winning candidate landed -- the instrument
        bool   haveBest  = false;
        for (const auto& d : dirs) {
            const FVec3 cand{ cur.x + d[0] * r, cur.y, cur.z + d[1] * r };
            const PolyId candPoly = NavMesh::FindPolyAt(cand.x, cand.y, cand.z);
            if (candPoly == NavMesh::kNoPoly) continue;    // walked off the mesh -- worse, not better
            if (!NavMesh::Walkable(candPoly)) continue;
            if (NavMesh::TerrainRefused(candPoly)) continue;
            float after = 0.0f;
            NavFootprint::Clears(cand, candPoly, &after);
            // ONLY IF IT HELPS -- strictly better than the corner's own measured clearance. A corner
            // already clear keeps its exact position: the spoken legs are computed from these points
            // and moving them for nothing would change the words.
            if (after > bestAfter) {
                bestAfter = after; bestCand = cand; bestPoly = candPoly; haveBest = true;
            }
        }
        if (haveBest) {
            poly[i] = bestCand;
            ++moved;
            // LOG-ONLY, and free: both poly ids were already resolved above to decide the move. A
            // corner that leaves the poly it was sampled from has been pushed out of the corridor
            // A* certified, which is the S117 suspect for why the ladder repairs short routes and
            // fails long dense ones. Counted, never acted on.
            if (stats) {
                ++stats->moved;
                if (bestPoly != home) ++stats->leftHome;
            }
        }
    }
    return moved;
}

} // namespace PathFunnel

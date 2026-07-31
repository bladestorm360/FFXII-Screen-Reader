#include "navigation/path_march.h"
#include "navigation/nav_mesh.h"

#include <cmath>

namespace PathMarch {

namespace {

// The re-ask step. At or below ~0.54 m the sweep's probe cone stays inside the body, which is what
// makes each call a question the engine can actually answer. 0.50 also tiles the line: consecutive
// destination spheres (radius 0.27) overlap, so there is no uncovered gap between steps.
constexpr float kSubStep = 0.50f;

// Step CAP for one leg's walk -- a COVERAGE bound, never a step-size divisor (Session 100 role
// change). 256 steps = 128 m of walked ground for one leg, at most 16% of a request's 1600-probe
// budget. S97 left this at 64 because nothing observed was binding on it; S99 then proved long legs
// MUST be walked (a 43.4 m leg's one-shot CLEAR was refuted in play), and 64 would have turned a
// >32 m walk into oversized steps -- the S95 domain error by another road. The cap now bounds how
// FAR the walk covers, and a capped walk reports Budget/truncated, never a coarser step.
constexpr int kMaxSubSteps = 256;

// ---- march constants -----------------------------------------------------------------------------

// Bound on polys visited marching one leg. Triangles are corridor-scale; a 43 m leg crosses tens of
// polys, not hundreds. Overrun means degenerate geometry -> NoVerdict. Memory reads only, so this is
// a loop guard, not a work budget.
constexpr int kMarchMaxPolysPerLeg = 512;

// Chord-parameter epsilon. Accepting only crossings with t > tCur + kEpsT excludes re-detecting the
// entry crossing without any entry-edge bookkeeping.
constexpr float kEpsT = 1e-4f;

// A crossing within this distance of an edge ENDPOINT is a vertex graze: in a triangle fan the
// refusing edge cannot be attributed to one edge, so its adjacency cannot be trusted either way.
// 2 cm is far below any gap a 0.27 m body could use.
constexpr float kVertexEps = 0.02f;

// How far past a refused crossing the graze scan looks for walkable mesh before calling it a
// breach. The engine mover's footprint is an ellipse whose semi-axes converge toward ~0.5 m
// (Session 100 decompile finding) -- an off-mesh excursion under the body's own diameter is one the
// depenetration slide survives in play, and it is exactly the shape of a taut chord kissing an
// inset corner or an edge sliver. Beyond it, the body's CENTRE must stand on refused ground, which
// is the canonical refusal. THE MOST JUDGMENT-LADEN CONSTANT IN THE MARCH: if working maps show
// `why=march` breaches on ground the tester walks, raise this first (marchGraze on the validate:
// line is the falsifier -- high grazes with zero false breaches is the design working).
constexpr float kGrazeAllow = 1.0f;

// Graze-scan sample spacing. Under the 0.54 m body diameter so no walkable-in-practice re-entry
// strip can be stepped over.
constexpr float kResampleStep = 0.25f;

} // namespace

FVec3 Lift(const FVec3& p) { return FVec3{ p.x, p.y + kBodyPad, p.z }; }

float GroundY(float x, float z, float seedY) {
    const NavMesh::PolyId p = NavMesh::FindPolyAt(x, seedY, z);
    float y = 0.0f;
    if (p != NavMesh::kNoPoly && NavMesh::Walkable(p) && NavMesh::PolyHeightAt(p, x, z, y)) return y;
    return seedY;
}

float LenXZ(const FVec3& a, const FVec3& b) {
    const float dx = b.x - a.x, dz = b.z - a.z;
    return std::sqrt(dx * dx + dz * dz);
}

bool Arrived(const MapQuery::BodyMove& mv, float tol) {
    if (!mv.valid) return true;                 // no world: treated as clear, never as blocked
    return (mv.requested - mv.achieved) <= tol;
}

const char* CauseName(StopCause c) {
    switch (c) {
        case StopCause::Sweep:  return "sweep";
        case StopCause::Budget: return "budget";
        case StopCause::March:  return "march";
        default:                return "none";
    }
}

bool WalkLeg(const FVec3& from, const FVec3& to, float tol, int probeBudget,
             int& probesUsed, float& reachedM, FVec3& stopAt, StopCause& cause) {
    reachedM = 0.0f;
    cause    = StopCause::None;
    const float total = LenXZ(from, to);
    if (total < 1e-4f) return true;

    int want = static_cast<int>(std::ceil(total / kSubStep));
    if (want < 1) want = 1;
    int steps = want;
    if (steps > kMaxSubSteps) steps = kMaxSubSteps;
    if (steps > probeBudget)  steps = probeBudget;
    if (steps < 1) { cause = StopCause::Budget; stopAt = from; return false; }

    // Steps are FIXED LENGTH (kSubStep along the leg; the final one short). When `steps < want` the
    // walk is CLAMPED: it covers what the cap/budget allow at full accuracy and reports Budget for
    // the rest. The pre-S100 version walked NOTHING when starved; walking what the budget allows is
    // strictly more measurement with the same honest verdict.
    FVec3 cur = from;
    for (int s = 1; s <= steps; ++s) {
        const float dist = (s * kSubStep < total) ? s * kSubStep : total;
        const float t = dist / total;
        FVec3 want3{ from.x + (to.x - from.x) * t,
                     from.y + (to.y - from.y) * t,
                     from.z + (to.z - from.z) * t };
        want3.y = GroundY(want3.x, want3.z, want3.y);

        MapQuery::BodyMove mv;
        MapQuery::BodySweep(Lift(cur), Lift(want3), mv);
        ++probesUsed;

        if (!Arrived(mv, tol)) {
            reachedM = LenXZ(from, cur) + (mv.valid ? mv.achieved : 0.0f);
            // ON THE GROUND, LIKE EVERY OTHER STOP (Session 96). `mv.reached` comes back in the
            // LIFTED frame; the caller uses this as a ROUTE POINT, and a waypoint 0.9 m in the air
            // would be a leg into the ceiling.
            if (mv.valid) {
                stopAt   = mv.reached;
                stopAt.y = GroundY(stopAt.x, stopAt.z, stopAt.y - kBodyPad);
            } else {
                stopAt = cur;
            }
            cause = StopCause::Sweep;
            return false;
        }
        // The ENGINE'S resolved position, not the ideal point -- depenetration slides the body along
        // a wall exactly as it does in play, and the next step aims back at the line.
        if (mv.valid) { cur.x = mv.reached.x; cur.z = mv.reached.z; }
        else          { cur.x = want3.x;      cur.z = want3.z;      }
        cur.y = GroundY(cur.x, cur.z, want3.y);
    }

    if (steps < want) {
        // Clean as far as it went, but the cap or the budget ended it before the far end. Coverage
        // cost, honestly reported; never a breach, never a coarser step.
        reachedM = (steps * kSubStep < total) ? steps * kSubStep : total;
        stopAt   = cur;
        stopAt.y = GroundY(stopAt.x, stopAt.z, cur.y);
        cause    = StopCause::Budget;
        return false;
    }
    reachedM = total;
    return true;
}

namespace {

// 2D cross product of (x1,z1) x (x2,z2).
inline float Cross2(float x1, float z1, float x2, float z2) { return x1 * z2 - z1 * x2; }

// Sample the chord forward from parameter `tFrom` looking for walkable mesh within the graze
// allowance. Y for FindPolyAt is seeded from `seedY` (the exited poly's plane at the crossing) so
// the lookup stays layer-correct over stacked walkway/channel geometry. Returns true and updates
// P/tCur on rescue.
bool GrazeScan(const FVec3& a, const FVec3& b, float legLen, float tFrom, float seedY,
               NavMesh::PolyId& P, float& tCur, int& grazes) {
    for (float d = kResampleStep; d <= kGrazeAllow + 1e-3f; d += kResampleStep) {
        float t2 = tFrom + d / legLen;
        bool  atEnd = false;
        if (t2 >= 1.0f) { t2 = 1.0f; atEnd = true; }
        const float px = a.x + (b.x - a.x) * t2;
        const float pz = a.z + (b.z - a.z) * t2;
        const NavMesh::PolyId q = NavMesh::FindPolyAt(px, seedY, pz);
        if (q != NavMesh::kNoPoly && NavMesh::Walkable(q)) {
            P    = q;
            tCur = t2;
            ++grazes;
            return true;
        }
        if (atEnd) break;
    }
    return false;
}

} // namespace

MarchResult MarchLeg(const FVec3& a, const FVec3& b, float endTol) {
    MarchResult r;
    const float legLen = LenXZ(a, b);
    if (legLen < 1e-4f) { r.verdict = MarchVerdict::Clear; return r; }

    NavMesh::PolyId P = NavMesh::FindPolyAt(a.x, a.y, a.z);
    if (P == NavMesh::kNoPoly) { r.why = "no-start-poly"; return r; }   // NoVerdict

    const float rx = b.x - a.x, rz = b.z - a.z;   // chord direction (XZ)
    float tCur = 0.0f;

    for (int iter = 0; iter < kMarchMaxPolysPerLeg; ++iter) {
        if (NavMesh::PolyContains(P, b.x, b.z)) { r.verdict = MarchVerdict::Clear; return r; }

        FVec3 v[3];
        if (!NavMesh::PolyVerts(P, v)) { r.why = "verts-unreadable"; return r; }   // NoVerdict

        // The exit edge: smallest chord parameter t > tCur where the chord crosses one of P's edges.
        float bestT = 2.0f;
        int   bestE = -1;
        float bestCx = 0.0f, bestCz = 0.0f;
        bool  bestVertexGraze = false;
        for (int e = 0; e < 3; ++e) {
            const FVec3& p0 = v[e];
            const FVec3& p1 = v[(e + 1) % 3];
            const float qx = p1.x - p0.x, qz = p1.z - p0.z;
            const float denom = Cross2(rx, rz, qx, qz);
            if (std::fabs(denom) < 1e-8f) continue;               // chord parallel to this edge
            const float wx = p0.x - a.x, wz = p0.z - a.z;
            const float t = Cross2(wx, wz, qx, qz) / denom;
            const float s = Cross2(wx, wz, rx, rz) / denom;
            if (t <= tCur + kEpsT || t > 1.0f + kEpsT) continue;
            if (s < -0.001f || s > 1.001f) continue;
            if (t < bestT) {
                bestT  = t;
                bestE  = e;
                bestCx = a.x + rx * t;
                bestCz = a.z + rz * t;
                // Vertex graze: the crossing sits within kVertexEps of either edge endpoint.
                const float d0 = std::sqrt((bestCx - p0.x) * (bestCx - p0.x) + (bestCz - p0.z) * (bestCz - p0.z));
                const float d1 = std::sqrt((bestCx - p1.x) * (bestCx - p1.x) + (bestCz - p1.z) * (bestCz - p1.z));
                bestVertexGraze = (d0 < kVertexEps || d1 < kVertexEps);
            }
        }

        // Y at the crossing, from the poly being EXITED -- layer-correct for the resample lookups.
        float exitY = a.y + (b.y - a.y) * (bestE >= 0 ? bestT : tCur);
        if (bestE >= 0) {
            float py = 0.0f;
            if (NavMesh::PolyHeightAt(P, bestCx, bestCz, py)) exitY = py;
        }

        if (bestE < 0) {
            // Containment says `b` is outside, numerics found no crossing -- a sliver or a graze the
            // epsilons ate. Resume-sample forward; failing that, decline to rule. NEVER a breach.
            if (GrazeScan(a, b, legLen, tCur, exitY, P, tCur, r.grazes)) continue;
            r.why = "no-exit-edge";
            return r;                                              // NoVerdict
        }

        if (bestVertexGraze) {
            // In a fan of triangles around a shared vertex the refusing edge cannot be attributed to
            // one edge, so its adjacency must not be trusted -- in either direction.
            if (GrazeScan(a, b, legLen, bestT, exitY, P, tCur, r.grazes)) continue;
            r.why = "vertex-graze";
            return r;                                              // NoVerdict
        }

        NavMesh::PolyId n = NavMesh::kNoPoly;
        if (!NavMesh::NeighborChecked(P, bestE, n)) { r.why = "neighbor-unreadable"; return r; }

        uint32_t nbrRaw = 0, nbrEff = 0;
        bool nbrWalkable = false;
        if (n != NavMesh::kNoPoly) {
            if (!NavMesh::PolyFlags(n, nbrRaw, nbrEff)) { r.why = "flags-unreadable"; return r; }
            nbrWalkable = NavMesh::Walkable(n);
        }

        if (n != NavMesh::kNoPoly && nbrWalkable) {
            // The mover's accept rule (FUN_0022f9b0 / FUN_002327d0, conf 0.99 since S75): the
            // neighbour exists and the party's floor class may stand on it. Step in.
            P    = n;
            tCur = bestT;
            continue;
        }

        // REFUSED CROSSING. Arrival forgiveness first: a route legitimately ends against an exit
        // seam, a shopfront, a notice board -- the same allowance the sweep's arrival test grants.
        if ((1.0f - bestT) * legLen <= endTol) { r.verdict = MarchVerdict::Clear; return r; }

        // Graze scan: a taut chord kissing an inset corner or an edge sliver leaves the mesh for
        // less than a body diameter and the depenetration slide survives it in play. Only when no
        // walkable mesh exists within the allowance is the refusal real.
        if (GrazeScan(a, b, legLen, bestT, exitY, P, tCur, r.grazes)) continue;

        r.verdict  = MarchVerdict::Breach;
        r.hitPoint = FVec3{ bestCx, exitY, bestCz };
        r.hitDistM = bestT * legLen;
        r.fromPoly = static_cast<int>(P);
        r.edge     = bestE;
        r.nbr      = (n == NavMesh::kNoPoly) ? -1 : static_cast<int>(n);
        r.nbrEff   = nbrEff;
        return r;
    }

    r.why = "poly-cap";
    return r;                                                      // NoVerdict
}

} // namespace PathMarch

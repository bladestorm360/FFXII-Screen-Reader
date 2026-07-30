#include "navigation/path_validate.h"
#include "navigation/map_query.h"
#include "navigation/nav_footprint.h"
#include "navigation/nav_mesh.h"

#include <cmath>

namespace PathValidate {

namespace {

// Body height, matching NavMesh::StraddleAt and entity_commands.cpp. The sweep is a body sweep so it has
// its own radius, but the LINE still has to start somewhere sane: at the feet it grazes the floor the
// path is standing on, which is how an earlier version of this check reported a breach on 100% of routes
// including a 0.9 m leg from the player's own position.
constexpr float kBodyPad = 0.9f;

// How far short of its requested endpoint the body may finish and still count as having got there.
// One body radius is the engine's own depenetration pull-back against a surface it is legitimately
// walking up to; the rest is slack for the +/-30-degree probe keeping the shortest reach.
constexpr float kEndSlack = 0.15f;

// The re-ask step. At or below ~0.54 m the sweep's probe cone stays inside the body, which is what
// makes each call a question the engine can actually answer -- see the header.
constexpr float kSubStep     = 0.50f;
// Bound on one leg's re-ask, so a 200 m leg cannot eat the whole request's budget. A leg longer than
// this simply walks in coarser steps; the alternative -- refusing to answer -- would put us straight
// back to guessing.
constexpr int   kMaxSubSteps = 64;

FVec3 Lift(const FVec3& p) { return FVec3{ p.x, p.y + kBodyPad, p.z }; }

// The walkmap's own floor height under a point. FUN_00380c40 PINS the actor's Y to the poly plane, so
// this is not an approximation of how the character moves vertically -- it is how it moves.
// Falls back to the caller's value when the point is off-mesh, which a taut chord can briefly be.
// The `Walkable` guard is the floor-TYPE test and nothing more. It briefly meant "the party's class may
// stand here" (Session 96) and this comment claimed it kept sub-steps off water; that reading is struck
// -- the tester walks the shallow water it refused. Whether the character can be THERE is answered by
// the sweep below, at character scale, which is the only instrument that has ever answered it.
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

// Did the body finish close enough to where it was asked to go? A DISTANCE, not a ratio -- see header.
bool Arrived(const MapQuery::BodyMove& mv, float tol) {
    if (!mv.valid) return true;                 // no world: treated as clear, never as blocked
    return (mv.requested - mv.achieved) <= tol;
}

// Is there a WALL across this span? The sweep sees volumes only where its own probes happen to fall;
// this asks the engine directly at both ends and at the midpoint, which is what catches a blocker
// standing inside a floor triangle. Cheap: three point tests, no sweep.
// TESTED AT BODY HEIGHT, NOT AT THE FEET. A wall volume stands ON the floor, so a point at exactly
// floor level sits on its boundary and can read as inside it -- which would block ordinary ground.
// What we actually care about is whether the BODY is in the wall, and the body is the metre above
// the floor. Same lift the sweep and the edge straddle already use.
// `testEnd` is false on the FINAL leg. THE TARGET IS ALLOWED TO BE INSIDE A VOLUME (Session 96) --
// exits are archways and map-jump surfaces, and shops, notice boards and chests stand against
// architecture, so the destination point sits inside a collision volume as a matter of course. Testing
// it made the target's own position veto every route to it: measured on map 311, a route that reached
// (47.0,-0.00,124.0) failed its last leg with `reached=0.00m why=wall` on the exit at (47.0,-0.00,132.0),
// and the same request logged `volXing=109` -- that area is full of volumes and the player walks it.
// The midpoint is still tested, so a wall ACROSS the final approach is still caught.
bool WallAcross(const FVec3& a, const FVec3& b, bool testEnd) {
    const FVec3 mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };
    if (MapQuery::PointInVolume(Lift(mid))) return true;
    return testEnd && MapQuery::PointInVolume(Lift(b));
}

// Walk the leg the way the CHARACTER does: short displacements, Y pinned to the walkmap under each
// step, and the engine's own resolved position carried forward so depenetration can slide the body
// along a wall exactly as it does in play.
//
// Returns true when the body reached the far end. `reachedM` receives how far along it actually got,
// which is what turns "BREACH" in the log from a verdict into a measurement, and `cause` says which
// of the three ways it ended -- see StopCause.
//
// THE STEP LENGTH IS FIXED AND IS NOT NEGOTIABLE AGAINST THE BUDGET (Session 96). This used to read
// `if (steps > probeBudget) steps = probeBudget;`, which keeps the leg and spends fewer probes on it
// -- i.e. it makes each step LONGER. That is precisely the domain error the whole re-ask exists to
// undo: past ~0.54 m the sweep's +/-30-degree probe cone leaves the body and it starts reporting a
// wall in any corridor narrower than the cone. A short budget must cost us COVERAGE, which is
// honestly reportable as `truncated`, never ACCURACY, which is silently a false wall.
bool WalkLeg(const FVec3& from, const FVec3& to, float tol, int probeBudget,
             int& probesUsed, float& reachedM, FVec3& stopAt, StopCause& cause, bool testEnd) {
    reachedM = 0.0f;
    cause    = StopCause::None;
    const float total = LenXZ(from, to);
    if (total < 1e-4f) return true;

    int steps = static_cast<int>(std::ceil(total / kSubStep));
    if (steps > kMaxSubSteps) steps = kMaxSubSteps;
    if (steps > probeBudget) {                  // not enough probes to walk it at all, at this step size
        cause  = StopCause::Budget;
        stopAt = from;
        return false;
    }
    if (steps < 1) { cause = StopCause::Budget; stopAt = from; return false; }

    FVec3 cur = from;
    for (int s = 1; s <= steps; ++s) {
        const float t = static_cast<float>(s) / static_cast<float>(steps);
        FVec3 want{ from.x + (to.x - from.x) * t,
                    from.y + (to.y - from.y) * t,
                    from.z + (to.z - from.z) * t };
        want.y = GroundY(want.x, want.z, want.y);

        // The volume test rides along on every sub-step, so a wall inside a floor triangle stops the
        // walk at the step that meets it rather than being swept straight through.
        if (WallAcross(cur, want, testEnd || s < steps)) {
            reachedM = LenXZ(from, cur);
            stopAt   = cur;
            cause    = StopCause::Wall;
            ++probesUsed;
            return false;
        }

        MapQuery::BodyMove mv;
        MapQuery::BodySweep(Lift(cur), Lift(want), mv);
        ++probesUsed;

        if (!Arrived(mv, tol)) {
            reachedM = LenXZ(from, cur) + (mv.valid ? mv.achieved : 0.0f);
            // ON THE GROUND, LIKE EVERY OTHER STOP (Session 96). `mv.reached` comes back in the LIFTED
            // frame, because that is what the sweep was handed -- so this branch used to report the
            // stop a body-height above the floor while the wall branch above reported it on the floor.
            // The log showed both and neither said which: `stop=(45.6,0.90,123.8)` against
            // `stop=(47.0,-0.00,124.0)`. Harmless while it was only a diagnostic; the caller now uses
            // this as a ROUTE POINT, and a waypoint 0.9 m in the air would be a leg into the ceiling.
            if (mv.valid) {
                stopAt   = mv.reached;
                stopAt.y = GroundY(stopAt.x, stopAt.z, stopAt.y - kBodyPad);
            } else {
                stopAt = cur;
            }
            cause    = StopCause::Sweep;
            return false;
        }
        // The ENGINE'S resolved position, not the ideal point. A body pushed aside by depenetration
        // keeps walking from where it actually is, and the next step aims back at the line -- which is
        // both what happens in play and what lets a leg that grazes a wall still complete.
        if (mv.valid) { cur.x = mv.reached.x; cur.z = mv.reached.z; }
        else          { cur.x = want.x;       cur.z = want.z;       }
        cur.y = GroundY(cur.x, cur.z, want.y);
    }
    reachedM = total;
    return true;
}

// WHAT IS ACTUALLY IN THE WAY (Session 96). Three engine calls, on a route that has ALREADY failed,
// answering the one question the breach line cannot: is the thing at the far end a floor border or a
// wall volume?
//
// It matters because the two want opposite repairs and we have been guessing between them. On map 311
// the body stopped at z = 123.8 on nine consecutive routes from seven different x; with a 0.27 m body
// pushed to tangency that puts the obstruction at z ~= 124.07, just past a corner at z = 124.0. A
// floor border there argues for insetting portal spans; a wall volume makes that change pure cost.
//
// AND THE CORNER TEST IS THE POINT OF IT. `CheckLegs` returns on a breach BEFORE reaching its own
// interior-corner check, so `tight=0@0` on a breaching route means NOT TESTED -- never "clear". That
// misreading nearly bought a global geometry change. Asking it here is the only way the number means
// what it looks like.
void Diagnose(const FVec3& a, const FVec3& b, const FVec3& stop, LegReport& r) {
    const NavMesh::PolyId at = NavMesh::FindPolyAt(b.x, b.y, b.z);
    r.badCornerPoly  = (at == NavMesh::kNoPoly) ? -1 : static_cast<int>(at);
    r.badCornerClear = (at != NavMesh::kNoPoly) && NavFootprint::Clears(b, at, &r.badCornerMargin);

    // At the stop, and a short way PAST it along the leg -- a volume the body has run into starts
    // somewhere ahead of where the body was allowed to finish, never at the stop itself.
    const float dx = b.x - a.x, dz = b.z - a.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    r.badStopInVolume = MapQuery::PointInVolume(Lift(stop));
    if (len > 1e-4f) {
        const FVec3 ahead{ stop.x + (dx / len) * 0.30f, stop.y, stop.z + (dz / len) * 0.30f };
        r.badAheadInVolume = MapQuery::PointInVolume(Lift(ahead));
    }
}

} // namespace

const char* CauseName(StopCause c) {
    switch (c) {
        case StopCause::Wall:   return "wall";
        case StopCause::Sweep:  return "sweep";
        case StopCause::Budget: return "budget";
        default:                return "none";
    }
}

LegReport CheckLegs(const std::vector<FVec3>& path, int probeCap, float arrivalTol) {
    LegReport r;
    if (path.size() < 2) return r;
    r.total = path.size() - 1;

    const float radius = NavFootprint::BodyRadius();
    const float tol    = radius + kEndSlack;

    for (size_t i = 1; i < path.size(); ++i) {
        if (r.probes >= probeCap) { r.truncated = true; break; }

        const FVec3& a = path[i - 1];
        const FVec3& b = path[i];

        // THE LAST LEG IS AN ARRIVAL, NOT A TRAVERSAL. See the header: a target you cannot stand on
        // stops the body short every time, and judging that by the mid-route tolerance is what turned
        // a route that got within 2.55 m of an exit into "goal unreachable, 15.2 m short".
        const bool  last    = (i + 1 == path.size());
        const float legTol  = (last && arrivalTol > tol) ? arrivalTol : tol;

        // WALLS FIRST. A volume across this leg is decisive and costs three point tests, where the
        // sweep below would sail through it whenever its own probes miss.
        if (WallAcross(a, b, !last)) {
            ++r.probes;
            ++r.checked;
            ++r.walls;
            r.ok         = false;
            r.firstBad   = i;
            r.badLength  = LenXZ(a, b);
            r.badReached = 0.0f;
            r.badStopAt  = a;
            r.badCause   = StopCause::Wall;
            return r;
        }

        // FAST PATH: one sweep over the whole leg. A `clear` verdict here is trustworthy -- the probe
        // cone only ever makes the test STRICTER, so nothing it passes can be blocked.
        MapQuery::BodyMove mv;
        MapQuery::BodySweep(Lift(a), Lift(b), mv);
        ++r.probes;
        ++r.checked;
        if (mv.valid) ++r.swept; else ++r.blind;
        if (mv.valid && mv.fraction < r.worstFraction) r.worstFraction = mv.fraction;

        if (!Arrived(mv, legTol)) {
            // OUTSIDE THE ONE-SHOT SWEEP'S DOMAIN. Re-ask in steps the engine can answer.
            ++r.resweeps;
            float reachedM = 0.0f;
            FVec3 stopAt{};
            StopCause cause = StopCause::None;
            const bool walkable =
                WalkLeg(a, b, legTol, probeCap - r.probes, r.probes, reachedM, stopAt, cause, !last);
            if (!walkable) {
                r.firstBad   = i;
                r.badLength  = LenXZ(a, b);
                r.badReached = reachedM;
                r.badStopAt  = stopAt;
                r.badCause   = cause;
                // WHAT IT STOPPED ON. `FindPolyAt` is seeded with the stop's own Y, so on a walkway
                // over a channel it names the surface the body was actually on rather than whichever
                // one happens to be on top. `stopAt` is on the ground (see the header), so no unlift.
                const NavMesh::PolyId sp = NavMesh::FindPolyAt(stopAt.x, stopAt.y, stopAt.z);
                if (sp != NavMesh::kNoPoly) {
                    uint32_t raw = 0, eff = 0;
                    NavMesh::PolyFlags(sp, raw, eff);
                    r.badStopPoly  = static_cast<int>(sp);
                    r.badStopWalk  = NavMesh::Walkable(sp);
                    r.badStopFlags = eff;
                }
                // OUT OF PROBES IS NOT A WALL. Reporting it as one bans a portal on the strength of a
                // measurement never taken, and every retry after that ban comes back longer (S95).
                // `checked` gives this leg back: the fast path counted it, and `ProvenPrefix` reads
                // `checked` as "legs actually verified" when it cuts a truncated route to its prefix.
                if (cause == StopCause::Budget) { --r.checked; r.truncated = true; break; }
                Diagnose(a, b, stopAt, r);
                r.ok = false;
                return r;
            }
            ++r.rescued;
        }

        // The corner the leg ARRIVES at, when it is an interior corner. A leg can be perfectly sweepable
        // and still end on a point the engine pushes the body off -- that is the whole reason the corner
        // inset exists, and counting it here is what makes the inset's effect visible rather than
        // theoretical. The final point is the target itself and is deliberately exempt: routes to a
        // shopfront or a wall-mounted notice board legitimately end hard against a boundary.
        //
        // COUNTED, NOT FATAL, and the loop keeps going -- see the header. The body still has to get
        // through, and the next leg's sweep is what answers that; returning here meant the question was
        // never asked on a quarter of all routes.
        if (i + 1 < path.size()) {
            if (r.probes >= probeCap) { r.truncated = true; break; }
            ++r.probes;
            const NavMesh::PolyId at = NavMesh::FindPolyAt(b.x, b.y, b.z);
            if (at != NavMesh::kNoPoly && !NavFootprint::Clears(b, at, nullptr)) {
                ++r.tightCorners;
                if (r.firstTight == 0) r.firstTight = i;
            }
        }
    }

    if (r.checked < r.total) r.truncated = true;
    // A BLIND CHECK IS NOT A PASS. If the collision world was not there for some legs, we did not
    // verify them -- we defaulted them. `truncated` is exactly the right word for that and it already
    // routes the caller to the honest frontier path instead of letting it call the route verified.
    if (r.blind > 0) r.truncated = true;
    return r;
}

} // namespace PathValidate

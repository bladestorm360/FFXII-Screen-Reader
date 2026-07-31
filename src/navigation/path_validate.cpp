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
//
// MEASURED AND DELIBERATELY LEFT AT 64 (Session 97). Past ~32 m this bound stops bounding the WORK and
// starts changing the STEP: `ceil(total / kSubStep)` clamped to 64 walks a 47 m leg in 0.73 m steps,
// beyond the ~0.54 m limit at which the sweep's +/-30-degree probes leave the body -- the same domain
// error S95 built the re-ask to undo. Real, and NOT fixed here: **the longest leg re-asked in the whole
// failing session was 12.14 m (25 steps), so 64 was never once the binding constraint**, and raising it
// only moves the constraint onto `probeBudget`, where a starved re-ask returns `Budget` -> `truncated`
// -> frontier -> "No path" on a route that used to walk coarsely and pass. A change that fixes nothing
// observed and can only refuse more does not belong in a build repairing two over-refusals.
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

// IS A COLLISION VOLUME ON THIS LINE? **A MEASUREMENT, NOT A VERDICT** (Session 97).
//
// This was `WallAcross` and it FAILED THE ROUTE. It refused 16 of 16 routes on map 315 -- every single
// breach on that map was `why=wall` -- while `MapQuery::BodySweep`, the instrument this file's own
// header calls authoritative, objected to not one leg. It is now counted and nothing more.
//
// THE PREMISE IT WAS BUILT ON IS FALSE, and the decompile says so plainly. It was added on the reading
// that "walls are volume primitives in CSR layers 1-2 ... so a wall standing inside a floor triangle
// passed every check the router had". `FUN_00230c10`'s ellipsoid push-out passes iterate with layer
// mask **7** -- layers 0, 1 AND 2 -- through `FUN_0022de60`, over the same `0x4000`-tagged, `0x90`-stride
// volume array. The sweep has always collided with these. Conf 0.97.
//
// AND IT IS THE COARSER OF THE TWO INSTRUMENTS, NOT THE SHARPER ONE. `FUN_00232490` installs
// `FUN_0022f8b0`, which tests **exactly one bit (31)** of the merged flags word -- no class test, no
// query class, no material. The engine's own movement collision reads `merged_flags & 7` against the
// mover's class: 0 always solid, 1 conditional on bit 30, **4 solid only when queryClass != 4 -- and the
// party's movers pass 4** -- 2/3/5/6/7 never collide at all, and bit 23 marks a hit that is recorded
// and does not block. It also hard-excludes the `>= 0x5000` range, which is the doors and the moving
// platforms. So it counts as walls a whole class of volume the party walks straight through, and misses
// the ones that actually shut.
//
// A point test also cannot answer a question about a LINE. One sample decided a 12 m leg, and the log
// caught it contradicting itself inside one request (`seq=41`): leg 2 of attempt 1,
// (15.7,114.1)->(20.2,114.6), midpoint ~(17.95,114.35) -> WALL; leg 2 of attempt 2,
// (16.5,115.0)->(63.6,114.4), passing within ~0.6 m of that same point, midpoint ~(40.05,114.7) ->
// clear, and swept at fraction 0.97. Which verdict a leg got depended on where its midpoint landed.
//
// SAFE TO REMOVE WHERE ROUTING WORKS, and that is measured rather than argued: across the whole failing
// session this test fired on map 315 and nowhere else -- `walls=0` on all 35 validation runs on maps
// 311 and 321. Deleting the veto cannot change any outcome there.
//
// The probe is KEPT, ground-pinned, so `volHit`/`volWalked` can retire the theory for good (or produce
// the coordinates that revive it). If a real wall test is ever needed, it is the class-aware
// `FUN_0022d4b0`, reachable via `FUN_002315e0(ctx, from, to)` -- not this one.
//
// Y COMES FROM THE WALKMAP, NOT FROM THE CORNERS. The old test lifted `(a.y + b.y) / 2`, which on a leg
// between corners at 3.74 and 10.25 (map 315) is an arbitrary altitude with no relation to the floor
// under the probe. `WalkLeg` pins each sub-step with `GroundY` before testing; the leg-level probe never
// did. The lift itself is right and stays: a wall volume stands ON the floor, so a point at exactly
// floor level sits on its boundary and can read as inside it.
bool WallSuspect(const FVec3& a, const FVec3& b) {
    FVec3 mid{ (a.x + b.x) * 0.5f, 0.0f, (a.z + b.z) * 0.5f };
    mid.y = GroundY(mid.x, mid.z, (a.y + b.y) * 0.5f);
    return MapQuery::PointInVolume(Lift(mid));
}

// Walk the leg the way the CHARACTER does: short displacements, Y pinned to the walkmap under each
// step, and the engine's own resolved position carried forward so depenetration can slide the body
// along a wall exactly as it does in play.
//
// Returns true when the body reached the far end. `reachedM` receives how far along it actually got,
// which is what turns "BREACH" in the log from a verdict into a measurement, and `cause` says which
// of the two ways it ended -- see StopCause.
//
// THE STEP LENGTH IS FIXED AND IS NOT NEGOTIABLE AGAINST THE BUDGET (Session 96). This used to read
// `if (steps > probeBudget) steps = probeBudget;`, which keeps the leg and spends fewer probes on it
// -- i.e. it makes each step LONGER. That is precisely the domain error the whole re-ask exists to
// undo: past ~0.54 m the sweep's +/-30-degree probe cone leaves the body and it starts reporting a
// wall in any corridor narrower than the cone. A short budget must cost us COVERAGE, which is
// honestly reportable as `truncated`, never ACCURACY, which is silently a false wall.
bool WalkLeg(const FVec3& from, const FVec3& to, float tol, int probeBudget,
             int& probesUsed, float& reachedM, FVec3& stopAt, StopCause& cause) {
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

        // NO VOLUME TEST HERE ANY MORE (Session 97). It used to stop the walk at the sub-step that met
        // a `PointInVolume` hit, "rather than being swept straight through" -- but the sweep does not
        // sweep through them: it iterates the volume layers itself, with the party's own query class,
        // which this one never had. See WallSuspect above. Sub-stepping it would only have made the
        // same wrong answer finer-grained.
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

        // THE VOLUME PROBE IS ASKED AND DECIDES NOTHING (Session 97). It used to run first and be
        // decisive -- "a volume across this leg is decisive ... where the sweep below would sail through
        // it". The sweep does not sail through them, and this probe cannot tell a wall from a region the
        // party walks; see WallSuspect. Both counters are printed, so the next log settles it.
        const bool volSuspect = WallSuspect(a, b);
        if (volSuspect) { ++r.probes; ++r.volHit; }

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
                WalkLeg(a, b, legTol, probeCap - r.probes, r.probes, reachedM, stopAt, cause);
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

        // THE LEG WALKED. If the volume probe flagged it, that flag is now MEASURED to be wrong about
        // this leg: the body went the whole way with the party's own query class. This is the counter
        // that either retires the volume theory or, by staying well below `volHit`, revives it.
        if (volSuspect) ++r.volWalked;

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

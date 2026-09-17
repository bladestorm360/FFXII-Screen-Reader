#include "navigation/path_validate.h"
#include "navigation/map_query.h"
#include "navigation/nav_footprint.h"
#include "navigation/nav_mesh.h"

#include <cmath>

namespace PathValidate {

namespace {

// The leg-scale walkers and their shared helpers (kBodyPad/kEndSlack, Lift/GroundY/LenXZ/Arrived,
// WalkLeg, MarchLeg) MOVED to path_march.{h,cpp} in Session 100 -- one definition, or the two
// walkers drift. Local aliases keep this file readable.
using PathMarch::Lift;
using PathMarch::GroundY;
using PathMarch::LenXZ;
using PathMarch::Arrived;

// A one-shot CLEAR above this length is a SCREEN, not a verdict -- the sub-step walk runs anyway
// (Session 100; see the header's strike of "when it says clear, the leg is clear").
//
// 12.0 m is the largest bound the DATA calls known-safe, not physics: WalkLeg verdicts at <= 12.14 m
// are field-proven across sessions (the longest leg the rescue path ever re-asked, with working maps
// staying working), there is zero evidence a one-shot CLEAR is safe beyond that, and one proof it is
// not (the 43.4 m leg 3 of map 315, S99 -- certified by one probe, refuted by the player's feet).
// Cost of the walk it triggers: ~2 probes/m, only on legs > 12 m, which working city maps rarely
// produce -- measured impact on maps 311/321 is ZERO legs.
constexpr float kLongLegResweep = 12.0f;

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
// mover's class: 0 always solid, 1 conditional on bit 30, **4 solid ONLY for queryClass 4 -- the
// party-only invisible walls** (polarity VERIFIED S100 by direct read of FUN_0022cc50:92-95 and
// FUN_0022d4b0:73-76; an earlier note here had it inverted) -- 2/3/5/6/7 never collide at all, and
// bit 23 marks a hit that is recorded and does not block. It also hard-excludes the `>= 0x5000`
// range, which is the doors and the moving platforms. So it counts as walls a whole class of volume
// the party walks straight through, and misses the ones that actually shut.
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
// the coordinates that revive it). If a real class-aware wall RAY is ever needed it is
// `FUN_00230b60` with class 4 (the engine's own party-class segment test; S100 research) -- NOT
// `FUN_002315e0`, whose class is hard-coded to -1 (it misses type-4 party-only barriers; the
// "class-aware via FUN_002315e0" recommendation that used to sit here is struck).
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

// `WalkLeg` MOVED to path_march.cpp (Session 100), with one contract change recorded there: the step
// cap became a COVERAGE bound (a capped walk completes clean and reports Budget) instead of a
// walk-nothing-when-starved refusal, and it was raised 64 -> 256 because long legs are now walked on
// a one-shot CLEAR too. The step LENGTH stays fixed at 0.5 m -- that part of the S96 rule is intact.

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

// WHAT IT STOPPED ON. `FindPolyAt` is seeded with the stop's own Y, so on a walkway over a channel
// it names the surface the body was actually on rather than whichever one happens to be on top.
// `stopAt` is on the ground (both walkers guarantee it), so no unlift.
void FillStopPoly(LegReport& r, const FVec3& stopAt) {
    const NavMesh::PolyId sp = NavMesh::FindPolyAt(stopAt.x, stopAt.y, stopAt.z);
    if (sp != NavMesh::kNoPoly) {
        uint32_t raw = 0, eff = 0;
        NavMesh::PolyFlags(sp, raw, eff);
        r.badStopPoly  = static_cast<int>(sp);
        r.badStopWalk  = NavMesh::Walkable(sp);
        r.badStopFlags = eff;
    }
}

} // namespace

LegReport CheckLegs(const std::vector<FVec3>& path, int probeCap, float arrivalTol) {
    LegReport r;
    if (path.size() < 2) return r;
    r.total = path.size() - 1;

    const float radius = NavFootprint::BodyRadius();
    const float tol    = radius + PathMarch::kEndSlack;

    for (size_t i = 1; i < path.size(); ++i) {
        if (r.probes >= probeCap) { r.truncated = true; break; }

        const FVec3& a = path[i - 1];
        const FVec3& b = path[i];

        // THE LAST LEG IS AN ARRIVAL, NOT A TRAVERSAL. See the header: a target you cannot stand on
        // stops the body short every time, and judging that by the mid-route tolerance is what turned
        // a route that got within 2.55 m of an exit into "goal unreachable, 15.2 m short".
        const bool  last    = (i + 1 == path.size());
        const float legTol  = (last && arrivalTol > tol) ? arrivalTol : tol;

        // THE ADJACENCY MARCH RUNS FIRST (Session 100) AND SPENDS NO PROBES -- `probeCap` prices
        // SWEEPS, and the march makes none (mesh reads plus, since the class-aware correction, one
        // cheap pure floor-test call per crossing). Charging it would starve the WalkLeg re-asks
        // into truncated -> frontier, an over-refusal. Its breach is applied AFTER the sweep below
        // so swept/blind/worstFrac keep their meaning on every leg.
        const PathMarch::MarchResult march = PathMarch::MarchLeg(a, b, legTol);
        r.marchGraze += march.grazes;
        if (march.verdict == PathMarch::MarchVerdict::NoVerdict) ++r.marchBlind; else ++r.march;

        // THE VOLUME PROBE IS ASKED AND DECIDES NOTHING (Session 97). It used to run first and be
        // decisive -- "a volume across this leg is decisive ... where the sweep below would sail through
        // it". The sweep does not sail through them, and this probe cannot tell a wall from a region the
        // party walks; see WallSuspect. Both counters are printed, so the next log settles it.
        const bool volSuspect = WallSuspect(a, b);
        if (volSuspect) { ++r.probes; ++r.volHit; }

        // The one-shot sweep, on every leg. NOT a verdict above sub-step scale (S100, see the
        // header) -- it screens for volume hits and prices the fraction counters.
        MapQuery::BodyMove mv;
        MapQuery::BodySweep(Lift(a), Lift(b), mv);
        ++r.probes;
        ++r.checked;
        if (mv.valid) ++r.swept; else ++r.blind;
        if (mv.valid && mv.fraction < r.worstFraction) r.worstFraction = mv.fraction;

        // A MARCH BREACH IS A BREACH REGARDLESS OF THE SWEEP (Session 100). The sweep is structurally
        // blind to adjacency walls -- one zero-radius centre ray plus a destination sphere, no
        // adjacency read anywhere -- so its CLEAR cannot overrule the march. WalkLeg is SKIPPED: its
        // instrument cannot see this refusal either, and probes spent "confirming" would un-confirm.
        if (march.verdict == PathMarch::MarchVerdict::Breach) {
            r.firstBad   = i;
            r.badLength  = LenXZ(a, b);
            r.badCause   = StopCause::March;
            r.badMarchPoly   = march.fromPoly;
            r.badMarchEdge   = march.edge;
            r.badMarchNbr    = march.nbr;
            r.badMarchNbrEff = march.nbrEff;
            // The stop is the crossing pulled BACK along the leg by a body radius (+ slack) and
            // ground-pinned: it becomes a ROUTE WAYPOINT for the retreat rung, so it must be a
            // standable point on the walkable side of the refused edge.
            const float pullM = NavFootprint::BodyRadius() + PathMarch::kEndSlack;
            const float back  = (march.hitDistM > pullM) ? march.hitDistM - pullM : 0.0f;
            const float t     = (r.badLength > 1e-4f) ? back / r.badLength : 0.0f;
            FVec3 stopAt{ a.x + (b.x - a.x) * t, 0.0f, a.z + (b.z - a.z) * t };
            stopAt.y     = GroundY(stopAt.x, stopAt.z, march.hitPoint.y);
            r.badReached = back;
            r.badStopAt  = stopAt;
            FillStopPoly(r, stopAt);
            Diagnose(a, b, stopAt, r);
            r.ok = false;
            return r;
        }

        const bool oneShotClear = Arrived(mv, legTol);
        const bool longLeg      = LenXZ(a, b) > kLongLegResweep;
        if (!oneShotClear || longLeg) {
            // OUTSIDE THE ONE-SHOT SWEEP'S DOMAIN -- either it said blocked (the S95 re-ask) or the
            // leg is longer than a one-shot CLEAR can certify (the S100 confirmation walk). The two
            // triggers keep separate counters so the log attributes them separately.
            if (oneShotClear) ++r.longWalks; else ++r.resweeps;
            float reachedM = 0.0f;
            FVec3 stopAt{};
            StopCause cause = StopCause::None;
            const bool walkable =
                PathMarch::WalkLeg(a, b, legTol, probeCap - r.probes, r.probes, reachedM, stopAt, cause);
            if (!walkable) {
                // A WALL-PINNED CORNER IS REACHED AT TANGENCY, NOT MISSED (Session 100; the S95
                // corner lesson applied to the ARRIVAL test). When the corner the leg aims at
                // fails the footprint test, the engine's depenetration forbids the body from
                // standing closer than one radius to it -- the walk stopping `radius + overlap`
                // short is the PHYSICS of arriving, not an obstruction. Measured on 315's bank
                // route: shortfall 0.54 = 0.27 radius + 0.27 overlap, and every repair rung
                // re-aimed at the same unstandable corner and re-failed. Accept the stop when the
                // measured bound covers it; the beacon advances legs at 2.0 m regardless, and a
                // genuine wall mid-leg still stops the body FAR shorter than this bound reaches.
                if (cause == StopCause::Sweep && !last) {
                    const float legLen2   = LenXZ(a, b);
                    const float shortfall = legLen2 - reachedM;
                    const NavMesh::PolyId cp = NavMesh::FindPolyAt(b.x, b.y, b.z);
                    float cornerMargin = 0.0f;
                    const bool cornerClear =
                        (cp != NavMesh::kNoPoly) && NavFootprint::Clears(b, cp, &cornerMargin);
                    if (cp != NavMesh::kNoPoly && !cornerClear) {
                        float overlap = (cornerMargin < 0.0f) ? -cornerMargin : 0.0f;
                        if (overlap > radius) overlap = radius;
                        // 0.35 not 0.25: an OBLIQUE wall projects the tangency stop further along
                        // the leg than the perpendicular bound (315 measured 0.86 against a 0.79
                        // bound -- refused by 7 cm). The inset fix removes most pinning at the
                        // geometry level; this is the backstop.
                        constexpr float kPinnedSlack = 0.35f;   // sweep cone + step rounding + obliquity
                        if (shortfall <= radius + overlap + kPinnedSlack) {
                            ++r.pinned;
                            if (i + 1 < path.size()) {
                                ++r.tightCorners;               // it IS a tight corner; count it
                                if (r.firstTight == 0) r.firstTight = i;
                            }
                            continue;                           // reached at tangency -- next leg
                        }
                    }
                }
                r.firstBad   = i;
                r.badLength  = LenXZ(a, b);
                r.badReached = reachedM;
                r.badStopAt  = stopAt;
                r.badCause   = cause;
                FillStopPoly(r, stopAt);
                // OUT OF PROBES IS NOT A WALL. Reporting it as one bans a portal on the strength of a
                // measurement never taken, and every retry after that ban comes back longer (S95).
                // `checked` gives this leg back: the fast path counted it, and `ProvenPrefix` reads
                // `checked` as "legs actually verified" when it cuts a truncated route to its prefix.
                if (cause == StopCause::Budget) { --r.checked; r.truncated = true; break; }
                Diagnose(a, b, stopAt, r);
                r.ok = false;
                return r;
            }
            if (!oneShotClear) ++r.rescued;
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

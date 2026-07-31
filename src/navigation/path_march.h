#pragma once

#include <cstdint>
#include "navigation/nav_types.h"
#include "navigation/map_query.h"

// The two LEG-SCALE walkers (Session 100). Both take a single route leg -- a taut chord between two
// funnel corners -- and answer "can the party actually walk this line", each with a different
// instrument:
//
//   * `MarchLeg` asks the MESH: it walks the chord poly-to-poly across the walkmap's own adjacency,
//     applying the engine mover's accept rule at every edge crossing -- SINCE S100 WITH THE MOVER'S
//     OWN CLASS (the leader's, via `TerrainRefused`), which adds one cheap pure engine floor-test
//     call per crossing. Still no probe budget: probes price SWEEPS, and this makes none.
//   * `WalkLeg` asks the ENGINE: it walks the chord in body-sized steps with `MapQuery::BodySweep`,
//     carrying the engine's own resolved position forward so depenetration slides exactly as in play.
//
// WHY BOTH EXIST -- the Session 100 finding that reframed four failed sessions. The engine's real
// movement refusal (`FUN_0022f9b0`) is PURELY adjacency-based: an edge is a wall iff the neighbour
// index is < 0 or the neighbour fails the class-4 walkable test. No geometry prims are involved. The
// body sweep (`FUN_00230c10`) is the complement: one zero-width centre ray over the geometry prims
// plus a 0.27 m sphere at the DESTINATION only -- it never reads adjacency at all. So a chord that
// leaves the walkable strip across a cliff lip or a mesh-boundary jog stops the player in play and
// passes the sweep at ANY length and ANY step size. The march is the instrument for exactly that
// class; the sweep remains the instrument for volumes. Neither can replace the other.
//
// GAME THREAD ONLY for WalkLeg (engine calls). MarchLeg is memory-only but reads the live walkmap,
// so it has the same nav-safe requirements as every other NavMesh consumer.
namespace PathMarch {

// Body height for lifting sweep endpoints off the floor, and the arrival slack added to the body
// radius. Shared with path_validate -- one definition, or the two walkers drift.
constexpr float kBodyPad  = 0.9f;
constexpr float kEndSlack = 0.15f;

FVec3 Lift(const FVec3& p);

// The walkmap's own floor height under a point; falls back to `seedY` off-mesh. FUN_00380c40 PINS
// the actor's Y to the poly plane, so this is not an approximation of vertical movement -- it IS it.
float GroundY(float x, float z, float seedY);

float LenXZ(const FVec3& a, const FVec3& b);

// Did the body finish close enough to where it was asked to go? A DISTANCE, not a ratio (S95).
bool Arrived(const MapQuery::BodyMove& mv, float tol);

// What ended the sub-step walk of a leg. `Wall` WAS DELETED in S97 (the PointInVolume veto) -- do
// not re-add it. `March` is the Session 100 cause: the mod's own adjacency march found a crossing
// the engine's mover would refuse.
enum class StopCause {
    None = 0,
    Sweep,     // the engine's body sweep would not carry the body that far -- floor or depenetration
    Budget,    // ran out of probes / step cap. NOT a breach: sets `truncated`, never `ok = false`
    March,     // the adjacency march met an edge with no walkable neighbour -- the mover's own refusal
};
const char* CauseName(StopCause c);

// Walk the leg the way the CHARACTER does: fixed 0.5 m displacements, Y pinned to the walkmap under
// each step, the engine's own resolved position carried forward. Returns true when the body reached
// the far end. On false, `cause` says whether it was a real stop (`Sweep`) or bounded coverage
// (`Budget` -- the step CAP or the probe budget ended the walk before the far end; the ground
// actually walked was clean).
//
// THE STEP LENGTH IS FIXED AND IS NOT NEGOTIABLE AGAINST ANY BOUND (Session 96, re-affirmed S100
// when the cap changed roles). Past ~0.54 m the sweep's probe cone leaves the body and it reports
// walls in corridors wider than the body. A bound must cost COVERAGE -- honestly reportable as
// `truncated` -- never ACCURACY, which is silently a false wall. So `steps` is clamped by the cap
// and the budget, each step stays 0.5 m, and a clamped walk that completes clean returns `Budget`
// with `reachedM` = the ground actually covered.
bool WalkLeg(const FVec3& from, const FVec3& to, float tol, int probeBudget,
             int& probesUsed, float& reachedM, FVec3& stopAt, StopCause& cause);

// ---- The adjacency march (Session 100) -----------------------------------------------------------

enum class MarchVerdict {
    Clear,      // every crossing along the chord satisfies the mover's accept rule
    Breach,     // a cleanly-identified crossing the mover would refuse, unrescued by the graze scan
    NoVerdict,  // the march could not decide -- counted, and validation proceeds EXACTLY as before
};

struct MarchResult {
    MarchVerdict verdict = MarchVerdict::NoVerdict;
    const char*  why     = "";   // NoVerdict reason for the log ("no-start-poly", "no-exit-edge", ...)
    FVec3    hitPoint{};         // Breach: the crossing point, Y from the exited poly's plane
    float    hitDistM = 0.0f;    // Breach: metres along the leg at the crossing
    int      fromPoly = -1;      // Breach: the poly the chord was leaving
    int      edge     = -1;      // Breach: which of its edges (0..2)
    int      nbr      = -1;      // Breach: the refused neighbour, -1 = none (a true boundary)
    uint32_t nbrEff   = 0;       // Breach: that neighbour's effective flags (0 when none)
    int      grazes   = 0;       // rescues: crossings refused-then-recovered within the graze allowance
};

// March the chord `a -> b` across the walkmap adjacency. Every ambiguity -- unreadable start poly,
// no identifiable exit edge, a vertex graze that cannot be attributed to one edge, a torn read, the
// poly cap -- is `NoVerdict`, never a breach: the march must FAIL OPEN, because inventing a wall on
// a working map is the one regression this project cannot afford (S96). A refused crossing within
// `endTol` of `b` is forgiven exactly as the sweep's arrival test forgives it -- routes legitimately
// end against exit seams, shopfronts and notice boards.
MarchResult MarchLeg(const FVec3& a, const FVec3& b, float endTol);

} // namespace PathMarch

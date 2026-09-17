#include "navigation/route_query.h"
#include "navigation/interact_target.h"
#include "navigation/map_seams.h"
#include "navigation/map_names.h"
#include "navigation/nav_common.h"
#include "navigation/nav_mesh.h"

#include <cmath>
#include <cstring>

namespace RouteQuery {

namespace {

// How near an exit counts as BEING AT IT, in X/Z metres. Measured need: on Muthru Bazaar the tester
// stood 0.78 m from the Rabanastre East End arrival and was still fed "East 3. 3 steps" on ten
// consecutive route presses, cycling through East / Northeast / Southeast / North and never
// converging, because the last two metres are a transition trigger and not a walk.
constexpr float kAtExitDist = 3.0f;
// ...and it must ALSO be within this much of the seam's own height (Session 74).
//
// Y used to be deliberately ignored here, on the reasoning that an exit's height was unreliable. That
// was true when the height came from the map-control blob, and it stopped being true in Session 64
// when an exit became a walkmap FLOOR POLYGON -- its Y is now a floor. Ignoring it meant Upper
// Apartments announced "At the exit" for the Highhall seam while the player stood 7.8 m beneath it,
// because the two exits there are 4 m apart horizontally and 9.7 m apart vertically.
//
// Generous on purpose: it only has to separate storeys, never to judge a slope or a doorstep.
constexpr float kAtExitDy   = 3.0f;

// How near an exit counts as arriving at it, for routing purposes. Deliberately the SAME numbers as
// "At the exit" above -- if the planner would call the player arrived there, the search must be allowed
// to finish there.
constexpr float kExitArriveDist = kAtExitDist;
constexpr float kExitArriveDy   = kAtExitDy;

// How near a target the engine applies NO radius to (class 1 -- see InteractTarget::Reach) the
// route is allowed to finish. This is a SANITY BOUND, not a discriminator, and the difference
// matters: A* pops in order of remaining distance to the target, so the first poly inside this
// bound is already about the nearest walkable point to the object -- the bound only stops the
// search settling for somewhere absurd. Deliberately the same 3.0 m as kExitArriveDist and for
// the same S96 reason: when the target has no polygon you can stand on, "walk onto it" is not a
// goal any search can meet, and refusing to finish nearby produces a FALSE "No path" on a
// target the player can walk to by hand. Measured case: Draklor 67F "C.D.B.", off-mesh, nearest
// walkable point 0.69 m away, refused against a 0.50 m radius that was never the engine's.
constexpr float kNoRadiusApproach = 3.0f;

} // namespace

Params For(const FVec3& pos, bool isTransition, void* sceneObj, int seamGroup) {
    Params q;
    q.target       = pos;
    q.isTransition = isTransition;
    q.seamGroup    = seamGroup;

    // Route to where you could STAND and interact, not to where the object is. Both halves of the
    // engine's own predicate come along: the vertical BAND says which surfaces you could interact
    // from, and the horizontal REACH says how close you have to be -- so the search can stop exactly
    // where `;` starts answering instead of walking you onto the target.
    //
    // A transition is excluded on purpose: its destination IS the surface you walk onto.
    InteractTarget::Band  band;
    InteractTarget::Reach reach;
    if (!isTransition) {
        band  = InteractTarget::ReadBandFor(sceneObj);
        reach = InteractTarget::ReadReachFor(sceneObj);
    } else {
        // AN EXIT IS ARRIVED AT, NOT LANDED ON (Session 96).
        //
        // Transitions used to be given no band and no reach at all, which makes PathSearch require A*
        // to finish on the exit's OWN polygon and nothing else. That is stricter than the rest of the
        // mod: the planner already calls anything within kAtExitDist "At the exit" and stops routing.
        //
        // It produced a FALSE "No path" on an exit the tester then walked to by hand. The log shows why:
        // `reach=1` (NavReach found a reachable poly within its 4.5 m slack) while A* failed, and
        // `edge=6` -- the edge tests were barely refusing anything, so the search was not walled in, it
        // simply could not finish on the one polygon it was told to finish on. A map-jump surface can
        // easily be bordered by water on the sides you would never approach from.
        //
        // Supplying a band and a reach turns on `NoteFallback`, the machinery that already records the
        // first poly A* pops that you could stand on and interact from -- proven code, used by every
        // non-transition target since S73. Nothing else changes: if the exit's own poly IS reachable,
        // IsGoal still matches it first and the fallback is never consulted.
        band.valid  = true;
        band.lo     = pos.y - kExitArriveDy;
        band.hi     = pos.y + kExitArriveDy;
        reach.valid = true;
        reach.radiusMin = kExitArriveDist;
    }
    // radiusMin, not radius: the ellipse radius is direction-dependent, and a goal poly has to be
    // interactable from whatever angle the route happens to arrive at. Unless the engine applies
    // no radius to this class at all, in which case radiusMin is not a conservative number, it is
    // the wrong layout read confidently -- and the approach bound above is used instead.
    const float reachRadius = !reach.valid       ? 0.0f
                            : reach.engineRadius ? reach.radiusMin
                                                 : kNoRadiusApproach;
    // WHICH SOURCE, for the line the route key logs before its request, so a wrong route can be
    // attributed without guessing. If a "No path" ever shows up under `class1-no-engine-radius`, the
    // bound is what to question, not the reach model.
    q.reachRead   = reachRadius;
    q.reachSource = isTransition        ? "transition-arrive"
                  : !reach.valid        ? "none (reach unreadable -> target's own poly)"
                  : reach.engineRadius  ? "class3-radiusMin"
                                        : "class1-no-engine-radius";
    // No band, no goal cylinder: the band and the reach only mean anything together.
    if (band.valid) {
        q.bandLo = band.lo;
        q.bandHi = band.hi;
        q.reach  = reachRadius;
    }
    return q;
}

bool AtTransition(const Params& q, const FVec3& from, float& dist, float& dy) {
    dist = NavCommon::Distance2D(from, q.target);
    dy   = std::fabs(from.y - q.target.y);
    return q.isTransition && dist <= kAtExitDist && dy <= kAtExitDy;
}

PathSearch::Plan Search(const Params& q, const FVec3& from, uint32_t epoch,
                        std::vector<FVec3>& rawPoly, std::vector<FVec3>& poly, PathSearch::Stats& st) {
    // THE WHOLE SEAM, for the search's failure path only (Session 98).
    //
    // `target` is ONE VERTEX of a map-jump surface -- the nearest to the player when the scan ran.
    // That is the right point to measure a distance to and the wrong one to end a route on: on map
    // 315 the nearest vertex of a 27 m seam was the corner the walkable approach reaches LAST, and
    // the route validated 21 of 22 legs, drove 20 m along the surface, and was called "No path".
    // `PathSearch::Run` consults this only after the ordinary single-point search has already
    // failed, so a route that works today never sees it.
    //
    // A `false` here means NOT SWEPT YET, not "no seams" (map_seams.h) -- so the vector stays empty
    // and the search behaves exactly as it does today. Never invent a fallback from a blind read.
    std::vector<NavMesh::PolyId> seamPolys;
    if (q.seamGroup != 0) {
        std::vector<MapQuery::MapJumpSurface> surfaces;
        // The SAME map id the seam sweep is primed with (PathPlanner::OnGameFrame), so the cache's
        // read guard (`g_seamMap != mapId`) can only agree with the writer's.
        if (MapQuery::CachedMapJumpSurfaces(MapNames::CurrentMapId(), surfaces)) {
            for (const MapQuery::MapJumpSurface& s : surfaces) {
                if (s.group != q.seamGroup) continue;
                seamPolys.reserve(s.polys.size());
                for (int p : s.polys) seamPolys.push_back(static_cast<NavMesh::PolyId>(p));
                break;
            }
        }
    }

    // (Session 106's danger-zone argument was REVERTED in S108 -- it made map 568's Door 2
    // unroutable. The zone table survives only as the F10 sneak-assist map whitelist.)
    return PathSearch::Run(from, q.target, epoch, q.bandLo, q.bandHi, q.reach, rawPoly, poly, st,
                           seamPolys.empty() ? nullptr : &seamPolys);
}

bool AnsweredAboutTarget(const PathSearch::Stats& st) {
    // startPoly stays -1 for "no-start-poly" and for the no-world early return; "start-unreadable" found a
    // poly and then could not read it.
    return st.startPoly >= 0 && std::strcmp(st.pass, "start-unreadable") != 0;
}

} // namespace RouteQuery

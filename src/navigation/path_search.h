#pragma once

#include <cstdint>
#include <vector>
#include "navigation/nav_types.h"
#include "navigation/nav_mesh.h"   // NavMesh::PolyId, for the optional seam goal set below
#include "navigation/path_danger.h" // PathDanger::Disc, for the optional penalty zones below

// The route SEARCH: A* over the GAME'S OWN NAVMESH (NavMesh), not over a grid of our own invention.
//
// WHAT THIS REPLACED AND WHY (Session 74). The previous search ran over a uniform 1.5 m grid that the
// mod sampled itself, storing one floor height per cell and synthesising edges between neighbouring
// cells behind a step-height gate. It produced a confident, wrong answer on any map with vertical
// structure: Upper Apartments announced its Highhall exit (7.8 m up) and its Lower Apartments exit
// (1.9 m down) one step apart, because both collapsed onto the same flat cells -- and the Highhall
// "route" was actually a near-goal FALLBACK that stopped 3 m short and was spoken as if it had
// arrived.
//
// Three things went with the grid, and none of them are coming back:
//   * `kMaxStep` / `kStepDiscont` -- the engine imposes NO step limit between adjacent polys
//     (walkability is a flag test), so these refused edges the game walks. Any staircase with a
//     riser over 0.35 m was unroutable.
//   * `SnapToWalkable` -- a 9 m ring search for "somewhere walkable near the goal", which is how a
//     goal 7.8 m overhead got answered with the floor beneath it.
//   * the near-goal and bridge recovery passes -- both existed to paper over the grid's invented
//     connectivity. A search over real adjacency either reaches the poly or genuinely cannot.
//
// GAME THREAD ONLY -- the volume check on each expanded edge is a walk-class segment cast.
namespace PathSearch {

// `Frontier` is a route to the reachable point CLOSEST to the goal, for when the goal itself cannot be
// reached. It exists because "no route" is not an acceptable answer to a blind player: they cannot see
// which way to walk to get around whatever is in the way, so refusing to route strands them.
//
// IT IS A DISTINCT ENUM VALUE, NOT A FLAG ON `Route`, deliberately -- the compiler then forces every
// consumer's switch to decide what to say about it. The failure this guards against is recorded at the
// very line this replaced: the old grid search emitted a near-goal fallback and SPOKE IT AS A NORMAL
// ROUTE, which walked the tester confidently to a spot 3 m from an exit 7.8 m overhead. A frontier route
// MUST announce its shortfall.
enum class Plan { Route, Frontier, NoPath };

// Diagnostic counters. On a failure `nearDist` is the single most useful number: it separates "the
// goal poly is one edge away behind a closed door" from "the goal is in a different component".
struct Stats {
    int  expands = 0;             // polys popped
    int  touched = 0;             // polys seen
    int  rays    = 0;             // body sweeps + footprint tests performed
    int  startPoly = -1;          // -1 = the player is not standing on any readable floor poly
    int  goalPoly  = -1;
    int  endPoly   = -1;          // the poly the search actually finished on
    float nearDist = -1.0f;       // metres from the goal centroid to the closest poly reached
    const char* pass = "mesh";
    int  attempts  = 1;           // A* passes run; > 1 means a breach was found and re-searched around
    int  bannedEdges = 0;         // portals re-costed across all attempts
    // Corridor waypoints spliced back into a leg whose taut chord did not walk (Session 96). Non-zero
    // means the route shipped is the un-pulled one -- the corridor was fine and the shortcut was not.
    int  repaired = 0;
    float shortfall = 0.0f;       // Frontier only: metres from the frontier point to the goal
};

// Plan a route from `from` to `to` on the map identified by `epoch`.
//
// `bandLo`/`bandHi` are the target's INTERACTION BAND and `reachRadius` the engine's measured
// horizontal interaction reach (InteractTarget::ReadReachFor). When both are supplied the search
// stops at the FIRST poly it can both stand on and interact from -- which is the nearest such poly
// by walking distance, so the route no longer overshoots past the point where `;` starts answering.
// Pass an inverted band (`bandLo > bandHi`) or a zero radius to route to the target's own poly.
//
// `rawPoly` is the portal path the spoken legs are measured from; `outPoly` is currently the same
// polyline (PathDirections does its own simplification).
// `seamPolys` (optional): the walkmap polys of a map-jump SURFACE, when the target is a walk-onto
// transition. READ since Session 101, and read in exactly one way.
//
// WHY IT EXISTS. A transition fires when you walk onto ANY part of its surface (S64), but `to` is
// ONE boundary VERTEX of that surface, picked by straight-line distance. That point answers "how
// far away is this exit" correctly and "where should the route end" wrongly, twice over: a vertex
// is on the walkable boundary by construction so the body can never finish on it (kArrivalTol has
// been absorbing that on every map since S75), and straight-line nearest is not WALKING nearest --
// on map 315's 27 m seam it is the corner the walkable approach reaches LAST, so the route drives
// 20 m ALONG the surface and a mid-route replan from the bank comes back "No path" 16.4 m short.
//
// HOW IT IS READ. The set is tested on each pop only to remember the FIRST member the search
// reaches; no A* decision changes and no `pass=mesh` route is affected. Then, ONLY when nothing
// above was willing to be spoken as a Route, `PathSurfaceGoal::Route` rebuilds a corridor to that
// member ending at the PORTAL the corridor crosses onto it, and proves it with the ordinary body
// walk at the ordinary arrival tolerance.
//
// WHAT IT MUST NOT BECOME. Session 98 aimed at the seam member nearest the banked proven prefix's
// end. The prefix already ended on a seam poly, so the aim point WAS the reference (`0.0m from ref`
// x18 in the S99 log): the re-run validated the prefix it was derived from and spoke a confident
// route to a dead end. Reverted in S100 -- `pass=seam` must never appear in a log again. RULE
// (S99): a route may never be validated against a point derived from that same route's own
// progress. A portal between two mesh triangles is not such a point, which is why it is the one
// the endpoint is taken from.
// `danger` (optional): soft penalty zones for THIS request -- see path_danger.h. Null for every
// request the planner did not arm (which is every request except the table-named door), so the
// pricing block below is unreachable, not merely skipped, on all other routes. Crossings inside a
// disc pay the disc's weight; nothing is cut and validation is untouched.
Plan Run(const FVec3& from, const FVec3& to, uint32_t epoch,
         float bandLo, float bandHi, float reachRadius,
         std::vector<FVec3>& rawPoly, std::vector<FVec3>& outPoly, Stats& stats,
         const std::vector<NavMesh::PolyId>* seamPolys = nullptr,
         const std::vector<PathDanger::Disc>* danger = nullptr);

} // namespace PathSearch

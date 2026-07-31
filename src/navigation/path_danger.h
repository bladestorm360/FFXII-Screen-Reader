#pragma once

#include <vector>
#include "navigation/nav_types.h"

// Soft PENALTY ZONES around scripted danger actors, for maps whose story events punish proximity.
//
// WHY THIS EXISTS (Session 106). Map 568's sneak minigame: the player calls the guard away and must
// reach the stair door while he is distracted, but the script checks distance-to-the-soldier eight
// times ("get too close and he's liable to notice you") and a catch turns the player back and
// restarts the sequence. That notice radius exists ONLY as literals inside the story script -- no
// volume, flag, or walkmap datum exposes it -- so the mesh cannot see it and the sweep will happily
// validate a route straight past the soldier's nose.
//
// THE MECHANISM IS GLOBAL, THE DATA IS PER-MAP, AND THE ARMING IS PER-TARGET:
//   * A zone is a disc: crossings inside it pay a soft price (same tier as a measured block --
//     avoided whenever an alternative exists, still crossable when it is the only way through).
//     Never a cut, never part of validation: the body walk stays the sole authority on walkability.
//   * The table below `ActiveZones` is USER-AUTHORIZED map-specific data (2026-07-31), keyed by the
//     game's own identifiers (npcdic name index), never by anything invented here.
//   * Zones arm ONLY when the route target IS the door the table names. Routing to anything else on
//     the same map -- expressly including the Palace Servant who STARTS the event chain and stands
//     beside these guards -- passes an empty set, and an empty set never reaches the search at all
//     (the planner passes null), so every other route is byte-identical to a build without this file.
//
// Positions are LIVE: the planner rebuilds the zone set on every request/replan (~2 s apart during a
// run), so the discs follow the soldier as the distraction moves him. Radius/weight start as
// conservative estimates and are tuned from the capture diagnostic below -- every real catch a
// tester hits logs the measured distances, so the numbers converge on the script's own.
namespace PathDanger {

struct Disc {
    FVec3 c{};        // live actor position (world)
    float r = 0.0f;   // metres
    float w = 0.0f;   // metres of added cost for a crossing inside the disc
};

// Fill `out` with the zones for THIS request. Empty unless (mapId, target) matches a table entry
// AND the named actors are currently listed. `target` is the route goal the player asked for.
void ActiveZones(uint32_t mapId, const FVec3& target, std::vector<Disc>& out);

// Sum of `w` over zones containing `p` (XZ disc test -- the script's own check is horizontal).
float PenaltyAt(const std::vector<Disc>& zones, const FVec3& p);

// Capture-distance diagnostic (log-only). Called once per game frame from the planner's field tick;
// does nothing unless the CURRENT map has a table entry, so every other map pays one int compare.
// A field-tick gap > 600 ms is a scripted scene (the catch is one); when the tick resumes this logs
// the player's pre-gap position and the distances to the table's actors, so each tester capture
// refines the radius from real data. One line per gap, tag DANGER.
void NoteFieldFrame();

} // namespace PathDanger

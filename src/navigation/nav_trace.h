#pragma once

#include "navigation/nav_types.h"

// WHERE THE TRANSITION ACTUALLY FIRED — the one fact about map exits we have never measured.
//
// Every model of the transition trigger so far has been inferred from the map-control blob and then
// refuted in play: `+0x54[N+1]` (S46), the `+0x54`∪`+0x70` union (S55), the nearest walkmap boundary and
// the trigger-bearing march (S58), and now the claim that the `+0x84` edge record's bearing is the
// direction you cross (S59 — the tester was sent east into a wall). Each was plausible from the data and
// wrong on the ground, and each cost a play session to find out.
//
// The reason is always the same: **the transition is a script ZONE (VM native `0x202d`) and nothing in
// the blob we can read tells us its geometry.** So stop inferring it. The player crosses these zones
// constantly; this records exactly where they were standing when a map change fired, plus the path they
// took in. That is ground truth for the trigger position, on every map, for free — and it is also the
// oracle any future model must reproduce before it ships.
//
// Cost: one distance compare per field frame, and a write only after the player has moved ~1 m.
// GAME THREAD ONLY (it reads the live player position through PlayerState).
namespace NavTrace {

// Called once per field frame with the current map id and player position. Appends to the breadcrumb
// ring when the player has moved far enough; when `mapId` changes, dumps the trail from the map just
// left — the last entry is where the transition fired.
void OnFieldFrame(int mapId, const FVec3& pos);

// Dump the current trail on demand (the `'` diagnostic), for a map the player has NOT yet left. Shows
// where they have been able to walk, which is what says whether a direction is blocked.
void DumpTrail();

} // namespace NavTrace

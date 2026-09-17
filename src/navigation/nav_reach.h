#pragma once

#include <cstdint>
#include "navigation/nav_types.h"

// "What can the party actually walk to on this map?" — computed ONCE per map, then answered by lookup.
//
// WHY IT EXISTS: exits whose position no route can reach should not be offered. Answering that with one
// A* per exit per rescan is not an option — a failed search on East End cost ~45,000 raycasts and ~60 ms,
// rescans fire several times a second, and a mod-side stall in the game's own frame is treated here as a
// bug, never as the game being slow. One flood fill replaces N searches and is then free.
//
// COST MODEL: the fill runs on the GAME THREAD in bounded slices (kCellsPerFrame expansions per field
// frame, ~1 ms) and stops when the component is closed. East End's reachable component is ~2,150 cells,
// so it settles in well under a second and then costs nothing. It is not a poll: it is an event-triggered
// job that runs to completion once per map epoch and goes idle.
//
// PREDICATE: the same floor + body-height corridor test the planner uses, at ZERO lateral margin. Zero is
// deliberate — this decides whether to HIDE something from the player, so it must err toward "reachable".
namespace NavReach {

// GAME THREAD. Advance the fill for `epoch` (restarting it when the epoch or the player's component
// changes). Cheap no-op once the fill is complete.
void OnGameFrame(uint32_t epoch, const FVec3& playerPos);

// GAME THREAD. Drop the set on map teardown.
void Invalidate();

// ANY THREAD. True once the fill has closed the component and answers can be trusted.
bool Ready();

// ANY THREAD. True when any cell within `tolerance` metres of `p` is in the reachable set. Always true
// while the fill is incomplete, so nothing is ever filtered on a half-built answer.
//
// The tolerance matters: door triggers sit ON the map seam, frequently a cell or two past the last
// walkable sample, so a zero-tolerance test would report real exits as unreachable.
bool Reachable(const FVec3& p, float tolerance);

// MEASUREMENT ONLY (Session 147). The same question answered off a second flood that also refuses
// terrain the leader's class cannot enter (bit 23 -- water, lava, bog, out of bounds), so the two
// answers can be compared in a log instead of argued about.
//
// **NOTHING MAY FILTER ON THIS.** It exists because our own archived logs print, on adjacent lines,
// a census calling a poly `*** UNWALKABLE (bit23) ***` and a routability line calling the exit
// standing on it `walk=1 reach=1` -- and `unreachable=0` in every log ever recorded, because the
// permissive flood crosses the Waterway's flooded channels. Whether THIS is the right second gate is
// exactly what is being measured: it must go false on the exits the player genuinely cannot reach
// AND stay true on the working bit-23 exits (Bhujerba's Travica Way is the control -- same flags,
// routes fine). If it fails either half it is the wrong instrument too, and the log will say so.
// Do not promote it to a filter before that measurement exists. See nav_reach.cpp.
bool ReachableStrict(const FVec3& p, float tolerance);

// Diagnostics: cells in the reachable set (0 until the fill starts).
int CellCount();

// ANY THREAD. How many times the flood has (re)started -- a new map, or the player turning up outside the
// component it closed (a lift, a scripted move). The Unreachable filter starts a new world generation on
// a change, because a route verdict taken from the other component says nothing about this one (S182).
// (Replaces S179's ContainsPoly, whose only reader was the flood verdict S182 removed.)
uint32_t Generation();

} // namespace NavReach

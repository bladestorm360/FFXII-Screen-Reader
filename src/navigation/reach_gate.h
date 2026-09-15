#pragma once

#include <cstdint>
#include <vector>
#include "navigation/nav_types.h"
#include "navigation/nav_mesh.h"
#include "navigation/entity_scan.h"

// "CAN THE PLAYER GET THERE RIGHT NOW?" -- the unreachable filter, Session 179.
//
// Behind the mod-menu row `Unreachable filter`, DEFAULT OFF. With the row off nothing here changes what
// is listed or how anything routes; the verdicts are still computed and LOGGED ("would hide"), so real
// maps can be checked before anyone turns it on.
//
// THE MEASUREMENT IT RESTS ON. A closed in-map door is not a collision object the mesh cannot see. The
// door's routine calls `setmapidfloor(N, class, 0)`, which forces the leader's floor-refusal bit (23)
// on for walkmap MATERIAL N through the override bank the mod already applies (NavMesh::PolyFlags'
// effective flags). Opening it calls the same with 1. So:
//
//   SCRIPT-CLOSED  =  the party's class bit is CLEAR in the poly's RAW flags
//                     and SET in its EFFECTIVE flags.
//
// That is deliberately NOT "bit 23 is set". Raw bit 23 is static map data that also marks ledges and
// out-of-bounds ground under exits that route fine (Travica Way), and cutting on it is the S96 lever
// that cost a working exit. Only a refusal the SCRIPT imposed at runtime counts here.
//
// THE VERDICT. A third flood -- separate state, separate restart, never touching NavReach's two --
// expands through `Walkable` and refuses script-closed polys. An entity is judged on the mesh polys
// found on rings up to 3 m around it (so an object you press from beside it is judged by where you
// stand):
//   Reachable          any of those polys is in the open component
//   BehindClosedFloor  none is, but one is in NavReach's permissive component: the only way there is
//                      through a floor a script has closed (a door, a flood gate, a barrier)
//   Disconnected       no mesh connection at all (across water, another level with no walkway)
//   Unknown            no answer yet, no poly found, or a flood is refilling -- NEVER hidden
// Enemies are never judged: they come to you, and hiding one is not a navigation question.
namespace ReachGate {

enum class Verdict : uint8_t { Unknown = 0, Reachable, BehindClosedFloor, Disconnected };

// GAME THREAD. Advance the open-component flood. Restarts on a new epoch, when the player stands
// outside the published set, and when the override table's bytes change (a door opened or closed).
void OnGameFrame(uint32_t epoch, const FVec3& playerPos);

// GAME THREAD. Drop everything on map teardown.
void Invalidate();

// GAME THREAD (reads the party's movement class). True when a script has closed `p` to the party --
// see the header note for the exact test.
bool ScriptClosed(NavMesh::PolyId p);

// ANY THREAD. Judge and store a verdict on every entity, logging each change of verdict once. Called
// by the list rebuild; it never removes anything -- the list filter decides, and only when the row is on.
void Annotate(std::vector<EntityScan::Entity>& list);

// Whether a stored verdict is one the filter hides.
inline bool Hides(uint8_t verdict) {
    return verdict == static_cast<uint8_t>(Verdict::BehindClosedFloor) ||
           verdict == static_cast<uint8_t>(Verdict::Disconnected);
}

} // namespace ReachGate

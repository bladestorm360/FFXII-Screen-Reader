#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "navigation/nav_types.h"
#include "navigation/nav_mesh.h"
#include "navigation/entity_scan.h"

// THE UNREACHABLE FILTER, AND THE SCRIPT-CLOSED FLOOR TEST UNDER IT -- Session 179, rebuilt Session 182.
//
// ---- script-closed floors ---------------------------------------------------------------------------
// A closed in-map door or a Sochen waterfall is not a collision object the mesh cannot see. Its routine
// calls `setmapidfloor(N, class, 0)`, which forces the party's floor-refusal bit on for walkmap MATERIAL N
// through the override bank the mod already applies (NavMesh::PolyFlags' effective flags). So:
//
//   SCRIPT-CLOSED  =  the party's class bit is CLEAR in the poly's RAW flags
//                     and SET in its EFFECTIVE flags.
//
// Deliberately NOT "bit 23 is set": raw bit 23 is static map data that also marks ledges and ground under
// exits that route fine (Travica Way), and cutting on it is the S96 lever that cost a working exit.
// PathSearch CUTS a script-closed poly, whatever the filter row says: it is the engine's own runtime
// refusal, not our inference (the user's S181 ruling -- price what we infer, cut what the game declares).
//
// ---- the filter ------------------------------------------------------------------------------------
// Mod-menu row `Unreachable filter`, default OFF. The user's contract (2026-09-17): Off lists everything
// and `\` says "No path" to anything with no valid path; On hides what has no valid path.
//
// THE VERDICT IS THE ROUTE KEY'S OWN ANSWER, AND ONLY WHEN THE PLAYER ASKED FOR IT. When a route the
// player requested (and heard) comes back "No path" -- NoPath, or a Frontier, which the planner speaks as
// "No path" -- that entity is hidden while the row is On. Nothing here ever runs a search, a flood, or any
// per-frame work of its own:
//   * S179 judged a flood of the mesh; it disagreed with the router (Pilgrim's Door 1 "reachable" to the
//     flood, "No path" to `\`) and in two play logs hid nothing.
//   * S182's first build ran real searches in the background on the game thread. REVOKED BY THE USER
//     before it was ever deployed: each check stalled a frame (2-43 ms), and a mod that can freeze the
//     game is not acceptable without express permission -- see CLAUDE.md and Lessons.md L-88.
// So an entity is hidden only AFTER `\` has said "No path" to it. That is the whole mechanism.
//
// A recorded answer holds for one WORLD STATE: the same map epoch, the same override-table bytes (a door
// or waterfall moving changes them) and the same NavReach component (a lift or scripted move changes
// it). Any change and the entity is listed again. All of that is compared at the list rebuild, on the
// thread doing the rebuild; the game thread only stores the answer the route key already produced.
//
// Enemies are never hidden: they come to you, and hiding one is not a navigation question.
namespace ReachGate {

enum class Verdict : uint8_t { Unknown = 0, Reachable, NoPath };

// ---- script-closed floors ------------------------------------------------------------------------
// The effective-flags bit that refuses the party's current movement class (bit 23 class 0, 25 class 1,
// 26 class 2, 27 class 3, 24 class 5), or 0 when no refusal bit is known -- in which case nothing is ever
// script-closed. GAME THREAD (reads the party's movement class); read it once per search.
uint32_t PartyRefuseBit();

inline bool ScriptClosedFlags(uint32_t raw, uint32_t eff, uint32_t bit) {
    return bit != 0 && (raw & bit) == 0 && (eff & bit) != 0;
}

// The walkmap material id a poly's floor-override entry is indexed by (FUN_00232020's first bank).
inline uint32_t Material(uint32_t raw) { return (raw >> 13) & 0x1F; }

// ---- WHICH CLOSURES THE GAME ACTUALLY DECLARES (Session 185) --------------------------------------
// Bit N set: some container-0 routine on THIS map calls `setmapidfloor(N, class 0, open)` -- that is,
// the map's own script has a way to OPEN material id N. 0xFFFFFFFF when the script could not be read,
// which deliberately reproduces S182's behaviour exactly rather than guessing.
//
// WHY IT EXISTS. `ScriptClosedFlags` above answers "the override bank refuses this floor and the raw
// bank does not", and S182 made that a CUT in A*. On Sochen Cave Palace every such floor is a door.
// On the Dreadnought Leviathan, 44 crossings matched the same shape and **the player walked straight
// through them** -- the exact falsifier S182 wrote down for its own cut ("a map where this fires and
// the player walks that crossing by hand").
//
// The user's rule settles what to do about it: PRICE WHAT WE INFER, CUT WHAT THE GAME DECLARES. A
// floor some door script can open is a declaration -- the game says it is a door and says how it
// opens. A floor that reads closed with no script anywhere on the map able to open it is an
// INFERENCE, and it is the one that was wrong. So the cut now needs both: the closed-flag shape AND a
// script that owns the material. Everything else falls through to the ordinary terrain PRICE, which
// is what S96 established and what every map but 184 has always used.
//
// GAME THREAD. Cached on `MapScript::ScriptFingerprint()`, so the routine table is read once per
// script load and every later search is a compare and a return.
uint32_t OpenableFloorMask();

// GAME THREAD. True when a script has closed `p` to the party.
bool ScriptClosed(NavMesh::PolyId p);

// ---- the filter ----------------------------------------------------------------------------------
// GAME THREAD, from PathPlanner's drain, for a request the player heard. Stores the answer against the
// label and place it was asked for, with the world state it was given in. O(records), no search.
void NoteRouteResult(const std::wstring& label, const FVec3& target, bool reachable);

// ANY THREAD (the list rebuild, under EntityList's lock). Stamp each entity with the recorded answer that
// still holds for the current world state, else Unknown. Removes nothing -- EntityList's filter decides.
void Annotate(std::vector<EntityScan::Entity>& list);

// Whether a stamped verdict is one the filter hides.
inline bool Hides(uint8_t verdict) { return verdict == static_cast<uint8_t>(Verdict::NoPath); }

} // namespace ReachGate

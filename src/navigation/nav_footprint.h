#pragma once

#include <cstdint>
#include "navigation/nav_types.h"
#include "navigation/nav_mesh.h"

// THE ENGINE'S HARD "YOU CANNOT WALK HERE", REPLICATED (Session 93).
//
// This is `FUN_0022f9b0` (RVA 0x10F9B0) as a pure memory-read predicate. It is the check the tester
// spent several sessions describing and that three earlier passes looked for in the wrong place: not a
// terrain TYPE, not a slope, not a height -- a BODY-versus-BOUNDARY test.
//
// WHAT THE ENGINE DOES, read line by line from the decompile:
//   * For each of the current triangle's three edges, take the neighbour across it. If the neighbour
//     index is < 0, OR `FUN_00230a40(world, neighbour, class)` rejects it, the neighbour is DEMOTED TO
//     "does not exist" (`0022f9b0:85-88` literally sets the index to -1). One branch, two causes.
//   * The character is an ELLIPSE, normalised to a unit circle by matrices FUN_0022ef20 builds from
//     `moveCtx+0x80`/`+0x84`. If the body comes within 1.0 of a demoted edge (`:120`) the engine pushes
//     the position back out to exact TANGENCY along the edge perpendicular (`:129-142`), accumulates
//     the correction at `moveCtx+0x40`, and sets `moveCtx+0x60 |= 0x10` -- the "was blocked" bit.
//   * It then recurses into every WALKABLE neighbour the body overlaps (`:152-167`), so the footprint
//     is tested against the whole local border rather than one triangle.
//
// WHY THAT PRODUCES THE OBSERVED BEHAVIOUR. The push removes only the component along the edge normal,
// and FUN_002327d0:233-238 zeroes the normal's Y whenever the actor is ground-locked (the default, per
// FUN_00380b80:8). So a head-on push cancels entirely -- no progress -- while an oblique one keeps its
// tangential part and slides. Exactly what the tester reported.
//
// WHY IT COVERS CLIFFS WITH NO HEIGHT TEST, and why the twice-struck step/slope gate was always wrong:
// FUN_00380c40:24-28 PINS the actor's Y to the poly plane, so there is no gravity on the walkmap and
// nothing to fall off. A cliff is not a drop, it is an edge whose neighbour index is < 0 -- refused by
// the identical branch that refuses a wall. Water, a fence line, the map edge and terrain the party's
// class cannot stand on are all the same branch by a different route. ONE mechanism.
//
// WHY THIS IS A REPLICA AND NOT A CALL. `FUN_0022f9b0` takes no footprint argument: the body reaches it
// only through the globals `DAT_020892c0` / `DAT_02089280`, which `FUN_0022ef20` must write first. So
// there is no way to call any footprint-aware engine predicate without writing game memory, and the mod
// is strictly read-only. Replicating is also this codebase's established idiom -- nav_mesh.cpp and
// map_query.cpp are already memory-only replicas of FUN_00231890, FUN_002324f0 and FUN_00231900.
// The body SWEEP half (walls, volumes, doors) is a genuine pure call and stays one: MapQuery::BodySweep.
namespace NavFootprint {

using NavMesh::PolyId;

// Can the party's body sit at `pos` -- taken to be on or near `poly` -- without overlapping a boundary
// of the walkable region? False means the engine would push the character off this point, so a route
// must not ask them to stand on it.
//
// `outMargin`, when given, receives the smallest (distance-to-border minus body radius) seen, in
// metres: negative is the overlap depth, positive is the clearance. It is logged rather than gated on,
// so a bad radius shows up as a number in the tester's next session instead of as an unroutable map.
bool Clears(const FVec3& pos, PolyId poly, float* outMargin = nullptr);

// The body radius the test uses. This is the engine's own collision radius (NavRva::MAP_BODY_RADIUS,
// 0.27f, the literal at all three FUN_00230c10 call sites) used as a CIRCLE.
//
// The engine shapes it as an ellipse from `moveCtx+0x80`/`+0x84`, reached via
// actor -> +0xC0 -> +0x138 -> +0x30 (FUN_00265970). Those two half-extents are NOT read here: the
// offset chain is unconfirmed against the live process, which puts it under the 0.98 bar, and
// FUN_002327d0:254 compares them against 2.0 so they are plainly per-actor rather than constant.
// A circle of the confirmed radius is the honest approximation until a probe settles the pair.
// Do NOT substitute XFORM_PLAYER_SHAPE for them -- that is the interaction reach envelope, a different
// quantity, and mixing the two is the exact "one name, two facts" failure this project keeps hitting.
float BodyRadius();

} // namespace NavFootprint

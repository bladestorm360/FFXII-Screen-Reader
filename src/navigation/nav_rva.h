#pragma once

#include <cstdint>

#include "core/phyre_types.h"

// Confirmed pathfinder / field-navigation RVAs and struct offsets.
//
// RVAs are relative to FFXII_TZA.exe's image base and resolve via
// Hooks::ResolveRva() / Hooks::InstallTyped(). Ghidra decompile addresses are
// ABSOLUTE (= RVA + 0x120000), so every RVA below is `Ghidra ABS - 0x120000`.
// Verify any change by adding 0x120000 back and matching the decompile symbol.
// Source: Docs/GameArchitecture.md "Pathfinder / Field Navigation (Phase 4)".
namespace NavRva {

// The actor-pool / BtlChr / scene-kind offsets this module reads live in core/phyre_types.h
// (shared with the battle readers). Pulled in unqualified so existing uses read unchanged.
using namespace PhyreTypes;

// ---- Field leader → player-position chain (from FUN_00317e60) ----------------
// DAT_022c7fe0 (ABS 0x022c7fe0): the field-controlled character's scene handle
// (a uint, NOT a pointer). FUN_003590d0 is literally `return DAT_022c7fe0;`.
constexpr uint32_t LEADER_HANDLE = 0x21A7FE0;
// DAT_02089340 (ABS 0x02089340): field-active gate; (byte & 0x10) != 0 while a
// field map is live. Null/zero on title + between maps.
constexpr uint32_t FIELD_ACTIVE = 0x1F69340;
// DAT_02098e10 (ABS 0x02098e10): base of the per-selector handle tables.
// FUN_00263ff0(sel) = &DAT_02098e10 + sel*0x51 (undefined8 units) = base + sel*0x288.
constexpr uint32_t HANDLE_TABLE_BASE = 0x1F78E10;
// 0x2D9F190 was ALSO declared here twice (PARTY_MGR_PTR, FIELD_STATE_BLOCK) and once in
// battle_state.cpp (RVA_BTLWORK). Both copies here were unused; it is PhyreTypes::BTLWORK_PTR now.

// ---- Bullet walkability (from FUN_006a1a70 / FUN_006a0310) -------------------
// FUN_006a0310 (ABS 0x006a0310): builds the per-map physics world; hook it to
// cache the physics context (its single RCX arg). Fires once per map load.
constexpr uint32_t BUILD_WORLD = 0x580310;
// FUN_006a1a70 (ABS 0x006a1a70): ready-made ray cast.
//   (context, from[xyz], to[xyz], out[8], filterGroup). Reads world at
//   *(context+0x60); returns 1 on hit (out[0..2]=hit point, out[3]=1.0,
//   out[4..6]=normal), 0 on miss. Filter 0xF = all groups.
constexpr uint32_t RAYCAST_WRAPPER = 0x581A70;
// FUN_0069f070 (ABS 0x0069f070): the per-world physics STEP. arg0 (RCX) is the SAME
// PPhysicsWorld ctx BUILD_WORLD populates and RAYCAST_WRAPPER queries — it reads the
// Bullet world at *(arg0+0x60) and even lazily calls FUN_006a0310(arg0) to build it.
// Unlike BUILD_WORLD (one-shot, fires only on a fresh map build), this runs EVERY field
// frame regardless of when we hooked in — so it captures the ctx on a save-load into an
// already-built area, which BUILD_WORLD misses. Signature: undefined8(ctx, stepCtx).
constexpr uint32_t WORLD_STEP = 0x57F070;
// NOTE: BUILD_WORLD + WORLD_STEP both live inside the master physics tick's per-region
// loop, which is SKIPPED when the region count is 0 — so a scene that builds no Bullet
// world (e.g. the scripted Nalbina prologue) never fires either. The capture points that
// fire while an actor WALKS on a world are RAYCAST_WRAPPER + CHAR_GROUND_RESOLVE below.
// FUN_006a5c00 (ABS 0x006a5c00): the char-controller ground/slope resolve. It guards
// *(arg0+8)!=0 and passes *(arg0+8) (the physics ctx) to the raycast, once per frame per
// walking actor. Hook it and cache *(arg0+8) — fires wherever a Bullet world is stepped.
// Signature: undefined8(void* p1, float* p2, float p3)  (p3 in XMM2).
constexpr uint32_t CHAR_GROUND_RESOLVE = 0x585C00;

// ---- SQEX field-collision walkability (the ACTUAL field walkmap) -------------
// The game's own floor/wall mesh collision — the world the AI NPCs walk on. Loaded
// with every field map, INDEPENDENT of Bullet (so it is live in the Nalbina prologue,
// which builds no Bullet region-world). Reentrant, read-only, arbitrary-point; the ctx
// is a global (no capture hook needed). This REPLACES the Bullet raycast walkability.
//
// FUN_003208c0: `bool groundAt(float x, float z, float* outY)` — returns true iff a
// walkable floor exists at (X,Z), writing its height to *outY. Uses its own cached ctx
// (DAT_02ec1370). MS x64: x=XMM0, z=XMM1, outY=R8. (The game's own arbitrary-XZ probe.)
constexpr uint32_t MAP_GROUND_AT = 0x2008C0;
// FUN_00230b60: `int segTest(void* ctx0, void* outHit16, const float from[4],
// const float to[4], u16 mask, u32 flags)` — segment-vs-walkmap test. Returns a hit
// index >=0 (BLOCKED) / <0 (CLEAR). from/to are {x,y,z,1}. This is the SQEX analogue of
// the Bullet raycast wrapper, and exactly what the NPC wall-feelers use.
constexpr uint32_t MAP_SEG_TEST  = 0x110B60;
// Collision-manager gate + ctx0 pointer (memory-only, from FUN_0026e500):
// gate = *(MAP_COLL_GATE) (the collision manager; 0 => field collision not loaded);
// ctx0 = *(MAP_COLL_CTX0) when the gate is set.
constexpr uint32_t MAP_COLL_GATE = 0x1F7A670;  // DAT_0209a670
constexpr uint32_t MAP_COLL_CTX0 = 0x1F7A678;  // DAT_0209a678
// FUN_00230b60's "mask" is a query-CLASS enum (compared ==4 in FUN_0022cc50), NOT a
// bitmask. Class 4 = WALKING: blocks real walls + character-only invisible walls, skips
// camera-only occluder planes / floors / ceilings / triggers. This is the exact class the
// PLAYER leader's own per-frame wall feelers use (FUN_0032cf50->FUN_003d97e0(...,4)->
// FUN_00230b60(...,4,0)), so a SegmentClear with it matches where the character can walk.
// flags: 0 = scan for nearest blocker (what movement uses); 1 = return on first hit.
// 0xffff/1 is the CAMERA/occlusion class (doubly wrong for routing) — kept only so the
// '-key self-test can log the walk-vs-camera contrast as a runtime confirmation.
constexpr uint16_t MAP_MASK_WALK     = 4;       // walking geometry — routing uses this
constexpr uint32_t MAP_SEG_FLAGS     = 0;       // nearest-blocker (movement)
constexpr uint16_t MAP_MASK_CAM      = 0xFFFF;  // camera/occlusion (diagnostic contrast only)
constexpr uint32_t MAP_SEG_FLAGS_CAM = 1;

// TWO DIFFERENT THINGS ARE CALLED "CLASS 4", AND CONFLATING THEM COST A WHOLE RESEARCH PASS
// (Session 93). Keep them named apart:
//   * MAP_CLASS_PARTY_SEG (this one, == MAP_MASK_WALK) is the SEGMENT/ray-vs-prim class. It is stored
//     at the query struct's +0x46 and compared `== 4` in FUN_0022cc50. It is what the two actor movers
//     pass as arg5 of FUN_00230c00 -> FUN_00230c10. CONFIRMED at the call sites.
//   * The FLOOR class -- the third argument of FUN_00230a40, which decides whether a poly is walkable
//     -- is a DIFFERENT field on a different object: `*(u16*)(walkCtrl + 0x50)`, read at
//     FUN_002327d0:267 and indexed by FUN_00380d30:20. Nothing in the movers writes it, and
//     FUN_00380b80:12 initialises it to 0xffff.
// The paragraph above used to describe the first and be relied on as though it described the second,
// which is what licensed `(effectiveFlags & 7) == 0` as "the" walkability rule and sent an entire
// investigation looking for a per-class terrain bit that does not exist. The CONCLUSION survives --
// 0xffff and 4 both fall through FUN_00230a40's 0/1/2/3/5 branches identically -- but its stated
// REASON was wrong, and the hard refusal is not in the flags word at all (see nav_footprint.h).
constexpr uint16_t MAP_CLASS_PARTY_SEG = 4;

// FUN_00230c10: `int bodySweep(void* ctx0, float outPos[4], const float from[4], const float to[4],
// u16 queryClass, float bodyRadius)`. The character controller's OWN "may I move here" test: a DDA
// segment walk, then an ellipsoid depenetration over CSR layers 0|1|2 with a sphere of `bodyRadius`,
// then two +/-30 degree probes keeping the shortest reach, then a pull-back along the travel axis by
// the penetration depth. Returns 0 = the whole displacement is legal; non-zero = blocked, and outPos
// is where the character actually ends up.
//
// Its complete 29-function call tree writes NOTHING to game memory, which is why the mod may call it
// (verified by transitive closure + assignment-target scan + an out-param pass). Confidence 0.98 on
// the purity, 0.99 on the signature and the two constants below -- both read off all three engine call
// sites (FUN_0032bcc0:52-55, FUN_0032beb0:36-38/:66-68, FUN_0032ca70:68-70).
constexpr uint32_t MAP_BODY_SWEEP   = 0x110C10;
// 0x3e8a3d71. The engine's own literal at every call site -- the character's collision body radius.
// NOT the interaction ellipse at XFORM_PLAYER_SHAPE: that one is a reach envelope for the `;` target
// test and is a different quantity. Do not substitute one for the other.
constexpr float    MAP_BODY_RADIUS  = 0.27f;

// ---- SQEX walkmap GRID structure (DIRECT read; the "map overlay" source) -----
// The walkmap is a uniform staggered ("brick") grid over a floor-triangle + wall-
// segment collision mesh — the same structure the engine's own full-grid enumerator
// FUN_0022ffe0 walks. ctx0 (= *MAP_COLL_CTX0) exposes a grid header, the vertex/floor/
// wall arrays, a CSR cell->list table, and the world origin. Reading these lets us bake
// a WHOLE-MAP walkability + height overlay with NO raycasts, and gives the exact per-map
// bounds for free (extent = nCols*cellSizeX by nRows*cellSizeZ meters, min-corner
// (-originX,-originZ)). Cell index is a 16-bit short in the engine, so nCols*nRows is
// hard-capped at 32767 => every map is a few hundred meters/side. Offsets from the
// decompiled bodies (FUN_00233050 world->cell, FUN_00231890 plane height); CONFIRMED at
// runtime by the baked self-diagnostic (direct read cross-checked vs MAP_GROUND_AT).
//
// ctx0 sub-fields:
constexpr uint32_t WALK_CTX_HEADER    = 0x00;  // ptr -> grid header (fields below)
constexpr uint32_t WALK_CTX_VERTS     = 0x08;  // ptr -> vertex array   (stride 0x10: x@0,y@4,z@8)
constexpr uint32_t WALK_CTX_POLYS     = 0x10;  // ptr -> floor-poly array (stride 0x20)
constexpr uint32_t WALK_CTX_WALLS     = 0x18;  // ptr -> wall-segment array (stride 0x90) [phase 2]
// CSR is FOUR LAYERS, not one: index = layer * (cellCount + 1) + cell. FUN_0022f830 drives every
// cell visit through exactly that arithmetic with layer in 0..3, selected by an 8-bit layer mask.
// Layer 0 = floor/plane polys, layers 1-2 = volumes, layer 3 = attribute/region polys. Reading
// layer 0 only (as we do) is correct for floors -- but the array is 4x longer than the old comment.
constexpr uint32_t WALK_CTX_CSR       = 0x20;  // ptr -> CSR offsets, u16[4 * (cellCount + 1)]
constexpr uint32_t WALK_CTX_PRIMS     = 0x28;  // ptr -> primitive index list (u16[])
constexpr uint32_t WALK_CTX_EXTENT_X  = 0x30;  // int nCols * cellSizeX (world width)
constexpr uint32_t WALK_CTX_EXTENT_Z  = 0x34;  // int nRows * cellSizeZ (world depth)
constexpr uint32_t WALK_CTX_ORIGIN_X  = 0x38;  // int originX (gridX = originX + worldX)
constexpr uint32_t WALK_CTX_ORIGIN_Z  = 0x3C;  // int originZ
constexpr uint32_t WALK_CTX_CELLCOUNT = 0x40;  // s16 nCols*nRows; the CSR layer stride is this + 1
constexpr uint32_t WALK_CTX_VOLCOUNT  = 0x44;  // u32 count of the 0x90-byte volume records
// grid header fields (at *ctx0):
constexpr uint32_t WALK_HDR_NCOLS     = 0x08;  // int cols (X axis)
constexpr uint32_t WALK_HDR_NROWS     = 0x0C;  // int rows (Z axis)
constexpr uint32_t WALK_HDR_CELL_X    = 0x10;  // int cellSizeX (world units/col, integer)
constexpr uint32_t WALK_HDR_CELL_Z    = 0x14;  // int cellSizeZ (world units/row)
// floor-poly entry (stride 0x20):
constexpr uint32_t WALK_POLY_STRIDE   = 0x20;
constexpr uint32_t WALK_POLY_PLANE_A  = 0x00;  // float plane A
constexpr uint32_t WALK_POLY_PLANE_B  = 0x04;  // float plane B (divisor; guard |B|>0.001)
constexpr uint32_t WALK_POLY_PLANE_C  = 0x08;  // float plane C
constexpr uint32_t WALK_POLY_FLAGS    = 0x0C;  // u32 flags; walk type = low 3 bits (0 = walkable)
constexpr uint32_t WALK_POLY_BASEVERT = 0x10;  // s16 base-vertex index -> vertex array
// The poly is a TRIANGLE: three s16 vertex indices, not one (Session 73, from FUN_002324f0, which
// walks `poly+0x10 + i*2` for i in 0..2). Only vert0 was known before, and without the other two
// there was no way to test whether a query point actually lies inside the poly -- so the plane was
// evaluated for points outside it and extrapolated to nonsense (a Clan Hall column reported floors
// at -2089 and +2537). WALK_POLY_BASEVERT is an alias of VERT0; both names kept, VERT0 preferred.
constexpr uint32_t WALK_POLY_VERT0    = 0x10;  // s16 vertex index 0 (== WALK_POLY_BASEVERT)
constexpr uint32_t WALK_POLY_VERT1    = 0x12;  // s16 vertex index 1
constexpr uint32_t WALK_POLY_VERT2    = 0x14;  // s16 vertex index 2
constexpr uint32_t WALK_POLY_TYPE_MASK = 0x7;

// ---- THE NAVMESH: per-edge ADJACENCY, Session 74 -------------------------------------------------
// The walkmap is not a soup of triangles indexed by a grid -- it is a connected NAVMESH, and these
// three s16s are the edges. `< 0` means no neighbour (a map boundary or a wall).
//
// Verified in the character mover FUN_002327d0, which carries a CURRENT POLY INDEX across frames:
//   iVar8 = FUN_002324f0(ctx, poly, &pos);                       // which edge did we cross? -1 = inside
//   sVar9 = *(short *)(param_1[2] + 0x16 + (poly * 0x10 + iVar8) * 2);   // = polyArr + poly*0x20 + 0x16 + edge*2
//   if (sVar9 < 0 || FUN_00230a40(ctx, sVar9, class) == 0) { blocked } else { poly = sVar9; }
// Corroborated at FUN_0022f9b0:86. Edge i is the edge from VERT(i) to VERT((i+1)%3), matching the
// index FUN_002324f0 returns.
//
// THIS IS THE ROUTING GRAPH. It is elevation-correct by construction -- a balcony and the floor
// beneath it are two disconnected components sharing a cell -- which is exactly what a uniform
// height-per-cell grid can never express.
constexpr uint32_t WALK_POLY_NEIGHBOR0 = 0x16;  // s16 neighbour poly across edge v0->v1
constexpr uint32_t WALK_POLY_NEIGHBOR1 = 0x18;  // s16 neighbour poly across edge v1->v2
constexpr uint32_t WALK_POLY_NEIGHBOR2 = 0x1A;  // s16 neighbour poly across edge v2->v0
constexpr uint32_t WALK_POLY_FLAGS2    = 0x1C;  // u32 second flags word (bit 0x400 observed)
// Plane B must be STRICTLY POSITIVE, not merely non-zero: FUN_00231890 gates on `0.001 < B` and
// FUN_00231900 repeats it. B <= 0 is a downward-facing poly (a ceiling); the engine never treats
// one as ground. Our old `|B| > 0.001` accepted them.
constexpr float    WALK_POLY_MIN_B     = 0.001f;
// Point-in-triangle epsilon from FUN_002324f0: an edge REJECTS the point when the normalised
// cross(edge, vertex->point).y is <= -0.0001, and an edge shorter than this cannot reject at all.
constexpr float    WALK_POLY_EDGE_EPS  = 0.0001f;
// MAP-JUMP SURFACE TAG, bits 3+ of the same flags word (Session 64).
//
// The `mapctrl` script sets these via `setmapidmj`, the sibling of `setmapidfloor` / `setmapidwall`
// which own other bit fields further up the word. A non-zero value marks the floor you walk onto to
// fire a transition, and the value IS the map-jump group id -- the same number the owning
// `__MJ_CTRL` routine passes to `setmapjumpgroup(K)`. So this tag is what ties a transition's
// GEOMETRY to its DESTINATION, both read from the map's own loaded data on the first frame.
//
// Measured on two maps: the field takes values 0x08/0x10/0x18/0x20/0x28/0x30 in the low byte, i.e.
// group 1..6 at bit 3. Muthru Bazaar has groups {1,3} and exactly two usable transitions; East End has
// {1..6}, one per controller.
//
// STRUCK (Session 74): the mask was 0x1F -- FOUR bits, not five. "the exact width of the field cannot
// matter" was wrong, and it is what put the Highhall exit in the wrong place. The engine reads
// `(flags >> 3) & 0xf` -- verified directly in BOTH FUN_00232020 (the flag-override decoder) and
// FUN_00230a40 (floor walkability). Bit 7 belongs to the next field, so with a 5-bit mask any seam
// polygon carrying it computed as `group + 16`, matched no `setmapjumpgroup(K)`, and was SILENTLY
// DROPPED. Upper Apartments' Highhall seam then read as 2 polys spanning a 0.3 m depth with a 1.9 m
// rise -- not a surface anyone can stand on, because it was a fragment of the real one.
constexpr uint32_t WALK_POLY_MJ_SHIFT  = 3;
constexpr uint32_t WALK_POLY_MJ_MASK   = 0xF;
constexpr uint32_t WALK_VERT_STRIDE   = 0x10;
// Primitive index encoding, from FUN_00232160 -- THREE ranges, not two:
//   [0x0000,0x4000)  floor/plane polygon,  polyArr + idx*0x20
//   [0x4000,0x5000)  static volume,        volArr  + (idx-0x4000)*0x90   (walls, pillars)
//   [0x5000,   ...)  DYNAMIC obstacle,     (idx-0x5000) into the manager's runtime tables --
//                    doors and moving platforms, enable at mgr+0x1D8, transform at mgr+0x1A0.
// STRUCK: ">= 0x5000 => empty/sentinel". That range is live, and it is why the polygon graph alone
// will happily route through a CLOSED GATE -- blockers are volumes, and volumes are not in floor
// adjacency. Hence the per-edge SegmentClear validator in nav_mesh.
constexpr uint16_t WALK_PRIM_FLOOR_MAX = 0x4000;
constexpr uint16_t WALK_PRIM_VOLUME_MAX = 0x5000;
constexpr uint32_t WALK_MAX_CELLS      = 32767; // 16-bit cell-index cap (sanity bound)

// ---- Runtime flag-override table (FUN_00232020) --------------------------------------------------
// A poly's EFFECTIVE flags are not the raw word: two banks of {u32 mask; u32 value;} rewrite it, and
// that is how a script opening a gate changes what is walkable without touching geometry.
//
//   A   = (flags >> 13) & 0x1F           material bank, entries 0x00-0x1F
//   C   = ((flags >> 3) & 0xF) + 0x40    group bank,    entries 0x40-0x4F
//   in  = (flags & ~mask[A]) | (value[A] & mask[A])
//   eff = (value[C] & mask[C]) | (in & ~mask[C])
//
// Floor FINDING deliberately bypasses this (FUN_00231900 uses raw bits, which is why our AllFloorsAt
// is right to). MOVEMENT does not -- FUN_00230a40 decodes it first. So edges use effective flags and
// floor lookup uses raw ones; they are different questions.
constexpr uint32_t WALK_FLAG_TABLE     = 0x1F7A3E0;  // DAT_0209a3e0; entry i = {u32 mask, u32 value} at i*8
constexpr uint32_t WALK_FLAG_ENTRIES   = 0x50;       // 0x00-0x1F material, 0x40-0x4F group
constexpr uint32_t WALK_FLAG_GROUP_BASE = 0x40;

// The party's movement class -- the third argument to FUN_00230a40. FUN_00230c10 takes it as arg5,
// and both actor movers (FUN_0032bcc0, FUN_0032ca70) pass 4 normally, switching to 0xffff ONLY as an
// unstick mode when already inside a volume or jammed within 0.27 units of a wall.
//
// Class 4 matches none of FUN_00230a40's 0/1/2/3/5 branches, so it falls through to "walkable" --
// meaning for the player, floor walkability is EXACTLY `(effectiveFlags & 7) == 0`, with no per-class
// opt-out bit. (Classes 1/2/3/5 gate on bits 25/26/27/24 and class 0 on bit 23; none apply to us.)
constexpr uint16_t WALK_CLASS_PARTY    = 4;

// ================= THE UNWALKABLE-TERRAIN CHECK (Session 96) =====================================
//
// FUN_00230a40(ctx0, s16 polyIdx, s16 moveClass) -> 1 = may stand, 0 = refused. **PURE** (reads the
// walkmap and WALK_FLAG_TABLE; writes nothing at all). THIS IS THE ANSWER TO "WHY DO WE ROUTE THROUGH
// WATER", and the mod had been replicating only its first line for three sessions:
//
//     if ((eff & 7) != 0) return 0;                      // type must be 0 (floor)
//     if (moveClass == 0) return ((eff >> 23) & 1) ^ 1;  // <<< THE PARTY LEADER
//     if (moveClass == 1) return ((eff >> 25) & 1) ^ 1;  // followers / NPCs
//     if (moveClass == 2) return ((eff >> 26) & 1) ^ 1;
//     if (moveClass == 3) return ((eff >> 27) & 1) ^ 1;
//     if (moveClass == 5) return ((eff >> 24) & 1) ^ 1;  // mounted
//     return 1;                                          // class 4 -- NEVER refused
//
// The DECOMPILE above is not in doubt. What was wrong is the claim built on it:
//
// ~~STRUCK (Session 96, end): "the party leader's floor class is 0, so bit 23 gates every poly it
// stands on, and `(eff & 7) == 0` is not the whole floor test for the player."~~
//
// **REFUTED IN PLAY. The tester walks through polys this refuses.** Wiring it into `NavMesh::Walkable`
// refused 399 of 690 floor prims on map 311 and cost an exit that had routed for the whole game up to
// that point. The water in Garamsythe is ankle-deep and the game has no swimming, so shallow water is
// ordinary floor with a puddle on it -- a predicate that refuses ground the player is demonstrably
// standing on is wrong, whatever function it came from.
//
// **THE WALK_CLASS_PARTY NOTE ABOVE STANDS, AND IT IS THE BETTER-EVIDENCED OF THE TWO.** It traced the
// argument FUN_00230a40 actually receives through the engine's own call sites -- FUN_0032bcc0 and
// FUN_0032ca70 pass 4 into FUN_00230c10 arg5, which lands at moveCtx+0x50. The struck claim reasoned
// instead from what FUN_002681d0 WRITES (0, at holder+0x153), and never established that the value the
// mover passes is the value that field holds. Class 4 hits no per-class branch, which is exactly
// consistent with the party walking on bit-23 ground. **Prefer the call site over the writer: only one
// of them says what the callee is handed.**
//
// The bit-23 mechanism itself is untouched -- attr 0 via the `mapid` script API (FUN_003792e0, RVA
// 0x2592E0) sets it, and the material bank can set it map-wide at runtime. What is NOT established is
// that it means "the party may not stand here". It is still asked, through NavMesh::TerrainRefused,
// and it decides nothing until it stops disagreeing with where the player is standing.
constexpr uint32_t MAP_FLOOR_WALKABLE  = 0x110A40;  // FUN_00230a40 -- CALL IT, do not replicate it
constexpr uint32_t MAP_EFFECTIVE_FLAGS = 0x112020;  // FUN_00232020(raw) -> effective. PURE.
constexpr uint16_t WALK_CLASS_LEADER   = 0;         // the player-controlled character
constexpr uint16_t WALK_CLASS_MAX      = 5;         // class domain is exactly {0..5}

// The leader's LIVE movement class. FUN_002681d0 (RVA 0x1481D0) computes it -- 0 for the
// player-controlled character, 1 for followers, 5 mounted, recomputed on leader switch, formation
// change and mount toggle -- and writes it to BOTH of these:
//     walkObj + 0x80   (u16)   where walkObj = *(*(character + 0xC0) + 0x138)
//     holder  + 0x153  (u8)    where holder  = *(character + 0xC0)
// The byte is one hop shorter, so that is what PlayerState reads. Chain verified against FUN_00265970
// (RVA 0x145970) and the writer's own tail. 0xffff at walkCtrl+0x50 is only the pre-init value from
// FUN_00380b80 for objects that never pass through FUN_002681d0 -- NOT what the party carries.
constexpr uint32_t CHAR_WALK_HOLDER    = 0xC0;
constexpr uint32_t WALKHOLDER_CLASS    = 0x153;     // u8
// ================================================================================================

// ---- THE VOLUME TEST (Session 96) ----------------------------------------------------------------
// FUN_00232490(ctx0, float pos[4], int flag) -> int hit count. PURE. **0 = no volume at this point.**
//
// THIS IS THE PREDICATE, and picking the wrong one cost a build. It is 90 bytes: set the per-primitive
// callback FUN_0022f8b0, the cell from FUN_00233050, **LAYER MASK 6 (CSR layers 1 and 2 = volumes)**,
// the position, a zeroed counter, then FUN_0022f830 to iterate -- and return the counter. Structurally
// identical to the volume half of FUN_00231400, which is how the field layout is known
// (+0 callback, +8 ctx, +0x10 cell u16, +0x16 mask u8, +0x18 pos, +0x20 counter, +0x24 flag);
// FUN_00231400 puts 1 in that last slot, so we do too. Confidence 0.98.
//
// WHY NOT FUN_00336390 / FUN_00231400 -- BOTH OVER-BLOCK FOR ROUTING (measured, Session 96).
// FUN_00231400 answers "may the party STAND here", which is a floor lookup AND a volume test, and it
// returns 0 when there is simply NO FLOOR under the point. FUN_00336390 wraps it and additionally
// requires the four (x+/-r, z+/-r) diagonals to pass. Every taut route corner is a portal endpoint
// inset by one body radius, i.e. ~0.27 m from the walkable boundary BY CONSTRUCTION -- so its
// diagonals land off the mesh, the test says "cannot stand", and routing reads ordinary corridor
// geometry as a wall. Shipped once: the tester got "almost every path is blocked" and "No path" on a
// corridor they had just walked down. Those two functions are for PLACEMENT (where may I put a
// character), where full clearance is the right question. For routing, ask only about volumes.
constexpr uint32_t MAP_POINT_IN_VOLUME = 0x112490;  // FUN_00232490 -- the one to use
constexpr uint32_t MAP_STAND_CLEAR     = 0x216390;  // FUN_00336390 -- placement only; over-blocks routing
constexpr uint32_t MAP_CAN_STAND       = 0x111400;  // FUN_00231400 -- floor AND volume; over-blocks routing
//
// The rest of the provenance, which applies to all three:
//
// WHY THIS MATTERS MORE THAN ANYTHING ELSE IN THIS FILE: the walkmap's floor adjacency knows nothing
// about walls. Blockers are VOLUME primitives -- [0x4000,0x5000) static (walls, pillars) and >=0x5000
// dynamic (doors, moving platforms) -- and until now the mod read ONLY floor polys, stepping over the
// volumes in the very per-cell list it walks (nav_mesh.cpp FindPolyAt). On a mesh whose triangles are
// often a whole corridor, a wall standing INSIDE a triangle was invisible to every test we had. That
// is what routed the tester into a hard stop.
//
// PURE: the transitive closure is FUN_0022f830 (CSR layer iterator, takes a layer mask), FUN_0022f8b0
// (its per-primitive callback), FUN_00230a40, FUN_002324f0, FUN_00233050, FUN_00233110, FUN_00381eb0,
// FUN_00202b70 -- every one 28-45 lines with ZERO DAT_ assignments. Same footing as the FUN_00230c10
// body sweep the mod already calls.
//
// CALLER MUST GATE ON MapQuery::HasWorld() and pass ctx0; the callee dereferences it unchecked.

// STRUCK BEFORE USE (Session 96): FUN_00232090 (RVA 0x112090) returns a PER-CLASS refusal mask, and
// it is USELESS TO US. It sets only mask bits 0/1/2/3/5 (from flag bits 23/25/26/27/24) and its
// consumer FUN_00380d30:20 reads it as `(mask >> actorClass) & 1`. The party's class is
// WALK_CLASS_PARTY = 4, so the shift lands on bit 4 -- which the function never sets. It can never
// change a verdict for the player, exactly as the note above already worked out ("none apply to us").
// Recorded so it is not adopted on a later reading of the decompile.
// Reference RVAs (read-only replication; NOT called):
constexpr uint32_t WALK_GRID_ENUM      = 0x10FFE0; // FUN_0022ffe0 (full-grid enumerator; bake model)
constexpr uint32_t WALK_WORLD_TO_CELL  = 0x113050; // FUN_00233050 (world XZ -> cell)
constexpr uint32_t WALK_PLANE_HEIGHT   = 0x111890; // FUN_00231890 (plane height at XZ)

// ---- THE DYNAMIC-OBSTACLE QUERY, DIAGNOSTIC-ONLY (Session 100) -----------------------------------
// FUN_00231690(ctx, outPush float[4], outPrim short*, shapeMat float[12], pos float[4], flags,
// enableDyn, bodyObj) -- the generic volume PUSH-OUT, and per the S100 decompile sweep the ONLY
// callable path that can see dynamic obstacle prims (>= 0x5000: doors, sluice gates, platforms;
// every ray/sphere callback hard-excludes them, so the sweep, the point-in-volume test and the mesh
// are all blind to a closed gate). CONDITIONALLY write-free at conf 0.97 -- BELOW THE 0.98 BAR:
// safe iff the caller-supplied bodyObj keeps +0x80/+0x84 <= 2.0 (we control it); above that it
// spills into the shared visit scratch below. The `'` probe's dyn diagnostic exists to raise that
// to >= 0.98 FROM C++ (user directive: C++ diagnostics, not Frida) by snapshot/diffing the scratch
// around a safe-path call. NOTHING ROUTES ON THIS until that record exists.
constexpr uint32_t MAP_VOLUME_PUSHOUT  = 0x111690;  // FUN_00231690
// The conditional-write targets the S100 research identified: the collision visit scratch
// (DAT_02088fe0 array + DAT_020891e0/e4 counters, shared by the mover/boundary/push-out family)
// and FUN_00230790's dynamic-obstacle OBB build scratch (DAT_022d91b0..bc).
constexpr uint32_t COLL_VISIT_ARRAY    = 0x1F68FE0; // DAT_02088fe0, int[128]
constexpr uint32_t COLL_VISIT_COUNT    = 0x1F691E0; // DAT_020891e0
constexpr uint32_t COLL_VISIT_AUX      = 0x1F691E4; // DAT_020891e4
constexpr uint32_t COLL_BUILD_SCRATCH  = 0x21B91B0; // DAT_022d91b0..bc, 16 bytes

// ---- Handle-table layout (from FUN_003588b0 + FUN_00263ff0) -----------------
constexpr uint32_t HANDLE_TABLE_STRIDE = 0x288;  // 0x51 * sizeof(uint64)
constexpr uint32_t HANDLE_TABLE_CONTAINERS = 5;  // 5 map containers (sel 0..4)
constexpr uint32_t TBL_GUARD_OFF       = 0x00;   // must be non-zero
constexpr uint32_t TBL_ENTRIES_OFF     = 0x08;   // int* entries (entries[0] = count)
constexpr uint32_t TBL_ACTIVE_OFF      = 0x10;   // byte; & 1 == active
constexpr uint32_t TBL_CAPACITY_OFF    = 0x20;   // int capacity
constexpr uint32_t ENTRIES_COUNT_OFF   = 0x00;   // int count at entries+0x00
constexpr uint32_t ENTRIES_SLOT0_OFF   = 0x08;   // object ptr at entries+0x08+slot*8
constexpr uint32_t OBJ_GENERATION_OFF  = 0x16;   // u16 generation (vs handle bits 20..30)

// ---- The container's own INTERACTION SUB-RANGES (from FUN_0025b820) ---------------------------
// The scan walks the whole entries array [0, count). The GAME does not: FUN_0025b820 walks exactly
// two (start, count) spans of the same slot space, one per interaction group --
//   group 1 (talk AND action, tested by FUN_0025bad0):  start = container+0x1DE, count = +0x1D6
//   group 0 (action only,      tested by FUN_0025be50): start = container+0x1DA, count = +0x1D2
// (Ghidra prints these as &DAT_02098fee / &DAT_02098fe6 / &DAT_02098fea / &DAT_02098fe2 indexed by
//  container*0x144 shorts == container*0x288 bytes; subtract the 0x2098e10 base for the offsets.)
//
// DIAGNOSTIC ONLY for now. These are logged next to the walked slot indices so the duplicate-listing
// bug (every shop appearing twice) can be settled from one session's log: if the twins sit OUTSIDE
// both spans, the fix is to walk the game's spans instead of the raw array. Do NOT narrow the walk
// on the hypothesis alone -- objects the mod lists on purpose (named gates, gimmick-band crystals)
// may legitimately live outside these ranges, and narrowing blind would silently drop them.
constexpr uint32_t TBL_GRP1_COUNT_OFF  = 0x1D6;  // u16 count, talk+action group
constexpr uint32_t TBL_GRP1_START_OFF  = 0x1DE;  // u16 first slot, talk+action group
constexpr uint32_t TBL_GRP0_COUNT_OFF  = 0x1D2;  // u16 count, action-only group
constexpr uint32_t TBL_GRP0_START_OFF  = 0x1DA;  // u16 first slot, action-only group

// ---- Scene-object interactivity flags (from the game's own interaction scanner
//      FUN_0025b820 / testers FUN_0025bad0 / FUN_0025be50). Every live interactive
//      field object (NPC, gate, door, switch, treasure, crystal) is reachable through
//      the handle table above; these flags say what kind of interaction it offers
//      RIGHT NOW.
//
// CAUTION -- these two bits are MODE STATE, not identity. FUN_0025ad10 / FUN_0025ae00 SET 0x400 and
// CLEAR 0x004 when an object enters talk mode, and clear 0x400 when its talk id is invalid, so an
// object's flags change during play and go to zero while it is disabled. Never decide WHAT an
// object is from these; use SCENEOBJ_KIND (below). Use them only for "what can I do with it now".
constexpr uint32_t SCENEOBJ_FLAGS_OFF  = 0x1C;   // u32 flags word on the scene object
constexpr uint32_t FLAG_TALK           = 0x400;  // talk prompt offered right now
constexpr uint32_t FLAG_ACTION         = 0x004;  // action prompt offered right now

// ---- The game's OWN interaction classifier: FUN_002675c0 (abs 0x2675c0, RVA 0x1475C0) ----------
// "Can the player interact with this object right now." Replicated MEMORY-ONLY by EntityScan (never
// called -- it is a per-object predicate on the input path). Its structure is also the authoritative
// person-vs-gimmick rule, and it is corroborated by FUN_0025bad0, the near-object scanner's
// candidate filter (kind 1 -> talk only, 4 -> both, 5 -> action only, 7 -> talk only, else reject):
//
//   if ((obj+0x0E & 0x10) == 0)                       -> false   // interaction ENABLED bit
//   if (!(obj+0x14 & 0x20) || (obj+3 & 0xE0) != 0x60) -> false   // model loaded, class 3
//   if (!FUN_002e9fe0(obj))                           -> false   // node visible/ready
//   if (obj+0x1C & 0x004) { id = obj+0xCC; return id valid && (obj+0x0E & 0xF) == 5; }  // ACTION
//   if (obj+0x1C & 0x400) { id = obj+0xDC; return id valid && (obj+0x0E & 0xF) == 1; }  // TALK
//
// => sceneObj+0x0E low nibble is the object KIND. 1 = TALK target (a person), 5 = ACTION gimmick
// (gate / door / switch / chest). 0.99: two independent engine functions assert it.
// (SCENEOBJ_KIND_OFF / KIND_MASK live in core/phyre_types.h, where the combatant scan already uses
//  them. NOTE: that header's `KIND_DEAD = 5` is WRONG -- 5 is the field-gimmick kind. See debug.md;
//  the combat track owns that correction.)
constexpr uint32_t INTERACT_PREDICATE   = 0x1475C0; // FUN_002675c0(obj) -- reference only, NOT called
constexpr uint8_t  KIND_TALK_TARGET     = 1;        // person: the engine's TALK kind
constexpr uint8_t  KIND_ACTION_GIMMICK  = 5;        // gate/door/switch/chest: the engine's ACTION kind
constexpr uint32_t SCENEOBJ_ENABLE_OFF  = 0x0E;     // same byte as the kind nibble
constexpr uint8_t  INTERACT_ENABLE_BIT  = 0x10;     // bit 4: interaction enabled (the STORY GATE)
constexpr uint32_t SCENEOBJ_ACTION_ID   = 0xCC;     // u16 action payload id (0xFFFF -> map-record default)
constexpr uint32_t SCENEOBJ_TALK_ID     = 0xDC;     // u16 talk payload id   (0xFFFF -> map-record default)
constexpr uint16_t PAYLOAD_ID_INHERIT   = 0xFFFF;   // "take it from the map's own object record"
constexpr uint32_t SCENEOBJ_READY_OFF   = 0x14;     // u8; & 0x20 = model loaded
constexpr uint8_t  READY_MODEL_BIT      = 0x20;
// Bit 0x40 on the SAME byte = "this COMBATANT is PRESENT in the world" (Session 148, measured).
// Observed values, one object dump plus a counter run across two play sessions:
//   0xF0  live party members, a live enemy      -> present
//   0x70  treasure, field gimmicks              -> ALWAYS set; says nothing (see the strike below)
//   0xB0  a DEFEATED enemy, and reserve slots that were never spawned -> absent
// The entity scan prunes on it, which is what finally removed corpses without a kill detector and
// without an HP test. That much is measured and stands.
//
// STRUCK (Session 150) -- this used to read: "Treasures keeping it SET is the load-bearing
// observation: it means the rule is about PRESENCE, not about being alive, so a despawned NPC or a
// consumed chest goes the same way." The treasure half is FALSE and was never measured; it was
// reasoned from the fact that UNOPENED treasure reads 0x70.
//
// THE REFUTATION: log `x64\logs\FFXII-Screen-Reader-2026-08-10_12-08-35.log:1133-1145` dumps two
// treasure slots the game NEVER PLACED -- world origin (0,0,0), no walkmap layer -- and both still
// read `r14=0x70`, bit 0x40 SET. The equivalent never-spawned ENEMY reserve in the same dump reads
// 0xB0, bit clear. On these objects the bit is set unconditionally and carries no placement or
// presence information at all; bit 0x80 is what actually separates the two populations. Corroborated
// by a flat `Treasure=5` across 30 rescans in that session, never once decrementing.
//
// So this pruner already runs on every treasure, in both walks, and PROVABLY CANNOT EVER FIRE ON
// ONE. Do not widen the bit to try to fix that -- it is being asked a question these objects do not
// answer. Collected-treasure state lives elsewhere; see the treasure notes in GameArchitecture.md.
// STRUCK AGAIN (Session 153) -- the S150 scoping above is NOT sufficient, and its stated reason was
// wrong. It read: "`isCharacter` ... is the population it was measured on and the only one it may
// speak for. A corpse is a character; a crystal, a gate, a door and a treasure are not, so none of
// them can ever reach this drop again." **A GATE CRYSTAL IS scene category 5-7.** With that gate
// compiled in, the live log still shows
//     absent: [0:17] +0x14=0x30 kind=4 "Rabanastre Crystal" at (115.0,-10.0,151.0)
// and `Gate=0` on a map that has one. Same bit, same failure, third object.
//
// The bit that separates the populations is 0x80 -- which the paragraph above already says, and
// which nothing acted on until S153. The pruner now requires the byte to match the shape ABSENT was
// measured in (0x80 set, 0x40 clear = 0xB0's high nibble) rather than reading 0x40 on its own.
// Derivation and the four measured values: `EntityScan::LooksAbsent` in entity_scan.cpp.
constexpr uint8_t  READY_PRESENT_BIT    = 0x40;
// The GUARD on the bit above, not a meaning of its own: it marks the byte shape 0x40 was measured
// in (0xF0 present / 0xB0 absent). Objects without it -- treasure 0x70, crystals 0x30 -- are never
// asked, because on them 0x40 takes both values and answers nothing.
constexpr uint8_t  READY_POPULATION_BIT = 0x80;
constexpr uint8_t  SCENEOBJ_CLASS_MASK  = 0xE0;     // high 3 bits of the +0x03 type byte
constexpr uint8_t  SCENEOBJ_CLASS_INTERACT = 0x60;  // class 3 == an interactable object
// The story gate is written by FUN_0026ba60(obj, enable) (RVA 0x14BA60), whose only caller
// FUN_0034afe0 (RVA 0x22AFE0) has ZERO in-binary callers -- i.e. it is a script-VM native. So the
// MAP'S OWN SCRIPT turns interactivity on and off per object; that is exactly the story flag.
constexpr uint32_t INTERACT_ENABLE_SET  = 0x14BA60; // FUN_0026ba60(obj, enable) -- reference only

// ---- Scene object / char component (from FUN_00263e30 / FUN_00317e60) -------
constexpr uint32_t SCENEOBJ_COMPONENT_OFF = 0x30;  // *(sceneObj+0x30) = char component
constexpr uint32_t COMPONENT_VALID_MASK   = 0x08;  // (*(uint*)component & 0x08) != 0

// ---- Physics context / world (from FUN_006a1a70) ----------------------------
constexpr uint32_t CTX_WORLD_OFF = 0x60;  // *(context+0x60) = live Bullet world

// ---- Live world position + facing (from FUN_00265020 / FUN_00266ad0) --------
// The scene object's transform node is at +0xB8; its first 3 floats are the cached
// world position. The node is set for every world-present object (obj+3 low-5-bit
// CATEGORY 1-7 incl. static gimmicks/gates); only category 0 (pure triggers) has a
// null node. We read the raw chain guarded on node != 0 — NOT the engine getter's
// class-nibble gate (obj+3 >> 5 in {1,3}), which zeroes gates whose class isn't 1/3.
constexpr uint32_t SCENEOBJ_TYPE_BYTE   = 0x03;   // low5 = category, high3 = class (diagnostic only)
constexpr uint32_t SCENEOBJ_XFORM_PTR   = 0xB8;   // *(sceneObj+0xB8) -> transform node

// Scene CATEGORY = the low 5 bits of SCENEOBJ_TYPE_BYTE. 5-7 are the classes carrying a char
// component (people/actors); 1-4 are position-only props and 0 is a null-node trigger. That much is
// established. There is NO established meaning for 5 vs 6 vs 7 individually.
//
// **STRUCK (Session 82) — `SCENE_CAT_PARTY/MAP_NPC/CREATURE = 5/6/7`.** Added the session before on
// one map's dump, where category 5 held the leader plus three unnamed bodies and 6 held every map
// NPC, and read as "5 = the party". The tester refuted it immediately: *"these are not party members,
// I have no other party members currently."* The falsification dump agreed -- every object the
// exclusion removed sat at the world ORIGIN, i.e. it was an unplaced reserve slot, which is a fact
// about POSITION that was misread as a fact about ROLE. The constants are deleted rather than
// renamed: nothing may key on this split until something other than one map says what it means.

// ---- The game's OWN chosen interaction target (Session 73) ---------------------------------------
// Written by FUN_0025bad0 / FUN_0025be50 (the scorers, reached from the per-field-frame scanner
// FUN_0025b820), reset every frame by FUN_0025d650, consumed by the confirm handler FUN_00268d10.
// Exactly ONE target survives -- the minimum score. There is no candidate list and no cycling.
constexpr uint32_t INTERACT_HAVE  = 0x1F7A2AA;   // DAT_0209a2aa u8: 1 = something was chosen
constexpr uint32_t INTERACT_SCORE = 0x1F7A2B0;   // DAT_0209a2b0 float, minimised (~1e10 = none)
constexpr uint32_t INTERACT_CONT  = 0x1F7A2B4;   // DAT_0209a2b4 s32 container  (-1 = none)
constexpr uint32_t INTERACT_SLOT  = 0x1F7A2B8;   // DAT_0209a2b8 s32 slot index (-1 = none)
constexpr uint32_t INTERACT_MODE  = 0x1F7A2BC;   // DAT_0209a2bc s32: 10 = talk, 2 = action
constexpr int32_t  INTERACT_MODE_TALK   = 10;
constexpr int32_t  INTERACT_MODE_ACTION = 2;

// ---- Transform-node fields used by the three interaction gates (FUN_0025bad0) --------------------
// In the decompile the player side is `param_3 = *(sceneObj + 0x5c)` on a `short*`, i.e. byte offset
// 0xB8 -- the SAME transform node the mod already reads position from. Offsets below are that node.
constexpr uint32_t XFORM_FACE_YAW_INTERACT = 0xA8;  // param_3[0x2a]: yaw the CONE test compares against
constexpr uint32_t XFORM_BAND_SCALE        = 0x24;  // pfVar1[9]   / param_3[9]
constexpr uint32_t XFORM_BAND_UP           = 0xC8;  // pfVar1[0x32]: upward extent multiplier
constexpr uint32_t XFORM_BAND_DOWN         = 0xCC;  // pfVar1[0x33]: downward extent multiplier
constexpr uint32_t XFORM_BAND_PLAYER_PAD   = 0xC0;  // param_3[0x30]: player-side downward pad
constexpr uint32_t XFORM_CONE_HALF_ANGLE   = 0xBC;  // pfVar1[0x2f]: per-target cone half-angle (rad)
constexpr uint32_t XFORM_SKIP_HEIGHT_BAND  = 0xDD;  // byte; non-zero => the band test is skipped
// sceneObj+0xC0 -> a struct whose +0x140/+0x144 floats offset the target's band centre.
constexpr uint32_t SCENEOBJ_BAND_STRUCT    = 0xC0;
constexpr uint32_t BAND_STRUCT_LO          = 0x140;
constexpr uint32_t BAND_STRUCT_HI          = 0x144;
constexpr uint32_t XFORM_POS_X          = 0x00;   // float X (ground)
constexpr uint32_t XFORM_POS_Y          = 0x04;   // float Y (elevation / up)
constexpr uint32_t XFORM_POS_Z          = 0x08;   // float Z (ground)

// ---- The interaction REACH (the distance gate's four extents) -------------------------------------
// FUN_0025bad0 calls FUN_003da5a0(out, playerXform+0x50, playerXform, targetXform+0x70, targetPosAdj)
// and rejects the candidate unless the return is < 0. That return is
//
//     dist2D - ( ellipse(playerShape -> target) + playerShape[+0x0C]
//              + ellipse(targetShape -> player) + targetShape[+0x0C] )
//
// so the bracketed sum IS the engine's horizontal interaction reach. Y is excluded from dist2D
// entirely -- interaction range is a CYLINDER (GameArchitecture.md).
//
// Each *_SHAPE is a 4-float record {semiAxisA, semiAxisB, yaw, extraRadius}. FUN_003da730 computes the
// ellipse radius along the direction to the other party:
//     w = cos(-yaw)*dz - sin(-yaw)*dx ; u = sin(-yaw)*dz + cos(-yaw)*dx
//     r^2 = (u*u + w*w) / ( u*u/(A*A) + w*w/(B*B) )        (0 when the two points coincide)
// and FUN_003a1d30 is sqrtf(fabs(x)) applied to that.
//
// The PLAYER shape sits at a different node offset than the TARGET shape -- the decompile passes
// `param_3 + 0x14` (an undefined4*, so +0x50 bytes) for one and `pfVar1 + 0x1c` (a float*, so +0x70
// bytes) for the other. They are NOT the same field; do not collapse them.
//
// Replaces the invented `kApproachRadius = 4.0f` (path_search.cpp), whose own comment records it as
// made up with the true reach bracketed only as 0.51 < r < 1.70.
constexpr uint32_t XFORM_PLAYER_SHAPE   = 0x50;   // player: {A,B,yaw,extra} at +0x50,+0x54,+0x58,+0x5C
constexpr uint32_t XFORM_TARGET_SHAPE   = 0x70;   // target: {A,B,yaw,extra} at +0x70,+0x74,+0x78,+0x7C
constexpr uint32_t SHAPE_SEMI_A         = 0x00;
constexpr uint32_t SHAPE_SEMI_B         = 0x04;
constexpr uint32_t SHAPE_YAW            = 0x08;
constexpr uint32_t SHAPE_EXTRA_RADIUS   = 0x0C;

// FUN_0025bad0:72-76 -- when this byte is set the engine ADDS this offset to the target's position
// BEFORE both the distance gate and the vertical band test. InteractTarget::ReadBandFor does not
// currently add it, so its band is wrong for any target carrying the flag.
constexpr uint32_t XFORM_POS_OFFSET_FLAG = 0x107;  // byte; bit 0 set => apply the offset below
constexpr uint32_t XFORM_POS_OFFSET_X    = 0x40;   // pfVar1[0x10]
constexpr uint32_t XFORM_POS_OFFSET_Y    = 0x44;   // pfVar1[0x11]
constexpr uint32_t XFORM_POS_OFFSET_Z    = 0x48;   // pfVar1[0x12]

// ---- OBJECT CLASS: there are TWO interactable classes with DIFFERENT field layouts ---------------
// `sceneObj+0x03 >> 5`. FUN_0025b820 runs two loops over two index ranges of the same container and
// scores them with two different functions. Reading one class's offsets on the other yields garbage
// that still looks like plausible floats, so every interaction read must branch on this.
//
//   class 3 -- characters / NPCs, scored by FUN_0025bad0
//   class 1 -- gimmicks / volume objects, scored by FUN_0025be50
//
// Verified by reading FUN_002646c0 (the engine's own interaction-point getter) and FUN_0025be50.
constexpr uint32_t SCENEOBJ_CLASS_SHIFT  = 5;      // byte at SCENEOBJ_TYPE_BYTE >> 5
constexpr uint8_t  SCENEOBJ_CLASS_CHAR   = 3;
constexpr uint8_t  SCENEOBJ_CLASS_VOLUME = 1;

// Class-1 band fields. FUN_0025be50 rejects unless
//     (node.y - node[+0x6C]) - playerPad*playerScale  <=  playerY  <=  node.y + node[+0x68]
// with the whole test skipped when the byte at node+0x5C is non-zero. Note there is NO band-scale
// multiplier here, unlike class 3 -- do not reuse XFORM_BAND_SCALE for these.
constexpr uint32_t XFORM_V_BAND_UP        = 0x68;
constexpr uint32_t XFORM_V_BAND_DOWN      = 0x6C;
constexpr uint32_t XFORM_V_SKIP_BAND      = 0x5C;   // byte; non-zero => band test skipped
constexpr uint32_t XFORM_V_CONE_HALF      = 0x50;   // class-1 facing-cone half angle
constexpr uint32_t XFORM_V_CONE_TARGET_X  = 0x10;   // the cone aims at THIS, not at the origin
constexpr uint32_t XFORM_V_CONE_TARGET_Z  = 0x18;

// DAT_0209a2b0 MIXES UNITS BETWEEN THE TWO SCORERS.
//   class 3 (FUN_0025bad0:139): score = (dist2D - reach) + dist2D  =>  reach = 2*dist2D - score
//   class 1 (FUN_0025be50):     score = FUN_003a1960(player, node) -- a plain distance measure
// So the self-checking reach identity is valid ONLY for class 3. There is no flag distinguishing
// them; the class nibble is the discriminator.
// Char component embedded 4x4 world matrix (row-major) at comp+0xE0:
constexpr uint32_t COMP_MATRIX          = 0xE0;   // right row @ +0x00
constexpr uint32_t COMP_MATRIX_FWD      = 0x100;  // NOT a forward row — internal point vector (see below)
constexpr uint32_t COMP_MATRIX_TRANSL   = 0x110;  // translation row: X@+0x00, Y@+0x04, Z@+0x08 (pos cross-check)
// NOTE: the OLD freecam globals DAT_020955e0/f0 were the dead freecam slot in our build (read
// (0,0,0)) — still correct to ignore. The GAMEPLAY camera + movement frame below are the real ones.

// ---- Movement frame: camera-relative confirmation + egocentric "forward" reference ----
// FFXII field movement is CAMERA-RELATIVE: the leader locomotion driver FUN_00358cb0 (RVA
// 0x238CB0) rotates the raw stick by the camera basis (FUN_004742a0, RVA 0x3542A0) into a WORLD
// move vector, then turns the character to face atan2f(moveX,moveZ). "Up on the stick" moves
// along camera-forward, not world-north — so egocentric directions need the camera look yaw.
// World move vector the driver writes each frame (reset to 0 while idle):
constexpr uint32_t MOVE_VEC_X = 0x21A7FD0;  // DAT_022c7fd0
constexpr uint32_t MOVE_VEC_Y = 0x21A7FD4;  // DAT_022c7fd4 (~0, ground move)
constexpr uint32_t MOVE_VEC_Z = 0x21A7FD8;  // DAT_022c7fd8
// Camera-forward reference for EGOCENTRIC directions ("North" = the way UP takes you). Read the
// forward row of the MOVEMENT camera matrix DAT_02aedf30 (row 2 @ +0x20) — the SAME matrix the stick
// rotator FUN_004742a0 consumes (worldMove = stickX*row0 - stickY*row2). A pure UP push has stickY>0,
// so worldMove = -stickY*row2 => the "direction UP takes you" yaw = atan2(-fwd.x, -fwd.z). SAME
// atan2(x,z) convention as faceNode, so it drops into the egocentric transform in place of
// ReadPlayerFacing — but unlike faceNode it is valid idle, after a camera rotate, AND in combat.
constexpr uint32_t CAMERA_FWD_X = 0x29CDF50;  // DAT_02aedf50 (camera matrix row2 .x = forward.x)
constexpr uint32_t CAMERA_FWD_Z = 0x29CDF58;  // DAT_02aedf58 (camera matrix row2 .z = forward.z)
// Scalar camera-forward yaw DAT_02aedf94 = atan2f(row2.x,row2.z) of the SIBLING/view matrix
// DAT_02aede70 (built with sign-flips ^0x80000000 on rows 0/1/3 in FUN_003820c0) — NOT the movement
// matrix, so its offset from the move heading is not constant (why the earlier diag's camLook
// wandered). Kept ONLY as a diagnostic cross-check, never as the egocentric reference.
constexpr uint32_t CAMERA_YAW_SCALAR = 0x29CDF94;  // DAT_02aedf94 (diagnostic only)
// Leader world facing yaw (radians) — the egocentric "forward" reference; persists when idle.
// TWO decompile-confirmed reads (the diagnostic logs both, the feature keeps the valid one):
//   (a) ACTOR facing cache: *(float*)(leaderActor + ACTOR_FACING_CACHE), leaderActor = *(LEADER_ACTOR_PTR).
//       FUN_00236300 rewrites it every frame from FUN_00263cc0(core). NOTE +0x15C, NOT +0x160 — the
//       0x160 neighbor is never written by the sync (read a flat 0.0 in the iteration-1 test).
//   (b) NODE facing slot: *(float*)((*(sceneObj+SCENEOBJ_XFORM_PTR)) + XFORM_FACING_YAW) — the class-3
//       slot on the SAME +0xB8 node we read position from (setter FUN_0026a0d0 / getter FUN_00263cc0).
constexpr uint32_t ACTOR_FACING_CACHE = 0x15C;  // actor+0x15C
constexpr uint32_t XFORM_FACING_YAW   = 0xA4;   // node+0xA4 (class-3 leader facing)

// ---- Field-actor pool (the game's own actor walk; from FUN_00236820/00236300)
// MOVED: the actor-pool globals, the per-actor offsets, the BtlChr layout and the scene-kind
// faction nibble now live in core/phyre_types.h, which this header pulls in -- the battle readers
// need the same offsets, and keeping a navigation-owned copy is what let four versions drift apart.
// `using namespace PhyreTypes` below keeps every existing unqualified use in the nav module working.
constexpr uint32_t FIELD_ACTIVE2    = 0x1F69300;  // DAT_02089300 (field/battle-active flag)
constexpr uint32_t DEF_CHARID        = 0x04;   // def+4 = roster char-id (0-6 party/7-25 guest/27-39 enemy) — diag only
constexpr uint32_t DEF_ID_U16       = 0x04;   // *(u16)(def+4): entity/definition id
// Pool (re)fill sites — hook one for a post-transition rescan (event-driven):
constexpr uint32_t POOL_INIT   = 0x1168D0;  // FUN_002368d0 (pool init / field reset)
constexpr uint32_t ENTITY_ACTIVATE = 0x11A570;  // FUN_0023a570 (per-entity activate)
constexpr uint32_t AREA_LOAD   = 0x2CA710;  // FUN_003ea710 (area load / publish sub-tables)

// ---- Track-1 native action handlers (formula: sel-0 slot = mapctrl.dbg_idx-5140;
//      native entry = the +0x20 handler). Used read-only: we read the underlying
//      arrays/fields, or install OBSERVE-only hooks — never invoke an action. -----
constexpr uint32_t GETMAPJUMPPOSBYINDEX   = 0x2338F0;  // -> array via FUN_00264b90 {x,y,z,angle}
constexpr uint32_t GETMAPDESTPOSBYINDEX   = 0x233ED0;
constexpr uint32_t GETMAPJUMPANGLEBYINDEX = 0x233490;
constexpr uint32_t GETMAPID               = 0x228100;
constexpr uint32_t SETSAVERAMSAVESTATUS   = 0x230600;  // (reference; not called)
constexpr uint32_t SETFIELDSIGN           = 0x234F20;  // observe-hook target (label capture)
constexpr uint32_t FIELDSIGNMES           = 0x236B70;  // observe-hook target (label text)
constexpr uint32_t SETNPCNAME             = 0x22B9A0;  // observe-hook target (NPC name)
constexpr uint32_t MAPJUMP_ARRAY_LOOKUP   = 0x144B90;  // FUN_00264b90 (exit array base/stride)
// Map data, exits, destinations and the planmapname tables now live in navigation/map_rva.h.
// They are the same domain as map_query / map_names / map_exits, and they were over half of
// this header. NavRva keeps the FIELD-NAVIGATION addresses: the walkmap, the handle table,
// the scene objects, the actor pool and the game-thread lifecycle.

// ---- Gimmick tables (classification; from FUN_0031c2f0) ----------------------
constexpr uint32_t GIMMICK_INSTANCE_TABLE = 0x2D9F120;  // DAT_02ebf120
constexpr uint32_t GIMMICK_DEF_TABLE      = 0x2D9F150;  // DAT_02ebf150 (def id / model)
// STRUCK: FIELD_STATE_BLOCK (0x2D9F190) + FIELDSIGN_CAT_A_OFF/B_OFF (0x5A7E/0x5A90), described here
// as "field-sign category tables". They are the PARTY ROSTER lists. That address is
// PhyreTypes::BTLWORK_PTR, and +0x5A7E is battle_state.cpp's OFF_ROSTER_L3 -- the removed field-sign
// code read `SafeReadU8(mgr, 0x5A7E + slot*2)`, the same base/offset/stride/width BtlChrForSlot uses,
// and 0x5A7E + 9*2 == 0x5A90, so "table B" is just the next 9-entry roster list. Do not revive this
// reading; see the note on BTLWORK_PTR in core/phyre_types.h. Exits are solved by the map's own
// __MJ_CTRL<N> script slots (Session 46), which need no category table.

// ---- Field-nav game-thread lifecycle (turn-by-turn A* runs on the game thread) --
// The route planner casts many walkability rays; doing that on the mod's input thread
// would race the physics step and read half-loaded/half-freed maps (crash). So the
// planner runs ON the game thread, drained once per field frame, hard-gated on the
// map being fully live, and its cached world is invalidated the instant teardown
// starts. RVAs/globals from the map-lifecycle RE (see GameArchitecture Pathfinder).
//
// FUN_0022a770(): the per-field-frame tick — entered exactly once per rendered frame
// while the game is in the real-time field/walking state (screen-state DAT_02064ad3==2),
// draining a pending route request at ENTRY (previous frame's fully-settled state).
// This REPLACES FUN_00314020 (0x1F4020): that render-step's mode==0 path sits behind a
// fixed-timestep accumulator AND an else-branch bypass (FUN_001800e0()!=0 -> FUN_002f1770
// directly), so it can stay silent during scripted sequences (the Reks tutorial). 0022a770
// has no such gate. No args; returns undefined8 (=1) — the hook is return-transparent.
constexpr uint32_t FIELD_FRAME       = 0x10A770;  // FUN_0022a770 (no args, returns u64)
// FUN_002695a0(): field-global teardown (single caller, game thread). Hook its START
// to invalidate the cached physics world + bump the map epoch BEFORE the game zeroes
// the leader ptr / frees the world / clears the 0x10 bit.
constexpr uint32_t FIELD_TEARDOWN    = 0x1495A0;  // FUN_002695a0 (returns void)
// FUN_003448f0: the script native the `0x0290` action slot dispatches -- a HORIZONTAL distance
// from an actor to a literal (x,z). Pops two coords + an actor id, resolves the actor
// (FUN_00264010 -> FUN_00265060 -> the actor's +0xB8 transform, the same offset the entity scan
// reads), measures with FUN_004686d0 = sqrtf(dx*dx + dz*dz), and stores the result through
// FUN_0026b4c0 into the VM ctx's return slot (base *(u64*)(ctx+0xA8), index *(i8*)(ctx+0x11),
// stride 0x28, value at +0xC, type tag 3 at +0x15).
//
// RESOLVED FROM THE BYTECODE, NOT THE NAME TABLE (Session 106): map 568's script has 8
// `CALLACT 0x0290` sites and none for `0x028f`, and this handler's body computes exactly the
// proximity the sneak minigame's catch needs. `mapctrl.dbg` labels index `0x028f` "distance" --
// off by one slot, which is the standing "resolve natives by BEHAVIOUR" rule again.
// Signature: void(ctx, _, _, vmState) -- MS x64 RCX/RDX/R8/R9.
constexpr uint32_t SCRIPT_DISTANCE   = 0x2248F0;  // FUN_003448f0 (ABS 0x3448f0)
// FUN_002677f0(object, mode) -> bool: **"is the party LEADER inside THIS object's volume?"** —
// the shared choke point under BOTH of the script's touch tests, which is what makes a per-object
// override possible at all (Session 113):
//   * native `0x26D` (instant)  -> FUN_0033fa40 -> FUN_002677f0(param_2, popped_mode)
//   * native `0x525` (waiting)  -> FUN_003407c0 (enter, stores the mode) / FUN_00340bc0 (poll)
//                                  -> FUN_002677f0(param_2, *param_3)
// Body: leader via FUN_003590d0 -> FUN_003588b0; party-slot bit `1 << leader[0x12]`; tested against
// a mask at `*(object+0xB8) + 0x60` (kind 1) or `+0x100` / `+0x228` (kind 3, by mode) — the same
// `+0xB8` scene transform the entity scan reads. `object` is param_1, so a caller can be answered
// per-object without touching any other trigger on the map.
//
// Map 568 uses `0x26D` x4 and `0x525` x21; `0x290` (distance) x8 is the other catch path. There is
// no distance3d / checkdistance3d / waitdistance3d in that script, so those two natives plus this
// function are the whole detection surface. Signature: bool(void* object, int mode).
constexpr uint32_t TOUCH_TEST        = 0x1477F0;  // FUN_002677f0 (ABS 0x2677f0)
// FUN_003dbb60(object, kind, routineIdx, mode, flag) -> int: **START A SCRIPT ROUTINE ON AN OBJECT**,
// the engine's own event-fire. Session 117, and it is the mechanism map 569's capture uses, which no
// script-native hook could ever have reached (`rrp_a03` calls ZERO of the touch natives).
//
// It builds an 8-byte event record {mode, 1, kind, routineIdx:u16, 0x8000, flags} and hands it to
// `FUN_003dbcf0(object, &rec, 1)`.
//
// **THE INDEX IS OBJECT-LOCAL (corrected Session 118; S117 read it as a routine-table index and the
// play log refuted that -- consecutive small indices per object resolving to `setup` / the map
// director).** `FUN_003dbcf0` bounds it against `**(u32**)(object+0x48)`: the count of the object's
// OWN EVENT TABLE, [count:u32][8-byte records], whose entry u32 at `tbl+4+idx*8` is a NAME-POOL
// OFFSET -- it is fed to `FUN_00263e40(blob, x) = blob + x + *(u32*)(blob+0x4C)`, and `+0x4C` is
// HDR_NAME_POOL. The blob is the object's own container's (`FUN_00263ff0(obj[0x15])` =
// HANDLE_TABLE_BASE + id*0x288, blob at +0). `MapScript::FiredRoutineName` walks this chain.
// Confidence 0.98: every step is a read the decompile shows verbatim, and the corrected model
// predicted the play log's per-object consecutive indices where the old one could not.
//
// WHO CALLS IT with a trigger volume: `FUN_0025c830(container, object)` -- the per-object trigger
// update. It zeroes the object's inside-mask, walks the FOUR party actors at `DAT_0209a1f0`, tests
// each against the volume, ORs the slot bit into `*(u32*)(*(object+0xB8) + 0x60)` -- the SAME mask
// `FUN_002677f0` reads -- and then fires:
//     kind 4  mask was 0 and is now non-zero      => ON ENTER  (routine at object+0xD0)
//     kind 2  mask was non-zero and is now 0      => ON LEAVE  (routine at object+0xD2)
//     kind 3 / 6                                  => the in-volume tests (object+0xCE/+0xD8/+0xE2)
// The ENTER branch is the one that goes on to `FUN_002e1cd0(0,0xd)` / `FUN_00268530(2)` -- taking the
// field into a scripted scene. That IS the capture on 569.
//
// Returns 1 when the caller should continue (`FUN_0025c830` bails on anything else); 2 is the
// engine's own "no event slot free" answer, which is what the mod returns when it declines a fire.
// Signature: int(void* object, u32 kind, u32 routineIdx, int mode, int flag) -- MS x64, 5th on stack.
constexpr uint32_t EVENT_FIRE        = 0x2BBB60;  // FUN_003dbb60 (ABS 0x3dbb60)
// FUN_0025c830(container, object) -> void: the per-object TRIGGER-VOLUME UPDATE described above.
// Single caller (`FUN_0025c230`'s object loop), return value used nowhere.
//
// HOOKED ONLY AS A SCOPE MARKER, and that is a containment decision, not a behavioural one.
// `FUN_003dbb60` has ~20 call sites and they are not all trigger volumes: `FUN_00269640` /
// `FUN_00269860` start conversation events, and `FUN_00269a90` / `FUN_00269ba0` / `FUN_00266530` are
// reached with a `param_5` the volume path never passes. A routine the SCRIPT asks for must run --
// suppressing, say, a `捕獲監視監督` ("capture watch supervisor") that the map's own setup starts
// could stall the sequence instead of saving it. Marking this function's extent makes "the fire came
// from a trigger volume the player walked into" a fact rather than an inference from the kind byte.
constexpr uint32_t TRIGGER_UPDATE    = 0x13C830;  // FUN_0025c830 (ABS 0x25c830)
// Liveness globals for IsFieldNavSafe(). The FIELD_ACTIVE 0x10 bit alone is NOT safe:
// it is set early on load (before area collision + world are ready) and cleared late
// on teardown (after they are freed). These back it up (DAT_02b5e0c0 is the earliest
// reliable "gone" signal; re-check the live world pointer every frame):
constexpr uint32_t AREA_ID           = 0x2A3E0B8;  // DAT_02b5e0b8 (u32; 0xFFFFFFFF = no area)
constexpr uint32_t AREA_COLLISION    = 0x2A3E0C0;  // DAT_02b5e0c0 (ptr; 0 = area collision not loaded)
constexpr uint32_t LEADER_ACTOR_PTR  = 0x1F7A1F0;  // DAT_0209a1f0 (ptr; leader actor, 0 = torn down)

} // namespace NavRva

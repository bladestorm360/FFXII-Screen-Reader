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
// *DAT_02ebf190 (ABS 0x02ebf190): party-manager pointer (source of truth for the
// leader index; diagnostic cross-check only in M0).
constexpr uint32_t PARTY_MGR_PTR = 0x2D9F190;

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
constexpr uint32_t WALK_CTX_CSR       = 0x20;  // ptr -> CSR cell->list offsets (u16[nCols*nRows+1])
constexpr uint32_t WALK_CTX_PRIMS     = 0x28;  // ptr -> primitive index list (u16[])
constexpr uint32_t WALK_CTX_ORIGIN_X  = 0x38;  // int originX (gridX = originX + worldX)
constexpr uint32_t WALK_CTX_ORIGIN_Z  = 0x3C;  // int originZ
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
constexpr uint32_t WALK_POLY_TYPE_MASK = 0x7;
constexpr uint32_t WALK_VERT_STRIDE   = 0x10;
// primitive index encoding (per-cell list entries): < 0x4000 => floor-poly index;
// 0x4000-0x4FFF => wall segment (idx-0x4000); >= 0x5000 => empty/sentinel.
constexpr uint16_t WALK_PRIM_FLOOR_MAX = 0x4000;
constexpr uint32_t WALK_MAX_CELLS      = 32767; // 16-bit cell-index cap (sanity bound)
// Reference RVAs (read-only replication; NOT called):
constexpr uint32_t WALK_GRID_ENUM      = 0x10FFE0; // FUN_0022ffe0 (full-grid enumerator; bake model)
constexpr uint32_t WALK_WORLD_TO_CELL  = 0x113050; // FUN_00233050 (world XZ -> cell)
constexpr uint32_t WALK_PLANE_HEIGHT   = 0x111890; // FUN_00231890 (plane height at XZ)

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

// ---- Scene-object interactivity flags (from the game's own interaction scanner
//      FUN_0025b820 / testers FUN_0025bad0 / FUN_0025be50). Every live interactive
//      field object (NPC, gate, door, switch, treasure, crystal) is reachable through
//      the handle table above; these flags say what KIND of interaction it offers.
//      A load-time GATE is an ACTION object (0x1C & 0x4) present from map load. ----
constexpr uint32_t SCENEOBJ_FLAGS_OFF  = 0x1C;   // u32 flags word on the scene object
constexpr uint32_t FLAG_TALK           = 0x400;  // talk target (NPC/person)
constexpr uint32_t FLAG_ACTION         = 0x004;  // action target (gate/door/switch/item)

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
constexpr uint32_t XFORM_POS_X          = 0x00;   // float X (ground)
constexpr uint32_t XFORM_POS_Y          = 0x04;   // float Y (elevation / up)
constexpr uint32_t XFORM_POS_Z          = 0x08;   // float Z (ground)
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
// ---- Map-jump EXIT ARRAY layout (per handle-table container; backs getmapjumpposbyindex) -----
// Derived from FUN_00264b90 (~0.85, PENDING confirmation via the '-key exit dump):
//   containerBase = HANDLE_TABLE_BASE + c*HANDLE_TABLE_STRIDE
//   exitBase = containerBase + *(u32)(containerBase + TBL_EXIT_OFF) + *(u32)(MAPJUMP_RELOC_BASE)
//   count    = *(u32)exitBase ; exit i: float x/y/z/angle at word [i*8 + 1..4] (EXIT_REC_STRIDE bytes)
constexpr uint32_t TBL_EXIT_OFF       = 0x54;       // u32 rel-offset to the exit sub-table (primary)
constexpr uint32_t TBL_EXIT_OFF_ALT   = 0x84;       // alt exit sub-table (destination-side positions)
constexpr uint32_t MAPJUMP_RELOC_BASE = 0x1E63530;  // _DAT_01f83530 (reloc delta added by FUN_0020e600)
constexpr uint32_t EXIT_REC_STRIDE    = 0x20;       // bytes per jump record (from +0x54 table)
constexpr uint32_t EXIT_COUNT_MAX     = 64;         // sanity clamp on the exit count
// ---- Exit DESTINATION AREA id (mapData+0x8c) via FUN_00264920 by JUMP INDEX --------------------------
// FUN_00264920(idx, buf): reads mapData+0x8c record idx (16B, tbl+4+idx*0x10) -> *(u16)(buf+4) = dest
// AREA id (record word[5] @ +0x0a; 0xffff/<0 = none). It is a MAP ID — resolve via MapQuery::
// ResolveFullAreaName (FUN_00377b60 for the sub-area + the FUN_00264f90/FUN_00377870 hop for the region).
// `idx` is a FIELD-SIGN (+0x70) record's +0x1d byte — that is the ONLY correct key, per FUN_002648f0.
// STRUCK: "the getter can be called DIRECTLY with the +0x54 jump index i ... index join is ~0.5 -> confirm
// by the walk-test". Never do this: it was a 0.5-confidence guess (below the 0.98 bar), and +0x54 and +0x8c
// are unrelated spaces. The +0x70 path below supersedes it and needs no join at all.
constexpr uint32_t DEST_DESC_BY_INDEX    = 0x144920; // FUN_00264920(idx, buf) -> *(u16)(buf+4) = dest area id
constexpr uint32_t TBL_DEST_OFF          = 0x8c;    // rel-offset to the dest-id table (hand-walk fallback)
constexpr uint32_t EXIT_DEST_REC_STRIDE  = 0x10;    // bytes per dest record (8x u16)
constexpr uint32_t EXIT_DEST_AREAID_OFF  = 0x0A;    // u16 word[5] = destination AREA id
constexpr uint16_t AREAID_NONE           = 0xFFFF;  // sentinel: exit has no named destination

// ---- planmapname: TWO tables, ONE offset array. Get these backwards and every name is region-only. ----
// STRUCK (was: "MAPAREA_NAME_BY_ID = 0x257870 / planmapname is indexed BY MAP ID, so FUN_00377870(mapId)
// is the SUB-AREA"). DISPROVEN — it made ResolveAreaName return "" on every map, so every exit and the
// map-entry announcement spoke the REGION alone ("Nalbina Fortress"). FUN_00377870's argument was never a
// map id.
//
// The blob DAT_02add0f8 ('PLMN') carries two offset tables sharing the u16 array at +0x10:
//   +0x04 = countA = 1312  -> table A = AREA names,   idx = MAP ID          -> FUN_00377b60(i): i < *(u32)(blob+4)
//   +0x08 = countB = 59    -> table B = REGION names, idx = REGION index    -> FUN_00377870(i): i < *(u32)(blob+8),
//                             read at array slot (countA + i) — i.e. rebased PAST table A
//   +0x0c = 1371 = countA + countB (combined array length; what parse_planmapname.py walks, which is why
//                  notes/planmapname_areas.csv holds BOTH tables concatenated: idx<1312 = areas,
//                  idx>=1312 = regions. 274="Aerial Gardens" (area); 1327="Nalbina Fortress" (region 15).)
// So FUN_00377870(279) fails its `279 < 59` bounds check and returns the EMPTY_STRING sentinel — exactly
// the sub="" seen live. PROVEN 4 ways: both decompiles; the header arithmetic; the game's own map resolver
// FUN_003c2320 (type-0 path, sole writer of out+0x10, = FUN_00377b60(rec+0x0c)) probe-logged returning
// Aerial Gardens/Inner Ward/Lower Apartments/Upper Apartments/The Highhall for 274/275/279/280/282; and the CSV.
constexpr uint32_t MAPAREA_NAME_BY_ID    = 0x257B60; // FUN_00377b60(mapId)    -> SUB-AREA name codec ptr (table A)
constexpr uint32_t MAPREGION_NAME_BY_IDX = 0x257870; // FUN_00377870(regionIdx)-> REGION name codec ptr (table B).
                                                     // NOT a map id — feed it MAP_NAME_INDEX_BY_ID's output.
// FUN_00264f90(mapId) -> planmapname REGION INDEX (map-master DAT_02099d88, record+6). The game's own
// region resolver (FUN_003145e0 / the HUD / FUN_003778b0) is FUN_00377870(FUN_00264f90(mapId)).
constexpr uint32_t MAP_NAME_INDEX_BY_ID  = 0x144F90;
constexpr int      MAP_ID_MAX            = 8191;      // sanity bound before the getter (map ids are small)

// RETIRED: EXIT_JUMP_DESTIDX_OFF (0x1d on a +0x54 JUMP record). The +0x54 records carry x/y/z/angle ONLY
// — FUN_00264b90 is the sole reader of that table in the whole binary and touches just word[i*8+1..4];
// bytes +0x10..+0x1f are read by NOTHING. The old chain read dead bytes and returned destIdx=0 / areaId
// 0xffff on every record of every map (confirmed in the live log). +0x1d is real, but only on the +0x70
// FIELD-SIGN records below — a different table.

// CORRECTION (this session): Session 43's claim that the +0x54 table is the party ARRIVAL/SPAWN table
// rather than the exits is WRONG, and is reverted. Its sole evidence was "FUN_00353490 places the party
// from it" — but FUN_00353490 is abs 0x353490 = RVA 0x233490 = GETMAPJUMPANGLEBYINDEX above, the script
// native `getmapjumpanglebyindex`; it returns a jump's angle and places nothing. mapData+0x54 is the
// MAP-JUMP POINT table, i.e. the intra-map "Mapjump" exits (Inner Ward -> Upper Apartments), as Sessions
// 39-42 had it and as the tester confirmed by walking into them.

// ---- Map EXIT DESTINATIONS = field-sign array at mapData+0x70 (FUN_00264ae0/002649b0/002648f0) --------
// This is the curated list the game draws as radar blips / 3D "→ <area>" arrows, and the ONLY source of
// exit destination names. Read via the game's own side-effect-free getters (they apply the ETB
// indirection + leader-visibility filter + story gate):
//   count = FUN_00264ac0(group); obj = FUN_002649b0(group, i) (null if not shown);
//   FUN_002648f0(obj, buf) -> buf{b0 usable, .., areaId u16 @+4}.
//
// STRUCTURE (FUN_00264ae0): +0x70 is a GROUP-OFFSET table, not an inline record array:
//   groupTable = blob + *(u32)(blob+0x70) + reloc = [u32 groupCount][u32 groupOff_0][u32 groupOff_1]...
//   group g sub-table = blob + groupOff_g = [u32 count][12B hdr][rec x 0x20]; rec_i = sub + 0x10 + i*0x20
//   FUN_002649b0 applies the story/visibility mask (rec+0x1c) only for group < 2.
//
// ABI (was a live bug): FUN_00264ac0 and FUN_00264ae0 take a GROUP index in ecx. Ghidra shows them as
// taking no args because FUN_00264ac0 never WRITES ecx — it passes its own incoming ecx through. The mod
// declared the count getter as `int(*)()` and called it with no argument, so it read whatever junk was in
// rcx, FUN_00264ae0's `param_1 < groupCount` bounds check failed, and the count came back 0 on EVERY map.
// "map-exits(+0x70): count=0" therefore never measured the data — it measured this bug. (The offline
// parser's matching "+0x70 is 0 in all 550 .mpk" is also not credible: it reports +0x8c = 0 too, yet the
// live log shows +0x8c populated with destCount=2, so it is reading the wrong blob base.)
constexpr uint32_t TBL_FIELDSIGN_OFF     = 0x70;      // u32 rel-offset to the field-sign GROUP table
constexpr uint32_t MAPEXIT_GROUP_MAX     = 4;         // groups to enumerate (sanity bound)
constexpr uint32_t MAPEXIT_COUNT         = 0x144AC0;  // FUN_00264ac0(group) -> int exit count in group
constexpr uint32_t MAPEXIT_OBJ_BY_INDEX  = 0x1449B0;  // FUN_002649b0(uint group, int i) -> record* / null
constexpr uint32_t MAPEXIT_DESTINFO      = 0x1448F0;  // FUN_002648f0(record*, buf*) fills buf (usable+areaId)
// FUN_00264ae0(group) -> the group's exit sub-table: [u32 count][0x0c hdr][rec x 0x20]. This is what both
// getters above call. We walk it directly because FUN_002649b0 additionally applies a RENDER gate for
// group<2 — it returns null unless the record's +0x1c per-member visibility bit is set for the member
// FUN_00377860() reports, i.e. unless the 3D "-> area" arrow is on screen this instant. Live-proven: the
// prologue draws no arrows, so all 3 of Nalbina Lower Apartments' records came back null. Exits must be
// listed because they EXIST, not because they're being drawn; the STORY gate we do want is reported
// separately by FUN_002648f0's buf[0]. Record math is FUN_002649b0's own: rec_i = base + 0x10 + i*0x20.
constexpr uint32_t MAPEXIT_TABLE_BY_GROUP = 0x144AE0; // FUN_00264ae0(group) -> group sub-table base / null
constexpr uint32_t MAPEXIT_TBL_HDR       = 0x10;      // bytes before record 0 (count u32 at +0x00)
constexpr uint32_t EXITREC_X_OFF     = 0x00;   // float world X
constexpr uint32_t EXITREC_Y_OFF     = 0x04;   // float world Y (height)
constexpr uint32_t EXITREC_Z_OFF     = 0x08;   // float world Z
constexpr uint32_t EXITREC_ENABLE_OFF = 0x0C;  // float; != 0 => drawn/shown
constexpr uint32_t EXITREC_VIS_OFF   = 0x1C;   // u8 per-member visibility mask
constexpr uint32_t EXITREC_DESTGRP_OFF = 0x1D; // u8 dest-area-group byte (used by FUN_002648f0)
constexpr uint32_t EXITBUF_USABLE_OFF = 0x00;  // buf: b0 != 0 => story gate satisfied (usable now)
constexpr uint32_t EXITBUF_AREAID_OFF = 0x04;  // buf: u16 destination area id (0xffff = none)

// ---- Minimap / naviicon MARKERS — RETIRED Session 44 -------------------------------------------------
// The naviicon flat array (DAT_02b45a80 / _DAT_02b45a70, stride 0x20) was surfaced in Session 43 on the
// HYPOTHESIS (only 0.6 conf on the subtype) that it carried save/gate-crystal/target/OBJECTIVE icons.
// Two follow-up decompile traces DISPROVED that: the array holds ONLY character/unit dots (party / ally /
// enemy / neutral), each a 1:1 duplicate of a live scene object the combatant + handle-table scans already
// list. There is NO objective/waypoint marker (zero setnaviicon/objective code in the whole decompile), NO
// crystal, NO treasure in it, and marker[+0x04] was the entity HANDLE, not a label id. Enumerating it added
// nothing and an Objective category had no source here. The consts (NAVIICON_*/MARK_*) and the enumerator
// are removed; keeping this note so the dead hypothesis is on record and not re-attempted.

// ---- Name resolution (the game's own master data, read memory-only) ----------
// Each field object stores its name key on its SCENE OBJECT (*(actor+0x10)) — read
// by the game's own resolver FUN_00263990(sceneObj): idx = *(s16)(sceneObj+0x102).
// If idx >= 0 the name is npcdic[idx]; if idx < 0 the name is a per-map custom
// string pointer at *(sceneObj+0xf8) (set by the map's fieldsignmes script). This
// replaces the old FUN_0035d380(1, def+4) call, which was the party/roster resolver
// fed a model index and always failed (-> the "Object" fallback).
constexpr uint32_t SCENEOBJ_NAME_IDX = 0x102;  // *(s16): >=0 npcdic index, <0 -> use +0xf8
constexpr uint32_t SCENEOBJ_NAME_STR = 0xf8;   // *(codec*): per-map custom string (when idx<0)
// npcdic.bin ("NPC0") is loaded once at boot into DAT_02b5e0d8 (resource cat 9/id
// 0x1f); the global holds the blob base pointer. Lookup (FUN_003eac10): slot = id*2
// (base name; odd slot = yomi/reading); name codec* = *(s32)(base + 0xc + slot*4).
constexpr uint32_t NPCDIC_BASE       = 0x2A3E0D8;  // DAT_02b5e0d8 (ptr to npcdic blob)
constexpr uint32_t NPCDIC_COUNT_OFF  = 0x08;       // *(int)(blob+8) = slot count (name+yomi pairs)
constexpr uint32_t NPCDIC_TABLE_OFF  = 0x0c;       // s32 offset table at blob+0xc, indexed by slot
constexpr uint32_t NPCDIC_NAME_MASK  = 0xffffbfff; // idx & this = npcdic id (game masks bit 14)
// AREA NAMES — the two paths are NOT interchangeable. See the planmapname table-A/B block above for the
// authoritative layout; the constants live there (MAPAREA_NAME_BY_ID / MAPREGION_NAME_BY_IDX).
//   SUB-AREA ("Lower Apartments") = FUN_00377b60(mapId)                 -> MAPAREA_NAME_BY_ID    = 0x257B60
//   REGION   ("Nalbina Fortress") = FUN_00377870(FUN_00264f90(mapId))   -> MAPREGION_NAME_BY_IDX = 0x257870
//
// STRUCK (was: "SUB-AREA = FUN_00377870(mapId) DIRECT ... planmapname's offset table is indexed BY MAP ID").
// DISPROVEN: FUN_00377870 bounds-checks against countB=59 (regions), so a map id like 279 always missed and
// returned the empty sentinel — that is why every exit AND the map-entry announcement spoke the region alone.
// The CSV's `mapid` column is the concatenated array index, not FUN_00377870's argument. The sub-area
// resolver is FUN_00377b60 (table A, bounds countA=1312), which this note had recorded as "NOT USED".
//
// FUN_003778b0(mapId) is the region pair above, and it TAKES a map id in RCX (Ghidra mislabels it (void)
// because it forwards RCX) — calling it argument-less returns the empty sentinel. Same register-passthrough
// trap as FUN_00264f90 and the FUN_00264ac0/FUN_00264ae0 group getters: assume Ghidra's "(void)" is a lie
// whenever the callee's first act is to use ECX/RCX.
constexpr uint32_t CURRENT_AREA_NAME = 0x2578B0;   // FUN_003778b0(mapId) = FUN_00377870(FUN_00264f90(mapId)) = REGION
constexpr uint32_t GETMAPID_FIELD    = 0x1F48F0;   // FUN_003148f0() -> gameState+0x1044 current map id (neg->0)
// DAT_01ceb638 = the shared empty-string sentinel ("") the name paths return on miss.
constexpr uint32_t EMPTY_STRING      = 0x1BCB638;

// ---- MAP-CONNECTION resolver: the in-game map's OWN exit -> destination database ---------------------
// This is the source the area/world map uses to label the surrounding LINKED areas (the DQ7R equivalent),
// and the game keeps it live for the field minimap from area entry onward — so we just CALL its getters.
// CONFIRMED live (probe, Nalbina subMap 9, 5 exits): record+0x0c = destination planmapname MAP ID ->
// ResolveMapName() gave "Aerial Gardens"(274) / "Inner Ward"(275) / "Lower Apartments"(279) /
// "Upper Apartments"(280) / "The Highhall"(282), matching planmapname_areas.csv exactly.
//   subMap = FUN_003bfdf0()            -> current sub-map index (does the map-id -> index mapping itself)
//   n      = FUN_003c0340(subMap)      -> exit count (adjacent first, then doors)
//   rec    = FUN_003bf430(subMap,i,&t) -> 0x18-byte record; t = 0 adjacent / 1 door / 3 invalid
constexpr uint32_t MAP_SUBMAP_INDEX  = 0x29FDF0;  // FUN_003bfdf0() -> current sub-map index
constexpr uint32_t MAP_CONN_COUNT    = 0x2A0340;  // FUN_003c0340(subMap) -> exit count
constexpr uint32_t MAP_CONN_RECORD   = 0x29F430;  // FUN_003bf430(subMap, i, &type) -> record*
constexpr uint32_t MAP_SUBMAP_NONE   = 0xFFFFFFFF; // FUN_003c0380 "not found" sentinel
constexpr uint32_t CONNREC_MAPX_OFF  = 0x00;      // s16 map-space X (minimap marker coords, NOT world)
constexpr uint32_t CONNREC_MAPY_OFF  = 0x02;      // s16 map-space Y
constexpr uint32_t CONNREC_DEST_OFF  = 0x0C;      // u16 DESTINATION planmapname map id  <-- the linkage
constexpr int      CONNREC_TYPE_DOOR = 1;         // type 1 = door/jump exit (0 = adjacent sub-map)

// ---- (STRUCK) Exit -> DESTINATION MAP id via the scene pool / field-sign site table ------------------
// Disproved at runtime: the scene-pool +0x714 / DAT_022c8060 site table are BATTLE data (BtlChr roster /
// target list), empty for exits; and the +0x8c dest table is indexed by a field-sign record's +0x1d byte,
// NOT the jump index (nothing pairs +0x8c[i] with +0x54[i]). Kept only so they are not re-attempted.
// 0.8-0.85, RUNTIME-CONFIRM before trusting. Each exit's scene object carries its destination map id at
// +0x714, set per region-change from the map-connection table (DAT_02ebf190). Also mirrored into the
// field-sign site record table. Same id space as gameState+0x1044 -> ResolveMapName(dest) names it.
constexpr uint32_t SCENE_POOL_ACCESSOR = 0x116820;  // FUN_00236820(i) -> pool object/handle for slot i
constexpr uint32_t SCENE_POOL_COUNT_FN = 0x116850;  // FUN_00236850() -> pool slot count
constexpr uint32_t SCENE_POOL_BASE     = 0x1F6E688; // DAT_0208e688 (raw pool base, stride 0xf50; fallback)
constexpr uint32_t SCENE_POOL_COUNT    = 0x1F6E6A0; // DAT_0208e6a0 (=0x20)
constexpr uint32_t SCENE_POOL_STRIDE   = 0xF50;     // bytes per pool object
constexpr uint32_t POOL_HANDLE_OBJ_OFF = 0x30;      // *(rec+0x30) -> object (FUN_00263e30), if accessor gives a handle
constexpr uint32_t POOL_OBJ_CONN_OFF   = 0x698;     // ptr -> map-connection entry (!=0 => exit gimmick)
constexpr uint32_t POOL_OBJ_DEST_OFF   = 0x714;     // u16 destination map id (0xffff none; 0x9f dynamic)
constexpr uint32_t POOL_OBJ_MODE_OFF   = 0x6B4;     // u8 mode (0 none / <9 raw dest / >=9 dynamic)
constexpr uint32_t POOL_OBJ_ARRIVAL_OFF = 0x710;    // u32 arrival jump-index on the destination map
constexpr uint32_t POOL_OBJ_POS_OFF    = 0xE0;      // field-actor world pos x/y/z (GameArchitecture.md:728)
constexpr uint32_t DYNAMIC_DEST_RESOLVE = 0x267090; // FUN_00387090(obj) -> dest map id for the 0x9f case
constexpr uint16_t DEST_DYNAMIC_SENTINEL = 0x9f;    // obj+0x714 == 0x9f => dynamic (party-dependent) dest
// Field-sign site record table (mirror; may be empty when no on-screen placards, like +0x70):
constexpr uint32_t SITE_TABLE_COUNT_FN = 0x23BB60;  // FUN_0035bb60() -> site count
constexpr uint32_t SITE_TABLE_HEADER   = 0x21A8060; // DAT_022c8060 (control block; +4 count, +0x20 recs)
constexpr uint32_t SITE_TABLE_COUNT    = 0x21A8064; // DAT_022c8064 (int site count)
constexpr uint32_t SITE_TABLE_RECS     = 0x21A8080; // DAT_022c8080 (record array base, stride 0xc0)
constexpr uint32_t SITE_REC_STRIDE     = 0xC0;      // bytes per site record
constexpr uint32_t SITE_REC_SITEID_OFF = 0x04;      // int site id (ties record to an exit)
constexpr uint32_t SITE_REC_DEST_OFF   = 0x5C;      // u16 destination map id (<0x4000 valid; 0xffff none)

// ---- Gimmick tables (classification; from FUN_0031c2f0) ----------------------
constexpr uint32_t GIMMICK_INSTANCE_TABLE = 0x2D9F120;  // DAT_02ebf120
constexpr uint32_t GIMMICK_DEF_TABLE      = 0x2D9F150;  // DAT_02ebf150 (def id / model)
constexpr uint32_t FIELD_STATE_BLOCK      = 0x2D9F190;  // DAT_02ebf190 (ptr to field-state mgr)
// Field-sign category tables inside *(FIELD_STATE_BLOCK): byte per entity slot,
// stride 2. The byte->category mapping (which value = Save/Exit/Treasure/...) is
// the runtime-pinned discriminator — the diagnostic dumps these to pin it.
constexpr uint32_t FIELDSIGN_CAT_A_OFF    = 0x5A7E;
constexpr uint32_t FIELDSIGN_CAT_B_OFF    = 0x5A90;

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
// Liveness globals for IsFieldNavSafe(). The FIELD_ACTIVE 0x10 bit alone is NOT safe:
// it is set early on load (before area collision + world are ready) and cleared late
// on teardown (after they are freed). These back it up (DAT_02b5e0c0 is the earliest
// reliable "gone" signal; re-check the live world pointer every frame):
constexpr uint32_t AREA_ID           = 0x2A3E0B8;  // DAT_02b5e0b8 (u32; 0xFFFFFFFF = no area)
constexpr uint32_t AREA_COLLISION    = 0x2A3E0C0;  // DAT_02b5e0c0 (ptr; 0 = area collision not loaded)
constexpr uint32_t LEADER_ACTOR_PTR  = 0x1F7A1F0;  // DAT_0209a1f0 (ptr; leader actor, 0 = torn down)

} // namespace NavRva

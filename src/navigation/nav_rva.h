#pragma once

#include <cstdint>

// Confirmed pathfinder / field-navigation RVAs and struct offsets.
//
// RVAs are relative to FFXII_TZA.exe's image base and resolve via
// Hooks::ResolveRva() / Hooks::InstallTyped(). Ghidra decompile addresses are
// ABSOLUTE (= RVA + 0x120000), so every RVA below is `Ghidra ABS - 0x120000`.
// Verify any change by adding 0x120000 back and matching the decompile symbol.
// Source: Docs/GameArchitecture.md "Pathfinder / Field Navigation (Phase 4)".
namespace NavRva {

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

// ---- Handle-table layout (from FUN_003588b0 + FUN_00263ff0) -----------------
constexpr uint32_t HANDLE_TABLE_STRIDE = 0x288;  // 0x51 * sizeof(uint64)
constexpr uint32_t TBL_GUARD_OFF       = 0x00;   // must be non-zero
constexpr uint32_t TBL_ENTRIES_OFF     = 0x08;   // int* entries (entries[0] = count)
constexpr uint32_t TBL_ACTIVE_OFF      = 0x10;   // byte; & 1 == active
constexpr uint32_t TBL_CAPACITY_OFF    = 0x20;   // int capacity
constexpr uint32_t ENTRIES_COUNT_OFF   = 0x00;   // int count at entries+0x00
constexpr uint32_t ENTRIES_SLOT0_OFF   = 0x08;   // object ptr at entries+0x08+slot*8
constexpr uint32_t OBJ_GENERATION_OFF  = 0x16;   // u16 generation (vs handle bits 20..30)

// ---- Scene object / char component (from FUN_00263e30 / FUN_00317e60) -------
constexpr uint32_t SCENEOBJ_COMPONENT_OFF = 0x30;  // *(sceneObj+0x30) = char component
constexpr uint32_t COMPONENT_VALID_MASK   = 0x08;  // (*(uint*)component & 0x08) != 0

// ---- Physics context / world (from FUN_006a1a70) ----------------------------
constexpr uint32_t CTX_WORLD_OFF = 0x60;  // *(context+0x60) = live Bullet world

// ---- Live world position + facing (from FUN_00265020 / FUN_00336710) --------
// The engine getter FUN_00265020 reads the scene object's transform pointer at
// +0xB8 (guarded by the type nibble), then the 3 world-position floats off it.
// Position is mirrored into the char component's embedded world matrix @ +0xE0.
constexpr uint32_t SCENEOBJ_TYPE_BYTE   = 0x03;   // (*(u8)(sceneObj+3) >> 5) must be 1 or 3
constexpr uint32_t SCENEOBJ_XFORM_PTR   = 0xB8;   // *(sceneObj+0xB8) -> transform
constexpr uint32_t XFORM_POS_X          = 0x00;   // float X (ground)
constexpr uint32_t XFORM_POS_Y          = 0x04;   // float Y (elevation / up)
constexpr uint32_t XFORM_POS_Z          = 0x08;   // float Z (ground)
// Char component embedded 4x4 world matrix (row-major) at comp+0xE0:
constexpr uint32_t COMP_MATRIX          = 0xE0;   // right row @ +0x00
constexpr uint32_t COMP_MATRIX_FWD      = 0x100;  // forward row: fwd.x@+0x00, fwd.y@+0x04, fwd.z@+0x08
constexpr uint32_t COMP_MATRIX_TRANSL   = 0x110;  // translation row: X@+0x00, Y@+0x04, Z@+0x08 (pos cross-check)

// ---- Field-actor pool (the game's own actor walk; from FUN_00236820/00236300)
// The authoritative live field-object list. `*ACTOR_POOL_BASE` is a pointer to
// ACTOR_POOL_COUNT actors of ACTOR_STRIDE bytes each.
constexpr uint32_t ACTOR_POOL_BASE  = 0x1F6E688;  // DAT_0208e688 (ptr to pool)
constexpr uint32_t ACTOR_POOL_COUNT = 0x1F6E6A0;  // DAT_0208e6a0 (u32 slot count)
constexpr uint32_t FIELD_ACTIVE2    = 0x1F69300;  // DAT_02089300 (field/battle-active flag)
constexpr uint32_t ACTOR_STRIDE     = 0xF50;
// Per-actor offsets:
constexpr uint32_t ACTOR_SCENEOBJ   = 0x10;   // *(actor+0x10) = scene object (== leader sceneObj for the leader)
constexpr uint32_t ACTOR_POS_X      = 0xE0;   // cached world X
constexpr uint32_t ACTOR_POS_Y      = 0xE4;   // cached world Y (elevation)
constexpr uint32_t ACTOR_POS_Z      = 0xE8;   // cached world Z
constexpr uint32_t ACTOR_YAW        = 0x160;  // cached facing yaw (radians)
constexpr uint32_t ACTOR_DEF_PTR    = 0x698;  // source definition ptr (null => empty slot)
constexpr uint32_t DEF_KIND_BYTE    = 0x05;   // *(s8)(def+5): 0 = character/NPC, 1 = gimmick
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

// ---- Name resolution (master data, current locale; from FUN_0035d380 / FUN_003778b0)
// FUN_0035d380(type=1, objid) fills a static name-context and returns it; the codec
// name string is at ctx+0x08 (NPC) or ctx+0x10 (gimmick). objid = *(u16)(def+4).
constexpr uint32_t OBJ_NAME_RESOLVE  = 0x23D380;  // FUN_0035d380(1, objid) -> ctx*
constexpr uint32_t NAMECTX_NPC_OFF   = 0x08;      // ctx+0x08 = NPC/char name codec*
constexpr uint32_t NAMECTX_GIMMICK_OFF = 0x10;    // ctx+0x10 = gimmick name codec*
// FUN_003778b0() -> current-area name codec string (no args; empty if not loaded).
constexpr uint32_t CURRENT_AREA_NAME = 0x2578B0;
// DAT_01ceb638 = the shared empty-string sentinel returned by both name paths.
constexpr uint32_t EMPTY_STRING      = 0x1BCB638;

// ---- Gimmick tables (classification; from FUN_0031c2f0) ----------------------
constexpr uint32_t GIMMICK_INSTANCE_TABLE = 0x2D9F120;  // DAT_02ebf120
constexpr uint32_t GIMMICK_DEF_TABLE      = 0x2D9F150;  // DAT_02ebf150 (def id / model)
constexpr uint32_t FIELD_STATE_BLOCK      = 0x2D9F190;  // DAT_02ebf190 (ptr to field-state mgr)
// Field-sign category tables inside *(FIELD_STATE_BLOCK): byte per entity slot,
// stride 2. The byte->category mapping (which value = Save/Exit/Treasure/...) is
// the runtime-pinned discriminator — the diagnostic dumps these to pin it.
constexpr uint32_t FIELDSIGN_CAT_A_OFF    = 0x5A7E;
constexpr uint32_t FIELDSIGN_CAT_B_OFF    = 0x5A90;

} // namespace NavRva

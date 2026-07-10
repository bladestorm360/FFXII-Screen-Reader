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
constexpr uint32_t ACTOR_NAME_STR   = 0x18;   // *(codec*)(actor+0x18) = localized combatant name (FUN_002b58b0 result, decode w/ GameText)
constexpr uint32_t ACTOR_ACTIVE_OFF = 0x00;   // *(u8)(actor+0x00) & ACTOR_ACTIVE_BIT = active / has model
constexpr uint32_t ACTOR_ACTIVE_BIT = 0x10;
constexpr uint32_t ACTOR_POS_X      = 0xE0;   // cached world X
constexpr uint32_t ACTOR_POS_Y      = 0xE4;   // cached world Y (elevation)
constexpr uint32_t ACTOR_POS_Z      = 0xE8;   // cached world Z
constexpr uint32_t ACTOR_YAW        = 0x160;  // cached facing yaw (radians)
constexpr uint32_t ACTOR_DEF_PTR    = 0x698;  // source definition ptr (null => empty slot)
// *(u8)(def+5) is PLAYER-vs-AI, NOT faction (runtime-disproven 2026-07-09: in the Reks prologue
// only Reks — the player-controlled leader — is 0; allies AND enemies are 1). Use it only to skip
// the player-controlled unit. Enemy-vs-ally = the def-attribute bit below.
constexpr uint32_t DEF_KIND_BYTE     = 0x05;
constexpr uint8_t  PLAYER_DEF_KIND   = 0;      // def+5 == 0 => player-controlled (the leader) — skip
// Enemy-vs-ally discriminator: the game's OWN faction test — identical in the damage path
// (FUN_0030ab40), the HUD builder (FUN_00329220), and the target classifier (FUN_002f8e90) — is
// the "scene-kind" nibble on the scene object: kind = *(u8)(sceneObj + 0x0e) & 0x0f (accessor
// FUN_00263c20). kind==3 => ally/party-side; kind in {1,2,7} => enemy; kind==5 => dead/removed.
// This is what def+5 CANNOT do in the guest/prologue setup (there every non-leader is def+5==1).
// (def+0x3c/0x64 are status-flag words, NOT faction — the earlier bit-24 test was wrong.)
constexpr uint32_t SCENEOBJ_KIND_OFF = 0x0E;   // *(u8)(sceneObj+0x0e) & KIND_MASK = scene-kind nibble
constexpr uint8_t  KIND_MASK         = 0x0F;
constexpr uint8_t  KIND_ALLY         = 3;      // party-side (guests + AI party)
constexpr uint8_t  KIND_DEAD         = 5;      // dead/removed — exclude from the scan
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
// FUN_003778b0() -> current-area name codec string (no args; empty if not loaded).
constexpr uint32_t CURRENT_AREA_NAME = 0x2578B0;
// DAT_01ceb638 = the shared empty-string sentinel ("") the name paths return on miss.
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

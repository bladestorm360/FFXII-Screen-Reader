#pragma once

#include <cstdint>

// Game struct layouts shared by more than one module: the field-actor pool, the BtlChr (combatant)
// record, and the scene object's faction nibble.
//
// WHY THIS FILE EXISTS. These offsets had drifted into four independent copies -- nav_rva.h,
// battle_target_reader.cpp ("mirrors src/navigation/nav_rva.h", said its own comment),
// battle_state.cpp, party_status.cpp and combat_events.cpp each re-declaring some subset. Four
// copies of an offset is four places to fix when a game update moves a field, and three of them
// will be missed. `battle_state.h` already records that the same thing happened to NameForBtlChr.
// Anything a second file needs belongs HERE; anything only one module uses stays in that module.
//
// These are OFFSETS and RVA-of-globals, not code. Every read through them still goes via
// core/mem_read.h -- nothing here dereferences anything.
//
// Provenance is kept on each line deliberately. The function name that establishes an offset is the
// reason nobody has had to re-derive it; do not strip these comments to save lines.
namespace PhyreTypes {

// ---- Field-actor pool (from FUN_00236820 / FUN_00236300) ---------------------------------------
// The authoritative live field-object list. `*ACTOR_POOL_BASE` points at ACTOR_POOL_COUNT actors of
// ACTOR_STRIDE bytes each. RVAs are into FFXII_TZA.exe (abs = RVA + 0x120000).
constexpr uint32_t ACTOR_POOL_BASE  = 0x1F6E688;  // DAT_0208e688 (ptr to pool)
constexpr uint32_t ACTOR_POOL_COUNT = 0x1F6E6A0;  // DAT_0208e6a0 (u32 slot count)
constexpr uint32_t ACTOR_STRIDE     = 0xF50;

// Per-actor offsets.
constexpr uint32_t ACTOR_ACTIVE_OFF = 0x00;   // *(u8)(actor+0) & ACTOR_ACTIVE_BIT = active / has model
constexpr uint8_t  ACTOR_ACTIVE_BIT = 0x10;
constexpr uint32_t ACTOR_SCENEOBJ   = 0x10;   // *(actor+0x10) = scene object
constexpr uint32_t ACTOR_NAME_STR   = 0x18;   // *(codec*)(actor+0x18) = localized combatant name
                                              //   (FUN_002b58b0 result -- already variant-selected;
                                              //    decode with GameText, do NOT SkipVariantPrefix)
constexpr uint32_t ACTOR_POS_X      = 0xE0;   // cached world X
constexpr uint32_t ACTOR_POS_Y      = 0xE4;   // cached world Y (elevation)
constexpr uint32_t ACTOR_POS_Z      = 0xE8;   // cached world Z
constexpr uint32_t ACTOR_YAW        = 0x160;  // cached facing yaw (radians)
constexpr uint32_t ACTOR_DEF_PTR    = 0x698;  // source definition / BtlChr ptr (null => empty slot)

// ---- BtlWork: the battle/party work block --------------------------------------------------------
// DAT_02ebf190 is a POINTER to the block, NOT the block. Treating it as the struct is the Session-48
// bug that made the 4/5/6 party keys silent, so the distinction is spelled out here rather than left
// to each caller's comment.
//
// ⚠️ NAME COLLISION, UNRESOLVED. This same address carried THREE names across the codebase --
// `RVA_BTLWORK` (battle_state.cpp, "POINTER to BtlWork"), `PARTY_MGR_PTR` and `FIELD_STATE_BLOCK`
// (both nav_rva.h) -- i.e. three mental models of one global. The last two were unused and have been
// deleted; this is now the only name. But note what fell out of merging them: nav_rva.h described
// `FIELD_STATE_BLOCK + 0x5A7E` as a "field-sign category table", while battle_state.cpp reads the
// SAME address as `OFF_ROSTER_L3`, the 9-entry party roster list -- and the roster reading is the one
// that is confirmed working in play. The field-sign reading was never used by any .cpp. It is
// probably wrong, but "probably" is below this project's 0.98 bar, so it is recorded here as a
// question to settle with evidence, NOT silently resolved. Do not build on the field-sign reading.
constexpr uint32_t BTLWORK_PTR   = 0x2D9F190;  // DAT_02ebf190 -- POINTER to BtlWork
constexpr uint32_t BTLWORK_MAGIC = 0x5071901;  // stamped at W+0x00 by FUN_002370c0; validates the deref

// Master-data relocation base. Every "pointer" in st2e master data is a u32 offset that needs this
// added (FUN_0020e600). Was declared twice: `RVA_RELOC` (battle_state.cpp) and `MAPJUMP_RELOC_BASE`
// (nav_rva.h).
constexpr uint32_t MASTERDATA_RELOC_BASE = 0x1E63530;  // _DAT_01f83530

// ---- BtlChr (combatant record) ------------------------------------------------------------------
// NOTE the width asymmetry, confirmed in the natives' own bodies: HP is i32, MP is i16. Reading MP
// as i32 pulls the neighbouring field in as garbage in the high half.
constexpr uint32_t BC_CHARID     = 0x04;   // u8  roster char id
constexpr uint32_t BC_KIND       = 0x05;   // u8  0 = party side (IsPartySide); see DEF_KIND_BYTE below
constexpr uint32_t BC_MAXHP      = 0x24;   // i32 (btlAtelGetHpMaxFromPartySlot)
constexpr uint32_t BC_MAXMP      = 0x28;   // i16 (btlAtelGetMpMaxFromPartySlot)
constexpr uint32_t BC_STATUS_A   = 0x3C;   // u32 status word A
constexpr uint32_t BC_CURHP      = 0x48;   // i32 (btlAtelGetHpNowFromPartySlot)
constexpr uint32_t BC_CURMP      = 0x4C;   // i16 (btlAtelGetMpNowFromPartySlot)
constexpr uint32_t BC_STATUS_B   = 0x64;   // u32 status word B
// MP-enabled guard: btlAtelGetMpMaxFromPartySlot returns 0 unless BOTH have their sign bit clear.
// The same guard appears independently in the HUD builder FUN_00329220 and the MP clamp
// FUN_00300ce0, so it is the game's own "does this character have an MP gauge" test.
constexpr uint32_t BC_MP_GUARD_A = 0x6C;   // i8
constexpr uint32_t BC_MP_GUARD_B = 0x7C;   // i8

// ---- Scene object: the faction nibble ------------------------------------------------------------
// The game's OWN faction test -- identical in the damage path (FUN_0030ab40), the HUD builder
// (FUN_00329220) and the target classifier (FUN_002f8e90) -- is the scene-kind nibble
// (accessor FUN_00263c20). This is what DEF_KIND_BYTE cannot do: in the guest/prologue setup every
// non-leader is def+5 == 1, so def+5 distinguishes only "player-controlled", never friend from foe.
constexpr uint32_t SCENEOBJ_KIND_OFF = 0x0E;   // *(u8)(sceneObj+0x0e) & KIND_MASK
constexpr uint8_t  KIND_MASK         = 0x0F;
constexpr uint8_t  KIND_ALLY         = 3;      // party-side (guests + AI party)
constexpr uint8_t  KIND_DEAD         = 5;      // dead/removed -- exclude from scans
// kinds 1, 2 and 7 are enemy.

// PLAYER-vs-AI, NOT faction (runtime-disproven 2026-07-09: in the Reks prologue only Reks -- the
// player-controlled leader -- is 0; allies AND enemies are 1). Use it only to skip the
// player-controlled unit. (def+0x3c / +0x64 are the status words above, NOT faction; the earlier
// bit-24 test was wrong.)
constexpr uint32_t DEF_KIND_BYTE   = 0x05;
constexpr uint8_t  PLAYER_DEF_KIND = 0;

} // namespace PhyreTypes

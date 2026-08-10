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
// NAME COLLISION -- RESOLVED (Session 51). This address carried THREE names: `RVA_BTLWORK`
// (battle_state.cpp), `PARTY_MGR_PTR` and `FIELD_STATE_BLOCK` (both nav_rva.h). One global, three
// mental models. The last two are deleted; this is the only name.
//
// STRUCK with it: nav_rva.h's "field-sign category tables" at `FIELD_STATE_BLOCK + 0x5A7E / +0x5A90`.
// They are the PARTY ROSTER lists, misread. The evidence is arithmetic, not judgement:
//   * The (since-removed) field-sign code read `SafeReadU8(mgr, 0x5A7E + slot*2)`. battle_state's
//     `BtlChrForSlot` reads `SafeReadU8(W, OFF_ROSTER_L3 + slot*2)` -- the SAME base, offset, stride
//     and width. One read, two labels.
//   * `0x5A7E + 9*2 == 0x5A90`, i.e. "category table B" is exactly where roster list 3's nine u16
//     entries end and the next 9-entry list begins. Two consecutive roster lists, not two tables.
//     (`OFF_LEADER` 0x5AA4 sits just past the second, consistent with the same block.)
//   * The roster reading drives the party-vitals keys and is confirmed working in play; the
//     field-sign reading never worked, was deleted from entity_list.cpp, and map exits were solved
//     a different way entirely (the map's own `__MJ_CTRL<N>` script slots, Session 46).
// So `BTLWORK + 0x5A7E` is roster list 3. There is nothing left to settle and nothing to revive.
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
// ELEMENT WEAKNESS, one byte, bits 0..7 = Fire Lightning Ice Earth Water Wind Holy Dark -- the same
// bit order as the element sprites and BattleState::ElementName. This is the mask the game itself
// draws as the target panel's "Weak:" row under Libra: FUN_00329220 copies it to snapshot +0x89,
// and FUN_002bfd20 hands that byte to FUN_00295d90, which emits message 0x2331 ("Weak: ") followed
// by one 0x4B27+bit element string per set bit. NOT a quartet -- Absorb/Half/Immune belong to the
// EQUIPMENT record and are never shown here.
constexpr uint32_t BC_WEAK_MASK  = 0x40;   // u8  element weakness bits (Libra's "Weak:" row)
constexpr uint32_t BC_CURHP      = 0x48;   // i32 (btlAtelGetHpNowFromPartySlot)
constexpr uint32_t BC_CURMP      = 0x4C;   // i16 (btlAtelGetMpNowFromPartySlot)
constexpr uint32_t BC_STATUS_B   = 0x64;   // u32 status word B
// MP-enabled guard: btlAtelGetMpMaxFromPartySlot returns 0 unless BOTH have their sign bit clear.
// The same guard appears independently in the HUD builder FUN_00329220 and the MP clamp
// FUN_00300ce0, so it is the game's own "does this character have an MP gauge" test.
constexpr uint32_t BC_MP_GUARD_A = 0x6C;   // i8
constexpr uint32_t BC_MP_GUARD_B = 0x7C;   // i8
// The two 16-byte EXTENDED status masks, OR'd together to give ~128 further status bits. The HUD
// snapshot carries the OR at its own +0x4C..+0x5B (FUN_00329220's 4x4 copy loop), which is how the
// LIBRA-PROOF bit was found: FUN_002bfd20 tests snapshot +0x51 bit 1 -- extended bit 41 -- and when
// it is set it blanks the HP digits AND zeroes the weakness row's alpha. That is the game hiding a
// mark's or boss's vitals behind "????" even with Libra up.
constexpr uint32_t BC_EXT_A      = 0x68;   // u8[16] extended status mask A
constexpr uint32_t BC_EXT_B      = 0x78;   // u8[16] extended status mask B (OR'd with A)
constexpr uint32_t BC_EXT_LIBRAPROOF_BYTE = 5;    // index into that OR: bits 40..47
constexpr uint8_t  BC_EXT_LIBRAPROOF_BIT  = 0x02; // ...bit 41 = "no Libra info for this unit"
// Progression. FUN_00312280 writes all three when an enemy dies: EXP capped at 99,999,999, LP at
// 99,999, and the level loop bumps +0x1C2 one step at a time. Corroborated by the status-menu block
// (GameArchitecture.md) and by license_reader.cpp, which already reads +0x190 as current LP.
constexpr uint32_t BC_EXP        = 0x18C;  // u32 total experience
constexpr uint32_t BC_LP         = 0x190;  // u32 current License Points
constexpr uint32_t BC_LEVEL      = 0x1C2;  // u8

// ---- Scene object: the faction nibble ------------------------------------------------------------
// The game's OWN faction test -- identical in the damage path (FUN_0030ab40), the HUD builder
// (FUN_00329220) and the target classifier (FUN_002f8e90) -- is the scene-kind nibble
// (accessor FUN_00263c20). This is what DEF_KIND_BYTE cannot do: in the guest/prologue setup every
// non-leader is def+5 == 1, so def+5 distinguishes only "player-controlled", never friend from foe.
constexpr uint32_t SCENEOBJ_KIND_OFF = 0x0E;   // *(u8)(sceneObj+0x0e) & KIND_MASK
constexpr uint8_t  KIND_MASK         = 0x0F;
constexpr uint8_t  KIND_ALLY         = 3;      // party-side (guests + AI party)
constexpr uint8_t  KIND_DEAD         = 5;      // NAME IS WRONG -- see the correction below
// kinds 1, 2 and 7 are enemy.
//
// CORRECTION (Session 54, offline): `KIND_DEAD = 5` is a MISNAME. Kind 5 is the FIELD GIMMICK kind --
// the engine's ACTION target (gate / door / switch / chest). Two engine functions say so: the
// interaction predicate FUN_002675c0 returns `(obj+0x0E & 0xF) == 5` as its ACTION verdict, and the
// near-object scanner's filter FUN_0025bad0 admits kind 5 for action only (kind 1 talk only, 4 both,
// 7 talk only, everything else rejected). The nav module now classifies on that (see nav_rva.h,
// KIND_ACTION_GIMMICK / KIND_TALK_TARGET). The constant is left in place because the COMBAT track
// owns the fix: ScanCombatants and battle_state.cpp read it, and the actor pool holds no gimmicks,
// so skipping kind 5 there is a no-op today rather than a live bug. Do not build anything new on the
// "dead" reading. Kind 1 is a PERSON (the live diag shows Vaan kind 1, Dire Rat kind 7), which also
// makes the "kinds 1, 2 and 7 are enemy" line above suspect -- verify before relying on it.

// PLAYER-vs-AI, NOT faction (runtime-disproven 2026-07-09: in the Reks prologue only Reks -- the
// player-controlled leader -- is 0; allies AND enemies are 1). Use it only to skip the
// player-controlled unit. (def+0x3c / +0x64 are the status words above, NOT faction; the earlier
// bit-24 test was wrong.)
constexpr uint32_t DEF_KIND_BYTE   = 0x05;
constexpr uint8_t  PLAYER_DEF_KIND = 0;

} // namespace PhyreTypes

#pragma once

#include <cstdint>
#include <string>

// Shared, READ-ONLY accessors for FFXII's battle state.
//
// Why this module exists: `NameForBtlChr` was duplicated in party_status.cpp and
// battle_target_reader.cpp, and both re-declared actor-pool constants that
// navigation/nav_rva.h already owns. Everything battle-related that more than one
// caller needs lives here.
//
// EVERY function is pure memory reads. The mod must never call the game's own
// resolvers from a mod thread:
//   * FUN_0035d330 writes a single process-global 0x98-byte scratch (_DAT_022ca520)
//     and returns a pointer to it -- not reentrant.
//   * FUN_002f8e90 writes _DAT_02ebf188 and calls FUN_0031b860, the full-pool scan
//     the project bans.
// So the recipes below are read-only reimplementations of those routines.
namespace BattleState {

// ---- BtlWork ---------------------------------------------------------------------------------
// DAT_02ebf190 (RVA 0x2D9F190) is a POINTER, not the struct. Treating it as the struct is the
// Session-48 bug that made 4/5/6 silent. Returns null unless the engine's own magic validates.
void* Work();

// BtlChr for a party roster slot 0..8 from roster list 3 (W+0x5A7E, the unmasked master party).
// Slots 0-2 are the active party, 3 is the guest, 4-8 the reserve. Null for an empty slot.
void* BtlChrForSlot(int slot);
constexpr int kRosterSlots = 9;

// ---- the party leader ------------------------------------------------------------------------
// FUN_00327150 reimplemented. `*(u8*)(W + 0x5AA4)` is the leader's BtlChr index.
// This REPLACES the `*(u8*)(bc + 5) == 0` test, which matches every roster character, not the
// leader. (DAT_0209a1f0[3] is NOT the leader -- that hypothesis was refuted.)
void* LeaderBtlChr();
void* LeaderActor();

// ---- master-data names -------------------------------------------------------------------------
// Localized name out of the game's own DEF-record table: FUN_0035d330(category, id) -> record+0x18,
// past the shared-pool 00 00 variant prefix. Categories seen so far: 0x02 character, 0x14 ability
// (magick / technick / action), 0x15 battle command + magick schools, 0x18 technick schools,
// 0x0B gambit condition.
//
// GAME CALL -- game thread ONLY. Empty when unresolvable, never a guess.
std::wstring DefName(uint32_t category, uint32_t id);

// A party character's own name, by roster char id. PURE MEMORY READS -- safe from ANY thread, which
// is the whole reason it exists beside DefName: `DefName(0x02, id)` answers the same question but
// gets there by CALLING FUN_0035d330, which stages its arguments in the STATIC record DAT_022ca520.
// PartyStatus::SpeakSlot runs on the INPUT thread (nav_commands.cpp, keys 4/5/6), so two callers in
// that one buffer would race -- and it would be a game call off the game thread besides.
//
// This walks what FUN_0031c5d0 `case 1` (category 2) walks: the character master table's own header,
// then the shared string pool. Empty when unresolvable, never a guess.
std::wstring CharacterName(uint8_t charId);

// ---- gambits ---------------------------------------------------------------------------------
// Is the GAMBIT master toggle on for the character whose SCENE HANDLE this is? That is the state
// the battle menu's Gambits row (cmdId 0x0D) flips, and the same one the pause-menu gambit screen
// shows. `*outResolved` distinguishes "off" from "could not read" -- the caller must stay SILENT on
// the latter rather than claim a state.
//
// Reimplements FUN_00309b40 + FUN_00272ee0 as pure memory reads (the mod is read-only, and Ghidra
// dropped the register-passed argument on both, so calling them was never an option):
//   rec  = scan i in 0..3, i < *(i32*)DAT_022c8064 : &DAT_022c8080 + i*0xC0 until *(i32*)(rec+4) == handle
//   idx  = *(i16*)(rec + 0x60)                       // BtlChr index, < 0x28
//   on   = *(u32*)(BtlWork + 8 + idx*0x1C8) & 0x04   // bit 2
// Four sites agree this is the flag: the getter FUN_00309b40 reads `>> 2 & 1`, the setter
// FUN_00311af0 writes `| 4`, the battle row draw FUN_00276be0 renders its mirror (FUN_00329220
// copies bit 2 -> party-record bit 7, `<< 5`), and the field gambit screen FUN_00567b60 stores the
// same getter's result as its master on/off.
bool GambitsEnabled(uint32_t sceneHandle, bool* outResolved);

// ---- actor pool ------------------------------------------------------------------------------
void* ActorForBtlChr(void* bc);          // scan actor+0x698 == bc
void* BtlChrForActor(void* actor);       // *(void**)(actor + 0x698)

// Localized combatant name from actor+0x18. Valid for party AND enemies, and already
// variant-selected by the binder, so it needs no SkipVariantPrefix. Empty if unresolvable.
std::wstring NameForActor(void* actor);
std::wstring NameForBtlChr(void* bc);

// Enemy instance letter ("Dire Rat B"). FUN_00263a10:
//     if (*(i16*)(so + 0x102) < 0) return *(u16*)(so + 0x100);  return 0;
// Returns 0 when the unit has no letter, else 1-based (1=A, 2=B, 3=C; FUN_002b58f0 indexes
// value-1). Confirmed live: three simultaneous Dire Rats reported 1, 2, 3.
uint16_t InstanceIndex(void* actor);

// Name with the instance letter appended, e.g. "Dire Rat B". The game separates them with
// control byte 0x06, which IS a space.
std::wstring DisplayNameForActor(void* actor);

// ---- faction ---------------------------------------------------------------------------------
enum class Faction { Party, Guest, Ally, Foe, Neutral, Unknown };
Faction FactionOf(void* actor);          // read-only reimplementation of FUN_002f8e90
bool    IsPartySide(void* bc);           // BtlChr kind byte == 0

// ---- "am I in battle?" -------------------------------------------------------------------------
// FFXII is seamless-battle: no encounter transition and NO GLOBAL to read (combat_system.md 7.1), so
// combat is inferred -- and it starts TWO ways: a foe commits against the party, or THE PLAYER SWINGS
// FIRST. Engagement is the OR. This replaces `bool PartyEngaged()`, which read only the aggro mask at
// `+0xEA4` and was therefore a being-ATTACKED test and nothing else.
//
// Returning the resolved target alongside the verdict is the other half of the point: the old boolean
// forced its one caller to re-derive "we are attacking" AFTER the gate had answered, one line too late
// to matter. Both faction filters, the commitment lifetime, and why no probe was needed are recorded at
// the DEFINITION -- do not restate them here.
//
// Pure memory reads over the actor pool; cheap enough to ask once per field frame. Game thread.
struct Engagement {
    bool     engaged   = false;   // targeted || committed -- the answer callers want
    bool     targeted  = false;   // a foe has committed against us (+0xEA4). Being ATTACKED.
    bool     committed = false;   // we have committed against a living foe. ATTACKING.
    void*    targetActor  = nullptr;  // the committed foe, already resolved; null unless `committed`
    uint32_t targetHandle = 0;
    uint16_t actionId     = 0;
};

// Scan the actor pool once and answer both directions. See the definition for why engagement has to be
// the OR, how long a commitment lingers, and why the commitment side filters to a LIVING Faction::Foe.
Engagement PartyEngagement();

// ---- the committed target (what the character is actually acting on) ---------------------------
// NOT the browse cursor at P+0x9FD8, which only follows the highlight -- confirmed live: the
// cursor moved across two enemies while the commitment held on a third.
struct Committed {
    int32_t  targetHandle = 0;
    uint16_t actionId     = 0xFFFF;
    bool     active       = false;   // true = mid-action, false = queued and waiting for ATB
    bool     valid        = false;
};
Committed CommittedTargetOf(void* actor);
void*     ActorForHandle(int32_t handle);

// ---- master-data names (read-only reimplementations) ------------------------------------------
// Ability/action name for an action id. Reads row+0x34 (the NAME index) -- NOT row+0x00, which is
// a description id that "Attack" and every "Reserve" row share. Guarded with id < count: actor+0x714
// is not exclusively an ability id (a whole 0x4000+ AI-opcode band exists).
std::wstring AbilityName(uint16_t actionId);

// The action's category (row+0x1E) -- the byte FUN_00469af0 switches on to pick its charge announce,
// and the byte combat_format.cpp switches on to pick the EXECUTION verb (the two are different
// vocabularies; see DamageLine). 0 when the id is not a real ability, which is also the basic-Attack
// value, so both land on "attacks" and a failed lookup degrades instead of lying.
//
// Resolved 0.99 offline against the shipped action_data.bin, all 543 rows -- full table in
// GameArchitecture.md "Action category byte row+0x1E":
//   0 basic Attack (1 row)  1 Magick (81)  2 Technick (24)  3 Item (51)  5 Esper summon (13)
//   6 Quickening (18)  7 enemy ability (235)  8 enemy internal (16)  9 concurrence (26)
//   10 Esper attack (16)  13/14/16/17 unidentified (6)  255 Reserve (56)
uint8_t AbilityCategory(uint16_t actionId);

// The action's ELEMENT mask (row+0x13), one bit per element in the standard order
// (0 Fire .. 7 Dark). 0 = non-elemental, which is 447 of the 543 shipped rows. See the .cpp for
// the derivation; feed set bits to ElementName.
uint8_t AbilityElements(uint16_t actionId);

// Battle status name for a status bit 0..31 (KO, Stone, Poison, Confuse, ...).
//
// Four statuses carry a SUPPRESS marker (rec+0x02 == 0xFF): KO, Invisible, HP Critical and X-Zone.
// They are hidden by default because on the battle HUD they are noise — KO is already announced as
// a death, HP Critical as a warning. `includeSuppressed` turns them back on for surfaces where
// they are the whole point: an accessory that blocks KO is exactly what a buyer needs to hear, and
// the game's own item panel lists them there (it reads the master name with no suppression check).
std::wstring StatusName(int bitIndex, bool includeSuppressed = false);

// Element name for an element bit 0..7 -- Fire, Lightning, Ice, Earth, Water, Wind, Holy, Dark.
// Game-supplied, never hardcoded; empty when it cannot be resolved. See the .cpp for the binding.
std::wstring ElementName(int bitIndex);

// Names of every set bit in an 8-bit element mask, comma-joined. Empty for a zero mask -- a
// non-elemental action must add NOTHING to a line, not the word "non-elemental".
std::wstring ElementNames(uint8_t elementMask);

// Names of every set bit in a status word, comma-joined. `statusWord` is BtlChr+0x3c | +0x64.
std::wstring StatusNames(uint32_t statusWord);

} // namespace BattleState

#pragma once

#include <cstdint>
#include <string>

// Turns raw combat events into speakable lines, and owns the realtime-vs-log-only policy.
//
// TWO TIERS (combat_system.md §9.1.1):
//   Tier 1  the game's own composed sentence, read VERBATIM. 102 messages, already localized.
//   Tier 2  synthesized, and ONLY where the game has no message at all -- chiefly the per-hit
//           damage line, because no "X attacks Y" message exists anywhere in the table and no
//           announce ever names a target.
//
// Every mod-emitted word lives in this file. It is deliberately small: names, abilities, statuses
// and items all come from the game's own data, so our text is only the connective tissue.
namespace CombatFormat {

// Should a game message (Tier 1) interrupt immediately, or just go to the log?
// Keyed by message id -- which is why the Tier-1 hook must be FUN_00536410 and not the ticker,
// since the ticker never receives the id.
bool ShouldSpeakNow(uint16_t msgId);

// Tier 2: "Vaan attacks Dire Rat B. 40", "Zombie casts Sludge on Vaan. 42 Dark"
// `verb` comes from the action record's category byte, mirroring the game's own vocabulary so our
// wording matches the game's when both appear.
//
// `elementMask` is BattleState::AbilityElements(actionId) -- the element FFXII only ever draws as
// an icon. 0 (447 of 543 action rows) adds nothing, and an element the action's own name already
// spells out is dropped rather than said twice. See ElementSuffix in the .cpp.
std::wstring DamageLine(const std::wstring& attacker,
                        const std::wstring& target,
                        const std::wstring& action,
                        uint16_t actionCategory,
                        int32_t  hpDelta,
                        uint8_t  outcome,
                        uint8_t  elementMask);

// Mechanic name for a result outcome code, or empty for an ordinary hit.
// Derived from the processing code, NOT from the on-screen sprite: the outcome is decided by which
// equipment slot gated the defensive roll (off-hand -> block, main-hand -> parry, animation set ->
// evade), so it needs no visual confirmation.
std::wstring OutcomeWord(uint8_t outcome);

// "Dire Rat B defeated". Mod-emitted BY NECESSITY: the game's KO message (0x10 "{0} has fallen")
// is gated to party | guest | ally by FUN_002fa390, so a foe going down produces no game text at
// all. Party KOs keep using the game's own wording -- only this side is ours.
std::wstring DefeatedLine(const std::wstring& who);

// The same line with the kill's rewards appended: "Dire Rat defeated. 34 EXP, 2 LP."
// FFXII has no end-of-battle results screen -- it pops floating +EXP/+LP numbers over the corpse,
// drawn as sprite digits with no text anywhere in the binary, so the sentence is ours. When both
// values are 0 (a kill the party got no credit for, or Trial Mode) this degrades to the bare
// DefeatedLine rather than saying "0 EXP, 0 LP".
std::wstring DefeatedLine(const std::wstring& who, uint32_t expGain, uint32_t lpGain);

} // namespace CombatFormat

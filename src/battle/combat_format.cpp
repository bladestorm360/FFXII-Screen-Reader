#include "battle/combat_format.h"

#include "speech/phrasebook.h"
#include "ui/mod_menu.h"

namespace CombatFormat {
namespace {

// ---------------------------------------------------------------------------------------------
// Realtime policy, as a DATA TABLE keyed by message id (combat_system.md §9.1.4). Deliberately not
// a chain of `if`s: this is a tuning knob the player adjusts in play. The first knob is now real --
// see ModMenu (F8 / F4) for the charge-announce pair. It did NOT go to mod_config.ini: that file
// belongs to the RVA byte-validator and hand-editing it masks validator failures (CLAUDE.md,
// CRITICAL PROJECT BLOCKER), so ModMenu keeps the mod's own store instead.
//
// The principle: interrupt for things you must ACT on or would otherwise never learn; log the rest.
// The game's own `dedup` bit and cull-exempt style bit were both considered as importance proxies
// and rejected -- style 0x21 is cull-exempt but covers routine heals -- so this is by semantics.
// ---------------------------------------------------------------------------------------------
bool InRange(uint16_t v, uint16_t lo, uint16_t hi) { return v >= lo && v <= hi; }

} // namespace

bool ShouldSpeakNow(uint16_t id) {
    // Your command FAILED -- you must act again, so these cannot wait.
    if (InRange(id, 0x05, 0x0C)) return true;
    // A unit went down, or came back.
    if (InRange(id, 0x10, 0x12)) return true;
    if (id == 0x1C)              return true;
    // One-shot results you would otherwise never learn: level up, loot, gil, steal, poach.
    if (id == 0x04)              return true;
    if (InRange(id, 0x24, 0x26)) return true;
    if (InRange(id, 0x2D, 0x34)) return true;
    if (InRange(id, 0x37, 0x38)) return true;
    if (InRange(id, 0x64, 0x65)) return true;
    // Boss mechanics that change how the fight must be played.
    if (InRange(id, 0x44, 0x4E)) return true;
    // Your damage type is being NULLIFIED -- attacking is pointless until it lifts.
    if (InRange(id, 0x3E, 0x43)) return true;
    // A whole command category is disabled party-wide.
    if (InRange(id, 0x4F, 0x5C)) return true;
    // "Back attack!" -- you are being flanked.
    if (id == 0x61)              return true;
    // An enemy or guest is COMMITTING TO AN ACTION -- it is charging now and you still have time to
    // interrupt, guard, or move. FUN_00469af0 is faction-gated `& 0x0A` (guest|foe), so a party
    // member can never reach either of these ids.
    //
    // 0x0D is "begins casting", which FUN_00469af0 emits ONLY for action category 1 (magick).
    // 0x0E is "readies", which it emits for categories 2, 7 and 9 -- and ENEMY ABILITIES ARE
    // CATEGORY 7. 0x0F "uses" (category 3) stays log-only: that is the routine-item tier.
    //
    // PLAYER-CONTROLLED as of S90: this pair is what the mod menu's Combat verbosity setting turns
    // on and off, and it defaults to Normal (OFF). Nothing else in this table is affected -- Verbose
    // adds these two ids and nothing more, and damage lines stay log-only in both modes (they are
    // appended with speakNow=false in combat_events.cpp and never consult this function).
    //
    // Recorded so it is not re-diagnosed: the GAME emits these less often than the action occurs, so
    // Verbose means "announce when the game announces", not "announce every cast". Three gates on
    // the game's side drop them -- the repeat gate in FUN_00304850 (it calls the announce only when
    // the action or the target differs from the previous one, so an enemy repeating one ability on
    // one target announces once), the ~24-unit distance cull in FUN_00469570 (style 0x01 is not
    // cull-exempt), and the 10-slot dedup ring in FUN_0046ab10. All three are the game's own
    // pacing, and reading its sentence is what keeps the wording verbatim in all 12 locales.
    // Do NOT "fix" any of them by hooking the emitter FUN_00469af0.
    if (id == 0x0D || id == 0x0E)
        return ModMenu::CombatVerbosity() == ModMenu::Verbosity::Verbose;

    // Everything else is log-only. Notably 0x0F (uses) is the routine-item tier, and 0x13-0x1B /
    // 0x1E-0x23 are routine restores and cures.
    return false;
}

std::wstring OutcomeWord(uint8_t outcome) {
    // result+0x04. The mapping comes from the roll ladder in FUN_003896b0 combined with the
    // equipment gate FUN_00384e50: DAT_02aedff0 requires the OFF-HAND slot (shield -> block),
    // DAT_02aedff4 the MAIN-HAND slot (weapon -> parry), DAT_02aedfec an animation set (evade).
    // There is no text for any of these in the binary -- they are sprites -- so these words are
    // mod-emitted by necessity, describing the mechanic we identified rather than a word we read.
    // They live in speech/phrasebook.cpp with that reason recorded; this is the canonical example
    // of what the phrasebook is for.
    using Phrase::Id;
    switch (outcome) {
        case 1: case 2:  return Phrase::Get(Id::Parried);
        case 3: case 4:  return Phrase::Get(Id::Blocked);
        case 5:          return Phrase::Get(Id::Evaded);
        case 6:          return Phrase::Get(Id::NoEffect);
        case 7:          return Phrase::Get(Id::Nullified);
        case 8:          return Phrase::Get(Id::Reflected);
        case 10:         return Phrase::Get(Id::Absorbed);
        case 11:         return Phrase::Get(Id::Avoided);
        default:         return std::wstring();   // 0 = ordinary hit, 9 = default seed
    }
}

std::wstring DefeatedLine(const std::wstring& who) {
    return who + Phrase::Get(Phrase::Id::Defeated);
}

std::wstring DefeatedLine(const std::wstring& who, uint32_t expGain, uint32_t lpGain) {
    // Silence beats filler: a kill the party got no credit for reports the kill and nothing else.
    if (expGain == 0 && lpGain == 0) return DefeatedLine(who);
    return who + Phrase::Get(Phrase::Id::DefeatedWithRewards)
               + std::to_wstring(expGain) + Phrase::Get(Phrase::Id::ExpSuffix)
               + std::to_wstring(lpGain)  + Phrase::Get(Phrase::Id::LpSuffix);
}

std::wstring DamageLine(const std::wstring& attacker,
                        const std::wstring& target,
                        const std::wstring& action,
                        uint16_t actionCategory,
                        int32_t  hpDelta,
                        uint8_t  outcome) {
    if (attacker.empty() && target.empty()) return std::wstring();

    // EXECUTION vocabulary, from the action record's category byte (row+0x1E). Deliberately NOT the
    // game's ANNOUNCE vocabulary: FUN_00469af0 is a CHARGE-phase emitter ("begins casting" /
    // "readies" / "uses") called once from FUN_00304850 at action start, whereas DamageLine runs on
    // the applier FUN_003112f0, AFTER the hit has landed. Mirroring the announce map here is what
    // made a connected enemy ability say "Urstrix A readies Slap on Vaan. 14" (S90) -- charge-phase
    // wording on an execution event. "readies" belongs to the charge announce and nowhere else.
    //
    // Categories verified 0.99 offline against the shipped action_data.bin (543 rows, stride 0x3C):
    //   0 basic Attack (1 row)   1 Magick (81)   2 Technick (24)   3 Item (51)   5 Esper summon (13)
    //   6 Quickening (18)   7 enemy ability (235)   8 enemy internal (16)
    //   9 Quickening concurrence (26)   10 Esper attack (16)   255 Reserve placeholders (56)
    // The 24 / 13 / 18 counts are exactly FFXII's technick, Esper and Quickening totals. See
    // GameArchitecture.md "Action category byte (row+0x1E)".
    const wchar_t* verb = Phrase::Get(Phrase::Id::Attacks);   // cat 0, and anything unidentified:
    bool namesAction = false;                                 // no action name, just "X attacks Y"
    switch (actionCategory) {
        case 1:                                   // Magick
            verb = Phrase::Get(Phrase::Id::Casts); namesAction = true; break;
        case 2: case 3: case 5: case 6: case 7: case 9: case 10:
            // Technick / Item / Esper summon / Quickening / enemy ability / concurrence / Esper attack
            verb = Phrase::Get(Phrase::Id::Uses);  namesAction = true; break;
        default: break;                           // 8 enemy-internal, 13/14/16/17, 255 Reserve
    }

    std::wstring s = attacker;
    if (!s.empty()) s += L' ';
    s += verb;
    if (namesAction && !action.empty()) {
        s += L' ';
        s += action;
        if (!target.empty()) { s += Phrase::Get(Phrase::Id::OnJoiner); s += target; }
    } else if (!target.empty()) {
        s += L' ';
        s += target;
    }

    const std::wstring word = OutcomeWord(outcome);
    if (!word.empty()) {
        // A defensive outcome replaces the number -- there is no number to report.
        s += L". ";
        s += word;
        return s;
    }

    if (hpDelta != 0) {
        // Sign, not the flag bits: +0x2c additionally encodes absolute-set modes, so the sign is
        // the only unambiguous damage-vs-heal test.
        const int32_t mag = hpDelta < 0 ? -hpDelta : hpDelta;
        s += L". ";
        if (hpDelta > 0) s += Phrase::Get(Phrase::Id::Heals);
        s += std::to_wstring(mag);
    }
    return s;
}

} // namespace CombatFormat

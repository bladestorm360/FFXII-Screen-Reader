#include "battle/combat_format.h"

namespace CombatFormat {
namespace {

// ---------------------------------------------------------------------------------------------
// Realtime policy, as a DATA TABLE keyed by message id (combat_system.md §9.1.4). Deliberately not
// a chain of `if`s: this is a tuning knob the player will want to adjust in play, and it should end
// up in mod_config.ini eventually.
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
    // An enemy or guest BEGINS CASTING -- a spell is charging and you still have time to interrupt
    // it, guard, or move out of the way. Only 0x0D: FUN_00469af0 maps action category 1 (magick) to
    // this id, and it is faction-gated to guest|foe (`& 0x0A`), so a party member can never reach it.
    // Its siblings 0x0E "readies" / 0x0F "uses" stay log-only -- those are the spam tier.
    //
    // KNOWN LIMIT, accepted by the user: 0x0D carries render style 0x01, which is NOT cull-exempt,
    // so the message bus drops it beyond ~24 world units. A caster hanging far back announces
    // nothing. Fixing that means hooking the emitter instead of reading the message; that was
    // considered and deliberately NOT taken, to keep the game's own verbatim wording in all 12
    // locales. Do not "fix" it by adding an emitter hook.
    if (id == 0x0D)              return true;

    // Everything else is log-only. Notably 0x0E-0x0F (readies / uses) fire constantly and are the
    // spam tier, and 0x13-0x1B / 0x1E-0x23 are routine restores and cures.
    return false;
}

std::wstring OutcomeWord(uint8_t outcome) {
    // result+0x04. The mapping comes from the roll ladder in FUN_003896b0 combined with the
    // equipment gate FUN_00384e50: DAT_02aedff0 requires the OFF-HAND slot (shield -> block),
    // DAT_02aedff4 the MAIN-HAND slot (weapon -> parry), DAT_02aedfec an animation set (evade).
    // There is no text for any of these in the binary -- they are sprites -- so these words are
    // mod-emitted by necessity, describing the mechanic we identified rather than a word we read.
    switch (outcome) {
        case 1: case 2:  return L"parried";
        case 3: case 4:  return L"blocked";
        case 5:          return L"evaded";
        case 6:          return L"no effect";
        case 7:          return L"nullified";
        case 8:          return L"reflected";
        case 10:         return L"absorbed";
        case 11:         return L"avoided";
        default:         return std::wstring();   // 0 = ordinary hit, 9 = default seed
    }
}

std::wstring DefeatedLine(const std::wstring& who) {
    return who + L" defeated";
}

std::wstring DefeatedLine(const std::wstring& who, uint32_t expGain, uint32_t lpGain) {
    // Silence beats filler: a kill the party got no credit for reports the kill and nothing else.
    if (expGain == 0 && lpGain == 0) return DefeatedLine(who);
    return who + L" defeated. " + std::to_wstring(expGain) + L" EXP, "
               + std::to_wstring(lpGain) + L" LP";
}

std::wstring DamageLine(const std::wstring& attacker,
                        const std::wstring& target,
                        const std::wstring& action,
                        uint16_t actionCategory,
                        int32_t  hpDelta,
                        uint8_t  outcome) {
    if (attacker.empty() && target.empty()) return std::wstring();

    // Verb from the action record's category byte (row+0x1E), mirroring the game's own vocabulary
    // in FUN_00469af0: 1 -> begins casting, 2/7/9 -> readies, 3 -> uses. A basic attack has no
    // announce at all, so "attacks" is ours.
    const wchar_t* verb = L"attacks";
    bool namesAction = false;
    switch (actionCategory) {
        case 1: verb = L"casts";   namesAction = true; break;
        case 2: case 7: case 9:
                verb = L"readies"; namesAction = true; break;
        case 3: verb = L"uses";    namesAction = true; break;
        default: break;
    }

    std::wstring s = attacker;
    if (!s.empty()) s += L' ';
    s += verb;
    if (namesAction && !action.empty()) {
        s += L' ';
        s += action;
        if (!target.empty()) { s += L" on "; s += target; }
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
        if (hpDelta > 0) s += L"heals ";
        s += std::to_wstring(mag);
    }
    return s;
}

} // namespace CombatFormat

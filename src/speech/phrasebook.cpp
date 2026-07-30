#include "speech/phrasebook.h"

#include <atomic>

// The table. ONE row per Id, in the SAME ORDER as the enum — the static_assert at the bottom is what
// keeps that true, so add entries to both or the build stops.
//
// EN(...) fills the English column and leaves the other 11 as nullptr, which Get() falls back from.
// That is deliberate: a nullptr means "not translated yet", and it is honest. Do not fill a column
// with a guessed translation to make it look complete.

namespace Phrase {
namespace {

struct Row { const wchar_t* loc[kLocaleCount]; };
#define EN(s) { { (s) } }

const Row kTable[] = {
    // -- Compass directions -------------------------------------------------------------------
    EN(L"North"), EN(L"Northeast"), EN(L"East"), EN(L"Southeast"),
    EN(L"South"), EN(L"Southwest"), EN(L"West"), EN(L"Northwest"),
    // -- Egocentric directions ----------------------------------------------------------------
    EN(L"forward"), EN(L"forward-right"), EN(L"right"), EN(L"backward-right"),
    EN(L"backward"), EN(L"backward-left"), EN(L"left"), EN(L"forward-left"),
    // -- Distance / elevation. The leading spaces are part of the phrase: these append to a
    //    number or a direction word, and a locale may not want a space at all.
    EN(L" (above)"), EN(L" (below)"), EN(L"right next to you"),
    EN(L" steps"), EN(L" more"), EN(L", then "),

    // -- Combat outcome words (sprites in the binary; see combat_format.cpp OutcomeWord) -------
    EN(L"parried"), EN(L"blocked"), EN(L"evaded"), EN(L"no effect"),
    EN(L"nullified"), EN(L"reflected"), EN(L"absorbed"), EN(L"avoided"),
    // -- Combat verbs + line pieces -----------------------------------------------------------
    EN(L"attacks"), EN(L"casts"), EN(L"readies"), EN(L"uses"), EN(L" on "), EN(L"heals "),
    EN(L" defeated"), EN(L" defeated. "), EN(L" EXP, "), EN(L" LP"), EN(L" below 20 percent"),

    // -- Entity categories --------------------------------------------------------------------
    EN(L"All"), EN(L"Exit"), EN(L"Save Crystal"), EN(L"Gate Crystal"), EN(L"Treasure"),
    EN(L"NPC"), EN(L"Interactables"), EN(L"Enemy"), EN(L"Items"), EN(L"Sign"), EN(L"Story-gated"),
    EN(L"Door"), EN(L"Shop"),

    // -- Navigation status --------------------------------------------------------------------
    EN(L"Position unavailable"), EN(L"Path clear"), EN(L"Blocked, bear "), EN(L"Blocked"),
    EN(L"Route unavailable"), EN(L"At the exit."), EN(L"No path"), EN(L"No targets"),
    EN(L"No target"), EN(L"Entering "),
    EN(L"Label cleared"), EN(L"Labelled "), EN(L"Diagnostic unavailable"), EN(L"Diagnostic logged"),
    // Format templates, not plain phrases: a locale reorders these, so the whole template moves.
    EN(L"%s. %d objects"), EN(L"%d objects"),

    // -- License board ------------------------------------------------------------------------
    EN(L"learned"), EN(L"can learn"), EN(L"not enough LP"), EN(L"locked"), EN(L"Locked"),
    EN(L"License board"), EN(L" license board"), EN(L" License Points"),

    // -- Gauges / status labels. Trailing space separates label from number.
    EN(L"Level "), EN(L"HP "), EN(L"MP "), EN(L"LP "), EN(L"EXP "), EN(L"Next "),
    EN(L" of "), EN(L" percent"), EN(L"queued"),

    // -- Field menu, Party screen (portrait position + alpha dim only; no string exists). USER-AUTHORIZED.
    // POSITIONAL TABLE: these two must sit at the SAME index as in phrasebook.h's enum. The
    // static_assert only checks the COUNT, so inserting at the wrong offset silently shifts every later
    // phrase by two and the build still passes.
    EN(L"In party"), EN(L"Not in party"),

    // -- Title menu (baked sprite art — the one sanctioned label exception). TitleExit is its own
    //    id and NOT shared with CatExit: identical in English, not necessarily anywhere else.
    EN(L"New Game"), EN(L"Load Game"), EN(L"Trial Mode"), EN(L"Credits"), EN(L"Exit"),

    // -- Shop / inventory / gil ---------------------------------------------------------------
    EN(L" gil"), EN(L" in inventory"), EN(L"x"),

    // -- Interaction target -------------------------------------------------------------------
    EN(L"Talk"), EN(L"Action"),

    // -- Ability summary ----------------------------------------------------------------------
    EN(L"empty"), EN(L"unavailable"),

    // -- Gambits (icon frame in battle, alpha dim on the field screen; no text exists) ---------
    EN(L"on"), EN(L"off"),

    // -- The mod talking about itself ---------------------------------------------------------
    EN(L"Speech on"), EN(L"Speech off"), EN(L"Combat log empty"),

    // -- Mod menu and its settings. The three sentences are the TESTER'S OWN WORDS (S90), which is
    //    what makes them admissible here; do not reword them without asking.
    EN(L"Mod menu"), EN(L"Mod menu closed"), EN(L"Combat verbosity"),
    EN(L"Normal"), EN(L"Verbose"),
    EN(L"Determine what is spoken aloud from the combat log."),
    EN(L"Normal speaks enemy defeat and EXP, party member low HP and KO, loot drops."),
    EN(L"Verbose speaks everything from normal, as well as enemies readying abilities or casting spells."),
    EN(L"Audio beacon"), EN(L"Off"), EN(L"On"),
    EN(L"A repeating sound that leads you along the route, panned toward where you need to walk."),
    EN(L"Off plays nothing."),
    // The trailing "and tracks your target in battle" was REMOVED in S95, not reworded for style: the
    // battle tracking is now its own setting, so that sentence had become false. Everything the tester
    // said about the route beacon itself is untouched.
    EN(L"On plays a sound that speeds up as you near each turn of the route."),
    EN(L"Audio beacon volume"),
    EN(L"How loud the route beacon plays."),
    EN(L"Target beacon"),
    EN(L"A repeating sound that tracks the enemy your party is fighting, panned toward it."),
    EN(L"Off plays nothing in battle."),
    EN(L"On plays in battle whenever your party has committed to a target, whether or not a route beacon is running."),
    EN(L"Target beacon volume"),
    EN(L"How loud the target beacon plays."),
};

#undef EN

static_assert(sizeof(kTable) / sizeof(kTable[0]) == static_cast<size_t>(Id::Count),
              "phrasebook.cpp kTable and Phrase::Id are out of sync — every Id needs exactly one row");

// Read from any thread (readers speak from both the game and input threads); only ever written by
// SetLocale, which is a startup-time call. Relaxed is enough: a torn read is impossible for an int
// and a stale read would at worst use English for one line.
std::atomic<int> g_locale{kLocaleEnglish};

} // namespace

const wchar_t* Get(Id id) {
    const size_t i = static_cast<size_t>(id);
    if (i >= static_cast<size_t>(Id::Count)) return L"";   // never fault, never return null
    const Row& row = kTable[i];
    const int loc = g_locale.load(std::memory_order_relaxed);
    if (loc > 0 && loc < kLocaleCount && row.loc[loc]) return row.loc[loc];
    const wchar_t* en = row.loc[kLocaleEnglish];
    return en ? en : L"";
}

void SetLocale(int locale) {
    if (locale < 0 || locale >= kLocaleCount) return;
    g_locale.store(locale, std::memory_order_relaxed);
}

int GetLocale() { return g_locale.load(std::memory_order_relaxed); }

} // namespace Phrase

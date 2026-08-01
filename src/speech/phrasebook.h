#pragma once

#include <cstddef>

// The mod's OWN words — the 12-locale dictionary CLAUDE.md has always described.
//
// RULE THIS EXISTS TO ENFORCE: the mod NEVER invents text the game already supplies. Item names,
// ability names, menu labels, dialogue, help text — all of that is read out of the game and decoded
// by GameText, and none of it belongs here. What DOES belong here is the small vocabulary the game
// renders as something a text hook cannot read (a sprite, an icon frame, an alpha dim) or does not
// render at all (the mod's own status announces, the pathfinder's directions).
//
// Every entry needs a reason. The three families and their justifications:
//   * Combat outcome words (parried / blocked / evaded / ...) — the game draws these as sprites;
//     there is no string for them anywhere in the binary. See combat_format.cpp's OutcomeWord.
//   * Gambit On/Off — battle draws a two-state icon frame (FUN_00242600) and the field gambit
//     screen an alpha dim (FUN_005682c0 / FUN_0056a010). Again no text exists.
//   * Title menu options (New Game / Load Game / ...) — title_menu.mrp is the game's ONE baked
//     sprite-art menu, and title_reader keys off texture source-rects. This is the sole sanctioned
//     exception to "no fabricated UI labels"; it predates this file and is only documented here.
// Everything else is the mod speaking about itself (directions, category names, its own toggles),
// which the game has no opinion about.
//
// LOCALIZATION: the table is Id -> const wchar_t*[kLocaleCount]. Only English is populated. Every
// other slot is nullptr and falls back to English, so adding a locale later is a data edit in
// phrasebook.cpp and touches no reader. Translations are NOT machine-generated — an invented
// translation of game vocabulary is the same fabricated-label failure this file exists to prevent.
//
// WHAT IS DELIBERATELY *NOT* HERE: punctuation and joining glue (", ", ": ", ". ", "/"), which stays
// at the call sites, plus file paths, env names and log-only strings. Call sites compose glue around
// phrases (`L", " + Get(Id::HP)`) rather than storing pre-glued variants.

namespace Phrase {

// Number of locale columns. Matches the game's own locale set; only index 0 (English) is filled.
constexpr int kLocaleCount = 12;
constexpr int kLocaleEnglish = 0;

enum class Id {
    // -- Compass directions (nav_common.cpp) -------------------------------------------------
    North, Northeast, East, Southeast, South, Southwest, West, Northwest,
    // -- Egocentric directions (nav_common.cpp) ----------------------------------------------
    Forward, ForwardRight, Right, BackwardRight, Backward, BackwardLeft, Left, ForwardLeft,
    // -- Distance / elevation (nav_common.cpp, path_directions.cpp) --------------------------
    AboveSuffix, BelowSuffix, RightNextToYou, StepsSuffix, MoreSuffix, ThenJoiner,

    // -- Combat outcome words: sprites, no text in the binary (combat_format.cpp) -------------
    Parried, Blocked, Evaded, NoEffect, Nullified, Reflected, Absorbed, Avoided,
    // -- Combat verbs + line pieces (combat_format.cpp, combat_events.cpp) -------------------
    // Two vocabularies, deliberately distinct. EXECUTION (DamageLine, off the damage applier):
    // Attacks / Casts / Uses. CHARGE (the action-start announce): Casts / `Readies`. `Readies` is
    // NOT an execution verb -- using it as one is what made a landed enemy ability say "Urstrix A
    // readies Slap on Vaan. 14" (S90). Do not reintroduce it into DamageLine's switch.
    Attacks, Casts, Readies, Uses, OnJoiner, Heals,
    Defeated, DefeatedWithRewards, ExpSuffix, LpSuffix, BelowTwentyPercent,

    // -- Entity categories (entity_classify.cpp, entity_postscan.cpp, entity_commands.cpp) ----
    CatAll, CatExit, CatSaveCrystal, CatGateCrystal, CatTreasure,
    CatNPC, CatInteractables, CatEnemy, CatItems, CatSign, StoryGated,
    CatDoor, CatShop,

    // -- Navigation status (entity_commands.cpp, path_planner.cpp, entity_list.cpp, ...) ------
    PositionUnavailable, PathClear, BlockedBearPrefix, BlockedWord, RouteUnavailable,
    AtTheExit, NoPath, NoTargets, NoTarget, EnteringPrefix,
    LabelCleared, LabelledPrefix, DiagnosticUnavailable, DiagnosticLogged,
    FmtAreaObjects, FmtObjects,          // format templates: word order is locale-dependent

    // -- License board (license_reader.cpp) ---------------------------------------------------
    Learned, CanLearn, NotEnoughLP, LockedLower, LockedUpper,
    LicenseBoard, LicenseBoardSuffix, LicensePointsSuffix,

    // -- Gauges / status labels (status_reader.cpp, party_status.cpp, battle_target_reader.cpp)
    LevelPrefix, HPPrefix, MPPrefix, LPPrefix, EXPPrefix, NextPrefix,
    OfJoiner, PercentSuffix, Queued,

    // -- Field menu, Party screen: membership is a portrait POSITION plus an alpha dim, with no text
    // anywhere in the binary to read. USER-AUTHORIZED this conversation (Session 93), for exactly these
    // two words and no others. (char_select_reader.cpp)
    InParty, NotInParty,

    // -- Title menu: baked sprite art, the one sanctioned label exception (title_reader.cpp) ---
    TitleNewGame, TitleLoadGame, TitleTrialMode, TitleCredits, TitleExit,

    // -- Shop / inventory / gil (shop_reader.cpp, gil_reader.cpp) -----------------------------
    GilSuffix, InInventorySuffix, TimesSuffix,

    // -- Interaction target (interact_target.cpp) ---------------------------------------------
    Talk, Action,

    // -- Ability summary (ability_entry.h, ability_summary_reader.cpp) ------------------------
    EmptySlot, Unavailable,

    // -- Gambits: battle draws an icon frame, the field screen an alpha dim; no text exists ----
    On, Off,

    // -- The mod talking about itself (speech.cpp, combat_log.cpp) ---------------------------
    // MenuCaptureOn/Off were removed in S90 with the F4 painter-interception A/B diagnostic they
    // announced; that toggle disabled row-text capture, so an accidental press silently killed
    // menu reading. F4 is now the combat-verbosity toggle.
    SpeechOn, SpeechOff, CombatLogEmpty,

    // -- Mod menu and its settings (mod_menu.cpp). Wording supplied by the user, S90 -----------
    ModMenu, ModMenuClosed, SettingCombatVerbosity,
    VerbosityNormal, VerbosityVerbose,
    VerbosityDesc, VerbosityDescNormal, VerbosityDescVerbose,
    SettingAudioBeacon, BeaconOff, BeaconOn,
    BeaconDesc, BeaconDescOff, BeaconDescOn,
    // S95: the in-combat target ping became a setting of its own, so it needs its own words. The
    // Off/On VALUES are reused from above rather than duplicated -- they are generic.
    SettingBeaconVolume, BeaconVolumeDesc,
    SettingTargetBeacon, TargetBeaconDesc, TargetBeaconDescOff, TargetBeaconDescOn,
    SettingTargetVolume, TargetVolumeDesc,
    // S100: auto-walk. The Off/On VALUES reuse BeaconOff/BeaconOn -- they are generic. Wording was
    // included in the approved S100 plan; `AutoWalkStopped` is spoken ONLY on the no-progress cap
    // and on route loss -- player-initiated stops and self-announcing ones (arrival cue, combat,
    // map transition) stay silent per the silence-is-normal rule.
    SettingAutoWalk, AutoWalkDesc, AutoWalkDescOff, AutoWalkDescOn, AutoWalkStopped,
    // (S106's four sneak-assist ids were removed in S115 with the setting they named. The feature is
    // automatic on the maps `path_danger.cpp` lists, so it has no menu row and speaks nothing.)

    Count
};

// The phrase for `id` in the active locale. Never null: an unpopulated locale slot falls back to
// English, and an out-of-range id yields the empty string rather than faulting.
const wchar_t* Get(Id id);

// Select the locale column (0 = English). Out-of-range values are ignored. Currently only ever
// called with English — the game's own locale selector is not identified yet.
void SetLocale(int locale);
int  GetLocale();

} // namespace Phrase

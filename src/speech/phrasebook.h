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
    CatDoor, CatShop, CatTrap,

    // -- Navigation status (entity_commands.cpp, path_planner.cpp, entity_list.cpp, ...) ------
    PositionUnavailable, PathClear, BlockedBearPrefix, BlockedWord, RouteUnavailable,
    AtTheExit, NoPath, NoTargets, NoTarget, EnteringPrefix,
    LabelCleared, LabelledPrefix, DiagnosticUnavailable, DiagnosticLogged,
    FmtAreaObjects, FmtObjects,          // format templates: word order is locale-dependent

    // -- License board (license_reader.cpp) ---------------------------------------------------
    // Two entries were deleted with the FUN_00323600 status switch they served (S149).
    // `LockedLower` ("locked") covered statuses 3/4/5/8, which the grid builder has already zeroed
    // to id 0xFFFF before the reader sees them. `NotEnoughLP` was dropped by user decision: the line
    // already carries the node's cost, `U` reads the total, and the GAME puts up its own message when
    // you confirm a node you cannot pay for — so the mod would be pre-empting the player's own
    // arithmetic. The board status answers one question only: will Confirm do anything?
    Learned, Available, LockedUpper,
    LicenseBoard, LicenseBoardSuffix, LicensePointsSuffix,

    // -- Gauges / status labels (status_reader.cpp, party_status.cpp, battle_target_reader.cpp)
    LevelPrefix, HPPrefix, MPPrefix, LPPrefix, EXPPrefix, NextPrefix,
    OfJoiner, PercentSuffix, Queued,

    // -- The one Libra word (battle_target_reader.cpp). USER-AUTHORIZED 2026-08-10, S147.
    // `o` on an enemy asks for the Libra readout. When Libra is down the game shows a bar and no
    // numbers, and it has no text anywhere saying so -- but staying silent here would be
    // indistinguishable from a broken key, because `o` is a direct question the player just asked.
    // This is the ONLY spoken exception in the file; the per-highlight autodetail path never says it.
    LibraNotActive,

    // -- Party menu, Party screen: membership is a portrait POSITION plus an alpha dim, with no text
    // anywhere in the binary to read. USER-AUTHORIZED this conversation (Session 93), for exactly these
    // two words and no others. (char_select_reader.cpp)
    InParty, NotInParty,

    // -- Title menu: baked sprite art, the one sanctioned label exception (title_reader.cpp) ---
    TitleNewGame, TitleLoadGame, TitleTrialMode, TitleCredits, TitleExit,

    // -- Shop / inventory / gil (shop_reader.cpp, gil_reader.cpp) -----------------------------
    GilSuffix, InInventorySuffix, TimesSuffix,

    // -- Save slots (save_reader.cpp) ---------------------------------------------------------
    // The game draws playtime as SPRITE DIGITS with no words anywhere near it, so there is no
    // game-supplied wording to read instead. Added on the tester's explicit request for playtime
    // on the save slots -- the number is unspeakable without a unit.
    HoursSuffix, MinutesSuffix, ClanPointsSuffix,

    // -- Clan Primer (primer_reader.cpp). The Hunts list draws COMPLETE as SPRITE ART with no string
    // behind it, exactly like the combat outcome words. There is no word for the incomplete state
    // because the game draws no badge for one -- nothing is reported rather than inventing "pending".
    HuntComplete,

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
    // S130's font-atlas row, RESTORED S177 after S147 removed it -- same six ids, same order,
    // same wording, with ONE edit: the row is now spoken as "Diacritics override" (the user's
    // name for it, 2026-08-31). The two VALUE words are its own rather than reusing Off/On:
    // this is not a feature being switched on. Flag any further reword to the user.
    SettingTextGlyphs, TextGlyphsDesc,
    TextGlyphsStandard, TextGlyphsPolish, TextGlyphsDescStandard, TextGlyphsDescPolish,
    // S147: autodetail. The Off/On VALUES reuse BeaconOff/BeaconOn -- they are generic. Wording
    // user-approved 2026-08-10. What it switches is what gets VOLUNTEERED, never what is reachable:
    // the `4`-`9` shop columns and `o` keep working identically in both modes.
    SettingAutoDetail, AutoDetailDesc, AutoDetailDescOff, AutoDetailDescOn,
    // Gamepad intercept. The Off/On VALUES reuse BeaconOff/BeaconOn -- they are generic. This row
    // is the KILL SWITCH for the pad hook: a mod that could break the controller outright is a mod a
    // pad player cannot report a bug from, so there is always a keyboard route back to a stock pad.
    // Wording from the approved plan; flag it to the user before rewording.
    SettingController, ControllerDesc, ControllerDescOff, ControllerDescOn,
    // S173: mod mode -- the pad's modifier. Both words were APPROVED with the S162 plan and
    // deliberately withheld until the mode existed, so this is the sanctioned moment to add them.
    // "Mod" is spoken on arming and "Cancelled" whenever the mode ends without running a command
    // (the modifier pressed twice, an unmapped button, or the arm timing out). A blind player must
    // be able to hear which of the two pads they are holding, so BOTH transitions speak.
    ModMode, ModCancelled,
    // (S106's four sneak-assist ids were removed in S115 with the setting they named. The feature is
    // automatic on the maps `path_danger.cpp` lists, so it has no menu row and speaks nothing.)

    // -- Equipment comparison and screens the game draws as ART (user-approved this session) ------
    // Every one of these names something FFXII renders with no text behind it, so there is nothing
    // to read back:
    //   StatUp/StatDown  -- the up/down arrow GLYPH beside a stat delta (FUN_002cc780 picks the
    //                       glyph off the sign bit; the number is drawn as its absolute value).
    //   CannotEquip      -- a greyed-out character column; the game says nothing, it just dims.
    //   AlreadyEquipped  -- the "this character is already wearing it" flag at colB+0xE0.
    //   Leader           -- the party-leader bit on a chooser row (row+0xFC bit 4).
    //   GameOver         -- baked texture art (`gameover_c.tm2`), like the title logo.
    StatUp, StatDown, CannotEquip, AlreadyEquipped, Leader, GameOver,

    // -- Bhujerba shout minigame (shout_meter.cpp). USER-SUPPLIED WORDING, 2026-08-05: the tester
    // called it "the infamy meter" when asking for it. The game draws this gauge as art and never
    // names it -- its only text is "<n> Bhujerbans heed your words" -- so there is no game string
    // to read instead. One word only; the direction of a change reuses StatDown above, and a
    // percentage reuses PercentSuffix, rather than inventing more.
    // `InEarshot` / `NoGuardsInEarshot` are only ever spoken once a row carries a MEASURED earshot
    // radius; with it unset the guard key gives distance and bearing and makes no safety claim.
    Infamy, InEarshot, NoGuardsInEarshot,
    // S134, tester's own wording ("civilians 3, guards 2"): the shout minigame rewards having as
    // many listeners around you as possible, so the guard key reports a CROWD rather than a list of
    // individuals. `People` is used until a measured guard id lets the count be split.
    People, Civilians, Guards,
    // The REPORTING window the crowd key names aloud while the real earshot is unmeasured. It is
    // spoken precisely so it is not a hidden claim about the game's rules.
    WithinPrefix,
    // The two context-gated mod-menu rows for the same minigame (S132). Wording is the tester's own
    // framing -- they asked for "the puzzle guide" and "instant success" as two separate toggles.
    SettingPuzzleGuide, PuzzleGuideDesc, PuzzleGuideDescOff, PuzzleGuideDescOn,
    SettingPuzzleSkip,  PuzzleSkipDesc,  PuzzleSkipDescOff,  PuzzleSkipDescOn,

    // -- The summoned Esper's duration gauge (party_status.cpp, key 8). USER-AUTHORIZED 2026-08-10,
    // S148, for this one word. The gauge is the lightning icon and pips beside the Esper's HP on the
    // battle HUD: icon art, with no string behind it anywhere -- the same case as Infamy above.
    // The COUNT reuses OfJoiner ("5 of 5") rather than adding a second joiner.
    //
    // DELIBERATELY NOT A UNIT. The backing pair (BtlWork+0x5AD8/+0x5ADC) is seeded from a per-Esper
    // constant and nothing in the read path establishes whether it depletes with time or with
    // actions, so the word names the gauge and claims nothing about what it counts. `PARTY` logs the
    // raw floats on every press; once a summon has been watched to its end this can be sharpened.
    SummonGauge,

    // -- Stilshrine of Miriam statue puzzle (statue_guide.cpp, key B). USER-AUTHORIZED 2026-08-12
    // for this feature, English only. Every word here names something FFXII renders as nothing at
    // all: the three guardians have no on-screen state and no text anywhere says which way one
    // points or whether it is right -- the rotation dialogue offers "clockwise" / "counterclockwise"
    // and then says nothing about the result, which is what makes the puzzle unplayable blind.
    //
    // `Clockwise` / `Counterclockwise` deliberately match the game's own two dialogue choices, so
    // the readout and the menu the player is about to use share one vocabulary.
    // `StatueSolved` / `StatueNotSolved` report the GAME's own per-statue verdict flag, never an
    // inference of ours; `StateUnknown` is spoken for a statue whose cells have not been measured,
    // because silently listing two of three would read as a two-statue puzzle.
    Statue, StatueSolved, StatueNotSolved, StateUnknown,
    Clockwise, Counterclockwise, Once, Twice,

    // S179: the Unreachable filter row. The user asked for the toggle in those words ("some sort of
    // unreachable filter ... it has to be a toggle"); the Off/On VALUES reuse BeaconOff/BeaconOn.
    // Appended at the END so no earlier positional row shifts. Flag any reword to the user.
    SettingUnreachable, UnreachableDesc, UnreachableDescOff, UnreachableDescOn,

    // S180: the Sochen Cave Palace door-puzzle row. The user asked for the toggle ("a mod toggle ...
    // that allows both puzzle flags to be set to solved"); the wording is ours and was flagged to them.
    // The Off/On VALUES reuse BeaconOff/BeaconOn. Door names are the game's own text, so the sentences
    // name the puzzles by what they are instead. Appended at the END; flag any reword to the user.
    SettingSochenPuzzles, SochenPuzzlesDesc, SochenPuzzlesDescOff, SochenPuzzlesDescOn,

    // S181: the by-hand guide for the same two puzzles (sochen_guide.cpp, key B). The game says
    // NOTHING about either sequence until the whole thing is right -- no step count, no "that one
    // worked", and the doors and exits it wants are named identically to their neighbours -- so
    // there is no game text to read instead. `solved` reuses StatueSolved, and the counts reuse
    // OfJoiner. Wording is ours; flag any reword to the user.
    SochenWaterfall, SochenDoorPuzzle, SochenStep, SochenOutOfTurn,

    // S183: the Pharos Third Ascent's Sigils of Sacrifice (sigil_colours.cpp). USER-AUTHORIZED 2026-09-17:
    // "this puzzle needs color coding". The game names only the Black, Green and Red sigils; the four
    // Sigils of Sacrifice are told apart by their glow alone, which has no text anywhere. Appended at the
    // END; flag any reword to the user.
    SigilWhite, SigilYellow, SigilPink, SigilPurple,

    // S191: the Soundscape rows. The user asked for the feature by that name ("a soundscape for each
    // type of interactible entity ... should, of course, be a toggle"), so the row NAME is their own
    // word rather than one we invented; the two sentences are ours. The Off/On VALUES reuse
    // BeaconOff/BeaconOn, and the volume row needs no value words at all (Kind::Percent renders a
    // number). Appended at the END so no earlier positional row shifts. Flag any reword to the user.
    SettingSoundscape, SoundscapeDesc, SoundscapeDescOff, SoundscapeDescOn,
    SettingSoundscapeVolume, SoundscapeVolumeDesc,

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

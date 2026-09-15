#pragma once

// The mod's OWN settings surface — the first screen in this project that belongs to the mod rather
// than to the game, so there is no game text to read and no game state to follow. `F8` opens it,
// `F4` toggles the one setting it holds from anywhere, and `o` reads the focused setting's
// description.
//
// WHY IT EXISTS: the combat log speaks a cherry-picked subset of events in realtime and logs the
// rest, and which subset that is has always been a compile-time table in combat_format.cpp with a
// comment saying it "should end up in mod_config.ini eventually". It cannot go there —
// mod_config.ini belongs to the RVA byte-validator and hand-editing it masks validator failures
// (CLAUDE.md, CRITICAL PROJECT BLOCKER) — so the mod keeps its own store instead.
//
// WHAT IT IS NOT: not a game menu, not modal, and it never pauses anything. Like the combat log it
// is pure on-demand state; opening it changes nothing about the game.
//
// ⚠ THE KEYS ARE NOT SWALLOWED. The mod is strictly read-only on input (it reads a `const`
// DirectInput buffer), so while this menu is open the arrow keys STILL reach the game and will move
// the party. That is a known, accepted constraint — the status virtual buffer has always had it. Do
// not "fix" it by swallowing keys; that would make the mod drive the game, which is a category
// change requiring explicit permission.

#include <cstdint>

namespace ModMenu {

// What the combat log speaks in realtime, on top of what it always logs.
//   Normal  — the shipped realtime set: enemy defeat + rewards, party low HP and KO, loot/steal/
//             poach/gil, failed commands, revive, level up, Paling/Shield/White Wind, party-wide
//             magick fields, Back attack.
//   Verbose — everything Normal speaks, plus an enemy or guest READYING an ability / BEGINNING to
//             cast (message ids 0x0D and 0x0E).
// Damage lines are log-only in BOTH modes and always have been; this setting never touches them.
enum class Verbosity : uint8_t { Normal = 0, Verbose = 1 };

// Whether the navigation audio beacon runs at all. Off silences it immediately, including a ping
// already sounding.
enum class Beacon : uint8_t { Off = 0, On = 1 };

// Settings the menu holds. Add here + in kSettings (mod_menu.cpp) + in the phrasebook, together.
// The order here IS the order the menu's Up/Down walks them.
//
// THE TWO BEACONS ARE SEPARATE SETTINGS (Session 95). They used to be one: the in-combat target ping
// only ever sounded if a route beacon happened to be running, because it lived behind the route
// beacon's `g_active` flag. They are different features -- one leads you somewhere, one tells you
// where the thing hitting you is -- and a player may well want the second without the first.
// SOME ROWS ARE CONTEXT-GATED (Session 132, tester's request). A setting may carry a visibility
// predicate; when it returns false the row is skipped by Up/Down/Home/End and by the menu's opening
// announcement, exactly as if it were not in the table. A setting with no predicate is always
// visible, which is every older row.
//
// **A HIDDEN ROW READS AS OFF** (Session 133, tester's rule). Every accessor below goes through the
// same gate the row's visibility uses, so a context-gated feature cannot act outside its context no
// matter what the stored value says -- there is no infamy meter to speak on a Bhujerba street with
// no shout sequence running, so the setting that speaks it answers off there. Consumers therefore
// do NOT each have to remember to re-check the context; asking the setting is enough.
//
// The STORED value is untouched by any of this. It still persists and comes straight back the moment
// the context returns, so a puzzle setting the player chose last week is still chosen when they next
// reach that puzzle.
enum class SettingId : int {
    CombatVerbosity = 0,
    AudioBeacon,          // the ROUTE beacon
    BeaconVolume,
    TargetBeacon,         // the in-combat target ping, gated on combat but not on a route
    TargetVolume,
    AutoWalk,             // S100: `\` also WALKS the route. Default Off; see auto_walk.h
    AutoDetail,           // S147: volunteer the detail on highlight instead of on a key. Default Off
    // The gamepad intercept's master switch. Default ON -- the feature exists to be used -- but it
    // is a ROW rather than a compile-time constant because a pad hook that misbehaved would leave a
    // pad player with no way to play and no way to report it. Off returns PadRouter::OnPoll on its
    // first line, so the input path becomes byte-identical to the mod with no pad support at all.
    Controller,
    // S130's row, RESTORED S177 at the user's instruction: which font atlas the decoder maps
    // bytes through. Two values, Standard and Polish translation, exactly as it always was --
    // only the spoken NAME changed, to "Diacritics override". It sits LAST of the always-visible
    // rows so every existing row keeps the position the player already knows.
    //
    // ⚠ IT OUTRANKS AUTODETECTION IN ONE DIRECTION ONLY. `Polish translation` forces the variant
    // and stands the detector down; `Standard` -- the default, and what every untouched install
    // carries -- means "no override" and hands the question back to detection. Reading value 0 as a
    // decision would force stock on everyone and the detector would never fire. See `SetVariant`.
    TextGlyphs,
    // S179: hide what cannot be walked to right now (behind a script-closed floor, or not connected),
    // and price closed floors hard in the router. Default OFF, and OFF is byte-identical routing and
    // listing: the user's condition for the feature was that working paths must not change unless the
    // player chooses it. Placed after TextGlyphs so every existing row keeps its position.
    UnreachableFilter,
    // S132, both visible ONLY while a shout-minigame sequence is actually running (shout_meter.h's
    // `PuzzleActive`, which reads the game's own gauge-shown bit -- not merely "you are in Bhujerba").
    PuzzleGuide,          // the spoken meter and the B/N keys.       Default ON  -- it only informs
    PuzzleSkip,           // one shout completes the minigame.        Default OFF -- it writes game state
    Count
};
// REMOVED Session 115: `SneakAssist`. It neutralises the palace guards' catch, and after S113 was
// play-confirmed the tester made it automatic on the two maps `path_danger.cpp` names -- so there is
// nothing left for a player to choose. A settings file still carrying `sneak_assist=1` is harmless:
// `Load()` ignores keys it does not know, by design.
//
// ~~REMOVED Session 147: `TextGlyphs`.~~ **RESTORED Session 177, unchanged** -- see the row in the
// enum above. S147 removed it on the grounds that detection had replaced it. Detection turned out
// never to have worked (two wrong constants, five releases), and because a mis-mapped font reads as
// ordinary text rather than as an error, nothing could show that -- so the row that HAD worked was
// deleted in favour of one that never had. It is back exactly as S130 shipped it: two values, same
// default, same `text_glyphs` key, only the spoken name changed to "Diacritics override".

// Loads the persisted settings and registers the input callbacks. Safe to call before Speech is up.
bool Init();
void Shutdown();

// The combat log's realtime policy reads this (CombatFormat::ShouldSpeakNow). Lock-free and safe
// from any thread: it is a relaxed atomic load, and the value only ever changes on a keypress.
Verbosity CombatVerbosity();

// The audio beacon reads these every field frame (AudioBeacon::OnGameFrame). Same lock-free relaxed
// load as CombatVerbosity, so they are safe to call from the game thread's hot path -- which matters
// more now, because `TargetBeaconOn` is what the beacon's O(1) idle check consults before deciding
// whether it may skip the frame entirely.
bool AudioBeaconOn();      // the ROUTE beacon
bool TargetBeaconOn();     // the in-combat target ping
bool AutoWalkOn();         // S100: whether `\` may engage auto-walk. Read from input + game threads
bool AutoDetailOn();       // S147: whether detail is VOLUNTEERED on highlight. Never gates a key
bool ControllerOn();       // whether the pad intercept may read or consume anything. Input thread
bool UnreachableFilterOn(); // S179: list filter (input thread) + closed-floor price (game thread)
bool PuzzleGuideOn();      // S132: whether the shout meter speaks and B/N answer
bool PuzzleSkipOn();       // S132: whether one shout completes the shout minigame

// Playback gain, 0..1, for each beacon. Never returns 0 -- the toggles above are how a beacon is
// turned off, so the quietest step is still audible and "silent" is never a volume the player can get
// stuck on without knowing why.
float BeaconVolume();
float TargetVolume();

// `F8` — open/close. Speaks "Mod menu. <setting>, <value>." on open, "Mod menu closed" on close.
void Toggle();

// `F4`. Advances a setting one step and wraps — the toggle idiom. Works whether or not the menu is
// open. Thin wrapper over Adjust, kept because F4 has one meaning the player already knows.
void CycleSetting(SettingId id);

// The menu's own Left/Right. THE ONE PLACE a setting's value changes: it moves the value, persists it,
// and speaks the new value. Two detectors, one emit function — see CLAUDE.md's "one choke point per
// surface".
//
// `delta` is -1 or +1. A two-valued setting WRAPS (so either arrow toggles it, as before); a volume
// CLAMPS at its ends, because wrapping 100% round to the quietest step on one keypress is a nasty
// surprise and the repeated spoken value is how the player hears they are at the end.
void Adjust(SettingId id, int delta);

// S174: `L3` on the pad. Flips the Controller row and speaks "Controller, <value>" — name included,
// which no other adjust path does. It is the escape hatch, so it must work in BOTH directions: the
// pad router runs it from a prologue ABOVE its own `ControllerOn()` gate, or turning the intercept
// off would take the only pad button that could turn it back on. Input thread only, via WM_PADCTRL.
void ToggleController();

// (`SetSilently` was removed in Session 115 along with the sneak-assist toggle, its only caller. It
// set a value without speaking it, for automatic changes the player did not ask for. If that need
// comes back, `git show` this session -- but do not re-add it speculatively: with no caller it is a
// second way to change a setting, which is exactly what `Adjust` exists to be the only one of.)

bool IsOpen();

} // namespace ModMenu

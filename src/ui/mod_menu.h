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
enum class SettingId : int { CombatVerbosity = 0, AudioBeacon, Count };

// Loads the persisted settings and registers the input callbacks. Safe to call before Speech is up.
bool Init();
void Shutdown();

// The combat log's realtime policy reads this (CombatFormat::ShouldSpeakNow). Lock-free and safe
// from any thread: it is a relaxed atomic load, and the value only ever changes on a keypress.
Verbosity CombatVerbosity();

// The audio beacon reads this every field frame (AudioBeacon::OnGameFrame). Same lock-free relaxed
// load as CombatVerbosity, so it is safe to call from the game thread's hot path.
bool AudioBeaconOn();

// `F8` — open/close. Speaks "Mod menu. <setting>, <value>." on open, "Mod menu closed" on close.
void Toggle();

// `F4`, and the menu's own Left/Right. THE ONE PLACE a setting's value changes: it advances the
// value, persists it, and speaks the new value. Two detectors, one emit function — see CLAUDE.md's
// "one choke point per surface". Works whether or not the menu is open.
void CycleSetting(SettingId id);

bool IsOpen();

} // namespace ModMenu

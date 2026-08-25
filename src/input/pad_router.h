#pragma once

#include <cstdint>
#include "input/pad_hook.h"

// THE PAD STATE MACHINE -- what each pad input means right now, and which of them the game must not
// see. Modelled on the FFPR mods' ControllerRouter, which is the design the tester asked for.
//
// PASSTHROUGH IS THE DEFAULT AND THE DEFAULT IS THE POINT. Every poll starts with nothing consumed;
// a mod function opts an input OUT of reaching the game, one input at a time. Anything the router
// does not claim arrives at the engine byte-identical to an unmodded run.
//
// IT REIMPLEMENTS NO FEATURE. Every action routes into the choke point the keyboard already uses --
// `NavCommands::OnNavKey`, `GilReader::Announce`, the party-slot handlers, `ModMenu::Toggle`. That is
// CLAUDE.md's centralization rule, and it is also what makes every future mod key reachable from the
// pad without touching this file.
//
// ---- THREADING, and why the gate is not evaluated here ------------------------------------------
// `OnPoll` runs on the game's input-poll thread. The predicates that decide "is the field really
// live and idle" -- IsFieldNavSafe, BattleCommandActive, PartyEngagement -- are GAME-THREAD reads
// (PartyEngagement walks the actor pool). Calling them from the poll is the exact mistake nav_probe
// had to be moved off the input thread to fix.
//
// So the verdict is COMPUTED ON THE GAME THREAD in `OnGameFrame` and published as a stamped relaxed
// atomic; `OnPoll` only reads it. Same shape as AutoWalk: all state on the game thread, the poll
// touches a couple of relaxed atomics and takes no locks.
//
// AND IT EXPIRES. The stamp makes the verdict go stale ~250 ms after the field tick stops, so a map
// transition, a pause or a stall silently ends consumption without any code having to know why --
// AutoWalk's mask-stamp idiom, for the same reason.
namespace PadRouter {

// Which surface the player is on, as judged on the game thread. Drives both what may be consumed
// and the survey log's context column.
enum class Context : uint8_t {
    Unknown = 0,   // no fresh verdict -- treated as "consume nothing"
    OffField,      // title, loading, between maps
    Field,         // field live, idle, no menu/dialogue/combat -- the only consumable context today
    FieldBusy,     // field live but a battle-command menu or a message box owns input
    Battle,        // party is engaged
};

enum class State : uint8_t { Normal, ModMode, ModMenu };

// INPUT-POLL THREAD, once per XInputGetState. Reads the PRE-consumption state, dispatches whatever
// the current context allows, and then clears from `state` exactly what the mod claimed. The only
// function in the mod that writes an XINPUT_STATE.
void OnPoll(uint32_t userIndex, PadHook::State* state);

// GAME THREAD, once per field frame (nav_hooks.cpp), alongside the beacon and auto-walk. Computes
// and stamps the context above. O(1) plus one actor-pool scan the beacon already pays for.
void OnGameFrame();

// Current context as last published. Log/diagnostic use; any thread.
Context CurrentContext();
State   CurrentState();

} // namespace PadRouter

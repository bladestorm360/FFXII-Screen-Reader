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
// ---- THE SCHEME (Session 173, revised 174, rebuilt 185 to the user's own layout) ---------------
//
// WHAT FFXII ITSELF SPENDS, which is what the scheme had to be built around: L1 Speed mode · L2 zoom,
// and lock-on in a fight · L3 area map · R2 map zoom, and hold-to-flee in a fight · R3 recentre
// camera · Select map · Start pause · Triangle party menu. R1 is the ONE control with no field job.
//
// NORMAL -- no modifier.
//   Right stick   Up    describe / Libra (`o`) in a menu, a message box or a battle; on a plain idle
//                       field it falls back to previous category (`-`). `o` earns the stick exactly
//                       where it has something to say.
//                 Down  next category (`=`)      Left  previous object (`[`)
//                                                Right next object (`]`)
//   D-pad         FIELD ONLY: party 1, 2, 3, guest (`4` `5` `6` `7`), clockwise from Up.
//                 everywhere else: dispatched as ARROW KEYS to the Status and Clan Primer buffers
//                       and NOT consumed, so the game's own cursor still gets them -- the keyboard
//                       behaves identically, because it cannot swallow a key at all. Combat is on
//                       this side of the line: a fight is always one command menu away, and party
//                       slots are not worth costing the player that cursor.
//   L1            THE INTERACT READOUT (`;`) -- what am I about to talk to, open or hit, and in a
//                       fight the enemy's name and HP. S185 took it from the game's Speed mode at
//                       the user's instruction: game speed is reachable from the options menu and
//                       from the keyboard, and pad buttons are too scarce to spend one on it.
//   R1            THE ROUTE KEY (`\`), in every context -- route to the current selection and start
//                       the beacon. S185 stopped it changing meaning in a fight: the route a player
//                       most needs mid-combat is a way OUT, and sending R1 to `p` there meant the
//                       one context where escaping matters was the one where the route key aimed at
//                       the enemy. `p` moved to mod + Y.
//   Back          arm mod mode. Speaks "Mod". Costs the game's map toggle, knowingly.
//   L3            reachability filter on/off.   R3   audio beacon on/off.
//   L3 + R3       switch the intercept off or on. Speaks "Controller, <value>".
//
//   BOTH SHOULDERS PASS THROUGH IN A MENU AND UNDER A TARGETING CURSOR, which is what keeps the
//   battle target list intact: there the context is FieldBusy, so the game keeps L1 and R1 as its
//   Foes / Party / Reserve / Allies group step -- the switch S184 built the spoken titles for.
//
// MOD MODE -- armed by Back, spends itself on the NEXT button, expires after 5 s.
//   Start  mod menu (`F8`)            B  mod menu (`F8`) -- closes it, and opens it when shut
//   A      summoned Esper (`8`), silent when none is out
//   X      gil (`g`)                  -- in a fight: enemy name and HP (`;`)
//   Y      rescan + area (`` ` ``)    -- in a fight: directions to the target (`p`)
//   Anything else -- Back again, the D-pad, either shoulder, either stick click -- ends the mode and
//   speaks "Cancelled".
//
//   FIVE BINDINGS, AND THE REST WENT BACK (S185). License Points, the combat-log step, `t` and `o`
//   left this table when the user set the layout above: the D-pad and the right stick now mean one
//   thing everywhere, and a button that changes job depending on a latch is a button the player has
//   to remember the state of. The keyboard keeps every one of those keys -- they cost nothing there.
//
// MOD MENU OPEN -- it is modal, so here the pad IS taken: D-pad and right stick move and change the
//   focused setting, A reads its description, B, Start or Back closes. So does Escape, from the
//   keyboard (S185).
//
// WHAT IS DELIBERATELY NOT BOUND: A, B, X and Y in Normal mode. They are the game's core verbs --
// talk, cancel, map, menu -- and a mod that eats one of them is a mod the player cannot play
// through. Everything they would have carried is one Back away instead.
//
// EVERY BINDING IS A VIRTUAL-KEY CODE handed to `InputTracker::DispatchModKey`. This file owns no
// behaviour: `\` from a pad and `\` from the keyboard are the same call into the same handler, so a
// change to what a key does needs no edit here, and a new mod key is one row in a table above.
//
// THE THUMB-CLICKS ARE THE ONE EXCEPTION TO "NO SETTING GETS A PAD BUTTON", and S185 widened it from
// one button to three bindings on the user's instruction. The rule it bends is real -- a switch is
// two presses away through the menu, which says what it changed -- but the reachability filter and
// the beacon are the two the user flips constantly mid-play, and the intercept's own kill switch
// cannot live behind a menu driven by the pad it switches off. They fire on RELEASE, not on press,
// which is what lets one pair of buttons carry two singles and a chord with no timer.

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
    Field,         // field live, idle, no menu/dialogue/combat -- the stick, the D-pad and R1
    FieldBusy,     // field live but a battle-command menu or a message box owns input
    Battle,        // party is engaged -- R1 routes to the target, but the D-pad and the camera stay
                   // the game's
};

// Log-facing names for a context and for a right-stick direction (0..3 = Up/Down/Left/Right).
// Shared with `pad_survey.cpp` so neither spelling can drift from the other.
const char* ContextName(Context c);
const char* StickDirName(int dir);

// Normal: nothing armed. ModMode: Back pressed, the next button is a mod command. ModMenu: the mod's
// own settings menu is open and owns the pad. Published for the log and for diagnostics; the poll
// decides its own branch from ModMenu::IsOpen() and the arm stamp, never from this.
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

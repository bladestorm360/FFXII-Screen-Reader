#pragma once

#include <cstdint>
#include "input/pad_router.h"

// NORMAL MODE'S BINDINGS -- what the right stick, the two shoulders and the D-pad mean when nothing is
// armed and the mod menu is closed. `pad_router.cpp` keeps the STATE MACHINE (edges, the thumb-click
// chord, the off switch, the mod menu, mod mode, Back, and the one apply); this file only answers "in
// this context, what does that control do". Split out at S194: the router was 571 lines, and the
// right-stick camera row gives the D-pad a second table.
//
// ---- THE RIGHT-STICK CAMERA ROW (S194, a tester's request through the user) ---------------------
//
// OFF (default) -- the shipped layout, byte for byte:
//   right stick   the pathfinder on a live field (and swallowed there, so the camera holds still);
//                 Up is describe / Libra (`o`) anywhere a description could be read.
//   D-pad         the party (`4` `5` `6` `7`, clockwise from Up) on the open field only.
//
// ON -- only the stick's CAMERA job is freed:
//   right stick   does NOTHING for the mod on the open field or in a fight with no menu up, and is
//                 never swallowed, so the camera turns. Wherever a command menu, a target cursor or a
//                 message box owns input it still works as it does with the row off (Up describes).
//   D-pad         open field: THE PATHFINDER, in the stick's own layout -- Up previous category
//                 (`-`), Down next category (`=`), Left previous object (`[`), Right next object
//                 (`]`). In a fight with no menu up: THE PARTY, clockwise from Up, as the field had it.
//                 Everywhere else it is unchanged: dispatched as arrow keys and passed to the game's
//                 cursor, so a battle menu or a target list keeps the D-pad.
//   R3            unchanged -- still the beacon toggle. The user's ruling: *"only the camera turning
//                 functions should be freed. r3 can still toggle the beacon"*. L3 + R3 flips THIS
//                 row (S194: *"make l3/r3 toggle between control schemes"*). The beacon re-aims as
//                 the player turns, which is why that tester can steer by it with the camera moving.
//
// The fight gate is the one L1 and R1 already use: `Context::Battle` is "engaged, and no command menu
// or message box up" -- the instant a menu opens the context is FieldBusy and the D-pad goes back to
// the game's cursor. That is why this reverses S174's "D-pad never in combat" only for the moment in
// a fight when there is no cursor for it to move.
namespace PadRouter {

// Right-stick directions, in the order every table here is written. `pad_router.cpp` computes the
// edges; `StickDirName` gives the log spelling.
enum Dir { DIR_UP = 0, DIR_DOWN, DIR_LEFT, DIR_RIGHT, DIR_COUNT };

// THE one dispatch: hands `vk` to InputTracker::DispatchModKey and writes the PAD line. Defined in
// pad_router.cpp; every binding in both files goes through it.
void Act(const char* padInput, int vk, const char* action, Context ctx);

// Normal mode, minus Back (which arms mod mode and so belongs to the state machine). Dispatches what
// `ctx` allows, returns the button bits the game must not see, and sets `*eatStick` when the right
// stick must be zeroed for the game. Input-poll thread.
uint16_t NormalBindings(Context ctx, uint16_t rising, const bool (&stickRising)[DIR_COUNT],
                        bool* eatStick);

} // namespace PadRouter

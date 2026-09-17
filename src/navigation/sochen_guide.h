#pragma once

// THE SOCHEN CAVE PALACE PUZZLE GUIDE -- `B`, anywhere in the palace: which step you are on, and
// which exit or door is the next one.
//
//     Waterfall puzzle, step 2 of 4. Mirror of the Soul 3.
//     Door puzzle, step 3 of 8. Door of Hours 5.
//
// It is the by-hand half of S180's `Solve door puzzles` row: that row skips both puzzles, this key
// lets the player actually do them. Nothing here writes game memory or installs a hook.
//
// WHY A GUIDE IS ENOUGH TO MAKE THEM PLAYABLE. Both puzzles are sequences of ordinary moves -- take
// that exit, open that door -- and the mod can already lead the player to a named object. What a
// blind player cannot do is know WHICH of five identical-sounding exits, or which of sixteen doors,
// is the next one, because the game says nothing until the whole sequence is right. So `B` names the
// next target and puts the `[` / `]` focus on it; the route key then leads there as it does for any
// other object. One choke point: this file never routes, it only focuses.
//
// THE TWO PUZZLES, decoded offline from the room scripts (GameArchitecture.md "Sochen Cave Palace
// door puzzles"):
//
//   WATERFALL (Falls of Time 184, Mirror of the Soul 185, Destiny's March 192). Four legs. Each leg:
//   leave Falls of Time by one specific exit, then come back into Falls of Time by one specific
//   entrance. The script keeps the state in the cross-script work globals (storage class 5):
//   `+0x80..+0x83` "left through leg N's exit", `+0x84..+0x87` "leg N completed". Any other arrival
//   into Falls of Time clears all eight. Leg 4 sets the save-block bit that opens both Pilgrim's
//   Doors.
//
//   DOOR / CLOCK (Destiny's March 192). Eight of the map's doors, opened in one fixed order, each
//   from one fixed SIDE -- the `a` and `b` sides are separate objects, which is why the table below
//   names routines and not doors. The script counts steps in its own module storage (class 1) at
//   `+0x3c`, and each door raises a flag when opened: a step only counts while no LATER door's flag
//   is up, so one door opened out of turn stalls the sequence until the map is reloaded. That is the
//   inscription's "Stray but once, another day must you return", and the guide says so.
//
// IT IS THE RESUME KEY TOO, and that is not a second feature: every press re-reads the script's own
// counters and re-focuses, so after a detour for a chest or a fight, `B` says where the sequence
// actually stands now and points at the next target again. From a room that is not one of the
// puzzle's own, it focuses the way back toward them instead of a step with nowhere to go.
//
// TELL THE PLAYER THIS ONCE, because the game does not: in Destiny's March a detour THROUGH one of the
// eight doors is itself a wrong move, since opening a door is what raises its flag. The guide reports
// that honestly rather than hiding it -- it says the count has stopped and that leaving and coming back
// restarts it.
//
// WHAT IT NEVER DOES: it never claims a step is done -- every number it speaks is read from the
// script's own counters on the frame the key is pressed. If the state cannot be read it says nothing
// and logs why, which is the statue guide's rule and this file follows it.
namespace SochenGuide {

// GAME THREAD, once per field tick, from nav_hooks' field-frame hook. Drains the `B` request.
// Outside the palace it is five slot reads and a return.
void OnFieldFrame();

// GAME THREAD, from the field-teardown hook.
void OnMapTeardown();

// INPUT THREAD. Raises a flag only; the work happens on the next field frame, because reading script
// variables is a game-thread job. `B` is shared with the shout meter and the statue guide -- each
// drains its own request against its own structural gate, and the three contexts are three different
// places in the game.
void RequestCheck();   // B

} // namespace SochenGuide

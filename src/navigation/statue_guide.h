#pragma once

// THE STATUE READOUT for the Stilshrine of Miriam -- `B`, anywhere in the dungeon, all three
// guardians at once:
//
//     Statue 1: counterclockwise once. Statue 2: solved. Statue 3: state unknown.
//
// WHY IT COVERS ALL THREE FROM ONE PRESS. The statues sit in three separate rooms and the puzzle
// only completes when all three are right, so "the one in this room" is not the question a player
// has. It is answerable because the state is not map-local: it lives in the persistent SAVE BLOCK
// (script storage class 0), shared by every script -- see statue_table.h. A statue whose save-block
// offset is known reads from anywhere; a statue whose offset is not yet known reads only while the
// player stands in its room, and its offset is LEARNED and logged the first time that happens.
//
// WHAT IT NEVER DOES:
//   * it does not encode a walkthrough. "Solved" is the game's own per-statue verdict flag, read
//     directly. The measured target is used for exactly one thing -- counting the turns left -- and
//     the flag always wins when the two disagree (which is logged, loudly);
//   * it does not write game memory, and it installs no hook;
//   * it does not speak outside this dungeon. `B` is shared with the Bhujerba shout minigame's
//     infamy meter, and the two contexts can never both be live: each drains its own request and
//     each is a silent no-op where it does not apply.
//
// SILENCE IS A REAL ANSWER HERE. If not one statue resolves -- the save block did not read -- the
// key says nothing and logs why, rather than reciting three "unknown"s.
namespace StatueGuide {

// GAME THREAD, once per field tick, from nav_hooks' field-frame hook. Refreshes which Stilshrine
// room script is live, drains the `B` request, and does the one-per-visit measurement logging.
// Off the dungeon it is five slot reads and a return -- it arms on a live `mrm_` script, never on a
// map id, so it cannot be fooled by a map the mod has not seen.
void OnFieldFrame();

// GAME THREAD, from the field-teardown hook. Drops the live-module cache and the per-visit log
// latches. LEARNED SAVE-BLOCK OFFSETS DELIBERATELY SURVIVE: an offset is a property of the module's
// descriptor table and of a save block at a fixed address, so it stays true for the whole session --
// which is the point. Once the player has walked through a room, that statue reads from anywhere
// for the rest of the session.
void OnMapTeardown();

// INPUT THREAD. Raises a flag only; the work happens on the next field frame, because resolving
// script variables and reading the save block is a game-thread job (the `'` probe's arrangement,
// nav_commands.cpp).
void RequestCheck();   // B

} // namespace StatueGuide

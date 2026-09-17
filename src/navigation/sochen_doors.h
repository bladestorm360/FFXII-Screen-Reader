#pragma once

// SOCHEN CAVE PALACE DOOR PUZZLES -- mark both solved, so their doors open.
//
// ⚠ A GAME-MEMORY WRITE. CLAUDE.md's standing rule is that the mod is read-only on game memory.
// USER-AUTHORIZED in the conversation of 2026-09-15 (Session 180), explicitly: "a mod toggle that
// only appears in the menu when actively in the palace that allows both puzzle flags to be set to
// solved so that the pilgrims and ascetic's doors can simply be opened." Same shape as the shout
// minigame's Instant success (shout_fill.h): a context-gated mod-menu row, default Off.
//
// THE FLAG, decoded offline from the room scripts (GameArchitecture.md "Sochen Cave Palace door
// puzzles"). One u8 in the storage-class-0 SAVE BLOCK at class0+0x918, declared identically
// (descriptor 0x00000918) by rui_a01/a02/a04/a05, rui_b01 and rui_b03:
//   bit 0x02  the waterfall puzzle is solved. Falls of Time (rui_a01) ORs it in on completion; both
//             Pilgrim's Doors in Mirror of the Soul (rui_a02 gim_door01/02) refuse to open without it,
//             and Falls of Time lays its waterfalls out in the solved pattern on load while it is set.
//   bit 0x01  the clock puzzle is solved. Every Door of Hours in Destiny's March (rui_b01) ORs it in
//             on completing the circuit; the Ascetic's Door (secret_door) refuses to open without it,
//             and the map's loader enables the exit behind that door while it is set.
// The other six bits belong to other doors and a lift, and are never touched:
//   0x04/0x08 lift switches (rui_a05, rui_b03), 0x10/0x20 locked big doors (rui_a04, rui_b01),
//   0x40/0x80 "the Ascetic's / Pilgrim's Door has been opened" -- the GAME sets those when the player
//   opens the door, so the door still opens through its own dialogue.
//
// THE BOUNDARIES, all non-negotiable:
//   1. THE ROW IS THE ONLY SWITCH, and it is visible only while a `rui_` script is live -- every
//      shipped `rui_` map script jumps only to Sochen maps. Hidden reads as Off (L-50), so outside the
//      palace this file does five slot reads and returns.
//   2. OFF IS BYTE-IDENTICAL. With the row Off nothing is written.
//   3. ONE BYTE, TWO BITS, OR ONLY. The write is `byte | 0x03`. Nothing is ever cleared, so turning
//      the row Off again does not undo a solve -- exactly as solving it by hand cannot be undone.
//   4. IT FAILS OPEN. Before writing, the live module's class-0 base must equal the save block
//      FUN_002ef2b0 returns (RVA 0x2044480, play-confirmed by the Clan Primer reader), and if the
//      module declares class0+0x918 at all, that descriptor must be exactly u8. Any mismatch logs the
//      numbers and writes nothing.
//   5. AT MOST ONE ATTEMPT PER MAP VISIT while the row is On; every attempt logs before and after.
//
// ACCEPTED LIMIT, stated rather than buried: both maps read the bits once at load for part of their
// behaviour (the waterfall layout; the exit behind the Ascetic's Door). Switching the row On while
// standing in one of those two rooms takes effect there on the next entry. The doors themselves check
// the bit when the player interacts, so they answer at once.
namespace SochenDoors {

// GAME THREAD, once per field tick, from nav_hooks' field-frame hook. Refreshes whether a Sochen
// script is live and, when the row is On, makes the one write this visit allows.
void OnFieldFrame();

// GAME THREAD, from the field-teardown hook. Re-arms the per-visit attempt and clears the live flag.
void OnMapTeardown();

// ANY THREAD. Whether a Sochen Cave Palace script is live -- the mod-menu row's visibility predicate.
bool InPalace();

} // namespace SochenDoors

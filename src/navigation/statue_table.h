#pragma once

#include <cstdint>

// THE MEASURED TABLE for the Stilshrine of Miriam statue-rotation puzzle.
//
// Three "Stone Brave" guardians, one per room, each turned a quarter turn at a time by a dialogue
// that offers only "Turn the statue clockwise" / "counterclockwise". Nothing on screen or in text
// says which way a statue now points or whether it is right, so the puzzle is unsolvable without
// sight except by brute force. `statue_guide` is the readout; this file is the data it reads.
//
// ---- WHERE THE STATE LIVES (Session 156, measured in play) -------------------------------------
//
// Each guardian keeps TWO cells in script storage class 0, and storage class 0 is the game's
// PERSISTENT SAVE BLOCK at 0x02164480 (`FUN_002ef2b0()`), shared by every script on every map:
//   * a FACING cell, s8, values 1..4, ninety degrees apart. CLOCKWISE INCREMENTS (4 wraps to 1);
//     COUNTERCLOCKWISE DECREMENTS (1 wraps to 4). Confirmed on two statues, both directions, nine
//     transitions.
//   * a CORRECTNESS FLAG cell, s8, 0 or 1 -- the GAME'S OWN VERDICT for that one statue. It set on
//     reaching the target facing and cleared on leaving it, three times, in both directions. The
//     player confirmed it by ear before the instrument did: the statue's eye effect and a faint
//     buzz over the ambience are on exactly while it reads 1.
//
// Two consequences the readout is built on:
//
//  1. **NO SOLUTION TABLE IS NEEDED.** The flag answers "is this one right" directly, so the mod
//     reads correctness out of the game rather than shipping a walkthrough (L-07: read the word the
//     verdict is read from, do not model the verdict). The TARGET below is used for one thing only
//     -- saying how many turns are left -- and never to decide solved-ness.
//  2. **THE TARGETS DIFFER PER STATUE** (map 600 -> 1, map 599 -> 2), so the values are compass
//     headings, matching the authors' own `北方向`/`東方向`/`南方向`/`西方向` routine names. Any
//     model in which all three read the same number when solved is REFUTED -- do not reintroduce
//     one.
//
// ---- WHY A ROOM CARRIES BOTH A VARIABLE INDEX AND A SAVE-BLOCK OFFSET --------------------------
//
// They are two ways to reach the same byte, and each works where the other cannot:
//   * the VARIABLE INDEX is a slot in that module's own descriptor table, so it resolves only while
//     the player is standing in that room -- but it resolves with no hardcoded address at all, and
//     it is how an offset gets measured in the first place;
//   * the SAVE-BLOCK OFFSET is class-0 base + a constant, so it reads from ANYWHERE in the game.
// `statue_guide` uses the index when the room is live, LEARNS the offset from it, and uses offsets
// for the rooms the player is not in. A module only DECLARES the subset of the save block it uses,
// which is why one module's descriptor table can never enumerate another's cells.
//
// ---- WHAT IS STILL UNMEASURED, AND HOW IT FILLS IN --------------------------------------------
//
// `mrm_c01`, the third guardian, has NEVER BEEN VISITED, so none of its five numbers exist. Nothing
// is guessed here: the row is present with every field marked unmeasured, the readout reports that
// statue as unknown rather than inventing a state, and `statue_guide` LOGS what a future session
// needs the moment the player first walks into that room. Map 600's two offsets are unmeasured for
// a narrower reason -- the capture that found its variable indices ran on a build whose log had no
// address column -- and they are learned automatically on the next visit.
namespace StatueTable {

constexpr int kRoomCount = 3;
constexpr int kFacings   = 4;      // 1..4, ninety degrees apart

// Every captured cell decoded as elemType 1 (s8). Used when reading through a raw save-block
// offset, where there is no descriptor to ask.
constexpr uint8_t kCellElemType = 1;

constexpr uint8_t kUnmeasuredVar = 0xFF;
constexpr int32_t kUnmeasuredOff = -1;
constexpr int8_t  kUnmeasuredTarget = 0;

// One guardian. `mapId` and `target` are 0, and the var / offset fields are the sentinels above,
// wherever the number has not been measured.
struct Room {
    const char* srcName;     // the module's own authoring name, e.g. "mrm_b03.src"
    int         mapId;       // 0 when the room has never been visited
    uint8_t     facingVar;   // descriptor index IN THAT MODULE
    uint8_t     flagVar;     // descriptor index IN THAT MODULE
    int8_t      target;      // the facing at which this statue is correct
    int32_t     facingOff;   // byte offset of the facing cell inside the class-0 save block
    int32_t     flagOff;     // byte offset of the flag cell inside the class-0 save block
};

// The three rooms, in the order they are spoken -- map id ascending, with the never-visited room
// last. The order is the numbering: index 0 is spoken as "Statue 1".
const Room* Rooms();

// The row whose module is `srcName` (exact match), or null. `srcName` is the raw `<module>.src`
// string the engine stores, which is what ShoutScript::RawModule carries.
const Room* FindBySrc(const char* srcName);

} // namespace StatueTable

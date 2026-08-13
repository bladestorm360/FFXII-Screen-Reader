#include "navigation/statue_table.h"

#include <cstring>

namespace StatueTable {
namespace {

// ---- THE SAVE-BLOCK LAYOUT, MEASURED (S157) ----------------------------------------------------
//
// `mrm_c01` declares ALL THREE statues' cells, which is what exposed the shape. Two tight runs:
//
//     flags    class0+0x882  0x883  0x884     one byte per statue, CONSECUTIVE
//                (599)  (600)  (603)
//     facings  class0+0x9B1  0x9B3  0x9B5     one byte per statue, STRIDE 2
//                (599)  (600)  (603)
//
// The interleaved facing bytes `+0x9B2` / `+0x9B4` / `+0x9B6` each read 1 once their room has been
// entered (`+0x9B6` flipped 0 -> 1 on first entry to 603, alongside the facing initialising to 1).
// Read as a per-statue "initialised" byte -- OBSERVED, not relied on by anything.
//
// **`+0x885` is the strongest candidate for "the puzzle is complete".** It flipped 0 -> 1 on the very
// same frame as the third statue's flag `+0x884`, and it is declared by `mrm_b02` -- the SWORD script
// on map 598, i.e. the thing that lifts on completion. Not used yet; recorded so the completion
// announcement does not need another capture.
//
// EVERY NUMBER HERE IS MEASURED OR IS A SENTINEL. There is no interpolation between rows.
//
// The tempting shortcut, recorded so it is not taken: the flag's variable index is the facing index
// MINUS SIX in both captured modules (0x0E/0x08 and 0x0D/0x07), which looks like one authoring
// template instantiated three times. It is written down as an observation and is NOT used to fill in
// `mrm_c01` -- indices are per-module, two instances are not a population (L-01), and a wrong index
// would read some unrelated byte of the save block and report a confident wrong state.
const Room kRooms[kRoomCount] = {
    // Walk of Prescience. FULLY MEASURED: both cells captured with their absolute addresses
    // (facing 0x02164E31, flag 0x02164D02) against the class-0 base 0x02164480, so both offsets are
    // known and this statue reads from anywhere in the game.
    { "mrm_b03.src", 599, 0x0D, 0x07, 2, 0x9B1, 0x882 },

    // Walk of Reason. FULLY MEASURED (S157 capture): facing `0x02164E33`, flag `0x02164D03`.
    { "mrm_b04.src", 600, 0x0E, 0x08, 1, 0x9B3, 0x883 },

    // The third guardian, map 603 -- the boss room, which is also where the last statue is turned.
    // Cells measured S157 by TWO INDEPENDENT NETS that agree exactly: the module's own declared-
    // variable diff (`var 0x1C @0x02164E35`, `var 0x14 @0x02164D04`) and the raw class-0 byte diff
    // (`+0x9B5`, `+0x884`). Its census is also what revealed the layout below.
    //
    // TARGET 3, on the same rule that fixed the other two: THE FLAG SET WHILE THE FACING READ 3.
    // 599's flag set at facing 2 and 600's at facing 1; this one set at facing 3, on the frame the
    // completion cell `+0x885` also went up. The facing then moved 3 -> 4 five seconds later without
    // the flag clearing -- which is the COMPLETION SEQUENCE turning a statue the player can no longer
    // touch ("The statue is firmly fixed in place"), not a player turn, so it says nothing about the
    // target. Confirming it by unsolving the puzzle would cost a trip for a number three independent
    // observations already agree on; the user made that call explicitly.
    { "mrm_c01.src", 603, 0x1C, 0x14, 3, 0x9B5, 0x884 },
};

} // namespace

const Room* Rooms() { return kRooms; }

const Room* FindBySrc(const char* srcName) {
    if (!srcName || !*srcName) return nullptr;
    for (int i = 0; i < kRoomCount; ++i)
        if (strcmp(kRooms[i].srcName, srcName) == 0) return &kRooms[i];
    return nullptr;
}

} // namespace StatueTable

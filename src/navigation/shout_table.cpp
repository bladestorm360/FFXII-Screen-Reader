#include "navigation/shout_table.h"

#include <cstring>

namespace ShoutTable {
namespace {

// ---- THE CENSUS -------------------------------------------------------------------------------
//
// Produced by `..\FFXII-Decompile\tools\ebp_shout_census.py` over the shipped bytecode. Per module
// it locates every `setgaugecounter` (native 0x1AE) call site, decodes the instruction stream that
// ends at each one, and takes the most-voted `PUSHV` operand as the meter variable; the success
// threshold is the constant the same variable is compared against with OPGTE.
//
// The three thresholds the scripts compare the meter against are all recovered, and they agree
// exactly with the mechanics read out of the bytecode by hand:
//     `v >= 100`  the success test        -> the story proceeds (message 9)
//     `v >= 30`   the Imperial penalty    -> a 30-step -1 loop, one step per frame
//     `v >= 1`    the idle decay          -> a single -1 through changegaugecounterbyframe
// `setgaugecountermax` is 100 on every row, which is why `fillValue` and the gauge maximum agree.
//
// EVERY MODULE USES THE SAME GAUGE AND ONLY THAT GAUGE. The census also counted every gauge native
// per module: initgauge / setgaugeshowstatus / setgaugecounter / changegaugecounterbyframe /
// setgaugecountermax / setgaugecountercondition / setgaugecountertype / setgaugehidestatus, all in
// the counts one shout gauge needs, and `getgaugecounter` appears NOWHERE. So a second gauge that
// could be mistaken for the infamy meter does not exist on these maps — the risk is retired
// statically rather than left to a runtime guard.
//
// The four `byu_?01` stubs (w/x/y/z) contain no `setgaugecounter` call and are deliberately absent.
//
// ---- GUARD IDENTITY: the leading candidate, NOT yet shipped ------------------------------------
//
// The 2026-08-05 play log ran the sequence on map 804 (Cloudborne Row) and its object dump lists
// exactly one plausible watcher among eight NPC identities:
//
//     387  Bhujerban Sainikah   x1     <- "sainikah" is the game's word for SOLDIER
//     386  Cloudborne Patron    x2         311  Archadian Wayfarer   x1
//     388  Cloudborne Resident  x2         409  Lhusu Miner          x1
//    1054  Informed Parijanah   x3        1137  Bhujerban Guru       x1
//
// NOTE WHAT IS NOT THERE: no "Imperial". Bhujerba is Ondore's city and its street watch is his own
// sainikah, so the assumption that the punisher is an Imperial -- carried from the palace sneak
// sequence, where 694 really was one -- looks wrong for this map.
//
// **387 IS A CANDIDATE, NOT A MEASUREMENT.** One NPC of that name on one map, with no penalty
// observed beside it, is nowhere near the 0.98 bar, and shipping it would have the crowd key
// confidently miscount. It stays a comment until a PENALTY capture puts it close and the CLEAN
// captures put it far -- which is exactly what shout_diag.cpp's instrument is for.
constexpr Row kRows[] = {
    { "byu_a01.src", 0x0D, 100, -1, 0.0f },
    { "byu_a02.src", 0x0E, 100, -1, 0.0f },
    { "byu_a03.src", 0x10, 100, -1, 0.0f },
    { "byu_a04.src", 0x10, 100, -1, 0.0f },
    { "byu_a07.src", 0x0B, 100, -1, 0.0f },
    { "byu_a08.src", 0x35, 100, -1, 0.0f },
    { "byu_a11.src", 0x0F, 100, -1, 0.0f },
    { "byu_a12.src", 0x0F, 100, -1, 0.0f },
    { "byu_a14.src", 0x0F, 100, -1, 0.0f },
    { "byu_a15.src", 0x0F, 100, -1, 0.0f },
    { "byu_a16.src", 0x0F, 100, -1, 0.0f },
    { "byu_a17.src", 0x3B, 100, -1, 0.0f },
    { "byu_a18.src", 0x32, 100, -1, 0.0f },
    { "byu_b01.src", 0x16, 100, -1, 0.0f },
};

} // namespace

const Row* ForSrcName(const char* srcName) {
    if (!srcName || !*srcName) return nullptr;
    for (const Row& r : kRows)
        if (std::strcmp(r.srcName, srcName) == 0) return &r;
    return nullptr;
}

int Count() { return static_cast<int>(sizeof(kRows) / sizeof(kRows[0])); }

} // namespace ShoutTable

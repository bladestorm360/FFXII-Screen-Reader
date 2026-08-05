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
// ---- GUARD IDENTITY: 387 "Bhujerban Sainikah", MEASURED ----------------------------------------
//
// The 2026-08-05 play log settles it, on three independent legs:
//
//   1. The map's object dump lists eight NPC identities and exactly one soldier -- 387 "Bhujerban
//      Sainikah" ("sainikah" is the game's own word for soldier). The others are Cloudborne Patron,
//      Cloudborne Resident, Informed Parijanah, Bhujerban Guru, Lhusu Miner, Archadian Wayfarer.
//   2. Five seconds before the penalty fired, the mod's own interaction reader spoke
//      "Action: Bhujerban Sainikah" -- so the player was inside the ENGINE's interaction reach of
//      one at the moment they shouted. No other NPC was that close.
//   3. The penalty burst (meter 23 -> 0 over 25 sets) arrived in the same log line as the game's
//      own rebuke: "Lies, exaggerations, and obfuscations! These are all prohibited in Bhujerba!
//      And slandering His Excellency above all else!" -- His Excellency being the Marquis, whose
//      guards these are.
//
// **IT IS NOT AN IMPERIAL.** That assumption came from the palace sneak sequence, where 694 really
// was one, and there is no Imperial anywhere on this map. Bhujerba is Ondore's city and its street
// watch is his own sainikah.
//
// EARSHOT IS STILL 0. Leg 2 puts the trigger at roughly the engine's interaction reach -- the
// tester's words were "I had to get pretty close" -- but "roughly interaction reach" is not a
// number, and shout range and talk range are not the same quantity. The instrument in
// shout_diag.cpp brackets it properly: largest PENALTY distance below, smallest CLEAN distance
// above.
constexpr Row kRows[] = {
    { "byu_a01.src", 0x0D, 100, 0.0f },
    { "byu_a02.src", 0x0E, 100, 0.0f },
    { "byu_a03.src", 0x10, 100, 0.0f },
    { "byu_a04.src", 0x10, 100, 0.0f },
    { "byu_a07.src", 0x0B, 100, 0.0f },
    { "byu_a08.src", 0x35, 100, 0.0f },
    { "byu_a11.src", 0x0F, 100, 0.0f },
    { "byu_a12.src", 0x0F, 100, 0.0f },
    { "byu_a14.src", 0x0F, 100, 0.0f },
    { "byu_a15.src", 0x0F, 100, 0.0f },
    { "byu_a16.src", 0x0F, 100, 0.0f },
    { "byu_a17.src", 0x3B, 100, 0.0f },
    { "byu_a18.src", 0x32, 100, 0.0f },
    { "byu_b01.src", 0x16, 100, 0.0f },
};

// The complete soldier set (see shout_table.h). Two ids, and the npcdic has no third.
constexpr int16_t kGuardNames[] = { 387, 1053 };

} // namespace

bool IsGuardName(int16_t nameIdx) {
    for (int16_t g : kGuardNames)
        if (g == nameIdx) return true;
    return false;
}

bool HaveGuardIdentity() { return true; }

const Row* ForSrcName(const char* srcName) {
    if (!srcName || !*srcName) return nullptr;
    for (const Row& r : kRows)
        if (std::strcmp(r.srcName, srcName) == 0) return &r;
    return nullptr;
}

int Count() { return static_cast<int>(sizeof(kRows) / sizeof(kRows[0])); }

} // namespace ShoutTable

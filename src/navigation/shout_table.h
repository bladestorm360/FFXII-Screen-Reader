#pragma once

#include <cstdint>

// THE SHOUT-MINIGAME TABLE — one row per map script that runs Bhujerba's "shout at the crowd"
// sequence (Vaan calls himself Captain Basch to raise an infamy gauge; an Imperial in earshot
// drops it; at 100 the story proceeds).
//
// KEYED ON THE SCRIPT'S OWN NAME, NOT ON A MAP ID. Every EBP2 module carries three NUL-terminated
// strings at file offset 0x110 — build stamp, author, and `<module>.src` — and the third is the
// authoring name of the script itself (`byu_a01.src`). That layout is FORMAT-LEVEL, not a per-map
// discovery: it parses correctly on all 809 EBP2 files in the game, map scripts and event scripts
// alike, with the name matching the file name every time. Keying on it means:
//   * no mapId -> script join has to be guessed (that join is genuinely unproven for the Bhujerba
//     streets, and a wrong row would act on the wrong map);
//   * the gate is the same thing the census measured, so the table cannot drift from its evidence.
//
// THE ROWS ARE A MEASUREMENT, produced by `..\FFXII-Decompile\tools\ebp_shout_census.py` from the
// shipped bytecode, and that script carries its own falsifier: it re-derives seven meter variable
// indices that were measured independently first (a01 0x0D, a02 0x0E, a03 0x10, a07 0x0B, a11 0x0F,
// a18 0x32, b01 0x16) and ABORTS rather than emit a table if any one disagrees. It reproduced all
// seven. The same run found the four `byu_?01` stub modules contain no `setgaugecounter` call at
// all, so they are correctly absent here rather than silently assumed.
//
// WHY `guardNameIdx` IS -1 EVERYWHERE. The identity of the Imperials whose earshot costs the player
// 30 points is NOT derivable from these scripts: a full native census of byu_a01's shout code
// (911 instructions, 0x34800-0x35200) contains no `distance` native of either slot hypothesis, and
// the "how many heeded" weights come from variables set elsewhere rather than from npcdic ids
// pushed as immediates. The tester's own Bhujerba logs are post-minigame, so they cannot supply it
// either. Rather than invent an id or a radius, the row keeps -1 and the guard key reports the
// game's own NPC names with direction and distance; the first play pass on the minigame dumps a
// full npcdic census to the log, which is what turns this field into a measured value.
namespace ShoutTable {

struct Row {
    const char* srcName;       // the module's own authoring name, e.g. "byu_a01.src"
    uint8_t     meterVarIdx;   // script variable the gauge counter is driven from
    uint8_t     fillValue;     // the script's OWN success threshold (`v >= N`), never a mod constant
};

// The row whose `srcName` matches, or nullptr. `srcName` is compared as an exact ASCII string.
const Row* ForSrcName(const char* srcName);

// Is this npcdic id one of the soldiers whose earshot costs the player 30 points?
//
// MEASURED, and the set is COMPLETE: the whole npcdic contains exactly two "Sainikah" entries --
// 387 "Bhujerban Sainikah" and 1053 "Informed Sainikah" -- and they are the same soldier in two
// states. The play captures show the swap happening: 1052 Informed Citydweller, 1054 Informed
// Parijanah and 1077 Informed Wayfarer all appear beside their plain counterparts, because the
// minigame replaces an NPC with an "Informed" variant once they have heard the rumour. A guard who
// has heard you is still a guard, so both ids count.
//
// It is a shared set rather than a per-row field because every Bhujerba map draws from the one
// npcdic; a row cannot disagree with another about what a soldier is.
bool IsGuardName(int16_t nameIdx);

// HOW FAR A SHOUT CARRIES, in metres. Applies to civilians and guards alike -- one radius, because
// "who can hear me from here" is one question.
//
// **THIS IS THE TESTER'S MEASUREMENT FROM PLAY**, not a derivation: they put the guard's trigger at
// about 3 steps and noted it "might be a little smaller". The shipped value rounds that UP by one
// step, deliberately:
//   * THE GUARD PATROLS, so the distance when the key is pressed is not the distance when the shout
//     resolves, and the honest response to that jitter is margin;
//   * THE TWO ERRORS ARE NOT EQUAL. A radius slightly too large says "guard in earshot" when the
//     player was just safe -- they move, and lose nothing. A radius slightly too small says the
//     street is clear when it is not, and costs 30 points. Err large.
// It also sits just above the two distances at which civilians were seen to heed (4.12 m, 4.28 m),
// so the crowd count matches what the captures show actually happening.
//
// The captured bracket it has to live inside: a PENALTY at 0.88 m and a CLEAN at 19.64 m.
//
// ONE CONSTANT, TUNABLE IN ONE PLACE. If play says it is wide or narrow, this is the only line to
// change -- there is no per-map variation to keep in step, because every Bhujerba script runs the
// same sequence.
float EarshotRadius();

// Whether the guard identity is known at all. False would put the crowd key back to counting
// "people"; it is true today and this exists so that stays a decision the data makes.
bool HaveGuardIdentity();


// How many rows the table holds (diagnostics only).
int Count();

} // namespace ShoutTable

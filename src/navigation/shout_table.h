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
    int16_t     guardNameIdx;  // npcdic id of the Imperials; -1 = not measured (see the note above)
    // Earshot, in metres. 0 = NOT MEASURED, and while it is 0 the guard key reports distance and
    // bearing without a verdict -- an invented radius would be worse than no answer. Filled from the
    // bracket the shipped measurement produces: the largest distance at which a shout was PENALISED
    // and the smallest at which one was CLEAN (see shout_meter.cpp's burst capture).
    float       earshotRadius;
};

// The row whose `srcName` matches, or nullptr. `srcName` is compared as an exact ASCII string.
const Row* ForSrcName(const char* srcName);

// IDENTITY FALLBACK, and nothing else (S134). Is `mapId` one of Bhujerba's streets?
//
// The script-name gate above is the real one, and it is what the instant fill still requires,
// because only a module gives the meter variable's address. But S133's play log had the shout
// sequence's own dialogue and `script modules: [0]=- ... [4]=-` on the same map, so on that build
// the name never resolved and every feature stayed dark. This lets the SPOKEN METER and the two
// keys work from the gauge alone while that is being fixed, without letting them speak for some
// other gauge elsewhere in the game.
//
// The ids are the game's own planmapname area ids for Bhujerba, and map 804 is a MEASUREMENT --
// the tester ran the sequence there. This is a narrowing gate, never a behaviour table: it decides
// only whether a gauge already on screen may be called the infamy meter.
bool MapIsShoutStreet(int mapId);

// How many rows the table holds (diagnostics only).
int Count();

} // namespace ShoutTable

#pragma once

#include <cstdint>

// ONE-PASS CAPTURE for the Stilshrine of Miriam statue-rotation puzzle.
//
// The puzzle: three "Stone Brave" guardians, each in its own room, each rotated a quarter turn at a
// time by a dialogue offering only "Turn the statue clockwise" / "counterclockwise". Nothing on
// screen or in text says which way a statue now faces or whether it is right, so a blind player
// cannot solve it at all. `statue_guide` is the readout; this file is how its table gets measured.
//
// ---- WHAT IS ALREADY KNOWN, AND WHAT IS NOT ----------------------------------------------------
//
// MEASURED OFFLINE from the shipped bytecode (`..\FFXII-Decompile\tools\ebp_statue_census.py`,
// which carries its own falsifier and reproduces the routine-name pools our play log printed):
//   * exactly FOUR scripts in the whole game mention a statue -- three guardians and the big sword;
//   * map 600 Walk of Reason = `mrm_b04.src`, map 599 Walk of Prescience = `mrm_b03.src`,
//     map 598 Cold Distance (the sword) = `mrm_b02.src`, third guardian = `mrm_c01.src`;
//   * the authors' own routine names give the whole rotation model -- four rest facings
//     (north/south/east/west), four clockwise transitions and four counterclockwise ones, so a
//     "turn" is exactly 90 degrees; plus an "all directions" / "all directions NG" verdict that
//     appears in EVERY guardian script, and a sword-lift event that is the completion.
//
// NOT OBTAINABLE OFFLINE: which script VARIABLE holds a statue's facing, its storage class, and each
// statue's target. The map-script `.ebp` container is NOT the layout `notes/EBP2_DBG_format.md`
// documents for the four controller scripts -- `hdr+0x18` on a map script addresses the MESSAGE
// region, not a routine table -- so the descriptor table is not reachable from the file. It IS
// reachable at runtime, because the engine builds it: that is what this file reads.
//
// ---- WHY IT IS BUILT TO ANSWER EVERYTHING IN ONE VISIT -----------------------------------------
//
// A second capture costs a full game reload, so this does not go looking for one variable. It
// snapshots the module's ENTIRE variable table every field frame and reports the ones that MOVED
// across a statue interaction. A counter that steps once per quarter turn is unmistakable in that
// diff, and the descriptor that comes with it gives the storage class for free -- class 4 is the
// cross-script global array, which is what decides whether the readout can cover all three
// guardians from anywhere in the dungeon or only the one in the room.
//
// ---- IT IS DELIBERATELY NOT GATED ON THE STATUE TABLE ------------------------------------------
//
// It arms on any live module whose `.src` starts `mrm_`. Gating an instrument on the thing it
// diagnoses is how S133's shout meter went dark on a map where its sequence was running: an empty
// table would silence exactly the log this exists to produce.
//
// ---- AND THE HALF THAT MATTERS MOST: WHEN THE GAME ITSELF SAYS "CORRECT" -----------------------
//
// A variable diff alone would only show numbers moving. It could not say WHICH value is right, so
// the target would have to be inferred -- which is modelling the verdict instead of reading the word
// the verdict is read from (L-07).
//
// The script says it out loud. Its routines are NAMED, in the authors' own Japanese: `全方向`
// ("all directions") and `全方向NG` ("all directions NG") are the solved / not-solved verdict, and
// `北方向` / `東方向` / `南方向` / `西方向` name the facing a statue settles into. So a routine FIRE
// carries the answer as a label -- no decoding, no inference.
//
// `FUN_003dbb60` starts a routine on an object, and **`SneakAssist` already hooks it**, so `OnEventFire`
// taps that existing detour and costs ZERO new hooks -- which matters, because MinHook is at 66
// installs against a 63-trampoline block. Off this dungeon the tap is a single bool test.
//
// LOG-ONLY. Nothing here speaks, and nothing here writes game memory.
namespace StatueDiag {

// Is a Stilshrine of Miriam script live right now? Cheap -- five slot reads.
bool InDungeon();

// GAME THREAD, once per field tick, from `nav_hooks.cpp`'s field-frame hook. Costs a handful of
// pointer reads on every map in the game that is not this dungeon, and returns on the first branch.
void OnFieldFrame();

// GAME THREAD, from INSIDE `SneakAssist`'s existing `FUN_003dbb60` detour, before that file's own
// mechanism gate (which early-outs on every map but its two). Resolves the fired routine's name and
// logs it when it is one of the statue routines, so the log records the game's own verdict --
// "facing NORTH", "ALL DIRECTIONS", "SWORD LIFT" -- rather than a number we have to interpret.
//
// Returns immediately unless a Stilshrine script is live. It must NEVER decline a fire or change
// the return value: this is an observer on somebody else's hook.
void OnEventFire(void* object, uint32_t kind, uint32_t routineIdx);

// GAME THREAD, from the field-teardown hook. Drops the snapshot and the scene-object cache -- a
// module record and a variable table are only meaningful inside the map they were read on.
void OnMapTeardown();

} // namespace StatueDiag

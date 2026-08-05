#pragma once

#include "navigation/shout_script.h"

// THE SHOUT MINIGAME'S INSTRUMENTS. Log-only, always: nothing here ever speaks.
//
// WHY THEY EXIST. Two facts the mod needs are simply not in the game's data where an offline pass
// could reach them — the npcdic id of the Imperials whose earshot costs 30 points, and how far that
// earshot reaches. The map scripts settle neither (their shout code calls no `distance` native at
// all), and the tester's existing Bhujerba logs are all recorded after the sequence is over. So the
// answer has to come from ONE play pass with an instrument aboard, exactly as map 569's catch was
// settled — and until it does, `ShoutTable` keeps `-1` / `0` and the guard key makes no claim it
// cannot support.
namespace ShoutDiag {

// Called as each meter change settles, tagged CLEAN (a shout that landed) or PENALTY (an Imperial
// heard it). Logs the npcdic census at that instant, capped per map visit.
//
// Reading the result: the GUARD is the npcdic id that is close on every PENALTY and absent or far
// on every CLEAN; EARSHOT is bracketed above by the largest distance at which that id was present
// on a penalty and below by the smallest at which it was present on a clean shout.
void CaptureBurst(const ShoutScript::Module& mod, bool clean, int start, int end);

// The `'` probe's shout section: the live module, the gauge and its shown bit, both toggles, and
// the full npcdic census with distances.
void Dump();

// Clears the per-map capture budget.
void OnMapTeardown();

} // namespace ShoutDiag

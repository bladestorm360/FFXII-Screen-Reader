#pragma once

#include <cstdint>

// The MAP TABLE of scripted danger actors: which maps run a stealth minigame that punishes
// proximity, and who does the watching.
//
// WHAT THIS WAS, AND WHAT IT IS NOW (Session 108). It shipped in S106 as soft PENALTY ZONES for the
// route search -- discs around the guards, priced so A* would route wide of them. **That was
// REVERTED: it made map 568's Door 2 unroutable where it had worked.** The discs sat across the only
// corridor to the stair, so instead of taking a wider berth the search bought its way onto ground
// the party's class cannot stand on (`corridor paid terrain=8000` on every armed request) and the
// route died in validation. A penalty is only a detour when a detour EXISTS; in a one-corridor room
// it is just a bribe the search pays with the nearest wrong thing.
//
// The table itself survives because two things still need it, neither of which touches routing:
//   * SNEAK ASSIST (`sneak_assist.h`) uses it as the MAP WHITELIST -- F10 is inert on any map with
//     no row here;
//   * the capture-distance diagnostic below, which is log-only and measures what the game's own
//     script considers "too close" so the numbers come from play rather than from an estimate.
//
// The reverted routing code is in `git show 1a6b9dc` if a future session wants to revisit it -- but
// read the paragraph above first: the radius was not the only thing wrong with it.
namespace PathDanger {

// Does this map have a danger row at all? One linear scan of a tiny constexpr table; no allocation,
// no game reads, safe from any thread.
bool MapHasRow(uint32_t mapId);

// The npcdic name index of this map's watching actors, or -1 when the map has no row. Sneak assist
// resolves it to live scene objects so it can silence the guards' own trigger volume and nothing
// else on the map (S113).
int16_t DangerNameIdx(uint32_t mapId);

// Capture-distance diagnostic (log-only). Called once per game frame from the planner's field tick;
// does nothing unless the CURRENT map has a table row, so every other map pays one int compare.
// A field-tick gap > 600 ms is a scripted scene (a capture is one); when the tick resumes this logs
// the player's pre-gap position and the distances to the table's actors, so each real capture
// measures the game's own notice radius. One line per gap, tag DANGER.
void NoteFieldFrame();

} // namespace PathDanger

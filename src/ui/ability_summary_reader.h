#pragma once

// Reader for the party-member ABILITY SUMMARY overlay — the two pages the player toggles with `F`
// while the license board is up (the board stays visible behind it).
//
// Despite being reached from the license board, this is NOT part of the license module: it is a
// shared party-member detail overlay that the board simply forwards its pad input into
// (FUN_0055c740 -> FUN_002c1a80). The mode byte menuCtx+0xDE7 selects the page — 2 = Technicks /
// Mist / Remedy Lore / Espers, 1 or 3 = Magicks, 0 = closed.
//
// Two page controllers share ONE entry format (stride 0x20): name codec at +0x00 — into which the
// game writes its own "?" placeholder for an unlearned row, so decoding it reproduces the screen
// verbatim — description codec at +0x10 (null when the name is "?"), and flags at +0x18 whose bit
// 0x20000 means learned/bright (clear = greyed).
//
// Each page has a single focus/refresh routine that fires BOTH on open/rebuild and after every
// cursor move, so one hook per page covers entry and navigation. There is no FUN_00247510 0x8000
// on this screen, which is why the shared menu dispatch never saw it.
//
// CONTRACT: read-only, SEH-guarded memory reads; no game calls. Text is the game's own, decoded
// via GameText. No dedup — the only state is a section-change transition detector.
namespace AbilitySummaryReader {

bool Init();
void Shutdown();

} // namespace AbilitySummaryReader

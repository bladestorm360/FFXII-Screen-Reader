#pragma once

#include <string>

#include "speech/phrasebook.h"

// Shared formatting of mod-emitted numbers, so a convention the player has learned in one surface
// reads identically in the next.
namespace PhraseFormat {

// "<prefix><n> percent" — the mod's standing convention for a bar the game draws with no number
// behind it (enemy HP, the Bhujerba infamy gauge).
//
// TRUNCATING INTEGER DIVISION, widened to 64-bit so `cur * 100` cannot overflow. No rounding, no
// bucketing, no snapping: 99.6% reads as "99", which is what battle_target_reader has always said
// and what a player pacing a gauge needs — a value that only reaches 100 when the gauge is
// genuinely full.
//
// RETURNS AN EMPTY STRING when `max <= 0`. Callers must treat that as "say nothing": a gauge whose
// maximum could not be read has no percentage, and silence beats "0 percent" (CLAUDE.md's
// never-speak-filler rule). This is the same guard the two battle sites carried inline.
std::wstring Percent(Phrase::Id prefix, long long cur, long long max);

} // namespace PhraseFormat

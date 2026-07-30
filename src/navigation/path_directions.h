#pragma once

#include <string>
#include <vector>
#include "navigation/nav_types.h"

// Turns a world-space polyline (player -> ... -> target) into spoken turn-by-turn
// legs: consecutive same-cardinal segments merge into one leg. Engine-independent
// (uses nav_common's X/Z cardinal + step math). Used for A* route output and for
// describing any multi-point path; a 2-point polyline degrades to a single leg
// (i.e. crow-flies).
//
// Legs are EGOCENTRIC and spoken in EGOCENTRIC WORDS ("ahead 18, right 7") — relative to where an UP
// push currently sends the leader, which is the only frame a direction is actionable in for a
// camera-relative game. `facingRad` comes from PlayerState::ReadCameraForwardStable, which always
// yields a reference (live camera → last good camera → the leader's own facing): a distance with no
// direction is useless to walk on, so there is no no-direction path.
//
// **PASS THE RAW CELL PATH, NOT THE SMOOTHED CHORD.** A diagonal word is only ever emitted for a
// genuinely diagonal stretch — a fine alternation of treads whose net line really is 45 degrees. A
// route with shape (an L, a gradual bend) is spoken as its actual legs. See the long note in the
// .cpp: string-pulling an L across open ground into one "25 ahead-right" chord both sends the player
// off-route and makes any drift come back as a wildly different direction.
namespace PathDirections {

// Full route as egocentric turn-by-turn legs, e.g. L"ahead 18, right 7. 25 steps."
// Empty if < 2 points or the whole route rounds to 0 steps.
//
// `outLegPoints`, when given, receives the WORLD CORNER AT THE END OF EACH SPOKEN LEG — one point
// per leg, in order, with the last element being the route's destination. This is what the audio
// beacon walks: leg 1's corner is where the first spoken instruction runs out, and so on.
//
// It is an out-param on the existing function rather than a second entry point ON PURPOSE. The
// corners have to come from the same simplify/collapse/absorb pipeline that produced the words, or
// the beacon would be aiming at a corner the player was never told about. Two implementations of
// "where do the legs end" is exactly the kind of parallel path that drifts.
std::wstring Describe(const std::vector<FVec3>& rawPolyline, float facingRad,
                      std::vector<FVec3>* outLegPoints = nullptr);

// Just the immediate next leg, e.g. L"ahead 13 steps".
std::wstring NextInstruction(const std::vector<FVec3>& rawPolyline, float facingRad);

} // namespace PathDirections

#pragma once

#include <string>
#include <vector>
#include "navigation/nav_types.h"

// Turns a world-space polyline (player -> ... -> target) into spoken turn-by-turn
// legs: consecutive same-cardinal segments merge into one leg. Engine-independent
// (uses nav_common's X/Z cardinal + step math). Used for A* route output and for
// describing any multi-point path; a 2-point polyline degrades to a single leg
// (i.e. crow-flies). Legs are EGOCENTRIC — each cardinal is relabeled relative to the
// player's facing `facingRad` (the SAME NavCommon frame the `/` describe uses), so
// "North" = forward. Movement is camera-relative, so this is what "push UP" follows.
namespace PathDirections {

// Full route as EGOCENTRIC turn-by-turn legs, each a single 8-point cardinal relative to
// `facingRad`: e.g. L"North 18, Northeast 5. 23 steps." Empty if < 2 points.
std::wstring Describe(const std::vector<FVec3>& polyline, float facingRad);

// Just the immediate next leg (first same-cardinal run), e.g. L"North 13 steps".
std::wstring NextInstruction(const std::vector<FVec3>& polyline, float facingRad);

} // namespace PathDirections

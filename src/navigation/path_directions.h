#pragma once

#include <string>
#include <vector>
#include "navigation/nav_types.h"

// Turns a world-space polyline (player -> ... -> target) into spoken turn-by-turn
// legs: consecutive same-cardinal segments merge into one leg. Engine-independent
// (uses nav_common's X/Z cardinal + step math). Used for A* route output and for
// describing any multi-point path; a 2-point polyline degrades to a single leg
// (i.e. crow-flies).
namespace PathDirections {

// Full route, e.g. L"South 13, West 14. 27 steps." Empty if < 2 points.
std::wstring Describe(const std::vector<FVec3>& polyline);

// Just the immediate next leg (first same-cardinal run), e.g. L"South 13 steps".
std::wstring NextInstruction(const std::vector<FVec3>& polyline);

} // namespace PathDirections

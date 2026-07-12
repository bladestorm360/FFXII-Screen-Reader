#include "navigation/path_directions.h"
#include "navigation/nav_common.h"

#include <cwchar>
#include <cmath>

namespace PathDirections {

// `poly` is the smoothed route (line-of-sight waypoints, not per-cell). Each leg is a
// straight segment, decomposed into its primary + secondary CARDINAL components — e.g.
// "North 16, East 2", never a lone "Northeast 18" (whose 45deg displacement is wrong).
// Larger axis first; a component that rounds to 0 steps is dropped.
std::wstring Describe(const std::vector<FVec3>& poly) {
    if (poly.size() < 2) return L"";

    std::wstring out;
    int total = 0;

    auto emit = [&](const wchar_t* dir, int steps) {
        if (steps == 0) return;
        total += steps;
        if (!out.empty()) out += L", ";
        out += dir;
        out += L" ";
        out += std::to_wstring(steps);
    };

    for (size_t i = 1; i < poly.size(); ++i) {
        const float dx = poly[i].x - poly[i - 1].x;
        const float dz = poly[i].z - poly[i - 1].z;
        const int stepsNS = NavCommon::DistanceToSteps(std::fabs(dz));
        const int stepsEW = NavCommon::DistanceToSteps(std::fabs(dx));
        if (std::fabs(dz) >= std::fabs(dx)) {          // dominant axis first
            emit(NavCommon::NorthSouthWord(dz), stepsNS);
            emit(NavCommon::EastWestWord(dx),  stepsEW);
        } else {
            emit(NavCommon::EastWestWord(dx),  stepsEW);
            emit(NavCommon::NorthSouthWord(dz), stepsNS);
        }
    }

    if (out.empty()) return L"";                       // whole route < half a step
    out += L". ";
    out += std::to_wstring(total);
    out += L" steps";
    return out;
}

std::wstring NextInstruction(const std::vector<FVec3>& poly) {
    if (poly.size() < 2) return L"";
    const wchar_t* d0 = NavCommon::CardinalBearing(poly[0], poly[1]);
    float dist = 0.0f;
    for (size_t i = 1; i < poly.size(); ++i) {
        const wchar_t* d = NavCommon::CardinalBearing(poly[i - 1], poly[i]);
        if (wcscmp(d, d0) != 0) break;
        dist += NavCommon::Distance2D(poly[i - 1], poly[i]);
    }
    std::wstring s = d0;
    s += L" ";
    s += std::to_wstring(NavCommon::DistanceToSteps(dist));
    s += L" steps";
    return s;
}

} // namespace PathDirections

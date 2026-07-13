#include "navigation/path_directions.h"
#include "navigation/nav_common.h"

#include <cwchar>

namespace PathDirections {

// `poly` is the smoothed route (line-of-sight waypoints). Each leg is one straight segment,
// spoken as a single EGOCENTRIC 8-point cardinal + step count via NavCommon::CardinalBearingRelative
// (relabeled to `facingRad`, the SAME frame the `/` describe uses) — so "North" = forward and `\`
// legs follow "push UP". Legs that round to 0 steps are dropped.
std::wstring Describe(const std::vector<FVec3>& poly, float facingRad) {
    if (poly.size() < 2) return L"";

    std::wstring out;
    int total = 0;
    for (size_t i = 1; i < poly.size(); ++i) {
        const int steps = NavCommon::DistanceToSteps(NavCommon::Distance2D(poly[i - 1], poly[i]));
        if (steps == 0) continue;
        total += steps;
        if (!out.empty()) out += L", ";
        out += NavCommon::CardinalBearingRelative(poly[i - 1], poly[i], facingRad);
        out += L" ";
        out += std::to_wstring(steps);
    }

    if (out.empty()) return L"";                        // whole route < half a step
    out += L". ";
    out += std::to_wstring(total);
    out += L" steps";
    return out;
}

std::wstring NextInstruction(const std::vector<FVec3>& poly, float facingRad) {
    if (poly.size() < 2) return L"";
    const wchar_t* d0 = NavCommon::CardinalBearingRelative(poly[0], poly[1], facingRad);
    float dist = 0.0f;
    for (size_t i = 1; i < poly.size(); ++i) {
        const wchar_t* d = NavCommon::CardinalBearingRelative(poly[i - 1], poly[i], facingRad);
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

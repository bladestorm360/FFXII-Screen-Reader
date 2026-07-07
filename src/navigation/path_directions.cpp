#include "navigation/path_directions.h"
#include "navigation/nav_common.h"

#include <cwchar>

namespace PathDirections {

std::wstring Describe(const std::vector<FVec3>& poly) {
    if (poly.size() < 2) return L"";

    std::wstring out;
    int total = 0;
    const wchar_t* curDir = nullptr;
    float curDist = 0.0f;

    auto flush = [&]() {
        if (!curDir) return;
        int steps = NavCommon::DistanceToSteps(curDist);
        total += steps;
        if (!out.empty()) out += L", ";
        out += curDir;
        out += L" ";
        out += std::to_wstring(steps);
    };

    for (size_t i = 1; i < poly.size(); ++i) {
        const wchar_t* d = NavCommon::CardinalBearing(poly[i - 1], poly[i]);
        float dist = NavCommon::Distance2D(poly[i - 1], poly[i]);
        if (curDir && wcscmp(d, curDir) == 0) {
            curDist += dist;
        } else {
            flush();
            curDir = d;
            curDist = dist;
        }
    }
    flush();

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

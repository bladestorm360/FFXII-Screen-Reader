#include "navigation/path_directions.h"
#include "navigation/nav_common.h"
#include "navigation/nav_grid.h"

#include <cmath>
#include <utility>
#include <cwchar>
#include <string>
#include <vector>

namespace PathDirections {

namespace {

// ---- THE DIAGONAL RULE ------------------------------------------------------------------------
// A DIAGONAL word ("Northeast", "Southwest", …) may only ever describe a stretch that is GENUINELY
// diagonal — a fine alternation like 1 North, 1 East, 1 North, 1 East … whose net line really is 45
// degrees. A route with any SHAPE to it — an L, or a gradual bend — must be spoken as its actual
// legs, never averaged into one diagonal. (Words are compass words on the RELATIVE frame: "North"
// means forward, "East" right. See nav_common.h.)
//
// WHY (reported in play): the planner's string-pull collapses a cell path to line-of-sight
// waypoints, so an L-shaped route across open ground — 18 North then 7 West — becomes a single
// chord and was announced as "30 Northwest". Two things go wrong. The player is sent along a line
// the route never takes; and the moment they drift off that imaginary diagonal (say they walk 10
// West early), they are past the North leg, the re-plan comes back with a completely different word
// — Northeast — and it reads as the directions flipping around. Speaking "18 North, 7 West" keeps the
// instruction on the route the planner actually found, so drifting produces a small correction
// instead of an inversion.
//
// So legs are built from the RAW cell path, not the smoothed chord: the smoothed polyline is a
// geometric shortcut, and its shape is exactly the information this needs to preserve.
//
// ...but the raw path is not usable as-is either. A* on a 1.5 m grid wanders, and speaking every run
// gave a 19-leg readout in play ("North 6, Northwest 6, North 4, Northwest 8, West 10, …") — no
// better than the single wrong diagonal. BOTH extremes are wrong, and the fix is the one the tester
// named: make the smoothing ACCURATE rather than greedy.
//
// So the path is first simplified with RAMER-DOUGLAS-PEUCKER at a tolerance of one grid cell. RDP
// keeps any point that deviates from its chord by more than the tolerance, which is exactly the
// property needed: a real corner (an L) deviates hugely and SURVIVES, while grid jitter along a
// straight or diagonal run deviates by less than half a cell and collapses. Unlike the planner's
// string-pull it never crosses a corner, because it is bounded by deviation, not by line-of-sight.

// A run of consecutive raw segments sharing one octant.
struct Run {
    int    octant = 0;      // NavCommon relative octant: EVEN = cardinal, ODD = diagonal
    float  dist   = 0.0f;   // metres along the path
    size_t first  = 0;      // index into the polyline of this run's first point
    size_t last   = 0;      // …and its last, so a collapsed stretch can use the true chord
};

// RDP tolerance: one routing cell. Below this a deviation is grid jitter, not a turn.
constexpr float kSimplifyTol = NavGrid::kFineCell;
// A run this short is a staircase tread, not a leg the player should be told to walk.
constexpr float kTreadMax = NavGrid::kFineCell * 1.6f;
// Fewer alternations than this is a corner, not a staircase.
constexpr int   kMinTreads = 3;
// A leg under this many steps is absorbed into its neighbour rather than announced.
constexpr int   kMinLegSteps = 2;
// Legs spoken before the rest is summarised. A turn-by-turn you cannot hold in your head is no more
// use than a wrong one; press the route key again as you go and the next legs are re-spoken.
constexpr size_t kMaxSpokenLegs = 5;

// Perpendicular distance from `p` to the segment a->b, on the ground plane.
float PerpDist(const FVec3& p, const FVec3& a, const FVec3& b) {
    const float vx = b.x - a.x, vz = b.z - a.z;
    const float wx = p.x - a.x, wz = p.z - a.z;
    const float len2 = vx * vx + vz * vz;
    if (len2 <= 1e-6f) return std::sqrt(wx * wx + wz * wz);
    float t = (wx * vx + wz * vz) / len2;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float dx = wx - t * vx, dz = wz - t * vz;
    return std::sqrt(dx * dx + dz * dz);
}

// Ramer-Douglas-Peucker, iterative (no recursion on a game thread). Keeps every point whose
// deviation from its chord exceeds `tol`, so real corners survive and grid jitter does not.
std::vector<FVec3> Simplify(const std::vector<FVec3>& in, float tol) {
    if (in.size() < 3) return in;
    std::vector<bool> keep(in.size(), false);
    keep.front() = keep.back() = true;
    std::vector<std::pair<size_t, size_t>> stack{ { 0, in.size() - 1 } };
    while (!stack.empty()) {
        const auto [lo, hi] = stack.back();
        stack.pop_back();
        if (hi <= lo + 1) continue;
        float worst = 0.0f;
        size_t at = lo;
        for (size_t i = lo + 1; i < hi; ++i) {
            const float d = PerpDist(in[i], in[lo], in[hi]);
            if (d > worst) { worst = d; at = i; }
        }
        if (worst <= tol) continue;
        keep[at] = true;
        stack.push_back({ lo, at });
        stack.push_back({ at, hi });
    }
    std::vector<FVec3> out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) if (keep[i]) out.push_back(in[i]);
    return out;
}

bool IsCardinal(int octant) { return (octant & 1) == 0; }

// Are two cardinal octants adjacent on the compass rose (ahead & right, right & behind, …)?
bool AdjacentCardinals(int a, int b) {
    if (!IsCardinal(a) || !IsCardinal(b) || a == b) return false;
    const int d = ((a - b) % 8 + 8) % 8;
    return d == 2 || d == 6;
}

// The diagonal octant lying between two adjacent cardinals.
int DiagonalBetween(int a, int b) {
    if (((a + 1) % 8) == ((b + 7) % 8)) return (a + 1) % 8;   // b is a+2
    return (b + 1) % 8;                                        // a is b+2
}

std::vector<Run> BuildRuns(const std::vector<FVec3>& poly, float facingRad) {
    std::vector<Run> runs;
    for (size_t i = 1; i < poly.size(); ++i) {
        const float d = NavCommon::Distance2D(poly[i - 1], poly[i]);
        if (d <= 0.0f) continue;
        const int oct = NavCommon::RelativeOctant(poly[i - 1], poly[i], facingRad);
        if (!runs.empty() && runs.back().octant == oct) {
            runs.back().dist += d;
            runs.back().last = i;
        } else {
            runs.push_back(Run{ oct, d, i - 1, i });
        }
    }
    return runs;
}

// Collapse maximal staircases — >= kMinTreads consecutive short runs alternating between two
// ADJACENT CARDINALS — into one diagonal leg whose distance is the true chord across the stretch
// (a 10-tread staircase covers ~14 steps of ground, not the 20 walked along the treads).
std::vector<Run> CollapseStaircases(const std::vector<Run>& runs, const std::vector<FVec3>& poly) {
    std::vector<Run> out;
    size_t i = 0;
    while (i < runs.size()) {
        size_t j = i;
        if (IsCardinal(runs[i].octant) && runs[i].dist <= kTreadMax) {
            while (j + 1 < runs.size() &&
                   runs[j + 1].dist <= kTreadMax &&
                   AdjacentCardinals(runs[j].octant, runs[j + 1].octant) &&
                   (j == i || runs[j + 1].octant == runs[j - 1].octant)) {
                ++j;
            }
        }
        if (j - i + 1 >= static_cast<size_t>(kMinTreads)) {
            Run merged;
            merged.octant = DiagonalBetween(runs[i].octant, runs[i + 1].octant);
            merged.first  = runs[i].first;
            merged.last   = runs[j].last;
            merged.dist   = NavCommon::Distance2D(poly[merged.first], poly[merged.last]);
            out.push_back(merged);
            i = j + 1;
        } else {
            out.push_back(runs[i]);
            ++i;
        }
    }
    return out;
}

struct Leg {
    const wchar_t* word  = nullptr;
    int            steps = 0;
};

std::vector<Leg> BuildLegs(const std::vector<FVec3>& rawPoly, float facingRad) {
    // Accurate smoothing FIRST (see the header note): corners survive, grid jitter goes.
    const std::vector<FVec3> poly = Simplify(rawPoly, kSimplifyTol);
    std::vector<Run> runs = CollapseStaircases(BuildRuns(poly, facingRad), poly);

    std::vector<Leg> legs;
    for (const Run& r : runs) {
        const int steps = NavCommon::DistanceToSteps(r.dist);
        if (steps == 0) continue;
        const wchar_t* w = NavCommon::RelativeWord(r.octant);
        if (!legs.empty() && wcscmp(legs.back().word, w) == 0) legs.back().steps += steps;
        else                                                   legs.push_back(Leg{ w, steps });
    }
    // Absorb a sub-kMinLegSteps leg into a neighbour. Its STEPS are added, never dropped, so the
    // total stays honest; only the spurious extra instruction goes away. This never invents a
    // diagonal — it only lengthens a leg that already exists.
    bool changed = true;
    while (changed && legs.size() > 1) {
        changed = false;
        for (size_t i = 0; i < legs.size(); ++i) {
            if (legs[i].steps >= kMinLegSteps) continue;
            const size_t into = (i == 0) ? 1 : i - 1;
            legs[into].steps += legs[i].steps;
            legs.erase(legs.begin() + static_cast<long>(i));
            changed = true;
            break;
        }
        if (changed) continue;
        for (size_t i = 1; i < legs.size(); ++i) {
            if (wcscmp(legs[i - 1].word, legs[i].word) != 0) continue;
            legs[i - 1].steps += legs[i].steps;
            legs.erase(legs.begin() + static_cast<long>(i));
            changed = true;
            break;
        }
    }
    return legs;
}

} // namespace

// `poly` is the RAW cell path (see the diagonal-rule note above — do NOT pass the smoothed chord).
// Each leg is one compass word + step count in the relative frame, the same frame and vocabulary the
// `/` describe uses, so a leg and a crow-flies bearing to the same point always agree.
std::wstring Describe(const std::vector<FVec3>& poly, float facingRad) {
    if (poly.size() < 2) return L"";
    std::vector<Leg> legs = BuildLegs(poly, facingRad);
    if (legs.empty()) return L"";                   // whole route < half a step

    int total = 0;
    for (const Leg& l : legs) total += l.steps;      // total == what we actually told them to walk

    // Speak at most kMaxSpokenLegs, then summarise the remainder. A 19-leg readout (measured in
    // play before the simplify pass) cannot be held in your head, so the tail is more useful as a
    // distance: walk the legs you were given and press the route key again.
    std::wstring out;
    int spoken = 0;
    for (size_t i = 0; i < legs.size() && i < kMaxSpokenLegs; ++i) {
        if (!out.empty()) out += L", ";
        out += legs[i].word;
        out += L" ";
        out += std::to_wstring(legs[i].steps);
        spoken += legs[i].steps;
    }
    if (legs.size() > kMaxSpokenLegs) {
        out += L", then ";
        out += std::to_wstring(total - spoken);
        out += L" more";
    }
    out += L". ";
    out += std::to_wstring(total);
    out += L" steps";
    return out;
}

std::wstring NextInstruction(const std::vector<FVec3>& poly, float facingRad) {
    if (poly.size() < 2) return L"";
    std::vector<Leg> legs = BuildLegs(poly, facingRad);
    if (legs.empty()) return L"";
    std::wstring s = legs.front().word;
    s += L" ";
    s += std::to_wstring(legs.front().steps);
    s += L" steps";
    return s;
}

} // namespace PathDirections

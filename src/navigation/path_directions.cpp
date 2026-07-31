#include "navigation/path_directions.h"
#include "navigation/nav_common.h"
#include "core/logger.h"
#include "speech/phrasebook.h"


#include <cmath>
#include <cstdio>
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
// Route-simplification tolerance. Was NavGrid::kFineCell back when the route was a staircase of
// 1.5 m cells; the polyline is now portal midpoints on the game's own navmesh, so this is simply
// the smallest deviation worth keeping as a separate leg.
constexpr float kSimplifyTol = 1.5f;
// A run this short is a staircase tread, not a leg the player should be told to walk.
constexpr float kTreadMax = 1.5f * 1.6f;
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

// AN OCTANT BUCKET IS NOT A DIRECTION (Session 96). `RelativeOctant` snaps to 45-degree buckets, so
// two consecutive segments at +22.4 and -22.4 degrees both read as "North" and used to be summed into
// ONE spoken leg -- a 44.8-degree bend delivered as a single heading, and the merge could repeat
// without bound. The player holds the heading they were given, the route does not, and on a taut path
// that runs one body radius from the wall the difference is a hard stop.
//
// Measured: a leg spoken as "North" was 3.55 degrees off, the player's Z never changed across 31.75 m
// of walking (a wall-slide signature), and they jammed 18.35 m along it -- 18.35 * tan(3.55) = 1.14 m,
// which is exactly the drift that put them into the wall.
//
// So a run now extends only while the new segment stays within kRunSpreadDeg of the bearing the run
// STARTED with. The words are unchanged -- this only stops one word covering a path that bends.
constexpr float kRunSpreadDeg = 12.0f;

float AngleDiffDeg(float a, float b) {
    float d = a - b;
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return std::fabs(d);
}

std::vector<Run> BuildRuns(const std::vector<FVec3>& poly, float facingRad) {
    std::vector<Run> runs;
    std::vector<float> runBearing;               // the bearing each run opened with
    for (size_t i = 1; i < poly.size(); ++i) {
        const float d = NavCommon::Distance2D(poly[i - 1], poly[i]);
        if (d <= 0.0f) continue;
        const int   oct = NavCommon::RelativeOctant(poly[i - 1], poly[i], facingRad);
        const float deg = NavCommon::RelativeBearingDeg(poly[i - 1], poly[i], facingRad);
        if (!runs.empty() && runs.back().octant == oct &&
            AngleDiffDeg(deg, runBearing.back()) <= kRunSpreadDeg) {
            runs.back().dist += d;
            runs.back().last = i;
        } else {
            runs.push_back(Run{ oct, d, i - 1, i });
            runBearing.push_back(deg);
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
    // Index into the SIMPLIFIED poly of the corner this leg ends at. Carried all the way through
    // the merge and absorb passes below so the audio beacon can aim at the same corners the words
    // describe. Runs already track this (Run::last); it used to be dropped at the Run -> Leg
    // boundary, which is why nothing downstream could ever point at a leg.
    size_t         endIdx = 0;
};

std::vector<Leg> BuildLegs(const std::vector<FVec3>& rawPoly, float facingRad,
                           std::vector<FVec3>* outLegPoints) {
    // Accurate smoothing FIRST (see the header note): corners survive, grid jitter goes.
    const std::vector<FVec3> poly = Simplify(rawPoly, kSimplifyTol);
    std::vector<Run> runs = CollapseStaircases(BuildRuns(poly, facingRad), poly);

    std::vector<Leg> legs;
    for (const Run& r : runs) {
        const int steps = NavCommon::DistanceToSteps(r.dist);
        if (steps == 0) continue;
        const wchar_t* w = NavCommon::RelativeWord(r.octant);
        // A merge always extends FORWARD along the route, so the survivor takes the later corner.
        if (!legs.empty() && wcscmp(legs.back().word, w) == 0) {
            legs.back().steps += steps;
            legs.back().endIdx = r.last;
        } else {
            legs.push_back(Leg{ w, steps, r.last });
        }
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
            // ONLY CARRY THE STEPS WHEN THE DIRECTION MATCHES (Session 96). This used to add them
            // unconditionally "so the total stays honest" -- but that is the wrong kind of honesty:
            // it kept the distance right while making the DIRECTION wrong, so "North 117, West 1"
            // was spoken as "North 118" and a metre of westward travel was described as northward.
            // A player walking a heading needs the heading to be true; a sub-two-step jog is under
            // the resolution of the instruction anyway and is better dropped than misattributed.
            if (wcscmp(legs[into].word, legs[i].word) == 0) legs[into].steps += legs[i].steps;
            // The survivor now covers both stretches, so it ends at whichever corner is later along
            // the route. Absorbing forward (i into i-1) extends it; absorbing leg 0 into leg 1
            // leaves leg 1's own corner, which already sits further on.
            if (legs[i].endIdx > legs[into].endIdx) legs[into].endIdx = legs[i].endIdx;
            legs.erase(legs.begin() + static_cast<long>(i));
            changed = true;
            break;
        }
        if (changed) continue;
        for (size_t i = 1; i < legs.size(); ++i) {
            if (wcscmp(legs[i - 1].word, legs[i].word) != 0) continue;
            legs[i - 1].steps += legs[i].steps;
            legs[i - 1].endIdx = legs[i].endIdx;
            legs.erase(legs.begin() + static_cast<long>(i));
            changed = true;
            break;
        }
    }

    if (outLegPoints) {
        outLegPoints->clear();
        outLegPoints->reserve(legs.size());
        for (const Leg& l : legs)
            outLegPoints->push_back(poly[l.endIdx < poly.size() ? l.endIdx : poly.size() - 1]);
        // The last beacon point is ALWAYS the destination. A trailing run that rounded to zero
        // steps produces no leg, so the final leg's own corner can stop short of the goal -- and a
        // beacon that switches off a few metres early is worse than one that is a step long.
        // path_search puts the exact target last in the polyline, so this is the real destination,
        // not an extrapolation.
        if (!outLegPoints->empty()) outLegPoints->back() = poly.back();
    }
    return legs;
}

} // namespace

// `poly` is the RAW cell path (see the diagonal-rule note above — do NOT pass the smoothed chord).
// Each leg is one compass word + step count in the relative frame, the same frame and vocabulary the
// `/` describe uses, so a leg and a crow-flies bearing to the same point always agree.
std::wstring Describe(const std::vector<FVec3>& poly, float facingRad,
                      std::vector<FVec3>* outLegPoints) {
    if (outLegPoints) outLegPoints->clear();
    if (poly.size() < 2) return L"";
    std::vector<Leg> legs = BuildLegs(poly, facingRad, outLegPoints);
    if (legs.empty()) {                             // whole route < half a step
        if (outLegPoints) outLegPoints->clear();
        return L"";
    }

    // REVERSAL INVARIANT (log-only). A route must never send the player one way and then straight
    // back; if it does, the polyline is wrong, not the wording.
    //
    // NOT a Session 77 regression, as this comment originally claimed. Log forensics across the whole
    // archive found **109 reversals in 1,059 routes, 86 of them in the OLD GRID era** -- and every
    // grid-era one came from `pass=strict`, a fully completed search with up to 601 expansions, not
    // from any recovery pass. The grid's were also far worse: up to 20 wasted steps
    // ("South 24, Northwest 20, North 22"), against never more than 6 on the navmesh.
    //
    // Two bugs wearing one name: the grid produced route-scale detours, the navmesh produces
    // leg-0 stubs on a waypoint the player has walked past. This detector postdates every archived
    // log, so none of that history was ever caught by it. It stays.
    //
    // Legs are octants 45 degrees apart, so a circular index gap of 3 or more is >= 135 degrees.
    for (size_t i = 1; i < legs.size(); ++i) {
        int a = -1, b = -1;
        for (int o = 0; o < 8; ++o) {
            if (legs[i - 1].word == NavCommon::RelativeWord(o)) a = o;
            if (legs[i].word     == NavCommon::RelativeWord(o)) b = o;
        }
        if (a < 0 || b < 0) continue;
        int d = a > b ? a - b : b - a;
        if (d > 4) d = 8 - d;
        if (d >= 3) {
            char m[176];
            snprintf(m, sizeof(m),
                     "REVERSAL: leg %zu -> %zu turns %d deg (%d steps then %d steps) -- the polyline "
                     "doubles back; the route geometry is wrong, not the wording",
                     i - 1, i, d * 45, legs[i - 1].steps, legs[i].steps);
            Log::Write("NAV-ROUTE", m);
        }
    }

    // Speak at most kMaxSpokenLegs, then say HOW MANY LEGS REMAIN. A 19-leg readout (measured in
    // play before the simplify pass) cannot be held in your head, so the tail is a count of what is
    // left to hear: walk the legs you were given and press the route key again.
    //
    // THE REMAINDER COUNTS LEGS, AND THE TOTAL IS GONE (Session 103, both from the tester).
    //
    // It used to read "North 19, East 5, Southeast 22, South 28, Southeast 20, then 225 more. 319
    // steps" -- where 225 was the leftover STEP count and 319 the route total. **Every number before
    // it in that sentence is glued to a direction word**, so "225 more" reads as 225 more of the
    // things being listed, i.e. legs; the unit only arrives in the next clause, and 225 against 319
    // does not reconcile unless you were summing the spoken legs as they went past. The tester read
    // it as "the pathfinder found 300+ legs" -- a fair reading of the sentence, and the pathfinder
    // was blameless (that route was 19 legs, 239 m, 34% over the straight line).
    //
    // So the remainder is now the count of UNSPOKEN LEGS, which is what the position in the sentence
    // was already promising, and the route total is dropped entirely: per-leg counts are what you
    // act on, and the total was the least actionable number in the line while landing last, where it
    // sticks. No new phrasebook string -- `ThenJoiner` and `MoreSuffix` are reused verbatim.
    //
    // `StepsSuffix` is deliberately NOT used here any more. It is still live in nav_common's
    // crow-flies phrases and in NextInstruction below; do not delete it.
    std::wstring out;
    for (size_t i = 0; i < legs.size() && i < kMaxSpokenLegs; ++i) {
        if (!out.empty()) out += L", ";
        out += legs[i].word;
        out += L" ";
        out += std::to_wstring(legs[i].steps);
    }
    if (legs.size() > kMaxSpokenLegs) {
        out += Phrase::Get(Phrase::Id::ThenJoiner);
        out += std::to_wstring(legs.size() - kMaxSpokenLegs);
        out += Phrase::Get(Phrase::Id::MoreSuffix);
    }
    out += L".";
    return out;
}

std::wstring NextInstruction(const std::vector<FVec3>& poly, float facingRad) {
    if (poly.size() < 2) return L"";
    std::vector<Leg> legs = BuildLegs(poly, facingRad, nullptr);
    if (legs.empty()) return L"";
    std::wstring s = legs.front().word;
    s += L" ";
    s += std::to_wstring(legs.front().steps);
    s += Phrase::Get(Phrase::Id::StepsSuffix);
    return s;
}

} // namespace PathDirections

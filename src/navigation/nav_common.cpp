#include "navigation/nav_common.h"

#include "speech/phrasebook.h"

#include <cmath>

namespace NavCommon {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr float kDegToRad = kPi / 180.0f;

// World units per spoken "step". FFXII's field world is in METERS (offline: Bullet gravity
// -10, char capsule height 0.8 + radius 0.6 ~= a 2 m humanoid). ~0.75 m stride => 1 step.
float g_unitsPerStep = 0.75f;

// Cardinal labels, index 0 = North, clockwise. FFXII world north = -Z, so 0deg = -Z ("north"),
// +90deg = +X ("east").
const Phrase::Id kCardinalId[8] = {
    Phrase::Id::North, Phrase::Id::Northeast, Phrase::Id::East,  Phrase::Id::Southeast,
    Phrase::Id::South, Phrase::Id::Southwest, Phrase::Id::West,  Phrase::Id::Northwest,
};
const wchar_t* Cardinal(int octant) { return Phrase::Get(kCardinalId[octant]); }

// EGOCENTRIC labels — the ALTERNATE vocabulary, RETAINED but NOT SHIPPED. Same octant indexing as
// kCardinal; only the words differ.
//
// The SHIPPED vocabulary is kCardinal, applied to the RELATIVE frame: "North" means forward (wherever
// forward currently points), "East" means right, "South" behind, "West" left. That is the tester's
// explicit preference -- "most prefer north/south/east/west directions ... essentially north=forward
// whether it's true north or not". A previous build swapped these words in on the theory that
// compass words on a relative frame were what made directions confusing; that theory was WRONG (the
// real cause was the route smoother inventing diagonals -- see path_directions.cpp), and it was
// reverted. Kept here for anyone who does want a literal ego frame, with "forward"/"backward" rather
// than "ahead"/"behind" per the same instruction.
const Phrase::Id kEgocentricId[8] = {
    Phrase::Id::Forward,  Phrase::Id::ForwardRight,  Phrase::Id::Right, Phrase::Id::BackwardRight,
    Phrase::Id::Backward, Phrase::Id::BackwardLeft,  Phrase::Id::Left,  Phrase::Id::ForwardLeft,
};
const wchar_t* Ego(int octant) { return Phrase::Get(kEgocentricId[octant]); }

// Bearing in degrees [0,360): 0 = -Z (game north), increasing toward +X (east).
float BearingDeg(const FVec3& from, const FVec3& to) {
    float dx = to.x - from.x;
    float dz = to.z - from.z;
    float deg = std::atan2(dx, -dz) * kRadToDeg;
    if (deg < 0.0f) deg += 360.0f;
    return deg;
}

int OctantOf(float deg) {
    int idx = static_cast<int>(std::floor(deg / 45.0f + 0.5f));
    return ((idx % 8) + 8) % 8;
}

float Norm360(float deg) {
    deg = std::fmod(deg, 360.0f);
    if (deg < 0.0f) deg += 360.0f;
    return deg;
}

// The leader `faceNode` yaw is in the game's `atan2(worldMoveX, worldMoveZ)` convention (0 = +Z);
// our BearingDeg is `atan2(dx, -dz)` (0 = north = -Z). For the SAME direction vector the two
// differ by a reflection: compass = 180 - faceNode. So this returns the facing as a compass
// bearing in [0,360). Subtracting it from a world BearingDeg rotates the frame so "forward" = 0.
float CompassFaceDeg(float facingRad) {
    return Norm360(180.0f - facingRad * kRadToDeg);
}

} // namespace

float Distance2D(const FVec3& a, const FVec3& b) {
    float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

float Distance3D(const FVec3& a, const FVec3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

const wchar_t* CardinalBearing(const FVec3& from, const FVec3& to) {
    return Cardinal(OctantOf(BearingDeg(from, to)));
}

const wchar_t* CardinalOfHeading(float headingRad) {
    float deg = headingRad * kRadToDeg;
    deg = std::fmod(deg, 360.0f);
    if (deg < 0.0f) deg += 360.0f;
    return Cardinal(OctantOf(deg));
}

// THE relative bearing. Everything relative -- the spoken octant word and the beacon's pan --
// is a rendering of this one number; see the header for the Session 92 mirror bug that came from
// a second caller deriving its own version of it.
float RelativeBearingDeg(const FVec3& from, const FVec3& to, float facingRad) {
    return Norm360(BearingDeg(from, to) - CompassFaceDeg(facingRad));
}

void BearingToPan(const FVec3& from, const FVec3& to, float facingRad,
                  float& outPan, float& outFront) {
    const float dx = to.x - from.x;
    const float dz = to.z - from.z;
    if (std::fabs(dx) < 1e-4f && std::fabs(dz) < 1e-4f) { outPan = 0.0f; outFront = 1.0f; return; }
    // 0 deg = forward, 90 = right, 180 = behind, 270 = left -> sin is the L/R axis, cos front/back.
    const float rel = RelativeBearingDeg(from, to, facingRad) * kDegToRad;
    outPan   = std::sin(rel);
    outFront = std::cos(rel);
}

const wchar_t* CardinalBearingRelative(const FVec3& from, const FVec3& to, float facingRad) {
    return Cardinal(OctantOf(RelativeBearingDeg(from, to, facingRad)));
}

const wchar_t* CardinalOfHeadingRelative(float headingRad, float facingRad) {
    const float ego = Norm360(headingRad * kRadToDeg - CompassFaceDeg(facingRad));
    return Cardinal(OctantOf(ego));
}

const wchar_t* EgoBearing(const FVec3& from, const FVec3& to, float facingRad) {
    return Ego(OctantOf(RelativeBearingDeg(from, to, facingRad)));
}

const wchar_t* EgoOfHeading(float headingRad, float facingRad) {
    const float ego = Norm360(headingRad * kRadToDeg - CompassFaceDeg(facingRad));
    return Ego(OctantOf(ego));
}

int RelativeOctant(const FVec3& from, const FVec3& to, float facingRad) {
    return OctantOf(RelativeBearingDeg(from, to, facingRad));
}

// SHIPPED vocabulary: compass words on the relative frame (North == forward).
const wchar_t* RelativeWord(int octant) {
    return Cardinal(((octant % 8) + 8) % 8);
}

// Alternate vocabulary, retained and unused.
const wchar_t* EgoWordOfOctant(int octant) {
    return Ego(((octant % 8) + 8) % 8);
}

const wchar_t* CardinalOfFacing(float facingRad) {
    return Cardinal(OctantOf(CompassFaceDeg(facingRad)));
}

int DistanceToSteps(float dist) {
    if (g_unitsPerStep <= 0.0f) return 0;
    int steps = static_cast<int>(dist / g_unitsPerStep + 0.5f);
    return steps < 0 ? 0 : steps;
}

std::wstring ElevationSuffix(const FVec3& from, const FVec3& to) {
    float dy = to.y - from.y;
    float thresh = g_unitsPerStep * 0.5f;   // ~half a step so tiny slope noise doesn't chatter
    if (dy > thresh)  return Phrase::Get(Phrase::Id::AboveSuffix);
    if (dy < -thresh) return Phrase::Get(Phrase::Id::BelowSuffix);
    return L"";
}

bool IsWithinReach(float dist2D) {
    return dist2D < g_unitsPerStep * 1.5f;   // HORIZONTAL only -- see ReachPhrase below
}

// The reach phrase, with elevation retained.
//
// Session 73 bug: all three DescribeDirection* variants early-returned a bare L"right next to you"
// the moment the HORIZONTAL distance was inside reach, which put the return ABOVE the line that
// appends ElevationSuffix. So the one phrase that most strongly implies "you can interact now" was
// also the only phrase that could never say "(above)" -- and the mod used it for an NPC 6.92 units
// directly overhead, in a 3D game full of plinths, balconies and bridges.
//
// The early return itself is CORRECT and stays: at melee range a bearing swings wildly and is
// meaningless, which is exactly why it was introduced. What was wrong was dropping the one
// component that does NOT degenerate at close range -- the vertical one. So keep the phrase, keep
// the stable wording, and re-attach the elevation.
//
// On level ground ElevationSuffix returns L"", so this is byte-identical to the old behaviour.
std::wstring ReachPhrase(const FVec3& from, const FVec3& to) {
    return std::wstring(Phrase::Get(Phrase::Id::RightNextToYou)) + ElevationSuffix(from, to);
}

std::wstring DescribeDirection(const FVec3& from, const FVec3& to) {
    float d2 = Distance2D(from, to);
    // "right next to you" (not "here"): this is now THE crow-flies phrase, and it inherits the
    // wording the egocentric one used, so the spoken result is unchanged at melee range.
    if (IsWithinReach(d2)) return ReachPhrase(from, to);
    std::wstring s = CardinalBearing(from, to);
    s += L", ";
    s += std::to_wstring(DistanceToSteps(d2));
    s += Phrase::Get(Phrase::Id::StepsSuffix);
    s += ElevationSuffix(from, to);
    return s;
}

std::wstring DescribeDirectionRelative(const FVec3& from, const FVec3& to, float facingRad) {
    float d2 = Distance2D(from, to);
    if (IsWithinReach(d2)) return ReachPhrase(from, to);
    // ALWAYS a direction. A distance with no direction is useless to walk on, so there is no
    // "omit the word" path here -- PlayerState::ReadCameraForwardStable falls back through the live
    // camera, the last good camera, then the leader's own facing, and only a caller with no player
    // at all could fail, in which case there is nothing to describe anyway.
    std::wstring s = CardinalBearingRelative(from, to, facingRad);
    s += L", ";
    s += std::to_wstring(DistanceToSteps(d2));
    s += Phrase::Get(Phrase::Id::StepsSuffix);
    s += ElevationSuffix(from, to);
    return s;
}

std::wstring DescribeDirectionEgo(const FVec3& from, const FVec3& to, float facingRad) {
    float d2 = Distance2D(from, to);
    if (IsWithinReach(d2)) return ReachPhrase(from, to);
    std::wstring s = EgoBearing(from, to, facingRad);
    s += L", ";
    s += std::to_wstring(DistanceToSteps(d2));
    s += Phrase::Get(Phrase::Id::StepsSuffix);
    s += ElevationSuffix(from, to);
    return s;
}

void  SetUnitsPerStep(float u) { if (u > 0.0f) g_unitsPerStep = u; }
float GetUnitsPerStep() { return g_unitsPerStep; }

} // namespace NavCommon

#include "navigation/nav_common.h"

#include <cmath>

namespace NavCommon {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kRadToDeg = 180.0f / kPi;

// World units per spoken "step". FFXII's field world is in METERS (offline: Bullet gravity
// -10, char capsule height 0.8 + radius 0.6 ~= a 2 m humanoid). ~0.75 m stride => 1 step.
float g_unitsPerStep = 0.75f;

// Cardinal labels, index 0 = North, clockwise. FFXII world north = -Z, so 0deg = -Z ("north"),
// +90deg = +X ("east").
const wchar_t* kCardinal[8] = {
    L"North", L"Northeast", L"East", L"Southeast",
    L"South", L"Southwest", L"West", L"Northwest",
};

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
    return kCardinal[OctantOf(BearingDeg(from, to))];
}

const wchar_t* CardinalOfHeading(float headingRad) {
    float deg = headingRad * kRadToDeg;
    deg = std::fmod(deg, 360.0f);
    if (deg < 0.0f) deg += 360.0f;
    return kCardinal[OctantOf(deg)];
}

const wchar_t* CardinalBearingRelative(const FVec3& from, const FVec3& to, float facingRad) {
    const float ego = Norm360(BearingDeg(from, to) - CompassFaceDeg(facingRad));
    return kCardinal[OctantOf(ego)];
}

const wchar_t* CardinalOfHeadingRelative(float headingRad, float facingRad) {
    const float ego = Norm360(headingRad * kRadToDeg - CompassFaceDeg(facingRad));
    return kCardinal[OctantOf(ego)];
}

const wchar_t* CardinalOfFacing(float facingRad) {
    return kCardinal[OctantOf(CompassFaceDeg(facingRad))];
}

int DistanceToSteps(float dist) {
    if (g_unitsPerStep <= 0.0f) return 0;
    int steps = static_cast<int>(dist / g_unitsPerStep + 0.5f);
    return steps < 0 ? 0 : steps;
}

std::wstring ElevationSuffix(const FVec3& from, const FVec3& to) {
    float dy = to.y - from.y;
    float thresh = g_unitsPerStep * 0.5f;   // ~half a step so tiny slope noise doesn't chatter
    if (dy > thresh)  return L" (above)";
    if (dy < -thresh) return L" (below)";
    return L"";
}

bool IsWithinReach(float dist2D) {
    return dist2D < g_unitsPerStep * 1.5f;
}

std::wstring DescribeDirection(const FVec3& from, const FVec3& to) {
    float d2 = Distance2D(from, to);
    if (IsWithinReach(d2)) return L"here";
    std::wstring s = CardinalBearing(from, to);
    s += L", ";
    s += std::to_wstring(DistanceToSteps(d2));
    s += L" steps";
    s += ElevationSuffix(from, to);
    return s;
}

std::wstring DescribeDirectionRelative(const FVec3& from, const FVec3& to, float facingRad) {
    float d2 = Distance2D(from, to);
    if (IsWithinReach(d2)) return L"right next to you";
    std::wstring s = CardinalBearingRelative(from, to, facingRad);
    s += L", ";
    s += std::to_wstring(DistanceToSteps(d2));
    s += L" steps";
    s += ElevationSuffix(from, to);
    return s;
}

void  SetUnitsPerStep(float u) { if (u > 0.0f) g_unitsPerStep = u; }
float GetUnitsPerStep() { return g_unitsPerStep; }

} // namespace NavCommon

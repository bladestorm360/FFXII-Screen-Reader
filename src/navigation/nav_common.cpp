#include "navigation/nav_common.h"

#include <cmath>

namespace NavCommon {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kRadToDeg = 180.0f / kPi;

// World units per spoken "step". FFXII's field world is in METERS (confirmed
// offline: Bullet gravity -10, character capsule height 0.8 + radius 0.6 ~= a 2 m
// humanoid, 0.04 m collision margin). A ~2 m humanoid's walking stride at ~2
// steps/s is ~0.75 m, so 1 step = 0.75 world units. Runtime-tunable for feel.
float g_unitsPerStep = 0.75f;

// Cardinal labels, index 0 = North, clockwise. Bearing convention: FFXII's world
// north is -Z (confirmed in-game: +Z read as South), so 0deg = -Z ("north"),
// +90deg = +X ("east"). E/W (dx) is not flipped. If a runtime pass shows E/W is
// also inverted, additionally negate dx here.
const wchar_t* kCardinal[8] = {
    L"North", L"Northeast", L"East", L"Southeast",
    L"South", L"Southwest", L"West", L"Northwest",
};

const wchar_t* kEgo[8] = {
    L"Ahead", L"Ahead and right", L"Right", L"Behind and right",
    L"Behind", L"Behind and left", L"Left", L"Ahead and left",
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

const wchar_t* EgocentricBearing(const FVec3& from, const FVec3& to, float playerYawRad) {
    // Rotate the world bearing into the player's frame (yaw is the facing about Y).
    float rel = BearingDeg(from, to) - playerYawRad * kRadToDeg;
    rel = std::fmod(rel, 360.0f);
    if (rel < 0.0f) rel += 360.0f;
    return kEgo[OctantOf(rel)];
}

const wchar_t* CardinalOfHeading(float yawRad) {
    float deg = yawRad * kRadToDeg;
    deg = std::fmod(deg, 360.0f);
    if (deg < 0.0f) deg += 360.0f;
    return kCardinal[OctantOf(deg)];
}

// Axis words reuse kCardinal (0=North, 2=East, 4=South, 6=West), north=-Z / east=+X.
const wchar_t* NorthSouthWord(float dz) { return kCardinal[dz < 0.0f ? 0 : 4]; }
const wchar_t* EastWestWord(float dx)   { return kCardinal[dx > 0.0f ? 2 : 6]; }

int DistanceToSteps(float dist) {
    if (g_unitsPerStep <= 0.0f) return 0;
    int steps = static_cast<int>(dist / g_unitsPerStep + 0.5f);
    return steps < 0 ? 0 : steps;
}

std::wstring ElevationSuffix(const FVec3& from, const FVec3& to) {
    float dy = to.y - from.y;
    // Threshold ~half a step so tiny slope noise doesn't chatter.
    float thresh = g_unitsPerStep * 0.5f;
    if (dy > thresh)  return L" (above)";
    if (dy < -thresh) return L" (below)";
    return L"";
}

bool IsWithinReach(float dist2D) {
    return dist2D < g_unitsPerStep * 1.5f;
}

std::wstring DescribeDirection(const FVec3& from, const FVec3& to,
                               bool egocentric, float playerYawRad) {
    float d2 = Distance2D(from, to);
    if (IsWithinReach(d2)) return L"here";
    const wchar_t* dir = egocentric ? EgocentricBearing(from, to, playerYawRad)
                                    : CardinalBearing(from, to);
    std::wstring s = dir;
    s += L", ";
    s += std::to_wstring(DistanceToSteps(d2));
    s += L" steps";
    s += ElevationSuffix(from, to);
    return s;
}

void  SetUnitsPerStep(float u) { if (u > 0.0f) g_unitsPerStep = u; }
float GetUnitsPerStep() { return g_unitsPerStep; }

} // namespace NavCommon

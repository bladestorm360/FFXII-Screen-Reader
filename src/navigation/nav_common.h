#pragma once

#include <string>
#include "navigation/nav_types.h"

// Engine-independent navigation math, in FFXII's coordinate frame. Structure is
// inspired by DQ7R, but the frame is FFXII's own (confirmed by FUN_00323350):
// the ground plane is X/Z, up is Y. All bearing/distance math therefore uses the
// (dx, dz) pair; Y is elevation only. Cardinal (world-space) directions are the
// default and need only positions; egocentric is an optional mode off player yaw.
namespace NavCommon {

// Ground-plane (X/Z) and full 3D distance.
float Distance2D(const FVec3& a, const FVec3& b);
float Distance3D(const FVec3& a, const FVec3& b);

// Cardinal 8-point compass word for the bearing from->to on the X/Z plane
// ("North","Northeast",...). Independent of facing and camera.
const wchar_t* CardinalBearing(const FVec3& from, const FVec3& to);

// Egocentric 8-point direction relative to the player's facing yaw (radians):
// "Ahead","Ahead and right","Right","Behind and right","Behind",...
const wchar_t* EgocentricBearing(const FVec3& from, const FVec3& to, float playerYawRad);

// Cardinal word for a heading angle (radians, same convention as ReadPlayerYaw:
// atan2(fwd.x, fwd.z)). Used for the "facing" readout.
const wchar_t* CardinalOfHeading(float yawRad);

// Distance -> whole "steps" using the configured units-per-step (FFXII-specific,
// runtime-tuned). Rounded to nearest, min 0.
int DistanceToSteps(float dist);

// L" (above)" / L" (below)" / L"" from the Y (elevation) delta.
std::wstring ElevationSuffix(const FVec3& from, const FVec3& to);

// Very close on the ground plane (short-circuit "you're basically there").
bool IsWithinReach(float dist2D);

// Full spoken phrase, e.g. L"Northeast, 24 steps (above)" (cardinal) or the
// egocentric variant. Returns L"here" when within reach.
std::wstring DescribeDirection(const FVec3& from, const FVec3& to,
                               bool egocentric, float playerYawRad);

// Tunables. Units-per-step and the reach radius are FFXII-specific; defaults are
// placeholders confirmed/adjusted in the runtime pass.
void  SetUnitsPerStep(float u);
float GetUnitsPerStep();

} // namespace NavCommon

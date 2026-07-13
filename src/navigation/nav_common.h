#pragma once

#include <string>
#include "navigation/nav_types.h"

// Engine-independent navigation math, in FFXII's coordinate frame (ground plane X/Z, up Y;
// confirmed via FUN_00323350). World bearing convention: north = -Z, east = +X, `atan2(dx, -dz)`.
// Two direction frames live here: WORLD-ABSOLUTE (`CardinalBearing`/`DescribeDirection`, true
// compass) and EGOCENTRIC (`*Relative`, relabeled to the player's facing). FFXII field movement
// is CAMERA-RELATIVE (confirmed live: "up on the stick" walks along the character's facing, not
// world-north), so spoken navigation uses the egocentric frame — "North" = forward.
namespace NavCommon {

// Ground-plane (X/Z) and full 3D distance.
float Distance2D(const FVec3& a, const FVec3& b);
float Distance3D(const FVec3& a, const FVec3& b);

// World-absolute 8-point compass word for the bearing from->to on the X/Z plane
// ("North","Northeast",...). This is THE direction function — used by both the `/` crow-flies
// describe and each `\` route leg, so they always agree.
const wchar_t* CardinalBearing(const FVec3& from, const FVec3& to);

// Cardinal word for a heading angle (radians, `atan2(dx, -dz)` convention). Used by the `/`
// obstacle hint's "bear <cardinal>" (a computed clear heading, not player facing).
const wchar_t* CardinalOfHeading(float headingRad);

// ---- EGOCENTRIC directions (relative to where UP takes you) ------------------------
// FFXII field movement is CAMERA-RELATIVE (proven: FUN_004742a0 rotates the stick by the camera
// matrix): "up on the stick" walks along CAMERA-forward, not world-north. So spoken directions are
// relabeled relative to that: "North" = forward (where UP takes you), East = right, South = behind,
// West = left. `facingRad` is the camera up-direction yaw in the game's `atan2(x, z)` convention
// (PlayerState::ReadCameraForward) — NOT the character's `faceNode`, which is stale when idle and
// points at the target in combat. We reconcile it to the compass frame internally.

// Egocentric 8-point cardinal for the bearing from->to, relative to `facingRad`.
const wchar_t* CardinalBearingRelative(const FVec3& from, const FVec3& to, float facingRad);

// Egocentric cardinal for a world heading (`atan2(dx,-dz)` radians), relative to `facingRad`.
const wchar_t* CardinalOfHeadingRelative(float headingRad, float facingRad);

// A yaw as a TRUE-NORTH cardinal (for the `;` readout: "Forward points north" — pass the camera
// up-direction so it names which real-world way forward/UP currently points).
const wchar_t* CardinalOfFacing(float facingRad);

// Distance -> whole "steps" using the configured units-per-step (FFXII-specific, runtime-tuned).
// Rounded to nearest, min 0.
int DistanceToSteps(float dist);

// L" (above)" / L" (below)" / L"" from the Y (elevation) delta.
std::wstring ElevationSuffix(const FVec3& from, const FVec3& to);

// Very close on the ground plane (short-circuit "you're basically there").
bool IsWithinReach(float dist2D);

// Full spoken crow-flies phrase, e.g. L"Northeast, 24 steps (above)". Returns L"here" within reach.
std::wstring DescribeDirection(const FVec3& from, const FVec3& to);

// EGOCENTRIC crow-flies phrase (cardinal relabeled to `facingRad`), e.g. L"North, 24 steps".
// Returns L"right next to you" within reach (kills the swinging cardinal at melee range).
std::wstring DescribeDirectionRelative(const FVec3& from, const FVec3& to, float facingRad);

// Tunables. Units-per-step is FFXII-specific; default is a placeholder tuned in the runtime pass.
void  SetUnitsPerStep(float u);
float GetUnitsPerStep();

} // namespace NavCommon

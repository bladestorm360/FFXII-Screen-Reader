#pragma once

#include <string>
#include "navigation/nav_types.h"

// Engine-independent navigation math, in FFXII's coordinate frame (ground plane X/Z, up Y;
// confirmed via FUN_00323350). World bearing convention: north = -Z, east = +X, `atan2(dx, -dz)`.
//
// THE SHIPPED FRAME IS RELATIVE, SPOKEN IN COMPASS WORDS (`CardinalBearingRelative` /
// `CardinalOfHeadingRelative` / `DescribeDirectionRelative`). "North" means FORWARD -- wherever
// forward currently points -- "East" right, "South" behind, "West" left. FFXII field movement is
// camera-relative, so the relative frame is the only one a direction is actionable in; and compass
// words are the tester's explicit preference over literal ego words.
//
// TWO WRONG TURNS ARE RECORDED HERE so they are not retried:
//   1. Making everything WORLD-ABSOLUTE. Stable, but useless: you cannot push "north".
//   2. Keeping the relative frame but swapping in ego words ("ahead", "behind-left"), on the theory
//      that compass-words-on-a-relative-frame was what made directions confusing. It was not. The
//      real cause was the ROUTE SMOOTHER inventing a diagonal the path never takes -- see the
//      diagonal rule in path_directions.cpp. Vocabulary was a red herring.
//
// Every relative path takes its reference from `PlayerState::ReadCameraForwardStable`, which always
// yields one (live camera -> last good camera -> the leader's own facing). There is deliberately NO
// "no direction available" path: a distance with no direction is useless to walk on. What must never
// happen is the old bug -- `float facingRad = 0.0f` with `ReadCameraForward`'s bool ignored, where
// `CompassFaceDeg(0) == 180` spoke every direction REVERSED.
//
// RETAINED, NOT SHIPPED: the WORLD-ABSOLUTE family (`CardinalBearing` / `CardinalOfHeading` /
// `DescribeDirection`) and the literal-ego vocabulary (`EgoBearing` / `EgoOfHeading` /
// `DescribeDirectionEgo`, wording "forward / forward-right / right / backward-right / …"), kept for
// anyone who wants a true ego readout.
namespace NavCommon {

// Ground-plane (X/Z) and full 3D distance.
float Distance2D(const FVec3& a, const FVec3& b);
float Distance3D(const FVec3& a, const FVec3& b);

// World-absolute 8-point compass word for the bearing from->to on the X/Z plane
// ("North","Northeast",...). RETAINED, NOT USED — see the file header: stable, but the player
// cannot push "north", so it is not actionable in a camera-relative game.
const wchar_t* CardinalBearing(const FVec3& from, const FVec3& to);

// Cardinal word for a heading angle (radians, `atan2(dx, -dz)` convention). Retained, not used.
const wchar_t* CardinalOfHeading(float headingRad);

// ---- RELATIVE directions in COMPASS WORDS — THE SHIPPED FRAME ----------------------
// `facingRad` is the camera up-direction yaw in the game's `atan2(x, z)` convention, from
// PlayerState::ReadCameraForwardStable — NOT the character's `faceNode`, which is stale when idle
// and points at the target in combat. "North" = forward, "East" = right, "South" = behind,
// "West" = left, whatever true compass direction those happen to be.

// The raw octant INDEX in the relative frame (0 = forward, 2 = right, 4 = behind, 6 = left,
// clockwise), and the word for an index. EVEN indices are cardinal, ODD are diagonal —
// PathDirections relies on that parity to enforce "a diagonal word only ever describes a genuinely
// diagonal stretch".
int            RelativeOctant(const FVec3& from, const FVec3& to, float facingRad);
const wchar_t* RelativeWord(int octant);      // SHIPPED: compass word
const wchar_t* EgoWordOfOctant(int octant);   // retained: "forward" / "backward-left" / …

// ---- LITERAL-EGO vocabulary — RETAINED, NOT USED -----------------------------------
// Identical math, worded "forward / forward-right / right / backward-right / backward /
// backward-left / left / forward-left". Kept for anyone who wants a true ego readout; the compass
// wording above is what ships.
const wchar_t* EgoBearing(const FVec3& from, const FVec3& to, float facingRad);
const wchar_t* EgoOfHeading(float headingRad, float facingRad);
std::wstring   DescribeDirectionEgo(const FVec3& from, const FVec3& to, float facingRad);

// SHIPPED: compass word for the bearing from->to in the RELATIVE frame ("North" = forward).
// Used by the `[`/`]`/`/` describe and by every `\` route leg, so the two always agree.
const wchar_t* CardinalBearingRelative(const FVec3& from, const FVec3& to, float facingRad);

// SHIPPED: compass word for a world heading (`atan2(dx,-dz)` radians) in the relative frame —
// the `/` obstacle hint's "Blocked, bear <word>".
const wchar_t* CardinalOfHeadingRelative(float headingRad, float facingRad);

// A yaw as a TRUE-NORTH cardinal ("Forward points north" — pass the camera up-direction so it names
// which real-world way forward/UP currently points). Retained; no callers.
const wchar_t* CardinalOfFacing(float facingRad);

// Distance -> whole "steps" using the configured units-per-step (FFXII-specific, runtime-tuned).
// Rounded to nearest, min 0.
int DistanceToSteps(float dist);

// L" (above)" / L" (below)" / L"" from the Y (elevation) delta.
std::wstring ElevationSuffix(const FVec3& from, const FVec3& to);

// Very close on the ground plane (short-circuit "you're basically there"). HORIZONTAL ONLY --
// pair it with ReachPhrase, never with a bare literal, or a target directly overhead reads as
// adjacent (Session 73).
bool IsWithinReach(float dist2D);

// L"right next to you" plus the elevation suffix. The ONLY thing that should ever be returned from
// an IsWithinReach short-circuit: the bare literal silently discards the vertical gap.
std::wstring ReachPhrase(const FVec3& from, const FVec3& to);

// Full spoken crow-flies phrase in the WORLD-ABSOLUTE frame. RETAINED, NOT USED —
// DescribeDirectionRelative is the shipped one. Returns L"right next to you" within reach.
std::wstring DescribeDirection(const FVec3& from, const FVec3& to);

// SHIPPED crow-flies phrase: compass word in the RELATIVE frame, e.g. L"North, 24 steps" meaning
// 24 steps straight ahead. Returns L"right next to you" within reach (kills a swinging cardinal at
// melee range, where a bearing is meaningless). ALWAYS emits a direction -- see the file header.
std::wstring DescribeDirectionRelative(const FVec3& from, const FVec3& to, float facingRad);

// Tunables. Units-per-step is FFXII-specific; default is a placeholder tuned in the runtime pass.
void  SetUnitsPerStep(float u);
float GetUnitsPerStep();

} // namespace NavCommon

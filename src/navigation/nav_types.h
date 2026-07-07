#pragma once

// Engine-independent 3D vector shared across the navigation module. FFXII field
// space is Y-up; the ground plane is X/Z. Bearing/distance math (nav_common)
// works on the X/Z plane and treats Y as elevation.
struct FVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

#pragma once

#include "navigation/nav_types.h"

// Read-only Bullet walkability queries. Wraps the game's ready-made ray cast
// (FUN_006a1a70), which reads the live collision world at *(context+0x60). The
// physics context is captured once per map by the map-load hook (nav_hooks) and
// handed here via SetContext. Every call guards the world pointer itself (the
// wrapper returns garbage if the world is null) and SEH-guards the call.
//
// This is a READ. rayTest walks the broadphase without mutating it. NOTE: heavy
// use (grid building in M2/M3) may want to run on the game thread; M0/M1 issue at
// most one ray per keypress.
namespace BulletQuery {

// Cache / clear the physics context (called by the map-load hook).
void SetContext(void* ctx);
void* GetContext();
void Invalidate();

// True when a context is cached AND its world (*(ctx+0x60)) is non-null.
bool HasWorld();

// Cast a ray from -> to. Returns true on hit and fills outHit (world hit point)
// and outNormal (unit surface normal). filterGroup 0xF = all collision groups.
bool Ray(const FVec3& from, const FVec3& to, FVec3& outHit, FVec3& outNormal,
         int filterGroup = 0xF);

// Floor probe: cast straight down through `at` (Y is up), from at.y+upPad to
// at.y-downDist. Returns true + the floor Y on hit.
bool FloorBelow(const FVec3& at, float upPad, float downDist, float& outFloorY);

// True if a straight horizontal path from `from` to `to` is unobstructed — no hit
// before reaching `to` (within `margin` meters). Both endpoints use from.y so the
// ray stays at body height. Returns true (clear) when there is no hit / no world;
// gate on HasWorld() first. One ray per call — cheap and safe on the input thread.
bool HorizontalClear(const FVec3& from, const FVec3& to, float margin);

} // namespace BulletQuery

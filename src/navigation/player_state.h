#pragma once

#include <cstdint>
#include "navigation/nav_types.h"

// Resolves the FIELD leader (the character the player currently controls) to its
// scene object + character component, and — once M0 pins the last hop — to live
// world position + facing. Everything here is READ-ONLY and SEH-guarded: the
// mod never writes the leader/component (the game's own FUN_00317e60 sets a flag
// bit on the component; we deliberately do NOT replicate that write).
//
// Resolution mirrors the game's own path (FUN_00317e60):
//   DAT_022c7fe0 (uint handle) -> handle-table resolve (FUN_003588b0, replicated
//   inline, memory-only) -> scene object -> *(sceneObj+0x30) char component
//   (valid when *component & 0x08). The component -> physics-controller ->
//   world-matrix hop is what M0 discovers; ReadPlayerPos/Yaw stay disabled until
//   the diagnostic dump pins that offset.
namespace PlayerState {

// True while a field map is live (DAT_02089340 & 0x10). False on title / between
// maps — every read below returns false/null when this is false.
bool IsFieldActive();

// Raw leader handle (DAT_022c7fe0). 0 if none.
uint32_t ReadLeaderHandle();

// Resolve a handle to its scene object via the generation-checked handle table.
// Memory-only replica of FUN_003588b0. Returns null on any validity failure
// (bad selector, inactive table, out-of-range slot, stale generation).
void* ResolveHandle(uint32_t handle);

// Leader scene object (resolved handle). Null if no valid leader / not on field.
void* ReadLeaderSceneObject();

// Leader character component (*(sceneObj+0x30)), gated on the *component & 0x08
// validity bit. Null if invalid. This is the anchor M0 dumps to find the
// physics controller + world matrix.
void* ReadLeaderComponent();

// Snapshot of every hop, capturing partial progress even when a later hop fails —
// consumed by the M0 diagnostic dump.
struct LeaderChain {
    bool     fieldActive   = false;
    uint32_t handle        = 0;
    uint32_t selector      = 0;
    uint32_t slot          = 0;
    uint32_t generation    = 0;
    void*    sceneObj      = nullptr;
    void*    component     = nullptr;
    uint32_t componentFlags = 0;
    bool     valid         = false;  // component resolved AND validity bit set
};
LeaderChain CaptureLeaderChain();

// ---- Post-M0 (offset pinned by the diagnostic dump) -------------------------
// Live leader world position / yaw. Disabled until M0 identifies the
// component->controller->matrix offset; return false meanwhile.
bool ReadPlayerPos(FVec3& out);
bool ReadPlayerYaw(float& outRadians);

} // namespace PlayerState

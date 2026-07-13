#pragma once

#include <cstdint>
#include <cstddef>
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

// The HARD gate for game-thread pathfinding: true ONLY when the field map is fully
// loaded and stable — field sim live AND area collision loaded AND a valid area id
// AND the actor pool + leader resolve AND the Bullet world is built. Stricter than
// IsFieldActive() because the 0x10 bit is set early on load / cleared late on
// teardown; this pairs it with the area-collision + live-world pointers so the
// planner never touches a half-loaded or half-freed map. Call on the game thread
// before any raycast. SEH-guarded throughout.
bool IsFieldNavSafe();

// Diagnostic companion to IsFieldNavSafe(): evaluates ALL 8 gate conditions (no
// short-circuit) and returns a bitmask of the ones that FAILED (0 == fully nav-safe).
// Bits: 0 field, 1 field2, 2 areaId, 3 areaColl, 4 actorPool, 5 leaderPtr, 6 world,
// 7 leaderObj. Both this and IsFieldNavSafe() evaluate the same single-source
// condition helpers, so they can never drift apart. Log-only; not a gate.
uint8_t NavSafeFailMask();
// Short name for gate-condition bit 0..7 (e.g. "world"); "?" out of range.
const char* NavSafeCondName(int bit);
// Format a fail-mask into buf as a comma-joined name list ("world,leaderObj"); empty
// string when mask == 0. buf is always null-terminated.
void FormatNavSafeMask(uint8_t mask, char* buf, size_t bufLen);

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

// ---- Live world position / facing (offset pinned; FUN_00265020 chain) -------
// World position of ANY scene object (leader, NPC, or static gimmick) via its
// transform pointer at sceneObj+0xB8. Returns false on any read/guard failure.
bool ReadSceneObjectPos(void* sceneObj, FVec3& out);
// Live leader world position.
bool ReadPlayerPos(FVec3& out);

// Live leader world FACING yaw (radians) — `faceNode`, the class-3 facing slot on the leader's
// transform node (node+0xA4; the SAME +0xB8 node as position). Convention: atan2(worldMoveX,
// worldMoveZ). NOTE: this is NOT the egocentric "forward" reference — it equals "where UP takes you"
// only WHILE actively walking in the field (the driver writes it only when moving; combat overrides
// it to face the target). Kept for the DIAGNOSTIC only; the feature uses ReadCameraForward. Returns
// false on any read/guard failure.
bool ReadPlayerFacing(float& outRad);

// Camera "up-direction" yaw (radians) — the world direction a pure UP push sends the leader, read
// LIVE from the MOVEMENT camera matrix DAT_02aedf30 row 2 (CAMERA_FWD_X/Z). Because worldMove =
// -stickY*row2 for UP, the up-direction = atan2(-fwd.x, -fwd.z). THIS is the egocentric "forward"
// reference: SAME atan2(x,z) convention as ReadPlayerFacing (a drop-in), but valid idle, after a
// camera rotate, and in combat (faceNode is not). Returns false on read failure or a zero row.
bool ReadCameraForward(float& outRad);

// ---- Movement frame (Phase A diagnostic + egocentric "forward" reference) ----
// One SEH-guarded snapshot of the pieces that determine "which way does the stick send
// me": the leader's world facing yaw, the gameplay camera's world look yaw, and the live
// world move vector the locomotion driver writes. All yaws are RADIANS in the game's
// atan2(x,z) convention, so their differences are directly comparable. `have*` flag each
// piece independently (a missing camera/actor ptr does not sink the others).
struct MoveFrame {
    bool  havePos       = false;  FVec3 pos;
    bool  haveFaceCache = false;  float faceCacheRad = 0.0f;  // leader actor+0x15C (per-frame cache)
    bool  haveFaceNode  = false;  float faceNodeRad  = 0.0f;  // (*(sceneObj+0xB8))+0xA4 (class-3 slot)
    bool  haveCamLook   = false;  float camLookRad   = 0.0f;  // DAT_02aedf94 scalar (diag cross-check)
    bool  haveMove      = false;  bool  moving        = false;
    float moveX = 0.0f, moveZ = 0.0f;  float moveYawRad = 0.0f;  // atan2(moveX, moveZ) when moving
    // Phase A calibration: camera-forward candidates from the MOVEMENT matrix DAT_02aedf30 row 2,
    // BOTH signs logged so one walk pins which equals the actual move direction.
    bool  haveCamFwd = false;  float camFwdX = 0.0f, camFwdZ = 0.0f;
    float camFwdRawRad = 0.0f;   // atan2( fwd.x,  fwd.z)
    float camFwdNegRad = 0.0f;   // atan2(-fwd.x, -fwd.z)  (the up-direction candidate ReadCameraForward uses)
    // GROUND TRUTH: world movement since the PREVIOUS ReadMoveFrame call (independent of every field).
    bool  haveDPos = false;  float dPosX = 0.0f, dPosZ = 0.0f, dPosDist = 0.0f;
    float dPosYawF = 0.0f;   // atan2(dx,  dz)  (faceNode / atan2(x,z) convention)
    float dPosYawB = 0.0f;   // atan2(dx, -dz)  (BearingDeg convention)
};
// Fill `out` with the current movement frame. Returns true if ANY field was read.
bool ReadMoveFrame(MoveFrame& out);

} // namespace PlayerState

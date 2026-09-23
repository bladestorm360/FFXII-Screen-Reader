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

// The HARD gate for game-thread pathfinding: true ONLY when the field map is loaded and stable — field
// sim live AND started, the actor pool + leader resolve, AND the walkmap is up. Stricter than
// IsFieldActive(), whose 0x10 bit is set early on load and cleared late on teardown.
//
// It no longer waits on the per-AREA resource manifest (Session 93): that pair is a TERMINAL state on
// maps which have no such resource, not a readiness signal, and waiting on it killed all routing on
// Ridorana/Pharos. Full mechanism at the definition. Game thread; SEH-guarded throughout.
bool IsFieldNavSafe();

// Diagnostic companion: evaluates ALL 8 conditions (no short-circuit) and returns a bitmask of those
// that FAILED. Bits: 0 field, 1 field2, 2 areaId, 3 areaManifest, 4 actorPool, 5 leaderPtr, 6 world,
// 7 leaderObj. Log-only.
//
// **A ZERO MASK IS NOT THE SAME THING AS NAV-SAFE.** Bits 2/3 are reported but are not in the gate, so
// `mask == 0` implies nav-safe while nav-safe does NOT imply `mask == 0`.
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
// The object's INTERACTION ANCHOR -- its transform position plus the offset at `xform+0x40..0x48`
// when the byte at `xform+0x107` is set. That offset is what `FUN_0025bad0` adds before BOTH the
// interaction distance gate and the vertical band test, so this is the point the engine actually
// measures against; the raw transform origin is not.
//
// Every entity position in the mod comes through here, so the `/` describe, the `\` route and the
// `;` readout all name the same point by construction.
bool ReadSceneObjectPos(void* sceneObj, FVec3& out);

// Log-only tally of how often the anchor offset above is actually present, and its largest magnitude.
// The offset's semantics are unverified, so this measures rather than assumes -- see the comment on
// ReadSceneObjectPos.
void GetAnchorStats(int& withOffset, int& plain, float& maxOffset);
// Live leader world position.
bool ReadPlayerPos(FVec3& out);

// The party leader's LIVE movement class — the third argument `FUN_00230a40` needs to decide whether a
// polygon is walkable. 0 normally, 5 mounted (see nav_rva.h WALK_CLASS_LEADER for the whole table and
// the offset chain).
//
// **NEVER HARDCODED, AND NEVER 4.** The mod used to assume 4, which is the one value `FUN_00230a40`
// never refuses anything for — so every per-class refusal, water included, was dropped. If the chain
// cannot be read or returns something outside {0..5} this falls back to **0**, the leader's normal
// value, and logs the raw byte once so a broken chain arrives as data rather than as silence.
//
// Cached briefly (the class only changes on leader switch, formation change or mounting), so this is
// cheap enough for the A* inner loop.
uint16_t PartyMovementClass();

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

// The same reference, but SAFE TO USE: returns the live value when the camera row is fresh, else the
// last value that WAS fresh. False only when no camera has ever been read this session.
//
// USE THIS, NOT ReadCameraForward, on every speech path. The raw getter leaves `outRad` untouched on
// failure, and every caller used to declare `float facingRad = 0.0f` and ignore the bool -- so an
// unrefreshed camera row silently meant "forward = 0", and CompassFaceDeg(0) == 180, i.e. EVERY
// direction spoken 180 degrees reversed. A stale-by-a-frame reference is a small error; a reversed
// one is the difference between walking to a thing and walking away from it.
bool ReadCameraForwardStable(float& outRad);

// Same as above, but also reports WHICH source produced the value, for the route/announce logs:
//   "live" = fresh camera row · "held" = last good row · "face" = leader facing fallback.
// This is the line whose absence made a 180-degree direction reversal take a session to diagnose: with
// it, "directions went wrong" is answerable from the log alone -- a moving `ref` is the game's camera
// changing under us (documented, not a bug), a static `ref` with flipped words is ours.
bool ReadCameraForwardStable(float& outRad, const char** srcOut);

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

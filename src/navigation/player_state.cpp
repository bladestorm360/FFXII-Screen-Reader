#include "navigation/player_state.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "navigation/map_query.h"
#include "core/hooks.h"
#include "core/mem_read.h"

#include <atomic>
#include <cmath>
#include <cstdio>

using namespace MemRead;

namespace PlayerState {

bool IsFieldActive() {
    void* p = Hooks::ResolveRva(NavRva::FIELD_ACTIVE);
    uint8_t b = 0;
    if (!SafeReadU8(p, 0, &b)) return false;
    return (b & 0x10) != 0;
}

namespace {

// How often the interaction-anchor offset is actually present, and how large it gets. Log-only, and
// deliberately cheap: three ints touched on a path that already runs per entity per rescan.
//
// This exists because the offset's SEMANTICS are unverified (~0.9 -- we know the engine applies it
// before the interaction gates, not what writes it). Rather than assert a meaning, count it: if the
// flag never fires, the change is a no-op and we should say so plainly instead of leaving it looking
// like a fix. If it fires with implausible magnitudes, that is a wrong offset showing itself.
int   g_anchorOffsetCount = 0;
int   g_anchorPlainCount  = 0;
float g_anchorMaxOffset   = 0.0f;

void NoteAnchorOffset(float ox, float oy, float oz) {
    ++g_anchorOffsetCount;
    const float m = std::sqrt(ox * ox + oy * oy + oz * oz);
    if (m > g_anchorMaxOffset) g_anchorMaxOffset = m;
}
void NoteAnchorPlain() { ++g_anchorPlainCount; }

// The 8 IsFieldNavSafe() conditions, each as ONE single-source predicate (true = OK).
// This is the ONLY place the conditions are spelled out — both the real gate (a &&
// chain, so short-circuit + semantics are preserved) and the diagnostic fail-mask
// evaluate these, so the gate and the diagnostic can never drift apart.
bool CondFieldActive() {   // bit 0 — field sim live this session (0x10)
    return IsFieldActive();
}
bool CondFieldStarted() {  // bit 1 — field module started (1 after first field entry)
    uint32_t started = 0;
    return SafeReadU32(Hooks::ResolveRva(NavRva::FIELD_ACTIVE2), 0, &started) && started != 0;
}
bool CondAreaId() {        // bit 2 — area id valid (0xFFFFFFFF = no area / mid-transition)
    uint32_t areaId = 0;
    return SafeReadU32(Hooks::ResolveRva(NavRva::AREA_ID), 0, &areaId) && areaId != 0xFFFFFFFF;
}
bool CondAreaCollision() { // bit 3 — area collision blob loaded (earliest "gone" signal)
    return PtrAt(Hooks::ResolveRva(NavRva::AREA_COLLISION), 0) != nullptr;
}
bool CondActorPool() {     // bit 4 — actor pool allocated (true post-boot)
    return PtrAt(Hooks::ResolveRva(NavRva::ACTOR_POOL_BASE), 0) != nullptr;
}
bool CondLeaderPtr() {     // bit 5 — leader actor ptr present (zeroed at teardown start)
    return PtrAt(Hooks::ResolveRva(NavRva::LEADER_ACTOR_PTR), 0) != nullptr;
}
bool CondWorld() {         // bit 6 — the SQEX field walkmap is loaded (re-checked live)
    return MapQuery::HasWorld();
}
bool CondLeaderObj() {     // bit 7 — leader resolves through the handle table
    return ReadLeaderSceneObject() != nullptr;
}
} // namespace

bool IsFieldNavSafe() {
    // Short-circuit && chain — identical order/semantics to the per-condition helpers,
    // so the cheapest checks gate the expensive world-deref + handle-walk as before.
    return CondFieldActive() && CondFieldStarted() && CondAreaId() &&
           CondAreaCollision() && CondActorPool() && CondLeaderPtr() &&
           CondWorld() && CondLeaderObj();
}

uint8_t NavSafeFailMask() {
    uint8_t m = 0;
    if (!CondFieldActive())    m |= 0x01;
    if (!CondFieldStarted())   m |= 0x02;
    if (!CondAreaId())         m |= 0x04;
    if (!CondAreaCollision())  m |= 0x08;
    if (!CondActorPool())      m |= 0x10;
    if (!CondLeaderPtr())      m |= 0x20;
    if (!CondWorld())          m |= 0x40;
    if (!CondLeaderObj())      m |= 0x80;
    return m;
}

const char* NavSafeCondName(int bit) {
    switch (bit) {
        case 0: return "field";
        case 1: return "field2";
        case 2: return "areaId";
        case 3: return "areaColl";
        case 4: return "actorPool";
        case 5: return "leaderPtr";
        case 6: return "world";
        case 7: return "leaderObj";
        default: return "?";
    }
}

void FormatNavSafeMask(uint8_t mask, char* buf, size_t bufLen) {
    if (!buf || bufLen == 0) return;
    buf[0] = '\0';
    size_t n = 0;
    for (int b = 0; b < 8; ++b) {
        if (!(mask & (1u << b))) continue;
        int r = snprintf(buf + n, bufLen - n, "%s%s", n ? "," : "", NavSafeCondName(b));
        if (r < 0) break;
        n += static_cast<size_t>(r);
        if (n >= bufLen) { buf[bufLen - 1] = '\0'; break; }
    }
}

uint32_t ReadLeaderHandle() {
    void* p = Hooks::ResolveRva(NavRva::LEADER_HANDLE);
    uint32_t h = 0;
    SafeReadU32(p, 0, &h);
    return h;
}

// Memory-only replica of FUN_003588b0 (+ inlined FUN_00263ff0). Every read is
// SEH-guarded; any structural failure resolves to null rather than dereferencing
// garbage.
void* ResolveHandle(uint32_t handle) {
    if (handle == 0) return nullptr;

    const uint32_t selector = (handle >> 16) & 0xf;
    const uint32_t slot     = handle & 0xffff;
    const uint32_t gen      = (handle >> 0x14) & 0x7ff;
    if (selector >= 5) return nullptr;

    void* tableBase = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!tableBase) return nullptr;
    void* table = static_cast<char*>(tableBase) +
                  static_cast<size_t>(selector) * NavRva::HANDLE_TABLE_STRIDE;

    void* guard   = PtrAt(table, NavRva::TBL_GUARD_OFF);
    void* entries = PtrAt(table, NavRva::TBL_ENTRIES_OFF);
    if (!guard || !entries) return nullptr;

    uint8_t active = 0;
    if (!SafeReadU8(table, NavRva::TBL_ACTIVE_OFF, &active) || (active & 1) == 0) return nullptr;

    uint32_t capacity = 0;
    if (!SafeReadU32(table, NavRva::TBL_CAPACITY_OFF, &capacity) || slot >= capacity) return nullptr;

    uint32_t count = 0;
    if (!SafeReadU32(entries, NavRva::ENTRIES_COUNT_OFF, &count) || slot >= count) return nullptr;

    void* obj = PtrAt(entries, NavRva::ENTRIES_SLOT0_OFF + slot * 8);
    if (!obj) return nullptr;

    uint16_t objGen = 0;
    if (!SafeReadU16(obj, NavRva::OBJ_GENERATION_OFF, &objGen)) return nullptr;
    if (static_cast<uint32_t>(objGen) != gen) return nullptr;  // stale handle

    return obj;
}

void* ReadLeaderSceneObject() {
    if (!IsFieldActive()) return nullptr;
    return ResolveHandle(ReadLeaderHandle());
}

void* ReadLeaderComponent() {
    void* sceneObj = ReadLeaderSceneObject();
    if (!sceneObj) return nullptr;
    void* comp = PtrAt(sceneObj, NavRva::SCENEOBJ_COMPONENT_OFF);
    if (!comp) return nullptr;
    uint32_t flags = 0;
    if (!SafeReadU32(comp, 0, &flags)) return nullptr;
    if ((flags & NavRva::COMPONENT_VALID_MASK) == 0) return nullptr;
    return comp;
}

LeaderChain CaptureLeaderChain() {
    LeaderChain c;
    c.fieldActive = IsFieldActive();
    c.handle      = ReadLeaderHandle();
    c.selector    = (c.handle >> 16) & 0xf;
    c.slot        = c.handle & 0xffff;
    c.generation  = (c.handle >> 0x14) & 0x7ff;
    if (!c.fieldActive || c.handle == 0) return c;

    c.sceneObj = ResolveHandle(c.handle);
    if (!c.sceneObj) return c;

    c.component = PtrAt(c.sceneObj, NavRva::SCENEOBJ_COMPONENT_OFF);
    if (!c.component) return c;

    SafeReadU32(c.component, 0, &c.componentFlags);
    c.valid = (c.componentFlags & NavRva::COMPONENT_VALID_MASK) != 0;
    return c;
}

// Live world position of ANY scene object: the transform node at sceneObj+0xB8, then
// its first three floats (cached world XYZ). The node is set for EVERY world-present
// object (categories 1-7, incl. NPCs and static gimmicks/gates); only pure triggers
// (category 0) have a null node. We read the raw chain and guard ONLY on node != 0 —
// deliberately NOT replicating the engine getter FUN_00265020's class-nibble gate
// ((*(u8)(sceneObj+3) >> 5) in {1,3}), which zeroes the result for a gate whose class
// byte isn't 1/3 (and would drop it from the scan). Memory-only, SEH-guarded.
bool ReadSceneObjectPos(void* sceneObj, FVec3& out) {
    if (!sceneObj) return false;

    void* node = PtrAt(sceneObj, NavRva::SCENEOBJ_XFORM_PTR);
    if (!node) return false;

    float x = 0, y = 0, z = 0;
    if (!SafeReadF32(node, NavRva::XFORM_POS_X, &x)) return false;
    if (!SafeReadF32(node, NavRva::XFORM_POS_Y, &y)) return false;
    if (!SafeReadF32(node, NavRva::XFORM_POS_Z, &z)) return false;

    // THE ENGINE'S INTERACTION ANCHOR, not the raw transform origin.
    //
    // `FUN_0025bad0:72-76` adds this offset to the target's position BEFORE both the distance gate
    // and the vertical band test, and `FUN_0025c230` does the same -- so for any object carrying the
    // flag, the point the engine measures interaction from is NOT where the transform sits. Reporting
    // the raw origin meant the mod described one point while the game judged another.
    //
    // Applied here, in the one function that feeds every entity position (entity_scan's two builders
    // and entity_list's live refresh), so the `/` describe, the `\` route and the `;` readout cannot
    // drift apart -- they are reading the same number.
    //
    // NOT ASSERTED: what writes this offset, or what it means semantically, is not established
    // (~0.9). The justification does not depend on it -- "report the point the engine measures from"
    // holds regardless -- but AnchorStats below counts how often the flag is actually set so the
    // first play session tells us instead of us assuming.
    uint8_t flag = 0;
    if (SafeReadU8(node, NavRva::XFORM_POS_OFFSET_FLAG, &flag) && (flag & 1) != 0) {
        float ox = 0, oy = 0, oz = 0;
        if (SafeReadF32(node, NavRva::XFORM_POS_OFFSET_X, &ox) &&
            SafeReadF32(node, NavRva::XFORM_POS_OFFSET_Y, &oy) &&
            SafeReadF32(node, NavRva::XFORM_POS_OFFSET_Z, &oz)) {
            x += ox; y += oy; z += oz;
            NoteAnchorOffset(ox, oy, oz);
        }
    } else {
        NoteAnchorPlain();
    }

    out = FVec3{ x, y, z };
    return true;
}

void GetAnchorStats(int& withOffset, int& plain, float& maxOffset) {
    withOffset = g_anchorOffsetCount;
    plain      = g_anchorPlainCount;
    maxOffset  = g_anchorMaxOffset;
}

bool ReadPlayerPos(FVec3& out) {
    return ReadSceneObjectPos(ReadLeaderSceneObject(), out);
}

// faceNode: the class-3 facing slot at node+0xA4 on the leader's transform node (the SAME node we
// read position from). Confirmed live in the iteration-2 diagnostic (tracked the camera 151/57/-10).
bool ReadPlayerFacing(float& outRad) {
    void* node = PtrAt(ReadLeaderSceneObject(), NavRva::SCENEOBJ_XFORM_PTR);
    if (!node) return false;
    return SafeReadF32(node, NavRva::XFORM_FACING_YAW, &outRad);
}

// Camera "up-direction" yaw: read the MOVEMENT matrix DAT_02aedf30 forward row (row 2 @ +0x20) and
// return atan2(-fwd.x, -fwd.z) — the direction a pure UP push moves the leader (worldMove =
// -stickY*row2). The (-,-) sign is the Phase-A best guess; the '-key diagnostic logs both signs vs
// the actual walked direction so it can be locked. A zero row means the camera slot wasn't refreshed
// this frame (no active camera) — treat as unavailable so callers keep the last good value.
bool ReadCameraForward(float& outRad) {
    float fx = 0.0f, fz = 0.0f;
    if (!SafeReadF32(Hooks::ResolveRva(NavRva::CAMERA_FWD_X), 0, &fx)) return false;
    if (!SafeReadF32(Hooks::ResolveRva(NavRva::CAMERA_FWD_Z), 0, &fz)) return false;
    if (fx == 0.0f && fz == 0.0f) return false;
    outRad = atan2f(-fx, -fz);
    return true;
}

// Live-or-last-good camera reference, with the leader's own facing as a final fallback. See the
// header for why every speech path must use this: the raw getter's failure mode, combined with
// callers ignoring its bool, silently produced a 180-degree reversal of every spoken direction.
// Written from the input thread and the game thread, so the cache is an atomic; a torn read is
// impossible and a one-frame-stale yaw is harmless.
//
// THREE SOURCES, in order, because a distance with no direction is useless to walk on and there must
// always be one: (1) the live camera row -- correct, this is where UP sends you; (2) the last row
// that WAS live -- at worst a frame stale; (3) `faceNode`, the leader's own facing, same
// atan2(x,z) convention. (3) is imperfect (it only updates while walking, and combat turns it toward
// the target) but it is a real orientation, which beats no direction at all.
bool ReadCameraForwardStable(float& outRad, const char** srcOut) {
    static std::atomic<float> s_lastGood{0.0f};
    static std::atomic<bool>  s_haveGood{false};
    const char* src = "none";
    float live = 0.0f;
    if (ReadCameraForward(live)) {
        s_lastGood.store(live, std::memory_order_relaxed);
        s_haveGood.store(true, std::memory_order_relaxed);
        outRad = live;
        src = "live";
        if (srcOut) *srcOut = src;
        return true;
    }
    if (s_haveGood.load(std::memory_order_relaxed)) {
        outRad = s_lastGood.load(std::memory_order_relaxed);
        if (srcOut) *srcOut = "held";
        return true;
    }
    const bool ok = ReadPlayerFacing(outRad);   // last resort: the leader's own heading
    if (srcOut) *srcOut = ok ? "face" : "none";
    return ok;
}

bool ReadCameraForwardStable(float& outRad) {
    return ReadCameraForwardStable(outRad, nullptr);
}

// One SEH-guarded snapshot of the movement frame. Each piece is independent: a missing
// camera or actor pointer only clears its own `have*` flag. All yaws use the game's own
// atan2(x, z) convention (camera per FUN_002565c0, move/heading per FUN_00358cb0), so the
// diagnostic's heading-vs-camera and move-vs-camera deltas are directly comparable.
bool ReadMoveFrame(MoveFrame& out) {
    out = MoveFrame{};
    // Persist the previous position so we can report the ACTUAL direction walked between two taps —
    // ground truth for the sign/handedness, independent of every facing/camera field. ReadMoveFrame
    // is called only from the '-key diagnostic, so this static tracks tap-to-tap movement.
    static bool  s_haveLast = false;
    static FVec3 s_lastPos{};
    bool any = false;

    out.havePos = ReadPlayerPos(out.pos);
    any |= out.havePos;

    if (out.havePos && s_haveLast) {
        const float dx = out.pos.x - s_lastPos.x;
        const float dz = out.pos.z - s_lastPos.z;
        out.dPosX = dx; out.dPosZ = dz;
        out.dPosDist = sqrtf(dx * dx + dz * dz);
        if (out.dPosDist > 1e-4f) {
            out.dPosYawF = atan2f(dx, dz);
            out.dPosYawB = atan2f(dx, -dz);
            out.haveDPos = true;
        }
    }
    if (out.havePos) { s_lastPos = out.pos; s_haveLast = true; }

    // Facing candidate (a): leader actor per-frame cache at actor+0x15C (NOT 0x160). Persists idle.
    void* leaderActor = PtrAt(Hooks::ResolveRva(NavRva::LEADER_ACTOR_PTR), 0);
    if (leaderActor) {
        float f = 0.0f;
        if (SafeReadF32(leaderActor, NavRva::ACTOR_FACING_CACHE, &f)) {
            out.faceCacheRad = f; out.haveFaceCache = true; any = true;
        }
    }

    // Facing candidate (b): the class-3 facing slot on the leader's transform node (the SAME
    // node we read position from), node+0xA4.
    void* node = PtrAt(ReadLeaderSceneObject(), NavRva::SCENEOBJ_XFORM_PTR);
    if (node) {
        float f = 0.0f;
        if (SafeReadF32(node, NavRva::XFORM_FACING_YAW, &f)) {
            out.faceNodeRad = f; out.haveFaceNode = true; any = true;
        }
    }

    // Camera-forward world yaw: scalar DAT_02aedf94 (radians, atan2(x,z) convention — directly
    // comparable to the facing yaws). 0.0 is a legal value, so we cannot reject it; the diagnostic
    // reveals whether it tracks the camera or is stuck.
    void* camYaw = Hooks::ResolveRva(NavRva::CAMERA_YAW_SCALAR);
    if (camYaw) {
        float c = 0.0f;
        if (SafeReadF32(camYaw, 0, &c)) { out.camLookRad = c; out.haveCamLook = true; any = true; }
    }

    // Camera-forward row of the MOVEMENT matrix (row 2). Log BOTH candidate signs so the walk pins
    // which equals the actual move direction; ReadCameraForward ships the (-,-) one.
    void* cfx = Hooks::ResolveRva(NavRva::CAMERA_FWD_X);
    void* cfz = Hooks::ResolveRva(NavRva::CAMERA_FWD_Z);
    if (cfx && cfz) {
        float fx = 0.0f, fz = 0.0f;
        if (SafeReadF32(cfx, 0, &fx) && SafeReadF32(cfz, 0, &fz) && !(fx == 0.0f && fz == 0.0f)) {
            out.camFwdX = fx; out.camFwdZ = fz;
            out.camFwdRawRad = atan2f(fx, fz);
            out.camFwdNegRad = atan2f(-fx, -fz);
            out.haveCamFwd = true; any = true;
        }
    }

    // Live world move vector (driver output; 0 while idle). Two float globals.
    void* mvx = Hooks::ResolveRva(NavRva::MOVE_VEC_X);
    void* mvz = Hooks::ResolveRva(NavRva::MOVE_VEC_Z);
    if (mvx && mvz) {
        float mx = 0, mz = 0;
        if (SafeReadF32(mvx, 0, &mx) && SafeReadF32(mvz, 0, &mz)) {
            out.moveX = mx; out.moveZ = mz; out.haveMove = true; any = true;
            if (mx * mx + mz * mz > 1e-6f) { out.moving = true; out.moveYawRad = atan2f(mx, mz); }
        }
    }

    return any;
}

} // namespace PlayerState

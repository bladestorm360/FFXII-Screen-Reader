#include "navigation/player_state.h"
#include "navigation/nav_rva.h"
#include "navigation/map_query.h"
#include "core/hooks.h"
#include "core/mem_read.h"

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
    out = FVec3{ x, y, z };
    return true;
}

bool ReadPlayerPos(FVec3& out) {
    return ReadSceneObjectPos(ReadLeaderSceneObject(), out);
}

// Facing yaw from the char component's embedded world matrix forward row
// (comp+0x100). Ground plane is X/Z; game north is -Z, so yaw = atan2(fwd.x,
// -fwd.z) — same convention as nav_common::BearingDeg, so the `;` facing readout
// and crow-flies bearings agree. Used by the facing readout + egocentric mode.
bool ReadPlayerYaw(float& outRadians) {
    void* comp = ReadLeaderComponent();
    if (!comp) return false;
    float fx = 0, fz = 0;
    if (!SafeReadF32(comp, NavRva::COMP_MATRIX_FWD + 0x00, &fx)) return false;
    if (!SafeReadF32(comp, NavRva::COMP_MATRIX_FWD + 0x08, &fz)) return false;
    outRadians = std::atan2(fx, -fz);
    return true;
}

} // namespace PlayerState

#include "navigation/player_state.h"
#include "navigation/nav_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"

#include <cmath>

using namespace MemRead;

namespace PlayerState {

bool IsFieldActive() {
    void* p = Hooks::ResolveRva(NavRva::FIELD_ACTIVE);
    uint8_t b = 0;
    if (!SafeReadU8(p, 0, &b)) return false;
    return (b & 0x10) != 0;
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

// Live world position via the engine's own getter (FUN_00265020): the scene
// object's transform pointer at +0xB8, then the 3 world-position floats. The
// controller (+0xD0 matrix) was only a writer of this same value — not needed.
bool ReadPlayerPos(FVec3& out) {
    void* sceneObj = ReadLeaderSceneObject();
    if (!sceneObj) return false;

    // Match the getter's type-nibble guard: (*(u8)(sceneObj+3) >> 5) in {1,3}.
    uint8_t typeByte = 0;
    if (!SafeReadU8(sceneObj, NavRva::SCENEOBJ_TYPE_BYTE, &typeByte)) return false;
    const uint8_t nib = typeByte >> 5;
    if (nib != 1 && nib != 3) return false;

    void* xform = PtrAt(sceneObj, NavRva::SCENEOBJ_XFORM_PTR);
    if (!xform) return false;

    float x = 0, y = 0, z = 0;
    if (!SafeReadF32(xform, NavRva::XFORM_POS_X, &x)) return false;
    if (!SafeReadF32(xform, NavRva::XFORM_POS_Y, &y)) return false;
    if (!SafeReadF32(xform, NavRva::XFORM_POS_Z, &z)) return false;
    out = FVec3{ x, y, z };
    return true;
}

// Facing yaw from the char component's embedded world matrix forward row
// (comp+0x100). Ground plane is X/Z, so yaw = atan2(fwd.x, fwd.z). Only the
// egocentric direction mode needs this; the exact sign/zero convention is
// confirmed against FUN_004686d0 at runtime. Cardinal bearings need no yaw.
bool ReadPlayerYaw(float& outRadians) {
    void* comp = ReadLeaderComponent();
    if (!comp) return false;
    float fx = 0, fz = 0;
    if (!SafeReadF32(comp, NavRva::COMP_MATRIX_FWD + 0x00, &fx)) return false;
    if (!SafeReadF32(comp, NavRva::COMP_MATRIX_FWD + 0x08, &fz)) return false;
    outRadians = std::atan2(fx, fz);
    return true;
}

} // namespace PlayerState

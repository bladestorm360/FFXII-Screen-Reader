#include "navigation/map_query.h"
#include "navigation/nav_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"

#include <cstdint>

namespace {

// FUN_003208c0(x, z, outY) -> bool (AL). MS x64: x=XMM0, z=XMM1, outY=R8.
typedef uint8_t(__fastcall* Pfn_GroundAt)(float x, float z, float* outY);
// FUN_00230b60(ctx0, outHit16, from[4], to[4], mask, flags) -> int (>=0 blocked, <0 clear).
typedef int(__fastcall* Pfn_SegTest)(void* ctx, void* out, const float* from,
                                     const float* to, uint16_t mask, uint32_t flags);

// ctx0 = the field-collision world pointer at DAT_0209a678, valid only when the manager
// gate DAT_0209a670 is set. Memory-only; both reads SEH-guarded via MemRead.
void* Ctx0() {
    if (!MemRead::PtrAt(Hooks::ResolveRva(NavRva::MAP_COLL_GATE), 0)) return nullptr;
    return MemRead::PtrAt(Hooks::ResolveRva(NavRva::MAP_COLL_CTX0), 0);
}

// POD-only SEH scopes: each call dereferences the walkmap; a fault (torn map mid-load /
// mid-teardown) degrades to a safe default rather than crashing.
static bool CallGroundAt(Pfn_GroundAt fn, float x, float z, float* outY) {
    __try { return fn(x, z, outY) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static int CallSegTest(Pfn_SegTest fn, void* ctx, void* out, const float* from,
                       const float* to, uint16_t mask, uint32_t flags) {
    __try { return fn(ctx, out, from, to, mask, flags); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }   // fault -> treat as clear
}

} // namespace

namespace MapQuery {

bool HasWorld() { return Ctx0() != nullptr; }

bool GroundAt(float x, float z, float& outY) {
    if (!HasWorld()) return false;
    Pfn_GroundAt fn = reinterpret_cast<Pfn_GroundAt>(Hooks::ResolveRva(NavRva::MAP_GROUND_AT));
    if (!fn) return false;
    float y = 0.0f;
    if (!CallGroundAt(fn, x, z, &y)) return false;
    outY = y;
    return true;
}

int SegmentHit(const FVec3& from, const FVec3& to, uint16_t mask, uint32_t flags) {
    void* ctx = Ctx0();
    if (!ctx) return -1;   // no world -> clear
    Pfn_SegTest fn = reinterpret_cast<Pfn_SegTest>(Hooks::ResolveRva(NavRva::MAP_SEG_TEST));
    if (!fn) return -1;
    // Equal-Y endpoints (both at from.y): for a vertical wall the query Y is irrelevant,
    // and holding Y flat stops a slope-climbing segment from clipping a rising floor poly.
    const float f[4] = { from.x, from.y, from.z, 1.0f };
    const float t[4] = { to.x,   from.y, to.z,   1.0f };
    float out[4] = {};
    return CallSegTest(fn, ctx, out, f, t, mask, flags);   // >=0 blocked, <0 clear
}

bool SegmentClear(const FVec3& from, const FVec3& to) {
    // Walk query class — matches exactly what the player's/NPCs' own wall feelers block on.
    return SegmentHit(from, to, NavRva::MAP_MASK_WALK, NavRva::MAP_SEG_FLAGS) < 0;
}

} // namespace MapQuery

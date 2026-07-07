#include "navigation/bullet_query.h"
#include "navigation/nav_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"

#include <atomic>
#include <cmath>

namespace {

// FUN_006a1a70(context, from[xyz], to[xyz], out[8], filterGroup) -> 0/1 in RAX.
typedef uint64_t(__fastcall* Pfn_RayCast)(void* ctx, const float* from,
                                          const float* to, float* out, int filter);

std::atomic<void*> g_ctx{nullptr};

// POD-only SEH scope: the call dereferences a game vtable, so an access
// violation (torn world during a physics step) degrades to "no hit".
static bool CallRayCast(void* ctx, const float* from, const float* to,
                        float* out8, int filter) {
    Pfn_RayCast fn = reinterpret_cast<Pfn_RayCast>(Hooks::ResolveRva(NavRva::RAYCAST_WRAPPER));
    if (!fn) return false;
    uint64_t hit = 0;
    __try {
        hit = fn(ctx, from, to, out8, filter);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return hit != 0;
}

} // namespace

namespace BulletQuery {

void SetContext(void* ctx) { g_ctx.store(ctx, std::memory_order_relaxed); }
void* GetContext() { return g_ctx.load(std::memory_order_relaxed); }
void Invalidate() { g_ctx.store(nullptr, std::memory_order_relaxed); }

bool HasWorld() {
    void* ctx = g_ctx.load(std::memory_order_relaxed);
    if (!ctx) return false;
    return MemRead::PtrAt(ctx, NavRva::CTX_WORLD_OFF) != nullptr;
}

bool Ray(const FVec3& from, const FVec3& to, FVec3& outHit, FVec3& outNormal, int filterGroup) {
    void* ctx = g_ctx.load(std::memory_order_relaxed);
    if (!ctx) return false;
    // Guard the world ourselves — the wrapper returns an uninitialized register
    // when *(ctx+0x60) is null (title / between maps).
    if (!MemRead::PtrAt(ctx, NavRva::CTX_WORLD_OFF)) return false;

    const float f[3] = { from.x, from.y, from.z };
    const float t[3] = { to.x, to.y, to.z };
    float out8[8] = {};
    if (!CallRayCast(ctx, f, t, out8, filterGroup)) return false;

    outHit    = FVec3{ out8[0], out8[1], out8[2] };
    outNormal = FVec3{ out8[4], out8[5], out8[6] };
    return true;
}

bool FloorBelow(const FVec3& at, float upPad, float downDist, float& outFloorY) {
    const FVec3 from{ at.x, at.y + upPad,   at.z };
    const FVec3 to  { at.x, at.y - downDist, at.z };
    FVec3 hit, normal;
    if (!Ray(from, to, hit, normal)) return false;
    outFloorY = hit.y;
    return true;
}

bool HorizontalClear(const FVec3& from, const FVec3& to, float margin) {
    const FVec3 flatTo{ to.x, from.y, to.z };
    FVec3 hit, normal;
    if (!Ray(from, flatTo, hit, normal)) return true;   // no hit / no world -> clear
    const float hdx = hit.x - from.x, hdz = hit.z - from.z;
    const float hitD = std::sqrt(hdx * hdx + hdz * hdz);
    const float tdx = flatTo.x - from.x, tdz = flatTo.z - from.z;
    const float toD = std::sqrt(tdx * tdx + tdz * tdz);
    return hitD >= toD - margin;                          // hit at/behind target -> clear
}

} // namespace BulletQuery

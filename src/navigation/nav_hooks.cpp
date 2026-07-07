#include "navigation/nav_hooks.h"
#include "navigation/nav_rva.h"
#include "navigation/bullet_query.h"
#include "core/hooks.h"
#include "core/logger.h"

#include <cstdint>
#include <cstdio>

namespace {

// FUN_006a0310(context) builds the per-map Bullet world into *(context+0x60) and
// returns a status code. We call the original, then cache the context.
typedef uint64_t(__fastcall* Pfn_BuildWorld)(void* ctx);
Pfn_BuildWorld s_origBuildWorld = nullptr;
void* s_lastLoggedCtx = nullptr;

uint64_t __fastcall HookedBuildWorld(void* ctx) {
    uint64_t r = s_origBuildWorld ? s_origBuildWorld(ctx) : 0;
    BulletQuery::SetContext(ctx);
    if (ctx != s_lastLoggedCtx) {   // O(unique map load), not per-call spam
        s_lastLoggedCtx = ctx;
        char msg[96];
        snprintf(msg, sizeof(msg), "physics context captured: %p (map-load)", ctx);
        Log::Write("NAV", msg);
    }
    return r;
}

} // namespace

namespace NavHooks {

bool Init() {
    bool ok = Hooks::InstallTyped(NavRva::BUILD_WORLD, &HookedBuildWorld, &s_origBuildWorld);
    Log::Write("NAV", ok ? "world-builder hook installed (ctx capture)"
                         : "world-builder hook FAILED to install");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(NavRva::BUILD_WORLD);
    BulletQuery::Invalidate();
}

} // namespace NavHooks

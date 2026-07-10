#include "navigation/nav_hooks.h"
#include "navigation/nav_rva.h"
#include "navigation/bullet_query.h"
#include "navigation/path_planner.h"
#include "navigation/entity_list.h"
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

// FUN_0022a770(): the per-field-frame tick, entered once per rendered frame in the
// real-time field/walking state. We drain a pending route request at ENTRY — reading
// only the previous frame's fully-settled state. Unlike the old FUN_00314020 drain,
// there is no accumulator / else-branch bypass that can hold the drain silent during
// scripted sequences. OnGameFrame is O(1) when nothing is pending and hard-gates on
// IsFieldNavSafe() before touching any map data. Return-transparent (a wrong return
// type would clobber RAX and corrupt the engine's frame state).
typedef uint64_t(__fastcall* Pfn_FieldFrame)();
Pfn_FieldFrame s_origFieldFrame = nullptr;

uint64_t __fastcall HookedFieldFrame() {
    EntityList::OnFieldFrame();   // auto-rescan when handle-table containers stream in (fixes empty list after a save-load)
    PathPlanner::OnGameFrame();
    return s_origFieldFrame ? s_origFieldFrame() : 1;
}

// FUN_002695a0(): field-global teardown. We invalidate the cached physics world and
// bump the route planner's map epoch at ENTRY — before the game zeroes the leader
// pointer / frees the world / clears the field-live bit — so the planner can never
// raycast a freed world or plan against a dying map. Returns void; keep it void.
typedef void(__fastcall* Pfn_Teardown)();
Pfn_Teardown s_origTeardown = nullptr;

void __fastcall HookedTeardown() {
    BulletQuery::Invalidate();
    PathPlanner::OnMapTeardown();
    if (s_origTeardown) s_origTeardown();
}

} // namespace

namespace NavHooks {

bool Init() {
    bool ok = Hooks::InstallTyped(NavRva::BUILD_WORLD, &HookedBuildWorld, &s_origBuildWorld);
    Log::Write("NAV", ok ? "world-builder hook installed (ctx capture)"
                         : "world-builder hook FAILED to install");

    // Game-thread route-planner hooks (Layer 3). Both are non-fatal if they fail —
    // the planner simply never runs / never invalidates, but the rest of nav is fine.
    bool okStep = Hooks::InstallTyped(NavRva::FIELD_FRAME, &HookedFieldFrame, &s_origFieldFrame);
    Log::Write("NAV", okStep ? "field-frame hook installed (route drain, game thread)"
                             : "field-frame hook FAILED to install");
    bool okTear = Hooks::InstallTyped(NavRva::FIELD_TEARDOWN, &HookedTeardown, &s_origTeardown);
    Log::Write("NAV", okTear ? "field-teardown hook installed (world invalidation)"
                             : "field-teardown hook FAILED to install");

    return ok;   // the ctx-capture hook is the one nav depends on
}

void Shutdown() {
    Hooks::Uninstall(NavRva::BUILD_WORLD);
    Hooks::Uninstall(NavRva::FIELD_FRAME);
    Hooks::Uninstall(NavRva::FIELD_TEARDOWN);
    BulletQuery::Invalidate();
}

} // namespace NavHooks

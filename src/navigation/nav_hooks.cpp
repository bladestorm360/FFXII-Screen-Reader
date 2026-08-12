#include "navigation/nav_hooks.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "navigation/bullet_query.h"
#include "navigation/path_planner.h"
#include "navigation/audio_beacon.h"
#include "navigation/auto_walk.h"
#include "navigation/nav_probe.h"
#include "navigation/entity_list.h"
#include "navigation/sneak_assist.h"
#include "navigation/shout_meter.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/stall_probe.h"
#include "core/mem_read.h"
#include "core/frame_probe.h"

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
    STALL_SCOPE("NavHooks::HookedBuildWorld");   // from here down is OURS; the call above is the game's
    BulletQuery::SetContext(ctx);
    if (ctx != s_lastLoggedCtx) {   // O(unique map load), not per-call spam
        s_lastLoggedCtx = ctx;
        // Log the world pointer too (the orig has run, so *(ctx+0x60) is populated) — a
        // later "not nav-safe" failMask bit-6 (world) is only interpretable if we know
        // whether the build actually produced a non-null world here.
        void* world = MemRead::PtrAt(ctx, NavRva::CTX_WORLD_OFF);
        char msg[128];
        snprintf(msg, sizeof(msg), "world-builder FIRED ctx=%p world=%p status=%llu (map-load)",
                 ctx, world, static_cast<unsigned long long>(r));
        Log::Write("NAV", msg);
    }
    return r;
}

// FUN_006a1a70(ctx, from[3], to[3], out[8], filter): the raycast wrapper — reads the world
// at *(ctx+0x60). The game calls it during actor/camera collision whenever a Bullet world
// exists, so we snapshot arg0 (the EXACT ctx our own raycasts need). Self-validating: only
// cache while we lack a live world AND *(ctx+0x60) != 0, so we can never cache a bogus ctx
// and never oscillate off a good one (teardown Invalidates -> we re-grab next raycast).
// Our own BulletQuery calls pass this hook too, but HasWorld() is true then -> no-op.
typedef uint64_t(__fastcall* Pfn_RayCast)(void* ctx, const float* from, const float* to,
                                          float* out, int filter);
Pfn_RayCast s_origRayCast = nullptr;
void* s_lastRayCtx = nullptr;

uint64_t __fastcall HookedRayCast(void* ctx, const float* from, const float* to,
                                  float* out, int filter) {
    {
        STALL_SCOPE("NavHooks::HookedRayCast");   // scoped block: excludes the trampoline below
    if (ctx && !BulletQuery::HasWorld()) {
        void* world = MemRead::PtrAt(ctx, NavRva::CTX_WORLD_OFF);
        if (world) {
            BulletQuery::SetContext(ctx);
            if (ctx != s_lastRayCtx) {
                s_lastRayCtx = ctx;
                char msg[128];
                snprintf(msg, sizeof(msg), "raycast-wrapper ctx captured: ctx=%p world=%p", ctx, world);
                Log::Write("NAV", msg);
            }
        }
    }
    }
    return s_origRayCast ? s_origRayCast(ctx, from, to, out, filter) : 0;
}

// FUN_006a5c00(p1, p2, p3): the char-controller ground/slope resolve. p1+8 is the physics
// ctx (the fn guards *(p1+8)!=0 then passes it to the raycast). Fires once/frame per walking
// actor — captures the field ctx wherever an actor moves on a Bullet world, even in scenes
// the raycast wrapper alone might not exercise. Same self-validating cache discipline.
typedef uint64_t(__fastcall* Pfn_CharGround)(void* p1, float* p2, float p3);
Pfn_CharGround s_origCharGround = nullptr;
void* s_lastGroundCtx = nullptr;

uint64_t __fastcall HookedCharGround(void* p1, float* p2, float p3) {
    {
        STALL_SCOPE("NavHooks::HookedCharGround");   // scoped block: excludes the trampoline below
    if (p1 && !BulletQuery::HasWorld()) {
        void* ctx = MemRead::PtrAt(p1, 8);
        if (ctx) {
            void* world = MemRead::PtrAt(ctx, NavRva::CTX_WORLD_OFF);
            if (world) {
                BulletQuery::SetContext(ctx);
                if (ctx != s_lastGroundCtx) {
                    s_lastGroundCtx = ctx;
                    char msg[128];
                    snprintf(msg, sizeof(msg), "char-ground ctx captured: ctx=%p world=%p", ctx, world);
                    Log::Write("NAV", msg);
                }
            }
        }
    }
    }
    return s_origCharGround ? s_origCharGround(p1, p2, p3) : 0;
}

// FUN_0069f070(ctx, stepCtx): the per-world physics STEP — runs every frame the field
// world advances, regardless of when we installed (unlike the one-shot BUILD_WORLD hook,
// which misses a save-load into an already-built area, leaving the world ctx uncaptured).
// arg0 is the SAME PPhysicsWorld ctx FUN_006a1a70 uses (both read the Bullet world at
// *(ctx+0x60); this step even lazily calls FUN_006a0310(ctx) to build it). We only READ
// arg0 and cache it; return-transparent (a wrong return type would clobber RAX).
typedef uint64_t(__fastcall* Pfn_WorldStep)(void* ctx, void* stepCtx);
Pfn_WorldStep s_origWorldStep = nullptr;
void* s_lastStepCtx = nullptr;

uint64_t __fastcall HookedWorldStep(void* ctx, void* stepCtx) {
    {
        STALL_SCOPE("NavHooks::HookedWorldStep");   // scoped block: excludes the trampoline below
    if (ctx) {
        BulletQuery::SetContext(ctx);
        if (ctx != s_lastStepCtx) {   // O(unique world), not per-frame spam
            s_lastStepCtx = ctx;
            void* world = MemRead::PtrAt(ctx, NavRva::CTX_WORLD_OFF);
            char msg[128];
            snprintf(msg, sizeof(msg), "physics-step ctx captured: ctx=%p world=%p (per-frame)",
                     ctx, world);
            Log::Write("NAV", msg);
        }
    }
    }
    return s_origWorldStep ? s_origWorldStep(ctx, stepCtx) : 0;
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
    StallProbe::GapTick("anchor:fieldframe", /*gapWarnMs=*/80.0);
    // One relaxed increment. Counts how often the field tick fires so it can be compared against
    // the game's OWN render-frame counter — the measurement the tier-C constants were guessed at
    // without. See core/frame_probe.h.
    FrameProbe::OnFieldFrame();
    {
        // OURS ONLY. This scope used to span s_origFieldFrame() below, so it reported the GAME's
        // entire per-frame field tick as mod cost -- the source of the bogus "223ms / 245ms single
        // call" readings that sent two investigations down the wrong path.
        STALL_SCOPE("NavHooks::HookedFieldFrame");
        EntityList::OnFieldFrame();   // auto-rescan when handle-table containers stream in (fixes empty list after a save-load)
        { STALL_SCOPE("PathPlanner::OnGameFrame"); PathPlanner::OnGameFrame(); }
        // AFTER the planner, so a route seeded on this frame starts pinging immediately rather than
        // a frame later. Returns on a single atomic load whenever no beacon is running, which is
        // the common case; see the polled-monitor note in audio_beacon.h.
        { STALL_SCOPE("AudioBeacon::OnGameFrame"); AudioBeacon::OnGameFrame(); }
        // AFTER the beacon, so the leg snapshot auto-walk steers by is post-advance -- same-frame
        // fresh, never a corner behind. One relaxed load when idle.
        { STALL_SCOPE("AutoWalk::OnGameFrame"); AutoWalk::OnGameFrame(); }
        // The `'` probe drains here rather than running on the input thread: Gate B needs
        // MapQuery::GroundAt, which is a game call. O(1) when nothing is pending.
        NavProbe::OnGameFrame();
        // Shout minigame: refreshes which map script is live, drains the B/N keys, and closes a
        // finished gauge burst. Same reason as the probe above -- reading npcdic names and live
        // transforms is a game-thread job. A few pointer reads when nothing is happening.
        { STALL_SCOPE("ShoutMeter::OnFieldFrame"); ShoutMeter::OnFieldFrame(); }
    }
    return s_origFieldFrame ? s_origFieldFrame() : 1;
}

// FUN_002695a0(): field-global teardown. We invalidate the cached physics world and
// bump the route planner's map epoch at ENTRY — before the game zeroes the leader
// pointer / frees the world / clears the field-live bit — so the planner can never
// raycast a freed world or plan against a dying map. Returns void; keep it void.
typedef void(__fastcall* Pfn_Teardown)();
Pfn_Teardown s_origTeardown = nullptr;

void __fastcall HookedTeardown() {
    {
        STALL_SCOPE("NavHooks::HookedTeardown");
        BulletQuery::Invalidate();
        PathPlanner::OnMapTeardown();
        // Sneak assist never survives a map change (S109) -- the map being torn down is the only
        // one it was armed for, and the next one has not authorized anything.
        SneakAssist::OnMapTeardown();
        // Same rule: a script-module identity and a resolved variable address belong to the map
        // they were read on, and the instant-fill latch must re-arm for the next one.
        ShoutMeter::OnMapTeardown();
    }
    if (s_origTeardown) s_origTeardown();
}

} // namespace

namespace NavHooks {

bool Init() {
    bool ok = Hooks::InstallTyped(NavRva::BUILD_WORLD, &HookedBuildWorld, &s_origBuildWorld);
    Log::Write("NAV", ok ? "world-builder hook installed (ctx capture)"
                         : "world-builder hook FAILED to install");

    // Per-frame ctx capture (the reliable one): the physics step fires every field frame,
    // so it captures the world ctx even on a save-load into an already-built area, which
    // the one-shot builder above misses. Non-fatal if it fails.
    bool okWorldStep = Hooks::InstallTyped(NavRva::WORLD_STEP, &HookedWorldStep, &s_origWorldStep);
    Log::Write("NAV", okWorldStep ? "world-step hook installed (per-frame ctx capture)"
                                  : "world-step hook FAILED to install");

    // The reliable captures: these fire while an actor WALKS on a Bullet world (the
    // builder/step above only run when the scene actually builds a physics region).
    bool okRayCast = Hooks::InstallTyped(NavRva::RAYCAST_WRAPPER, &HookedRayCast, &s_origRayCast);
    Log::Write("NAV", okRayCast ? "raycast-wrapper hook installed (ctx capture)"
                                : "raycast-wrapper hook FAILED to install");
    bool okCharGround = Hooks::InstallTyped(NavRva::CHAR_GROUND_RESOLVE, &HookedCharGround, &s_origCharGround);
    Log::Write("NAV", okCharGround ? "char-ground hook installed (ctx capture)"
                                   : "char-ground hook FAILED to install");

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
    Hooks::Uninstall(NavRva::WORLD_STEP);
    Hooks::Uninstall(NavRva::RAYCAST_WRAPPER);
    Hooks::Uninstall(NavRva::CHAR_GROUND_RESOLVE);
    Hooks::Uninstall(NavRva::FIELD_FRAME);
    Hooks::Uninstall(NavRva::FIELD_TEARDOWN);
    BulletQuery::Invalidate();
}

} // namespace NavHooks

#include "navigation/sneak_assist.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>

#include "core/hooks.h"
#include "core/logger.h"
#include "navigation/map_names.h"
#include "navigation/nav_rva.h"
#include "navigation/path_danger.h"
#include "ui/mod_menu.h"

namespace SneakAssist {
namespace {

// FUN_003448f0(ctx, _, _, vmState) -- the script native the 0x0290 slot dispatches. Only the first
// argument is read here (the VM context that carries the return slot); the rest are passed through.
using ScriptDistanceFn = void(__fastcall*)(void*, void*, void*, void*);
ScriptDistanceFn s_orig = nullptr;

// Where FUN_0026b4c0 stores a native's result, replicated exactly:
//     base = *(u64*)(ctx + 0xA8);  idx = *(i8*)(ctx + 0x11);
//     *(u32*)(base + 0xC + idx*0x28) = value;   *(u8*)(base + 0x15 + idx*0x28) = 3;
// We rewrite ONLY the value word, leaving the type tag the game just wrote.
constexpr uintptr_t OFF_CTX_SLOTBASE = 0xA8;
constexpr uintptr_t OFF_CTX_SLOTIDX  = 0x11;
constexpr uintptr_t OFF_SLOT_VALUE   = 0x0C;
constexpr size_t    SLOT_STRIDE      = 0x28;

// The clamp. Map 568's room is ~40 m across and the mod's own routes speak tens of metres, so this
// is orders of magnitude beyond any threshold a proximity check could carry -- while staying far
// from float extremes, so an unexpected downstream arithmetic use cannot produce an inf/NaN.
constexpr float kClampMetres = 9999.0f;

std::atomic<bool> s_installed{false};
// Log-only, and reset on every disarm: the watcher polls per frame, so without this the first-clamp
// evidence would be O(frames). This suppresses a LOG line, never speech (CLAUDE.md's log-volume
// exception -- the per-frame producer is the map script's own watcher routine).
std::atomic<bool> s_loggedThisArming{false};

bool ToggleOn() { return ModMenu::SneakAssistOn(); }

// SEH-guarded: the ctx comes from the game's VM, and a torn/streaming pointer must degrade to
// "leave the value alone", never to a fault inside a native call.
bool ClampResultSlot(void* ctx, float* outOriginal, bool* outWasFloat) {
    __try {
        auto base = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(ctx) + OFF_CTX_SLOTBASE);
        if (!base) return false;
        const int idx = *reinterpret_cast<int8_t*>(reinterpret_cast<uint8_t*>(ctx) + OFF_CTX_SLOTIDX);
        if (idx < 0 || idx > 63) return false;
        auto* value = reinterpret_cast<uint32_t*>(base + OFF_SLOT_VALUE +
                                                  static_cast<uintptr_t>(idx) * SLOT_STRIDE);

        // WHICH REPRESENTATION -- MEASURED PER CALL, NOT GUESSED. FUN_004686d0 returns sqrtf's float,
        // and FUN_0026b4c0 stores a 32-bit word; the decompile does not show whether the float is
        // stored as bits or converted to an int, and a wrong guess would write a nonsense number. The
        // two readings are cleanly separable for a real distance, so the original value decides:
        // 8.13 m is 0x41022D0E, which reads as 1.09e9 when taken as an int; the integer 8 reads as
        // 1.1e-44 when taken as a float. Anything in [0.001, 100000) is the float reading.
        const uint32_t raw = *value;
        float asFloat = 0.0f;
        memcpy(&asFloat, &raw, sizeof(asFloat));
        const bool isFloat = std::isfinite(asFloat) &&
                             std::fabs(asFloat) >= 0.001f && std::fabs(asFloat) < 100000.0f;

        if (isFloat) {
            float clamp = kClampMetres;
            uint32_t bits = 0;
            memcpy(&bits, &clamp, sizeof(bits));
            *value = bits;
            if (outOriginal) *outOriginal = asFloat;
        } else {
            *value = static_cast<uint32_t>(kClampMetres);
            if (outOriginal) *outOriginal = static_cast<float>(raw);
        }
        if (outWasFloat) *outWasFloat = isFloat;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall HookedScriptDistance(void* ctx, void* a2, void* a3, void* vm) {
    // THE UNARMED PATH IS THE FIRST BRANCH -- with the toggle off, this function is the original
    // plus one relaxed atomic load, and no write below is reachable.
    if (!ToggleOn()) {
        s_loggedThisArming.store(false, std::memory_order_relaxed);
        if (s_orig) s_orig(ctx, a2, a3, vm);
        return;
    }
    if (!PathDanger::MapHasRow(static_cast<uint32_t>(MapNames::CurrentMapId()))) {
        if (s_orig) s_orig(ctx, a2, a3, vm);
        return;
    }

    // Run the game's own native first: it pops its arguments and writes the result slot, so the VM
    // stack stays exactly as the engine left it. Only then is the stored value replaced.
    if (s_orig) s_orig(ctx, a2, a3, vm);
    if (!ctx) return;

    float original = 0.0f;
    bool  wasFloat = false;
    const bool ok = ClampResultSlot(ctx, &original, &wasFloat);

    if (!s_loggedThisArming.exchange(true, std::memory_order_relaxed)) {
        char m[208];
        snprintf(m, sizeof(m),
                 "clamp ACTIVE on map %d: script distance %.2f -> %.0f (%s slot)%s -- "
                 "first clamp this arming; the watcher polls per frame so later ones are silent",
                 MapNames::CurrentMapId(), original, kClampMetres,
                 wasFloat ? "float" : "int", ok ? "" : "  <== SLOT UNREADABLE, value left alone");
        Log::Write("SNEAK", m);
    }
}

} // namespace

bool Init() {
    const bool ok = Hooks::InstallTyped(NavRva::SCRIPT_DISTANCE, &HookedScriptDistance, &s_orig);
    s_installed.store(ok, std::memory_order_release);
    Log::Write("SNEAK", ok ? "script-distance hook installed (sneak assist available; default OFF)"
                           : "script-distance hook FAILED to install -- sneak assist unavailable");
    return ok;
}

void Shutdown() {
    s_installed.store(false, std::memory_order_release);
}

bool ArmedHere() {
    return s_installed.load(std::memory_order_acquire) && ToggleOn() &&
           PathDanger::MapHasRow(static_cast<uint32_t>(MapNames::CurrentMapId()));
}

} // namespace SneakAssist

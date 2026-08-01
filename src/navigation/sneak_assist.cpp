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
// ---- IS THE HOOKED NATIVE EVER CALLED? (S110 -- log-only, toggle-independent) --------------------
// The first play test armed the toggle on map 568, ran the minigame through three complete
// shout/capture cycles, and produced ZERO clamp lines. Toggle on + map covered + hook installed
// should have logged one. Something upstream of the clamp is wrong, and exactly two hypotheses fit:
// the script never calls this native during the sequence (so the capture is driven by something
// else -- the map's rect/touch tests are the obvious candidate), or the hook is not on the function
// the VM dispatches. **These counters tell the two apart with one play session**, and they run
// regardless of the toggle so the answer does not depend on the feature working.
std::atomic<uint32_t> s_callsThisMap{0};
std::atomic<bool>     s_loggedFirstCall{false};
// Log-only, and reset on every disarm: the watcher polls per frame, so without this the first-clamp
// evidence would be O(frames). This suppresses a LOG line, never speech (CLAUDE.md's log-volume
// exception -- the per-frame producer is the map script's own watcher routine).
std::atomic<bool> s_loggedThisArming{false};

bool ToggleOn() { return ModMenu::SneakAssistOn(); }

// The startup safety net for the persisted value. The toggle is forced off on every map teardown,
// but a crash, a kill, or an old settings file could leave `sneak_assist=1` on disk and hand the
// next launch an armed feature nobody switched on this session. Applied once in Init().
void ForceOffAtStartup() {
    if (!ModMenu::SneakAssistOn()) return;
    ModMenu::SetSilently(ModMenu::SettingId::SneakAssist, 0);
    Log::Write("SNEAK", "startup: stored value was On -- forced OFF (it is never on by default)");
}

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
    // Log-only census (S110). One relaxed increment per call, and ONE log line per map load -- the
    // script polls this, so anything per-call would be O(frames). Deliberately ABOVE the toggle
    // check: the question it answers ("is this native called at all?") must not depend on the
    // feature being switched on.
    s_callsThisMap.fetch_add(1, std::memory_order_relaxed);
    if (!s_loggedFirstCall.exchange(true, std::memory_order_relaxed)) {
        char m[160];
        snprintf(m, sizeof(m), "native FIRED on map %d (first call this map) -- the hook is on a "
                               "function the script actually calls", MapNames::CurrentMapId());
        Log::Write("SNEAK", m);
    }

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
    ForceOffAtStartup();
    const bool ok = Hooks::InstallTyped(NavRva::SCRIPT_DISTANCE, &HookedScriptDistance, &s_orig);
    s_installed.store(ok, std::memory_order_release);
    Log::Write("SNEAK", ok ? "script-distance hook installed (sneak assist available; default OFF)"
                           : "script-distance hook FAILED to install -- sneak assist unavailable");
    return ok;
}

void Shutdown() {
    s_installed.store(false, std::memory_order_release);
}

bool AvailableHere() {
    return PathDanger::MapHasRow(static_cast<uint32_t>(MapNames::CurrentMapId()));
}

bool ArmedHere() {
    return s_installed.load(std::memory_order_acquire) && ToggleOn() && AvailableHere();
}

void OnMapTeardown() {
    // The census verdict for the map being left (S110, log-only). A ZERO here on a covered map
    // whose sequence the player actually played is the falsifier: it means the script never calls
    // this native, so no clamp of it could ever have worked and the capture is driven by something
    // else. Printed for EVERY map, because "which maps use it at all" is the same question.
    {
        const uint32_t n = s_callsThisMap.exchange(0, std::memory_order_relaxed);
        s_loggedFirstCall.store(false, std::memory_order_relaxed);
        char m[144];
        snprintf(m, sizeof(m), "map %d census: the distance native was called %u time(s) while it "
                               "was loaded", MapNames::CurrentMapId(), n);
        Log::Write("SNEAK", m);
    }
    // Unconditional: read the toggle, and if it is on, put it back off. Checked on EVERY teardown
    // rather than only when leaving a covered map, because the state that matters is "armed while
    // the player walks into somewhere new", and a map load is exactly where that would happen.
    if (!ModMenu::SneakAssistOn()) return;
    ModMenu::SetSilently(ModMenu::SettingId::SneakAssist, 0);
    s_loggedThisArming.store(false, std::memory_order_relaxed);
    Log::Write("SNEAK", "map change: sneak assist forced OFF (it never carries across a map)");
}

} // namespace SneakAssist

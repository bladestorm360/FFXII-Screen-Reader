#include "core/message_macro.h"

#include <atomic>
#include <cstdio>

#include "core/hooks.h"
#include "core/logger.h"

namespace MessageMacro {
namespace {

// FUN_002E1B70(slot, index, valA, valB) -- FOUR parameters, counted from the CALLEE's own decompile
// as the S129 rule requires. The native shim above it (FUN_0034CF20) pops four VM values and passes
// them in script order, so these are literally `setmesmacro`'s arguments.
constexpr uint32_t RVA_SET_MACRO = 0x1C1B70;

typedef void(__fastcall* Pfn_SetMacro)(int, int, uint32_t, uint32_t);
Pfn_SetMacro s_orig = nullptr;

// Written on the game thread (the script VM), read on whichever thread decodes text. Relaxed is
// enough: each is a lone 32-bit value with no ordering relationship to the other, and the worst a
// torn pair could produce is one line printing the previous number.
std::atomic<int32_t> s_valA{0};
std::atomic<int32_t> s_valB{0};
std::atomic<bool>    s_have{false};
std::atomic<bool>    s_logged{false};

void __fastcall HookedSetMacro(int slot, int index, uint32_t a, uint32_t b) {
    s_valA.store(static_cast<int32_t>(a), std::memory_order_relaxed);
    s_valB.store(static_cast<int32_t>(b), std::memory_order_relaxed);
    s_have.store(true, std::memory_order_release);

    // ONE line, the first time only. WHICH of the two words the printer uses is not settled by the
    // decompile, so both are recorded and a single play pass decides it -- the same measure-rather-
    // than-guess shape the gauge work used. Per-call logging would be O(every message in the game).
    if (!s_logged.exchange(true, std::memory_order_relaxed)) {
        char m[176];
        snprintf(m, sizeof(m),
                 "first macro write: slot=%d index=%d valA=%u valB=%u -- the spoken number is valA; "
                 "if a line reads wrong, valB is the other candidate",
                 slot, index, a, b);
        Log::Write("MESMACRO", m);
    }
    if (s_orig) s_orig(slot, index, a, b);
}

} // namespace

bool Latest(int32_t* outValue, int32_t* outOther) {
    if (!s_have.load(std::memory_order_acquire)) return false;
    if (outValue) *outValue = s_valA.load(std::memory_order_relaxed);
    if (outOther) *outOther = s_valB.load(std::memory_order_relaxed);
    return true;
}

bool Init() {
    const bool ok = Hooks::InstallTyped(RVA_SET_MACRO, &HookedSetMacro, &s_orig);
    Log::Write("MESMACRO", ok
        ? "setmesmacro hook installed (dialogue macros can now be spoken)"
        : "setmesmacro hook FAILED -- dialogue macros stay blank, as before");
    return ok;
}

} // namespace MessageMacro

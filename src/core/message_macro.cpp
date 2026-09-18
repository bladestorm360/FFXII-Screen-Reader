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

// THE TABLE, kept the shape the game stores it in: FUN_002E1B70 clamps slot to [0,7] and addresses
// `(slot*0x20 + index)*8`, so a slot holds 0x20 indices. Same thread discipline as the pair above --
// written by the script VM, read by whichever thread decodes text, each cell a lone 32-bit value.
constexpr int kSlots   = 8;
constexpr int kIndices = 0x20;
std::atomic<int32_t> s_tblVal[kSlots][kIndices];
std::atomic<int32_t> s_tblKind[kSlots][kIndices];
std::atomic<bool>    s_tblHave[kSlots][kIndices];
std::atomic<int>     s_lastSlot{-1};

void __fastcall HookedSetMacro(int slot, int index, uint32_t a, uint32_t b) {
    if (slot >= 0 && slot < kSlots && index >= 0 && index < kIndices) {
        s_tblKind[slot][index].store(static_cast<int32_t>(a), std::memory_order_relaxed);
        s_tblVal[slot][index].store(static_cast<int32_t>(b), std::memory_order_relaxed);
        s_tblHave[slot][index].store(true, std::memory_order_relaxed);
        s_lastSlot.store(slot, std::memory_order_release);
    }
    s_valA.store(static_cast<int32_t>(a), std::memory_order_relaxed);
    s_valB.store(static_cast<int32_t>(b), std::memory_order_relaxed);
    s_have.store(true, std::memory_order_release);

    // ONE line, the first time only -- per-call logging would be O(every message in the game).
    //
    // THIS LINE IS WHY THE VALUE IS RIGHT NOW. The first build spoke `valA` and logged both, because
    // the decompile does not say which word the printer uses. The play log read `valA=0 valB=2`
    // while the screen said "2 Bhujerbans heed your words" -- so the pair is (kind, value), and the
    // measurement cost one run instead of a guessing round. `kind != 0` is worth noticing: it means
    // a macro TYPE this has never seen, and the number may not be a plain integer.
    if (!s_logged.exchange(true, std::memory_order_relaxed)) {
        char m[192];
        snprintf(m, sizeof(m),
                 "first macro write: slot=%d index=%d kind=%u value=%u -- `value` is what is spoken; "
                 "a non-zero kind means a macro type this build has not seen",
                 slot, index, a, b);
        Log::Write("MESMACRO", m);
    }
    if (s_orig) s_orig(slot, index, a, b);
}

} // namespace

bool Latest(int32_t* outValue, int32_t* outKind) {
    if (!s_have.load(std::memory_order_acquire)) return false;
    if (outValue) *outValue = s_valB.load(std::memory_order_relaxed);   // the VALUE (measured)
    if (outKind)  *outKind  = s_valA.load(std::memory_order_relaxed);   // the kind selector
    return true;
}

bool ValueAt(int index, int32_t* outValue, int32_t* outKind) {
    const int slot = s_lastSlot.load(std::memory_order_acquire);
    if (slot < 0 || index < 0 || index >= kIndices) return false;
    if (!s_tblHave[slot][index].load(std::memory_order_relaxed)) {
        // A miss is the shape a WRONG SLOT takes, so say so once instead of printing another row's
        // number. One line per session: a real miss repeats every frame the prompt is on screen.
        static std::atomic<bool> s_missLogged{false};
        if (!s_missLogged.exchange(true, std::memory_order_relaxed)) {
            char m[192];
            snprintf(m, sizeof(m),
                     "macro index %d not written in slot %d -- that escape stays blank. If a number "
                     "is missing on screen, the show path used a different slot than the writer did.",
                     index, slot);
            Log::Write("MESMACRO", m);
        }
        return false;
    }
    if (outValue) *outValue = s_tblVal[slot][index].load(std::memory_order_relaxed);
    if (outKind)  *outKind  = s_tblKind[slot][index].load(std::memory_order_relaxed);
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

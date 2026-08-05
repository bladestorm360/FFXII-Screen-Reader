#include "navigation/shout_gauge.h"

#include <cstdio>

#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"

namespace ShoutGauge {
namespace {

constexpr uint32_t RVA_MGR        = 0x2A42D80;  // DAT_02B62D80: pointer slot for the gauge manager
constexpr uint32_t MGR_GAUGE_PTR  = 0xC0;
constexpr uint32_t MGR_FLAGS      = 0xC8;
constexpr uint32_t GAUGE_STYLE    = 0xC0;
constexpr uint32_t GAUGE_FLAGS    = 0xD8;
constexpr uint32_t GAUGE_MAX_OFF  = 0xE0;
constexpr uint32_t GAUGE_VAL_OFF  = 0xE4;
constexpr uint32_t SHOWN_BIT      = 0x4;

// ---- the shout-gauge arming ---------------------------------------------------------------------
//
// `FUN_00408560(a, b, c)` is the C function the `setgaugecountercondition` native forwards to, and
// its THREE parameters are counted from the callee's own decompile (the S129 rule): the native shim
// pops three VM values and calls it with them in script order, so `a, b, c` here are literally the
// numbers the script wrote. That is what makes this a measurement rather than a fingerprint read
// back through the engine's own handling -- `FUN_00407300` multiplies the triple by 60 when the
// counter type is 1 (which the shout gauge is) and reorders it depending on `gauge+0xDC` before
// storing it at `+0xEA/+0xEC/+0xEE`, so the stored form is two transformations away from what the
// script actually said.
constexpr uint32_t RVA_CONDITION = 0x2E8560;
constexpr uint32_t kShoutA = 200, kShoutB = 200, kShoutC = 100;

typedef void(__fastcall* Pfn_Condition)(uint32_t, uint32_t, uint32_t);
Pfn_Condition s_origCondition = nullptr;
bool s_isShoutGauge = false;   // game thread only: the script VM runs there

void* GaugeObject(uint8_t* outMgrFlags) {
    void* mgrSlot = Hooks::ResolveRva(RVA_MGR);
    if (!mgrSlot) return nullptr;
    void* mgr = nullptr;
    if (!MemRead::SafeReadPtr(mgrSlot, &mgr) || !mgr) return nullptr;
    if (outMgrFlags) MemRead::SafeReadU8(mgr, MGR_FLAGS, outMgrFlags);
    return MemRead::PtrAt(mgr, MGR_GAUGE_PTR);
}

void __fastcall HookedCondition(uint32_t a, uint32_t b, uint32_t c) {
    const bool shout = (a == kShoutA && b == kShoutB && c == kShoutC);
    if (shout != s_isShoutGauge) {
        // A DIFFERENT triple disarms as decisively as ours arms: it means the gauge being
        // configured belongs to some other script, and the shout features must not speak for it.
        s_isShoutGauge = shout;
        char m[176];
        snprintf(m, sizeof(m), "gauge condition (%u, %u, %u) -> %s", a, b, c,
                 shout ? "THIS IS THE SHOUT GAUGE" : "not the shout gauge; features disarmed");
        Log::Write("SHOUT", m);
    }
    if (s_origCondition) s_origCondition(a, b, c);
}

} // namespace

State Read() {
    State s;
    void* gauge = GaugeObject(&s.mgrFlags);
    if (!gauge) return s;

    MemRead::SafeReadU8(gauge, GAUGE_STYLE, &s.styleC0);
    MemRead::SafeReadU8(gauge, GAUGE_STYLE + 1, &s.styleC1);
    MemRead::SafeReadU32(gauge, GAUGE_FLAGS, &s.flags);
    s.shown = (s.flags & SHOWN_BIT) != 0;

    uint32_t v = 0, mx = 0;
    if (!MemRead::SafeReadU32(gauge, GAUGE_VAL_OFF, &v)) return s;
    if (!MemRead::SafeReadU32(gauge, GAUGE_MAX_OFF, &mx)) return s;
    s.value = static_cast<int32_t>(v);
    s.max   = static_cast<int32_t>(mx);
    s.ok    = true;
    return s;
}

bool IsShown() {
    void* gauge = GaugeObject(nullptr);
    if (!gauge) return false;
    uint32_t flags = 0;
    if (!MemRead::SafeReadU32(gauge, GAUGE_FLAGS, &flags)) return false;
    return (flags & SHOWN_BIT) != 0;
}

bool IsShoutGauge() { return s_isShoutGauge; }

void OnMapTeardown() { s_isShoutGauge = false; }

bool InitHooks() {
    const bool ok = Hooks::InstallTyped(RVA_CONDITION, &HookedCondition, &s_origCondition);
    Log::Write("SHOUT", ok ? "gauge-condition hook installed (identifies WHICH gauge is on screen)"
                           : "gauge-condition hook FAILED -- shout features cannot arm and stay silent");
    return ok;
}

} // namespace ShoutGauge

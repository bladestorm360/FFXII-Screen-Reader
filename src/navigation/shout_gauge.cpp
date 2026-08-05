#include "navigation/shout_gauge.h"

#include "core/hooks.h"
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

void* GaugeObject(uint8_t* outMgrFlags) {
    void* mgrSlot = Hooks::ResolveRva(RVA_MGR);
    if (!mgrSlot) return nullptr;
    void* mgr = nullptr;
    if (!MemRead::SafeReadPtr(mgrSlot, &mgr) || !mgr) return nullptr;
    if (outMgrFlags) MemRead::SafeReadU8(mgr, MGR_FLAGS, outMgrFlags);
    return MemRead::PtrAt(mgr, MGR_GAUGE_PTR);
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

} // namespace ShoutGauge

#include "navigation/shout_meter.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "navigation/shout_fill.h"
#include "navigation/shout_script.h"
#include "navigation/shout_table.h"
#include "navigation/entity_list.h"
#include "navigation/nav_common.h"
#include "navigation/nav_types.h"
#include "navigation/map_names.h"
#include "navigation/player_state.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "speech/phrase_format.h"
#include "speech/phrasebook.h"
#include "speech/speech.h"

namespace ShoutMeter {
namespace {

// ---- The gauge HUD (CONFIRMED from FUN_004085B0 / FUN_00408250) --------------------------------
//
// `setgaugecounter` (script native 0x1AE) -> shim FUN_0034EA90 -> FUN_004085B0(int), whose single
// argument IS the new counter value. THE DETOUR'S ARITY IS ONE INT, counted from the CALLEE's
// decompile as S129 requires -- the shim above it decompiles as `void(void)` while actually reading
// R9, and hooking THAT would have been the same 4-args-on-a-6-arg-function defect that cost the
// shop menu a crash.
constexpr uint32_t RVA_GAUGE_SET  = 0x2E85B0;   // FUN_004085B0(int newValue)
constexpr uint32_t RVA_GAUGE_MGR  = 0x2A42D80;  // DAT_02B62D80: pointer to the gauge manager
constexpr uint32_t MGR_GAUGE_PTR  = 0xC0;       // -> the gauge object
constexpr uint32_t MGR_FLAGS      = 0xC8;       // & 2 suppresses the engine's own write
constexpr uint32_t GAUGE_MAX_OFF  = 0xE0;
constexpr uint32_t GAUGE_VAL_OFF  = 0xE4;
constexpr uint32_t GAUGE_STYLE    = 0xC0;       // see the note in ReadGauge

typedef void(__fastcall* Pfn_GaugeSet)(int);
Pfn_GaugeSet s_orig = nullptr;

// ---- State. Everything here is GAME THREAD ONLY except the two key flags -----------------------
ShoutScript::Module s_module;
bool s_censusLogged  = false;
bool s_moduleLogged  = false;

struct Burst {
    bool active           = false;
    int  startValue       = 0;      // the gauge's value when the burst opened
    int  lastValue        = 0;
    int  max              = 0;
    int  setsThisInterval = 0;
    int  totalSets        = 0;
};
Burst s_burst;

std::atomic<bool> s_reqMeter{false};
std::atomic<bool> s_reqGuard{false};

// Read the live gauge. Re-resolved from the manager global EVERY time -- never cached across
// frames, because the object is owned by the HUD and dies with it.
//
// NOTHING GATES ON THE STYLE BYTE. FUN_004085B0 as decompiled both nulls the gauge object when
// *(gauge+0xC0) is non-zero AND dispatches its redraw on that same byte being 0/1/3, which cannot
// both be true; the likely reading is that Ghidra folded two adjacent fields (+0xC0/+0xC1). Since
// the value at +0xE4 is what the script asked for either way, the bytes are LOGGED and never
// tested -- a measurement riding the diagnostics rather than a guess baked into a branch.
bool ReadGauge(int* outValue, int* outMax, uint8_t* outC0, uint8_t* outC1, uint8_t* outMgrFlags) {
    void* mgrSlot = Hooks::ResolveRva(RVA_GAUGE_MGR);
    if (!mgrSlot) return false;
    void* mgr = nullptr;
    if (!MemRead::SafeReadPtr(mgrSlot, &mgr) || !mgr) return false;
    if (outMgrFlags) MemRead::SafeReadU8(mgr, MGR_FLAGS, outMgrFlags);

    void* gauge = MemRead::PtrAt(mgr, MGR_GAUGE_PTR);
    if (!gauge) return false;
    if (outC0) MemRead::SafeReadU8(gauge, GAUGE_STYLE, outC0);
    if (outC1) MemRead::SafeReadU8(gauge, GAUGE_STYLE + 1, outC1);

    uint32_t v = 0, mx = 0;
    if (!MemRead::SafeReadU32(gauge, GAUGE_VAL_OFF, &v)) return false;
    if (!MemRead::SafeReadU32(gauge, GAUGE_MAX_OFF, &mx)) return false;
    if (outValue) *outValue = static_cast<int32_t>(v);
    if (outMax)   *outMax   = static_cast<int32_t>(mx);
    return true;
}

// THE ONE SPEECH CHOKE POINT for this surface. Both detectors -- the burst coalescer and the B key
// -- funnel through here so the wording, the logging and the interrupt policy live in one place.
//
// `interrupt` is false for automatic announcements: the game prints its own "<n> Bhujerbans heed
// your words" and, at 100, the line that starts the next scene. Cutting those off to say a
// percentage would trade the game's own words for ours.
void EmitMeter(int value, int max, bool rising, bool interrupt, const char* why) {
    // SANITY BEFORE SPEECH. The gauge object outlives any one minigame, so a read taken before the
    // script has set this sequence up can hold a maximum of zero or a counter left over from
    // something else. A percentage is only spoken when the pair is internally consistent; anything
    // else is logged and stays silent, because a wrong number here is worse than no number.
    if (max <= 0 || value < 0 || value > max) {
        char m[176];
        snprintf(m, sizeof(m), "%s: value=%d max=%d is not a usable gauge -- staying silent",
                 why, value, max);
        Log::Write("SHOUT", m);
        return;
    }
    const std::wstring pct = PhraseFormat::Percent(Phrase::Id::Infamy, value, max);
    if (pct.empty()) return;
    std::wstring text = pct;
    if (!rising) {
        // A drop is the event the player must react to, so it is named. "down" is Phrase::StatDown,
        // reused rather than a new invented word.
        text = std::wstring(Phrase::Get(Phrase::Id::Infamy)) + Phrase::Get(Phrase::Id::StatDown) +
               L", " + std::to_wstring(static_cast<long long>(value) * 100 / max) +
               Phrase::Get(Phrase::Id::PercentSuffix);
    }
    char m[192];
    snprintf(m, sizeof(m), "%s: %d/%d (%s)", why, value, max, rising ? "rise" : "drop");
    Log::Write("SHOUT", m);
    Speech::Output(text, interrupt);
}

// One-off census of the NPCs a shout map carries, with their npcdic ids. THIS IS THE DELIVERABLE
// that turns ShoutTable's `guardNameIdx = -1` into a measured value: the Imperials whose earshot
// costs 30 points are not identifiable from the scripts, and the tester's existing Bhujerba logs
// are all post-minigame, so the first play pass through the sequence is the only place the answer
// can come from. Log-only, once per map.
void LogNpcCensus() {
    FVec3 me{};
    if (!PlayerState::ReadPlayerPos(me)) return;
    std::vector<EntityList::NearbyNPC> npcs;
    const int n = EntityList::CollectNearestNPCs(me, 64, npcs);
    char head[160];
    snprintf(head, sizeof(head),
             "NPC census on %s (map %d): %d NPC(s) -- the guard identity for ShoutTable comes from "
             "whichever of these is present when the meter DROPS",
             s_module.srcName, MapNames::CurrentMapId(), n);
    Log::Write("SHOUT", head);
    for (const EntityList::NearbyNPC& e : npcs) {
        char line[96];
        snprintf(line, sizeof(line), "  nameIdx=%-5d %6.1fm  ", e.nameIdx, e.dist2D);
        Log::WriteW("SHOUT", line, e.label);
    }
}

void SpeakNearestNpcs() {
    FVec3 me{};
    if (!PlayerState::ReadPlayerPos(me)) {
        Log::Write("SHOUT-KEY", "guard: player position unavailable -- silent");
        return;
    }
    std::vector<EntityList::NearbyNPC> npcs;
    const int n = EntityList::CollectNearestNPCs(me, 3, npcs);
    if (n <= 0) {
        Log::Write("SHOUT-KEY", "guard: no NPCs listed here");
        Speech::Output(Phrase::Get(Phrase::Id::NoTargets), true);
        return;
    }
    float facing = 0.0f;
    PlayerState::ReadCameraForwardStable(facing);

    std::wstring text;
    for (int i = 0; i < n; ++i) {
        if (i) text += L". ";
        text += npcs[i].label + L", " +
                NavCommon::DescribeDirectionRelative(me, npcs[i].pos, facing);
    }
    Log::WriteW("SHOUT-KEY", "guard: ", text);
    Speech::Output(text, true);
}

void DrainKeys() {
    if (s_reqMeter.exchange(false, std::memory_order_acq_rel)) {
        if (!s_module.valid) {
            Log::Write("SHOUT-KEY", "meter: no shout script live here -- silent no-op");
        } else {
            int v = 0, mx = 0;
            uint8_t c0 = 0, c1 = 0, mf = 0;
            if (!ReadGauge(&v, &mx, &c0, &c1, &mf)) {
                Log::Write("SHOUT-KEY", "meter: gauge object unavailable -- silent");
            } else {
                char m[176];
                snprintf(m, sizeof(m), "meter key: gauge +0xC0=%u +0xC1=%u mgr+0xC8=%u", c0, c1, mf);
                Log::Write("SHOUT-KEY", m);
                EmitMeter(v, mx, /*rising=*/true, /*interrupt=*/true, "meter key");
            }
        }
    }
    if (s_reqGuard.exchange(false, std::memory_order_acq_rel)) {
        if (!s_module.valid) {
            Log::Write("SHOUT-KEY", "guard: no shout script live here -- silent no-op");
        } else {
            SpeakNearestNpcs();
        }
    }
}

void RefreshModule() {
    char names[192] = {};
    const ShoutScript::Module found = ShoutScript::FindShoutModule(names, sizeof(names));

    if (found.valid) {
        const bool isNew = !s_module.valid || s_module.record != found.record;
        s_module = found;
        if (isNew || !s_moduleLogged) {
            s_moduleLogged = true;
            char m[256];
            snprintf(m, sizeof(m),
                     "module matched: %s in slot %d (map %d) -- meter var 0x%02X, goal %d. Modules: %s",
                     s_module.srcName, s_module.slot, MapNames::CurrentMapId(),
                     s_module.row->meterVarIdx, s_module.row->fillValue, names);
            Log::Write("SHOUT", m);
        }
        if (!s_censusLogged) { s_censusLogged = true; LogNpcCensus(); }
        return;
    }

    // No shout script here. Everything below the hook's first branch is unreachable from now on.
    if (s_module.valid) { s_module = ShoutScript::Module(); s_moduleLogged = false; }
}

// ---- The hook ----------------------------------------------------------------------------------
void __fastcall HookedGaugeSet(int newValue) {
    // THE OFF-TABLE PATH IS THE FIRST BRANCH. `s_module` is decided on the field frame, never here,
    // so this costs one bool: with no shout script live the function is the original and nothing
    // else -- no gauge read, no burst, and ShoutFill's write is not on the path at all. Every other
    // gauge in the game passes through untouched.
    if (!s_module.valid) {
        if (s_orig) s_orig(newValue);
        return;
    }

    // The gauge's own value BEFORE the original overwrites +0xE4. FUN_004085B0 stores the argument
    // and only then dispatches its redraw, so this is the last moment the previous value exists.
    int preSet = 0, maxV = 0;
    const bool haveGauge = ReadGauge(&preSet, &maxV, nullptr, nullptr, nullptr);

    if (s_orig) s_orig(newValue);

    STALL_SCOPE("ShoutMeter::HookedGaugeSet");

    if (!s_burst.active) {
        s_burst = Burst();
        s_burst.active = true;
        s_burst.startValue = haveGauge ? preSet : newValue;
    }
    s_burst.lastValue = newValue;
    if (maxV > 0) s_burst.max = maxV;
    ++s_burst.setsThisInterval;
    ++s_burst.totalSets;

    // Feature 3: a RISING set means the player just landed a shout. See shout_fill.h for the
    // charter and the falsifier; it writes at most once per map visit and declines by default.
    if (haveGauge && newValue > preSet) ShoutFill::TryInstantFill(s_module, preSet, newValue);
}

} // namespace

bool Init() {
    const bool ok = Hooks::InstallTyped(RVA_GAUGE_SET, &HookedGaugeSet, &s_orig);
    Log::Write("SHOUT", ok ? "gauge-writer hook installed (script setgaugecounter -> HUD)"
                           : "gauge-writer hook FAILED to install -- the infamy meter will not speak");
    return ok;
}

void OnFieldFrame() {
    RefreshModule();
    DrainKeys();

    if (!s_burst.active) return;
    if (s_burst.setsThisInterval > 0) {   // still running: the script sets one step per frame
        s_burst.setsThisInterval = 0;
        return;
    }
    // A whole field frame with no new set: the change has settled.
    const int  start  = s_burst.startValue;
    const int  value  = s_burst.lastValue;
    const int  max    = s_burst.max;
    const int  sets   = s_burst.totalSets;
    // DIRECTION IS MEASURED WITHIN THE BURST, never against the last value spoken. The idle decay
    // runs through `changegaugecounterbyframe`, a different native that never reaches this hook, so
    // a cross-burst comparison would call a real rise a drop whenever the gauge had bled down in
    // between.
    // `>=`, not `>`: a burst that ends where it started is not a loss, and neither is one whose
    // opening value could not be read (startValue then holds the first set's own value). Only a
    // MEASURED decrease is announced as one.
    const bool rising = value >= start;
    s_burst = Burst();

    char m[176];
    snprintf(m, sizeof(m), "burst closed: %d -> %d over %d set(s)", start, value, sets);
    Log::Write("SHOUT", m);
    EmitMeter(value, max, rising, /*interrupt=*/false, "burst");
}

void OnMapTeardown() {
    s_module        = ShoutScript::Module();
    s_burst         = Burst();
    s_censusLogged  = false;
    s_moduleLogged  = false;
    ShoutFill::OnMapTeardown();
}

void RequestMeterCheck() { s_reqMeter.store(true, std::memory_order_release); }
void RequestGuardCheck() { s_reqGuard.store(true, std::memory_order_release); }

} // namespace ShoutMeter

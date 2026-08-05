#include "navigation/shout_meter.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "navigation/shout_diag.h"
#include "navigation/shout_fill.h"
#include "navigation/shout_gauge.h"
#include "navigation/shout_script.h"
#include "navigation/shout_table.h"
#include "navigation/entity_list.h"
#include "navigation/nav_common.h"
#include "navigation/nav_types.h"
#include "navigation/map_names.h"
#include "navigation/player_state.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/stall_probe.h"
#include "speech/phrase_format.h"
#include "speech/phrasebook.h"
#include "speech/speech.h"
#include "ui/mod_menu.h"

namespace ShoutMeter {
namespace {

typedef void(__fastcall* Pfn_GaugeSet)(int);
Pfn_GaugeSet s_orig = nullptr;

// ---- State. Everything here is GAME THREAD ONLY except the two key flags -----------------------
ShoutScript::Module s_module;
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

// The `N` key. Once ShoutTable carries a measured `guardNameIdx` this reports GUARDS ONLY, with the
// earshot verdict when the radius is measured too. Until then it reports the nearest NPCs by the
// game's own names -- useful, and honest about not knowing which of them matters.
void SpeakGuards() {
    FVec3 me{};
    if (!PlayerState::ReadPlayerPos(me)) {
        Log::Write("SHOUT-KEY", "guard: player position unavailable -- silent");
        return;
    }
    const int16_t guardIdx = s_module.row ? s_module.row->guardNameIdx : static_cast<int16_t>(-1);
    const float   radius   = s_module.row ? s_module.row->earshotRadius : 0.0f;

    std::vector<EntityList::NearbyNPC> npcs;
    EntityList::CollectNearestNPCs(me, 32, npcs);
    if (guardIdx >= 0) {
        std::vector<EntityList::NearbyNPC> guards;
        for (const EntityList::NearbyNPC& e : npcs)
            if (e.nameIdx == guardIdx) guards.push_back(e);
        npcs.swap(guards);
    }
    if (npcs.size() > 3) npcs.resize(3);

    if (npcs.empty()) {
        // No guard anywhere on the map is a REAL answer when we know what a guard is, so say it.
        // Not knowing yet is a different thing, and that one stays quiet.
        Log::Write("SHOUT-KEY", guardIdx >= 0 ? "guard: none listed on this map"
                                              : "guard: no NPCs listed here");
        if (guardIdx >= 0) Speech::Output(Phrase::Get(Phrase::Id::NoGuardsInEarshot), true);
        else               Speech::Output(Phrase::Get(Phrase::Id::NoTargets), true);
        return;
    }

    float facing = 0.0f;
    PlayerState::ReadCameraForwardStable(facing);

    std::wstring text;
    bool anyInEarshot = false;
    for (size_t i = 0; i < npcs.size(); ++i) {
        if (i) text += L". ";
        text += npcs[i].label + L", " +
                NavCommon::DescribeDirectionRelative(me, npcs[i].pos, facing);
        if (radius > 0.0f && npcs[i].dist2D <= radius) anyInEarshot = true;
    }
    // The verdict is only ever appended when the radius is a MEASURED number. With `earshotRadius`
    // still 0 the player gets distance and bearing and no claim about safety.
    if (radius > 0.0f)
        text += std::wstring(L". ") +
                Phrase::Get(anyInEarshot ? Phrase::Id::InEarshot : Phrase::Id::NoGuardsInEarshot);

    Log::WriteW("SHOUT-KEY", "guard: ", text);
    Speech::Output(text, true);
}

// Both keys answer ONLY while the sequence is actually running and the guide is switched on.
// Anywhere else they are silent no-ops with one log line -- the `;`/`7` precedent, never a spoken
// "not available here".
bool KeysAnswer(const char* which) {
    if (!PuzzleActive()) {
        char m[128];
        snprintf(m, sizeof(m), "%s: no shout sequence running here -- silent no-op", which);
        Log::Write("SHOUT-KEY", m);
        return false;
    }
    if (!ModMenu::PuzzleGuideOn()) {
        char m[128];
        snprintf(m, sizeof(m), "%s: puzzle guide is switched off -- silent no-op", which);
        Log::Write("SHOUT-KEY", m);
        return false;
    }
    return true;
}

void DrainKeys() {
    if (s_reqMeter.exchange(false, std::memory_order_acq_rel) && KeysAnswer("meter")) {
        const ShoutGauge::State g = ShoutGauge::Read();
        if (!g.ok) {
            Log::Write("SHOUT-KEY", "meter: gauge object unavailable -- silent");
        } else {
            char m[176];
            snprintf(m, sizeof(m), "meter key: gauge +0xC0=%u +0xC1=%u mgr+0xC8=%u flags=0x%08X",
                     g.styleC0, g.styleC1, g.mgrFlags, g.flags);
            Log::Write("SHOUT-KEY", m);
            EmitMeter(g.value, g.max, /*rising=*/true, /*interrupt=*/true, "meter key");
        }
    }
    if (s_reqGuard.exchange(false, std::memory_order_acq_rel) && KeysAnswer("guard")) {
        SpeakGuards();
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
    const ShoutGauge::State pre = ShoutGauge::Read();
    const int  preSet    = pre.value;
    const int  maxV      = pre.max;
    const bool haveGauge = pre.ok;

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

    // Feature 3: a RISING set means the player just landed a shout. Three gates, in cost order --
    // the player's own switch, then the sequence-live bit, then the falsifier inside TryInstantFill.
    // See shout_fill.h for the charter; it writes at most once per map visit and declines by default.
    if (haveGauge && newValue > preSet && ModMenu::PuzzleSkipOn() && PuzzleActive())
        ShoutFill::TryInstantFill(s_module, preSet, newValue);
}

} // namespace

bool PuzzleActive() {
    return s_module.valid && ShoutGauge::IsShown();
}

bool Init() {
    const bool ok = Hooks::InstallTyped(ShoutGauge::RVA_WRITER, &HookedGaugeSet, &s_orig);
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

    // The measurement runs regardless of the guide toggle: it is log-only, it is the whole reason
    // the guard identity and the earshot radius can ever be filled in, and a player who switched
    // the SPEECH off has not asked for the diagnostics to stop.
    ShoutDiag::CaptureBurst(s_module, rising, start, value);

    if (ModMenu::PuzzleGuideOn())
        EmitMeter(value, max, rising, /*interrupt=*/false, "burst");
}

void OnMapTeardown() {
    s_module        = ShoutScript::Module();
    s_burst         = Burst();
    s_moduleLogged  = false;
    ShoutDiag::OnMapTeardown();
    ShoutFill::OnMapTeardown();
}

void RequestMeterCheck() { s_reqMeter.store(true, std::memory_order_release); }
void RequestGuardCheck() { s_reqGuard.store(true, std::memory_order_release); }

} // namespace ShoutMeter

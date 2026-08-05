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
// Refreshed once per field frame and read by the gauge hook, so the hook's first branch costs one
// bool rather than a pointer chase through the HUD on every gauge write in the game.
bool s_active        = false;

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

// The `N` key: THE CROWD, not a list of individuals.
//
// The minigame rewards having as many listeners around you as possible -- the increment loop scales
// with how many Bhujerbans heed -- so "who are the three nearest people" is the wrong question. What
// the player is deciding is whether to shout HERE or move first, and that is a count.
//
// It degrades in one step. With a measured earshot radius the answer is the one the tester asked
// for -- "5 civilians, 1 guard in earshot"; with only a measured guard id the split is reported over
// everyone listed; with neither it is a bare count plus the nearest few, which still tells the
// player whether they are standing in a crowd or on an empty street. NOTHING here invents a
// distance: the window is only ever the game's own measured radius.
void SpeakCrowd() {
    FVec3 me{};
    if (!PlayerState::ReadPlayerPos(me)) {
        Log::Write("SHOUT-KEY", "crowd: player position unavailable -- silent");
        return;
    }
    const bool  haveGuards = ShoutTable::HaveGuardIdentity();
    const float radius     = ShoutTable::EarshotRadius();

    std::vector<EntityList::NearbyNPC> npcs;
    EntityList::CollectNearestNPCs(me, 64, npcs);
    if (npcs.empty()) {
        Log::Write("SHOUT-KEY", "crowd: no NPCs listed here");
        Speech::Output(Phrase::Get(Phrase::Id::NoTargets), true);
        return;
    }

    // THE WINDOW IS EARSHOT. Counting everyone on the map was the first version's mistake ("way,
    // way too broad"); a 10-step reporting window was the second, because a window the player has to
    // translate is not detection. This is the shipped earshot -- see shout_table.h for where the
    // number comes from and why it is rounded the way it is.
    const bool  measured = (radius > 0.0f);
    const float window   = measured ? radius : NavCommon::GetUnitsPerStep() * 10.0f;

    int civilians = 0, guards = 0;
    for (const EntityList::NearbyNPC& e : npcs) {
        if (e.dist2D > window) continue;
        if (haveGuards && ShoutTable::IsGuardName(e.nameIdx)) ++guards;
        else                                                   ++civilians;
    }

    std::wstring text;
    if (haveGuards) {
        text = std::to_wstring(civilians) + L" " + Phrase::Get(Phrase::Id::Civilians) + L", " +
               std::to_wstring(guards)    + L" " + Phrase::Get(Phrase::Id::Guards);
    } else {
        text = std::to_wstring(civilians) + L" " + Phrase::Get(Phrase::Id::People);
    }
    // Name the window for what it is. `measured` is true today; the fallback survives only so that
    // zeroing the radius degrades to a stated distance rather than to a silent, invisible one.
    if (measured) {
        text += std::wstring(L" ") + Phrase::Get(Phrase::Id::InEarshot);
    } else {
        text += std::wstring(Phrase::Get(Phrase::Id::WithinPrefix)) + L"10" +
                Phrase::Get(Phrase::Id::StepsSuffix);
    }

    // The nearest guard is the one thing worth a bearing: it is what the player would move away
    // from. Civilians are a crowd to stand in, not individuals to find.
    if (guards > 0) {
        float facing = 0.0f;
        PlayerState::ReadCameraForwardStable(facing);
        for (const EntityList::NearbyNPC& e : npcs) {
            if (!ShoutTable::IsGuardName(e.nameIdx) || e.dist2D > window) continue;
            text += L". " + e.label + L", " +
                    NavCommon::DescribeDirectionRelative(me, e.pos, facing);
            break;   // nearest first, so the first match is the nearest
        }
    }

    Log::WriteW("SHOUT-KEY", "crowd: ", text);
    Speech::Output(text, true);
}

// Both keys answer ONLY while the sequence is actually running and the guide is switched on.
// Anywhere else they are silent no-ops with one log line -- the `;`/`7` precedent, never a spoken
// "not available here".
//
// The two tests below overlap on purpose: `PuzzleGuideOn` is context-gated and so already returns
// false off a sequence. Asking `PuzzleActive` FIRST is what lets the log say which of the two
// actually stopped the key, instead of blaming a setting the player never touched.
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
    if (s_reqGuard.exchange(false, std::memory_order_acq_rel) && KeysAnswer("crowd")) {
        SpeakCrowd();
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
    // THE OFF-SEQUENCE PATH IS THE FIRST BRANCH. `s_active` is decided on the field frame, never
    // here, so this costs one bool: with no shout sequence live the function is the original and
    // nothing else -- no burst, no speech, and ShoutFill's write is not on the path at all. Every
    // other gauge in the game passes through untouched.
    if (!s_active) {
        if (s_orig) s_orig(newValue);
        return;
    }

    // RESOLVE THE EXECUTING SCRIPT WHILE WE ARE INSIDE ITS NATIVE CALL. This is the one moment the
    // engine's own current-module global names the script that drove the gauge, so it needs no
    // scanning and no assumption about which slot holds what. The field-frame scan stays as the
    // fallback; whichever answers, the fill needs a module and only gets one from here.
    if (!s_module.valid) {
        const ShoutScript::Module m = ShoutScript::FromCurrentModule();
        if (m.valid) {
            s_module = m;
            if (!s_moduleLogged) {
                s_moduleLogged = true;
                char lm[224];
                snprintf(lm, sizeof(lm),
                         "module resolved FROM THE NATIVE CALL: %s -- meter var 0x%02X, goal %d",
                         s_module.srcName, s_module.row->meterVarIdx, s_module.row->fillValue);
                Log::Write("SHOUT", lm);
            }
        }
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
    // TWO ENGINE-SIDE FACTS, both the script's own: this gauge was configured as the shout gauge
    // (its condition triple is (200,200,100), which no other script in the game uses), and it is on
    // screen right now. Nothing here consults a map id, and nothing depends on resolving a script
    // module -- which is what let S133's build go dark on a map where the sequence was running.
    return ShoutGauge::IsShoutGauge() && ShoutGauge::IsShown();
}

bool Init() {
    // The condition hook decides WHICH gauge is on screen; the writer hook reads its value. Without
    // the first, the second never arms and the feature stays correctly silent.
    ShoutGauge::InitHooks();
    const bool ok = Hooks::InstallTyped(ShoutGauge::RVA_WRITER, &HookedGaugeSet, &s_orig);
    Log::Write("SHOUT", ok ? "gauge-writer hook installed (script setgaugecounter -> HUD)"
                           : "gauge-writer hook FAILED to install -- the infamy meter will not speak");
    return ok;
}

void OnFieldFrame() {
    RefreshModule();
    s_active = PuzzleActive();
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

    // A BURST THAT CHANGED NOTHING IS NOT AN EVENT, and must not be spoken. The idle decay reaches
    // this hook after all: `changegaugecounterbyframe` tweens the bar down and the script then calls
    // `setgaugecounter` with the value it has ALREADY reached, so every decay tick arrives here as a
    // one-set burst whose start equals its end. The 2026-08-05 log is full of them --
    // "26 -> 26", "25 -> 25", "24 -> 24" -- each one announced as a rise, which is the meter
    // chattering its way down on its own while the player does nothing.
    //
    // This is NOT speech dedup: it does not compare against what was last said, and two identical
    // real changes both announce. It is the difference between an event and no event. A shout that
    // nobody heeds also lands here, and silence is right there too -- the game says "No one heeds
    // your words" itself.
    const bool changed = (value != start);

    // The measurement runs regardless of the guide toggle AND regardless of whether anything
    // changed: it is log-only, it is the whole reason the guard identity and the earshot radius can
    // ever be filled in, and a player who switched the SPEECH off has not asked for the diagnostics
    // to stop. Only real changes are worth a capture, though -- a decay tick measures nothing.
    if (changed) ShoutDiag::CaptureBurst(s_module, rising, start, value);

    if (changed && ModMenu::PuzzleGuideOn())
        EmitMeter(value, max, rising, /*interrupt=*/false, "burst");
}

void OnMapTeardown() {
    s_module        = ShoutScript::Module();
    s_burst         = Burst();
    s_moduleLogged  = false;
    s_active        = false;
    ShoutGauge::OnMapTeardown();
    ShoutDiag::OnMapTeardown();
    ShoutFill::OnMapTeardown();
}

void RequestMeterCheck() { s_reqMeter.store(true, std::memory_order_release); }
void RequestGuardCheck() { s_reqGuard.store(true, std::memory_order_release); }

} // namespace ShoutMeter

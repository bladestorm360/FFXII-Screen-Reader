#include "ui/mod_menu.h"

#include "core/logger.h"
#include "input/input_tracker.h"
#include "speech/phrasebook.h"
#include "speech/speech.h"
#include "navigation/shout_meter.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ModMenu {
namespace {

using Phrase::Id;

// ---- the settings table -------------------------------------------------------------------------
// One row per SettingId, in enum order.
//
// TWO KINDS OF SETTING (Session 95, when volume arrived). A `Named` setting has a spoken word per
// value and a sentence per value; `values`/`descs` are parallel arrays indexed by the value. A
// `Percent` setting has neither -- its value RENDERS AS A NUMBER, which is deliberate: a digit string
// needs no phrasebook row, translates itself, and adding five invented loudness adjectives to a
// 12-locale table would be exactly the fabricated-label failure the phrasebook rules exist to stop.
enum class Kind : uint8_t { Named, Percent };

// Volume steps: 20% .. 100%, five of them. Deliberately does NOT reach 0 -- each beacon has its own
// Off toggle, and a volume that can silence a switched-on feature is a support question waiting to
// happen ("the beacon is on but I hear nothing").
constexpr int kVolumeSteps   = 5;
constexpr int kVolumePercent = 20;    // value i speaks as (i + 1) * 20 percent

struct Setting {
    Id   name;
    Kind kind;
    int  count;          // how many values; what Adjust wraps or clamps on
    Id   values[2];      // Named only
    Id   descs[2];       // Named only
    Id   desc;           // the setting-level sentence, spoken before the value's sentence
    const char* key;     // token used in the settings file; never spoken
    int  defValue;       // used when there is no stored file, or the stored value is out of range
    // CONTEXT GATE (S132). Null = always visible, which is every row that predates this. When it
    // returns false the row is skipped by the cursor and by the opening announcement -- the VALUE is
    // untouched and still persists, so a setting stays where the player left it.
    bool (*visible)();
};

const Setting kSettings[] = {
    { Id::SettingCombatVerbosity, Kind::Named, 2,
      { Id::VerbosityNormal,     Id::VerbosityVerbose },
      { Id::VerbosityDescNormal, Id::VerbosityDescVerbose },
      Id::VerbosityDesc, "combat_verbosity", 0 },
    { Id::SettingAudioBeacon, Kind::Named, 2,
      { Id::BeaconOff,     Id::BeaconOn },
      { Id::BeaconDescOff, Id::BeaconDescOn },
      Id::BeaconDesc, "audio_beacon", 1 },
    { Id::SettingBeaconVolume, Kind::Percent, kVolumeSteps,
      {}, {}, Id::BeaconVolumeDesc, "beacon_volume", kVolumeSteps - 1 },
    // Defaults ON, because that is what today's behaviour already was -- the target ping came free
    // with the route beacon. Splitting the setting must not silently take a feature away.
    { Id::SettingTargetBeacon, Kind::Named, 2,
      { Id::BeaconOff,           Id::BeaconOn },
      { Id::TargetBeaconDescOff, Id::TargetBeaconDescOn },
      Id::TargetBeaconDesc, "target_beacon", 1 },
    { Id::SettingTargetVolume, Kind::Percent, kVolumeSteps,
      {}, {}, Id::TargetVolumeDesc, "target_volume", kVolumeSteps - 1 },
    // Default OFF -- auto-walk moves the character, and a feature that drives the game must be
    // something the player deliberately switched on, never something an install surprised them with.
    { Id::SettingAutoWalk, Kind::Named, 2,
      { Id::BeaconOff,       Id::BeaconOn },
      { Id::AutoWalkDescOff, Id::AutoWalkDescOn },
      Id::AutoWalkDesc, "auto_walk", 0 },
    // S147. Default OFF, which is today's behaviour exactly: the shop comparison answers `4`-`9`
    // and Libra answers `o`, and neither volunteers itself. ON adds the volunteering and takes
    // nothing away -- both keys keep working, because a toggle that removed a way to ASK would be a
    // regression rather than a setting.
    { Id::SettingAutoDetail, Kind::Named, 2,
      { Id::BeaconOff,         Id::BeaconOn },
      { Id::AutoDetailDescOff, Id::AutoDetailDescOn },
      Id::AutoDetailDesc, "auto_detail", 0, nullptr },
    // Default ON. Unlike auto-walk this does not DRIVE the game -- it only decides whether the mod
    // may read the pad and withhold what it claims -- so shipping it on is not the surprise auto-walk
    // would have been. It is here as the pad player's way back to a stock controller.
    { Id::SettingController, Kind::Named, 2,
      { Id::BeaconOff,         Id::BeaconOn },
      { Id::ControllerDescOff, Id::ControllerDescOn },
      Id::ControllerDesc, "controller", 1, nullptr },
    // S132, tester's request: the two shout-minigame rows, CONTEXT-GATED to a running sequence.
    //
    // Default ON for the guide: it only ever tells the player something, and a puzzle whose whole
    // content is invisible to them is exactly where the mod should be talking by default.
    //
    // Default OFF for the skip, on the auto-walk precedent three rows above: it WRITES GAME STATE and
    // completes a story sequence, so it must be something the player deliberately switched on rather
    // than something their first shout in Bhujerba surprised them with. One press of Right turns it
    // on and it stays on.
    { Id::SettingPuzzleGuide, Kind::Named, 2,
      { Id::BeaconOff,          Id::BeaconOn },
      { Id::PuzzleGuideDescOff, Id::PuzzleGuideDescOn },
      Id::PuzzleGuideDesc, "puzzle_guide", 1, &ShoutMeter::PuzzleActive },
    { Id::SettingPuzzleSkip, Kind::Named, 2,
      { Id::BeaconOff,         Id::BeaconOn },
      { Id::PuzzleSkipDescOff, Id::PuzzleSkipDescOn },
      Id::PuzzleSkipDesc, "puzzle_skip", 0, &ShoutMeter::PuzzleActive },
};

static_assert(sizeof(kSettings) / sizeof(kSettings[0]) == static_cast<size_t>(SettingId::Count),
              "mod_menu.cpp kSettings and ModMenu::SettingId are out of sync");

constexpr int kCount = static_cast<int>(SettingId::Count);

bool RowVisible(int i) {
    if (i < 0 || i >= kCount) return false;
    return kSettings[i].visible == nullptr || kSettings[i].visible();
}


// Step the cursor `dir` places over VISIBLE rows only, wrapping. Returns the current index unchanged
// when nothing is visible but it (or nothing at all is) -- the walk can never spin forever.
int StepVisible(int from, int dir) {
    for (int n = 0; n < kCount; ++n) {
        from = (from + dir + kCount) % kCount;
        if (RowVisible(from)) return from;
    }
    return from;
}

// The first/last visible row, for Home/End and for opening the menu on a hidden cursor.
int FirstVisible() {
    for (int i = 0; i < kCount; ++i) if (RowVisible(i)) return i;
    return 0;
}
int LastVisible() {
    for (int i = kCount - 1; i >= 0; --i) if (RowVisible(i)) return i;
    return 0;
}

// Read from the game thread (CombatFormat::ShouldSpeakNow, on the message-bus hook) and written from
// the input thread. Relaxed is enough: each is a lone byte-sized value with no ordering relationship
// to anything else, and a one-frame-stale read at worst logs a line that would have been spoken.
std::atomic<int>  g_values[kCount] = {};
std::atomic<bool> g_open{false};
int               g_cursor = 0;      // input thread only
bool              g_initialized = false;

// A HIDDEN ROW READS AS OFF (S133, tester's rule, and it is the right shape). A context-gated
// setting is one whose feature has no meaning outside its context: there is no infamy meter on a
// Bhujerba street with no shout sequence running, and nothing worth counting as a guard there
// either. So rather than have every consumer remember to re-check the context, THE SETTING ITSELF
// answers off whenever its row is not applicable, and the context test lives in exactly one place --
// the same predicate that hides the row.
//
// THE STORED VALUE IS UNTOUCHED. Only the read is forced; the player's choice comes straight back
// the moment the context returns, which is what makes a persisted setting worth having at all.
//
// Value 0 is Off, and also the first value, for every two-valued row. No Percent row is gated today;
// if one ever is, its author has to decide what "not applicable" means for a number first, because 0
// there is the quietest step rather than a natural off.
int EffectiveValue(SettingId id) {
    const int i = static_cast<int>(id);
    if (i < 0 || i >= kCount) return 0;
    if (!RowVisible(i)) return 0;
    return g_values[i].load(std::memory_order_relaxed);
}

// ---- persistence ---------------------------------------------------------------------------------
// %LOCALAPPDATA%, NOT the game folder: the install is read-only to this mod (CLAUDE.md) and only
// build_and_deploy writes there. Same store directory entity_labels.cpp already creates and uses.
// Empty when the variable is missing, which simply disables persistence — the menu still works, the
// choice just does not survive a restart.
std::wstring StorePath(bool createDir) {
    wchar_t* base = nullptr;
    size_t   len  = 0;
    if (_wdupenv_s(&base, &len, L"LOCALAPPDATA") != 0 || !base) return std::wstring();
    std::wstring dir = std::wstring(base) + L"\\FFXII-Screen-Reader";
    free(base);
    if (createDir) CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\mod_settings.txt";
}

void Save() {
    const std::wstring path = StorePath(/*createDir=*/true);
    if (path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) return;
    for (int i = 0; i < kCount; ++i)
        fprintf(f, "%s=%d\n", kSettings[i].key, g_values[i].load(std::memory_order_relaxed));
    fclose(f);
}

// Unknown keys and out-of-range values are IGNORED, not clamped onto a neighbouring setting: a file
// written by a future build with more settings must degrade to defaults, never to wrong ones.
void Load() {
    // Defaults FIRST, so a missing file, an unreadable one, or a key absent from an older file all
    // land on the intended value rather than on whatever zero happens to mean for that setting.
    for (int i = 0; i < kCount; ++i)
        g_values[i].store(kSettings[i].defValue, std::memory_order_relaxed);

    const std::wstring path = StorePath(/*createDir=*/false);
    if (path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const int v = atoi(eq + 1);
        for (int i = 0; i < kCount; ++i) {
            if (strcmp(line, kSettings[i].key) != 0) continue;
            if (v >= 0 && v < kSettings[i].count) g_values[i].store(v, std::memory_order_relaxed);
            break;
        }
    }
    fclose(f);
}

// ---- speech --------------------------------------------------------------------------------------
// Glue (". ", ", ") is composed here rather than stored in the phrasebook, per its header rule.
std::wstring ValueOf(int i) {
    const Setting& s = kSettings[i];
    const int v = g_values[i].load(std::memory_order_relaxed);
    // A number, not a word. `%` is punctuation the screen reader already voices ("eighty percent"),
    // so this needs no phrasebook row in any of the twelve locales.
    if (s.kind == Kind::Percent) return std::to_wstring((v + 1) * kVolumePercent) + L"%";
    return Phrase::Get(s.values[v]);
}

// Gain 0..1 for a Percent setting, for the audio engine.
float GainOf(SettingId id) {
    const int i = static_cast<int>(id);
    if (i < 0 || i >= kCount || kSettings[i].kind != Kind::Percent) return 1.0f;
    const int v = g_values[i].load(std::memory_order_relaxed);
    return static_cast<float>((v + 1) * kVolumePercent) / 100.0f;
}

std::wstring NameAndValue(int i) {
    return std::wstring(Phrase::Get(kSettings[i].name)) + L", " + ValueOf(i);
}

void LogState(const char* what, int i) {
    char m[128];
    snprintf(m, sizeof(m), "%s: %s=%d", what, kSettings[i].key,
             g_values[i].load(std::memory_order_relaxed));
    Log::Write("MODMENU", m);
}

// ---- input ---------------------------------------------------------------------------------------
// Arrows + Home/End, offered to us BEFORE the status virtual buffer. Returns true only when the menu
// is open, so every one of these keys behaves exactly as it always did while it is closed.
bool OnMenuNavKey(int vk) {
    if (!g_open.load(std::memory_order_relaxed)) return false;
    switch (vk) {
        // All four walk VISIBLE rows only (S132). A context-gated row that is not currently
        // applicable is skipped exactly as if it were absent from the table.
        case VK_UP:
            g_cursor = StepVisible(g_cursor, -1);
            Speech::Output(NameAndValue(g_cursor), true);
            return true;
        case VK_DOWN:
            g_cursor = StepVisible(g_cursor, +1);
            Speech::Output(NameAndValue(g_cursor), true);
            return true;
        case VK_HOME:
            g_cursor = FirstVisible();
            Speech::Output(NameAndValue(g_cursor), true);
            return true;
        case VK_END:
            g_cursor = LastVisible();
            Speech::Output(NameAndValue(g_cursor), true);
            return true;
        // Left and right are now DIRECTIONAL. They used to both advance, on the reasoning that every
        // setting was two-valued so "previous" and "next" were the same move -- with a note to widen
        // it when a setting with three or more values arrived. Volume is that setting.
        case VK_LEFT:
            Adjust(static_cast<SettingId>(g_cursor), -1);
            return true;
        case VK_RIGHT:
            Adjust(static_cast<SettingId>(g_cursor), +1);
            return true;
        default:
            return false;
    }
}

// `o` while the menu is open: the setting-level sentence, then the sentence for the CURRENT value,
// so the description changes as the setting does. Declines when closed, and the menu reader's own
// describe handler then runs untouched.
bool OnDescribe() {
    if (!g_open.load(std::memory_order_relaxed)) return false;
    const Setting& s = kSettings[g_cursor];
    std::wstring out = Phrase::Get(s.desc);
    // A Percent setting has no per-value sentence -- "eighty percent" explains itself, and inventing
    // five sentences that differ only in a number would be noise.
    if (s.kind == Kind::Named) {
        out += L' ';
        out += Phrase::Get(s.descs[g_values[g_cursor].load(std::memory_order_relaxed)]);
    }
    Speech::Output(out, true);
    return true;
}

// (S130's ApplyTextGlyphs was removed in S147 along with the row it pushed. The glyph variant is
// no longer a setting to apply -- GameText detects it from the loaded font atlas itself, on its own
// schedule, and the menu has nothing to say about it.)

} // namespace

bool Init() {
    if (g_initialized) return true;
    Load();                                     // seeds defaults, then overlays the stored file
    InputTracker::SetModMenuNavCallback(&OnMenuNavKey);
    InputTracker::SetModMenuDescribeCallback(&OnDescribe);
    g_initialized = true;
    for (int i = 0; i < kCount; ++i) LogState("initialized", i);
    return true;
}

void Shutdown() {
    if (!g_initialized) return;
    InputTracker::SetModMenuNavCallback(nullptr);
    InputTracker::SetModMenuDescribeCallback(nullptr);
    g_open.store(false, std::memory_order_relaxed);
    g_initialized = false;
}

Verbosity CombatVerbosity() {
    const int v = EffectiveValue(SettingId::CombatVerbosity);
    return static_cast<Verbosity>(v);
}

bool AudioBeaconOn() {
    return EffectiveValue(SettingId::AudioBeacon) == static_cast<int>(Beacon::On);
}

bool TargetBeaconOn() {
    return EffectiveValue(SettingId::TargetBeacon) == static_cast<int>(Beacon::On);
}

bool AutoWalkOn() {
    return EffectiveValue(SettingId::AutoWalk) == static_cast<int>(Beacon::On);
}

// S147. Read from the game thread (the shop highlight handler, the target-change announce) and the
// input thread (F7), so the same relaxed-atomic discipline as every row above. It gates only what is
// VOLUNTEERED -- `4`-`9` and `o` never consult it, by design.
bool AutoDetailOn() {
    return EffectiveValue(SettingId::AutoDetail) == static_cast<int>(Beacon::On);
}

// S132. Read from the game thread (the gauge hook and the field frame) and the input thread (the
// key handlers), so the same relaxed-atomic-load discipline as the rows above.
//
// BOTH ARE CONTEXT-GATED, so both go through EffectiveValue and answer FALSE whenever no shout
// sequence is running -- however the stored value happens to be set. That is the whole guarantee
// this pair needs: outside the sequence there is no meter to speak, nothing to call a guard, and
// nothing the instant fill could honestly write.
bool PuzzleGuideOn() { return EffectiveValue(SettingId::PuzzleGuide) == static_cast<int>(Beacon::On); }
bool PuzzleSkipOn()  { return EffectiveValue(SettingId::PuzzleSkip)  == static_cast<int>(Beacon::On); }

bool ControllerOn() {
    return EffectiveValue(SettingId::Controller) == static_cast<int>(Beacon::On);
}

float BeaconVolume() { return GainOf(SettingId::BeaconVolume); }
float TargetVolume() { return GainOf(SettingId::TargetVolume); }

bool IsOpen() { return g_open.load(std::memory_order_relaxed); }

void Toggle() {
    const bool open = !g_open.load(std::memory_order_relaxed);
    g_open.store(open, std::memory_order_relaxed);
    if (!open) {
        Speech::Output(Phrase::Get(Id::ModMenuClosed), true);
        Log::Write("MODMENU", "closed");
        return;
    }
    g_cursor = FirstVisible();   // S132: never open on a context-gated row that does not apply now
    Speech::Output(std::wstring(Phrase::Get(Id::ModMenu)) + L". " + NameAndValue(g_cursor) + L".", true);
    Log::Write("MODMENU", "opened");
}

void CycleSetting(SettingId id) { Adjust(id, +1); }

void Adjust(SettingId id, int delta) {
    const int i = static_cast<int>(id);
    if (i < 0 || i >= kCount) return;
    const Setting& s = kSettings[i];
    const int cur = g_values[i].load(std::memory_order_relaxed);

    int next = cur + (delta < 0 ? -1 : 1);
    if (s.kind == Kind::Percent) {
        if (next < 0) next = 0;
        if (next >= s.count) next = s.count - 1;
    } else {
        next = ((next % s.count) + s.count) % s.count;   // wrap, and never go negative
    }
    // Speak even when the value did not move: at the end of a volume range the repeated number IS
    // how the player hears they have run out of range. Going silent there would read as a dropped
    // keypress. Skip the write and the file, though -- nothing changed.
    if (next != cur) {
        g_values[i].store(next, std::memory_order_relaxed);
        Save();
        LogState("set", i);
    }
    // The value alone, not the setting name: F4 is a dedicated key whose meaning the player already
    // knows, and inside the menu they just heard the name. Short enough to use mid-fight.
    Speech::Output(ValueOf(i), true);
}

// (`SetSilently` lived here until Session 115. It set a value without speaking it -- the one caller
// was sneak assist's auto-off on a map change -- and went with the toggle that needed it. `Adjust`
// is once again the ONLY place a setting's value changes, which is what CLAUDE.md's
// one-choke-point-per-surface rule asks for.)

} // namespace ModMenu

#include "ui/mod_menu.h"

#include "core/logger.h"
#include "input/input_tracker.h"
#include "speech/phrasebook.h"
#include "speech/speech.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ModMenu {
namespace {

using Phrase::Id;

// ---- the settings table -------------------------------------------------------------------------
// One row per SettingId, in enum order. `values` and `descs` are parallel: values[i] is the spoken
// name of value i, descs[i] the sentence explaining what value i does. `count` is how many values
// the setting has, which is what CycleSetting wraps on.
struct Setting {
    Id  name;
    int count;
    Id  values[2];
    Id  descs[2];
    Id  desc;            // the setting-level sentence, spoken before the value's sentence
    const char* key;     // token used in the settings file; never spoken
    int defValue;        // used when there is no stored file, or the stored value is out of range
};

const Setting kSettings[] = {
    { Id::SettingCombatVerbosity, 2,
      { Id::VerbosityNormal,     Id::VerbosityVerbose },
      { Id::VerbosityDescNormal, Id::VerbosityDescVerbose },
      Id::VerbosityDesc, "combat_verbosity", 0 },
    { Id::SettingAudioBeacon, 2,
      { Id::BeaconOff,     Id::BeaconOn },
      { Id::BeaconDescOff, Id::BeaconDescOn },
      Id::BeaconDesc, "audio_beacon", 1 },
};

static_assert(sizeof(kSettings) / sizeof(kSettings[0]) == static_cast<size_t>(SettingId::Count),
              "mod_menu.cpp kSettings and ModMenu::SettingId are out of sync");

constexpr int kCount = static_cast<int>(SettingId::Count);

// Read from the game thread (CombatFormat::ShouldSpeakNow, on the message-bus hook) and written from
// the input thread. Relaxed is enough: each is a lone byte-sized value with no ordering relationship
// to anything else, and a one-frame-stale read at worst logs a line that would have been spoken.
std::atomic<int>  g_values[kCount] = {};
std::atomic<bool> g_open{false};
int               g_cursor = 0;      // input thread only
bool              g_initialized = false;

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
    return Phrase::Get(s.values[g_values[i].load(std::memory_order_relaxed)]);
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
        case VK_UP:
            g_cursor = (g_cursor + kCount - 1) % kCount;
            Speech::Output(NameAndValue(g_cursor), true);
            return true;
        case VK_DOWN:
            g_cursor = (g_cursor + 1) % kCount;
            Speech::Output(NameAndValue(g_cursor), true);
            return true;
        case VK_HOME:
            g_cursor = 0;
            Speech::Output(NameAndValue(g_cursor), true);
            return true;
        case VK_END:
            g_cursor = kCount - 1;
            Speech::Output(NameAndValue(g_cursor), true);
            return true;
        case VK_LEFT:
        case VK_RIGHT:
            // Both directions advance: every setting here is two-valued, so "previous" and "next"
            // are the same move. Widen this when a setting with three or more values arrives.
            CycleSetting(static_cast<SettingId>(g_cursor));
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
    out += L' ';
    out += Phrase::Get(s.descs[g_values[g_cursor].load(std::memory_order_relaxed)]);
    Speech::Output(out, true);
    return true;
}

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
    const int v = g_values[static_cast<int>(SettingId::CombatVerbosity)].load(std::memory_order_relaxed);
    return static_cast<Verbosity>(v);
}

bool AudioBeaconOn() {
    return g_values[static_cast<int>(SettingId::AudioBeacon)].load(std::memory_order_relaxed)
           == static_cast<int>(Beacon::On);
}

bool IsOpen() { return g_open.load(std::memory_order_relaxed); }

void Toggle() {
    const bool open = !g_open.load(std::memory_order_relaxed);
    g_open.store(open, std::memory_order_relaxed);
    if (!open) {
        Speech::Output(Phrase::Get(Id::ModMenuClosed), true);
        Log::Write("MODMENU", "closed");
        return;
    }
    g_cursor = 0;
    Speech::Output(std::wstring(Phrase::Get(Id::ModMenu)) + L". " + NameAndValue(g_cursor) + L".", true);
    Log::Write("MODMENU", "opened");
}

void CycleSetting(SettingId id) {
    const int i = static_cast<int>(id);
    if (i < 0 || i >= kCount) return;
    const int next = (g_values[i].load(std::memory_order_relaxed) + 1) % kSettings[i].count;
    g_values[i].store(next, std::memory_order_relaxed);
    Save();
    // The value alone, not the setting name: F4 is a dedicated key whose meaning the player already
    // knows, and inside the menu they just heard the name. Short enough to use mid-fight.
    Speech::Output(ValueOf(i), true);
    LogState("set", i);
}

} // namespace ModMenu

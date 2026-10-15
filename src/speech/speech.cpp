#include "speech/speech.h"
#include "speech/phrasebook.h"
#include "core/logger.h"
#include "core/stall_probe.h"
#include <Windows.h>
#include <mutex>
#include <atomic>
#include <cstring>

// Tolk function typedefs (loaded at runtime — Tolk is user-supplied)
typedef void(__cdecl* PFN_Tolk_Load)();
typedef bool(__cdecl* PFN_Tolk_IsLoaded)();
typedef void(__cdecl* PFN_Tolk_Unload)();
typedef void(__cdecl* PFN_Tolk_TrySAPI)(bool);
typedef const wchar_t*(__cdecl* PFN_Tolk_DetectScreenReader)();
typedef bool(__cdecl* PFN_Tolk_HasSpeech)();
typedef bool(__cdecl* PFN_Tolk_Output)(const wchar_t*, bool);
typedef bool(__cdecl* PFN_Tolk_Speak)(const wchar_t*, bool);
typedef bool(__cdecl* PFN_Tolk_Silence)();

static HMODULE g_tolkDll = nullptr;
static std::atomic<bool> g_speechEnabled{true};
static std::mutex g_tolkMutex;
static std::atomic<uint64_t> g_utterances{0};

static PFN_Tolk_Load                g_Tolk_Load = nullptr;
static PFN_Tolk_IsLoaded            g_Tolk_IsLoaded = nullptr;
static PFN_Tolk_Unload              g_Tolk_Unload = nullptr;
static PFN_Tolk_TrySAPI             g_Tolk_TrySAPI = nullptr;
static PFN_Tolk_DetectScreenReader  g_Tolk_DetectScreenReader = nullptr;
static PFN_Tolk_HasSpeech           g_Tolk_HasSpeech = nullptr;
static PFN_Tolk_Output              g_Tolk_Output = nullptr;
static PFN_Tolk_Speak               g_Tolk_Speak = nullptr;
static PFN_Tolk_Silence             g_Tolk_Silence = nullptr;

static std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    // Three UTF-8 bytes per UTF-16 unit is sufficient even for surrogate pairs.
    // Keep full speech diagnostics, using the logger's shared conversion path.
    std::string utf8(wide.size() * 3 + 1, '\0');
    Log::ToUtf8(wide, utf8.data(), utf8.size());
    utf8.resize(std::strlen(utf8.c_str()));
    return utf8;
}

// ---- Speech diagnostic -------------------------------------------------------------------------------
// EVERY speech path logs, spoken or not. Output() — the PREFERRED entry point (speech + braille), and so
// the one most of the mod actually calls — was the only path that logged nothing, so no log ever recorded
// what the mod said. That made "what did it announce?" answerable only from memory and easy to confuse
// with an identical string from another subsystem (the Load screen's "Nalbina Fortress: Lower Apartments"
// save-slot label vs. the area announcement). Suppressed calls log their REASON too: silence with no
// record is indistinguishable from a bug that never fired.
static void LogSpoken(const char* tag, const std::wstring& text) {
    Log::Write(tag, WideToUtf8(text).c_str());
}
static void LogSuppressed(const char* tag, const std::wstring& text, const char* why) {
    std::string m = "[not spoken: ";
    m += why;
    m += "] ";
    m += WideToUtf8(text);
    Log::Write(tag, m.c_str());
}

// All access to DLL exports, including availability checks, shares this mutex.
// Clear pointers before a later caller can reach an unloaded library.
static void ReleaseTolk(bool unload) {
    if (g_tolkDll) {
        if (unload && g_Tolk_Unload) g_Tolk_Unload();
        FreeLibrary(g_tolkDll);
    }
    g_tolkDll = nullptr;
    g_Tolk_Load = nullptr;
    g_Tolk_IsLoaded = nullptr;
    g_Tolk_Unload = nullptr;
    g_Tolk_TrySAPI = nullptr;
    g_Tolk_DetectScreenReader = nullptr;
    g_Tolk_HasSpeech = nullptr;
    g_Tolk_Output = nullptr;
    g_Tolk_Speak = nullptr;
    g_Tolk_Silence = nullptr;
}

// One dispatch path preserves each caller's queue/mute policy and diagnostic tag.
// No event or text deduplication. Count only output the Tolk backend accepted.
static void Dispatch(const char* tag, const std::wstring& text, bool interrupt, bool bypassMute) {
    if (!bypassMute && !g_speechEnabled.load(std::memory_order_relaxed)) {
        LogSuppressed(tag, text, "muted");
        return;
    }
    STALL_SCOPE("Speech::Output");
    bool available = false, accepted = false;
    {
        std::lock_guard<std::mutex> lock(g_tolkMutex);
        available = g_Tolk_Output != nullptr;
        if (available) accepted = g_Tolk_Output(text.c_str(), interrupt);
        if (accepted) g_utterances.fetch_add(1, std::memory_order_relaxed);
    }
    // Logging stays outside the Tolk lock.
    if (!available) LogSuppressed(tag, text, "Tolk unavailable");
    else if (!accepted) LogSuppressed(tag, text, "Tolk rejected output");
    else LogSpoken(tag, text);
}

namespace Speech {

bool Init() {
    std::lock_guard<std::mutex> lock(g_tolkMutex);
    if (g_tolkDll) return true;
    // Tolk.dll is USER-SUPPLIED. Absence is non-fatal — the mod logs and
    // continues silently with all speak calls as no-ops.
    g_tolkDll = LoadLibraryA("Tolk.dll");
    if (!g_tolkDll) {
        Log::Write("SPEECH", "Tolk.dll not found in game directory — "
                             "mod continues silently. User must deploy Tolk.dll "
                             "and nvdaControllerClient64.dll to the x64 folder.");
        return false;
    }

    g_Tolk_Load               = (PFN_Tolk_Load)GetProcAddress(g_tolkDll, "Tolk_Load");
    g_Tolk_IsLoaded           = (PFN_Tolk_IsLoaded)GetProcAddress(g_tolkDll, "Tolk_IsLoaded");
    g_Tolk_Unload             = (PFN_Tolk_Unload)GetProcAddress(g_tolkDll, "Tolk_Unload");
    g_Tolk_TrySAPI            = (PFN_Tolk_TrySAPI)GetProcAddress(g_tolkDll, "Tolk_TrySAPI");
    g_Tolk_DetectScreenReader = (PFN_Tolk_DetectScreenReader)GetProcAddress(g_tolkDll, "Tolk_DetectScreenReader");
    g_Tolk_HasSpeech          = (PFN_Tolk_HasSpeech)GetProcAddress(g_tolkDll, "Tolk_HasSpeech");
    g_Tolk_Output             = (PFN_Tolk_Output)GetProcAddress(g_tolkDll, "Tolk_Output");
    g_Tolk_Speak              = (PFN_Tolk_Speak)GetProcAddress(g_tolkDll, "Tolk_Speak");
    g_Tolk_Silence            = (PFN_Tolk_Silence)GetProcAddress(g_tolkDll, "Tolk_Silence");

    if (!g_Tolk_Load || !g_Tolk_Output) {
        Log::Write("SPEECH", "Failed to resolve Tolk function pointers (Tolk.dll loaded but exports missing)");
        ReleaseTolk(false);
        return false;
    }

    if (g_Tolk_TrySAPI) {
        g_Tolk_TrySAPI(true);
    }

    g_Tolk_Load();

    const wchar_t* srName = g_Tolk_DetectScreenReader ? g_Tolk_DetectScreenReader() : nullptr;
    if (srName) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Screen reader detected: %ls", srName);
        Log::Write("SPEECH", msg);
    } else {
        Log::Write("SPEECH", "No screen reader detected (SAPI fallback may be available)");
    }

    return true;
}

void Speak(const std::wstring& text, bool interrupt) {
    Dispatch("SPEAK", text, interrupt, false);
}

void SpeakQueued(const std::wstring& text) {
    Dispatch("SPEAK-Q", text, false, false);
}

// ---- UTTERANCE COUNT (S192) ---------------------------------------------------------------------
// A monotonic tick of accepted output, not completed playback, so a caller can tell whether anything has been
// said since some earlier moment. It exists for AUTO DETAIL's ordering problem: a volunteered
// description must never get ahead of the row line it belongs behind, and the readers that speak
// that line are scattered across a dozen files and several deferral paths. Asking "has anything been
// spoken for this focus yet?" at the one choke point every one of them funnels through answers it
// without any of them having to report in.
//
// NOT a dedup and not a gate on speech: nothing consults it before speaking, and it can only ever
// cause the mod to say LESS of its own volunteered extra, never to suppress something a player
// asked for.
uint64_t UtteranceCount() { return g_utterances.load(std::memory_order_relaxed); }

void Output(const std::wstring& text, bool interrupt) {
    Dispatch("SPEAK-OUT", text, interrupt, false);
}

void Silence() {
    std::lock_guard<std::mutex> lock(g_tolkMutex);
    if (g_Tolk_Silence) g_Tolk_Silence();
}

bool IsAvailable() {
    std::lock_guard<std::mutex> lock(g_tolkMutex);
    return g_Tolk_IsLoaded && g_Tolk_IsLoaded();
}

std::wstring GetScreenReaderName() {
    std::lock_guard<std::mutex> lock(g_tolkMutex);
    if (!g_Tolk_DetectScreenReader) return L"";
    const wchar_t* name = g_Tolk_DetectScreenReader();
    return name ? std::wstring(name) : std::wstring();
}

void SetEnabled(bool enabled) {
    g_speechEnabled.store(enabled);
    Log::Write("SPEECH", enabled ? "Speech enabled" : "Speech disabled (muted)");
}

bool IsEnabled() {
    return g_speechEnabled.load(std::memory_order_relaxed);
}

void ToggleEnabled() {
    bool newVal = !g_speechEnabled.load();
    g_speechEnabled.store(newVal);
    Log::Write("SPEECH", newVal ? "Speech unmuted (F1)" : "Speech muted (F1)");
    Raw(Phrase::Get(newVal ? Phrase::Id::SpeechOn : Phrase::Id::SpeechOff), true);
}

void Raw(const std::wstring& text, bool interrupt) {
    Dispatch("SPEAK-RAW", text, interrupt, true);
}

void Shutdown() {
    {
        std::lock_guard<std::mutex> lock(g_tolkMutex);
        ReleaseTolk(true);
    }
    Log::Write("SPEECH", "Speech module shut down");
}

} // namespace Speech

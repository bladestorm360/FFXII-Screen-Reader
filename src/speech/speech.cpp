#include "speech/speech.h"
#include "core/logger.h"
#include <Windows.h>
#include <mutex>
#include <atomic>

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
    if (wide.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(),
                                   nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "(encoding error)";
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(),
                         &utf8[0], len, nullptr, nullptr);
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

namespace Speech {

bool Init() {
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
        FreeLibrary(g_tolkDll);
        g_tolkDll = nullptr;
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
    if (!g_speechEnabled.load(std::memory_order_relaxed)) { LogSuppressed("SPEAK", text, "muted"); return; }
    if (!g_Tolk_Output) { LogSuppressed("SPEAK", text, "Tolk unavailable"); return; }
    {
        std::lock_guard<std::mutex> lock(g_tolkMutex);
        g_Tolk_Output(text.c_str(), interrupt);
    }
    LogSpoken("SPEAK", text);
}

void SpeakQueued(const std::wstring& text) {
    if (!g_speechEnabled.load(std::memory_order_relaxed)) { LogSuppressed("SPEAK-Q", text, "muted"); return; }
    if (!g_Tolk_Output) { LogSuppressed("SPEAK-Q", text, "Tolk unavailable"); return; }
    {
        std::lock_guard<std::mutex> lock(g_tolkMutex);
        g_Tolk_Output(text.c_str(), false);
    }
    LogSpoken("SPEAK-Q", text);
}

void Output(const std::wstring& text, bool interrupt) {
    if (!g_speechEnabled.load(std::memory_order_relaxed)) { LogSuppressed("SPEAK-OUT", text, "muted"); return; }
    if (!g_Tolk_Output) { LogSuppressed("SPEAK-OUT", text, "Tolk unavailable"); return; }
    {
        // Scope the lock so the file write happens OUTSIDE it (matches Speak) — logging every Output call
        // must not serialize behind the Tolk mutex.
        std::lock_guard<std::mutex> lock(g_tolkMutex);
        g_Tolk_Output(text.c_str(), interrupt);
    }
    LogSpoken("SPEAK-OUT", text);
}

void Silence() {
    if (!g_Tolk_Silence) return;
    std::lock_guard<std::mutex> lock(g_tolkMutex);
    g_Tolk_Silence();
}

bool IsAvailable() {
    return g_Tolk_IsLoaded && g_Tolk_IsLoaded();
}

std::wstring GetScreenReaderName() {
    if (!g_Tolk_DetectScreenReader) return L"";
    std::lock_guard<std::mutex> lock(g_tolkMutex);
    const wchar_t* name = g_Tolk_DetectScreenReader();
    if (name) return std::wstring(name);
    return L"";
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
    Raw(newVal ? L"Speech on" : L"Speech off", true);
}

void Raw(const std::wstring& text, bool interrupt) {
    // Bypasses mute guard — always outputs (e.g., "Speech off" announcement)
    if (!g_Tolk_Output) { LogSuppressed("SPEAK-RAW", text, "Tolk unavailable"); return; }
    {
        std::lock_guard<std::mutex> lock(g_tolkMutex);
        g_Tolk_Output(text.c_str(), interrupt);
    }
    LogSpoken("SPEAK-RAW", text);
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_tolkMutex);
    if (g_Tolk_Unload) {
        g_Tolk_Unload();
    }
    if (g_tolkDll) {
        FreeLibrary(g_tolkDll);
        g_tolkDll = nullptr;
    }
    Log::Write("SPEECH", "Speech module shut down");
}

} // namespace Speech

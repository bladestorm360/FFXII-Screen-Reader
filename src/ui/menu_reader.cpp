#include "ui/menu_reader.h"
#include "ui/menu_observer.h"
#include "ui/text_capture.h"
#include "input/input_tracker.h"
#include "speech/speech.h"
#include "core/logger.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace {

// Phase B is text-source-agnostic: we don't know yet which text-wrapper arg
// carries position info (Stream A G-A3 confirms this). So the picker is
// temporal: on each focus change, look at text events captured since the
// PRIOR focus change (or up to N ms of history if it's the first change).
// We speak the LATEST captured event in that window.
//
// This will be wrong for some menus (background labels drawn last, etc.)
// — the log dump tells us what we picked vs. all candidates so we can
// refine the picker after observation. NO HARDCODING of option names
// ever appears in this file or any other; the picker selects from
// game-captured text, never from a table.
constexpr uint64_t FIRST_FOCUS_LOOKBACK_MS = 200;
constexpr size_t   MAX_QUERY_EVENTS = 32;

// Input-correlation gate. Title screens (and many gameplay screens) have
// background animation, particles, scrolling text, etc. that move the
// cursor field or its neighbours every frame even without user input —
// MenuObserver's edge-trigger on (X,Y) change would fire constantly,
// drowning the actual focus-change event in animation noise. Only speak
// if there was a real keyboard event within this window of the focus
// change.
//
// NOTE: keyboard only (WH_KEYBOARD_LL). Gamepad input does NOT currently
// update the input timestamp, so gamepad-driven focus changes will be
// suppressed by this gate. Known limitation tracked in
// project_menu_reader_scaffolding.md; gamepad path = future work.
constexpr uint64_t INPUT_GATE_WINDOW_MS = 200;

std::mutex g_mutex;
uint64_t g_priorFocusMs = 0;
void*    g_priorMenuObj = nullptr;
bool g_initialized = false;

// Convert a UTF-8 or wide char-mix to a UTF-8 char buffer for logging.
void LogWide(const char* prefix, const std::wstring& text) {
    char utf8[512] = {};
    if (!text.empty()) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                            utf8, sizeof(utf8) - 1, nullptr, nullptr);
    }
    char line[768];
    snprintf(line, sizeof(line), "%s\"%s\"", prefix, utf8);
    Log::Write("READER", line);
}

void OnFocusChanged(const MenuObserver::MenuSnapshot& snap) {
    // Determine the lookback window.
    uint64_t now = snap.timestampMs;
    uint64_t lookbackStart;
    bool isFirstForObj;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_priorMenuObj != snap.menuObj || g_priorFocusMs == 0) {
            isFirstForObj = true;
            lookbackStart = (now > FIRST_FOCUS_LOOKBACK_MS)
                            ? (now - FIRST_FOCUS_LOOKBACK_MS) : 0;
        } else {
            isFirstForObj = false;
            lookbackStart = g_priorFocusMs;
        }
        g_priorFocusMs = now;
        g_priorMenuObj = snap.menuObj;
    }

    uint64_t msSinceInput = InputTracker::MsSinceLastInput();
    bool inputCorrelated = msSinceInput <= INPUT_GATE_WINDOW_MS;

    char header[320];
    snprintf(header, sizeof(header),
             "focus change: menu_obj=%p type=%u X=%u Y=%d vis=%d "
             "controller=0x%X (%s lookback) ms_since_input=%llu %s",
             snap.menuObj, snap.typeByte, snap.cursorX, (int)snap.cursorY,
             snap.cursorVisible ? 1 : 0, snap.controllerRva,
             isFirstForObj ? "first" : "delta",
             (unsigned long long)(msSinceInput == UINT64_MAX ? 0 : msSinceInput),
             inputCorrelated ? "IN-WINDOW" : "no-recent-input");
    Log::Write("READER", header);

    // Gate 1: cursor must be visible.
    if (!snap.cursorVisible) {
        Log::Write("READER", "  cursor not visible — staying silent");
        return;
    }

    // Gate 2: input must be recent. Suppresses speech caused by animation
    // moving the cursor field with no actual user input. The first focus
    // for a menu_obj is allowed through unconditionally because opening a
    // menu IS a user action even if our timestamp didn't catch the exact
    // input event (e.g., gamepad press, or input that pre-dates Init).
    if (!isFirstForObj && !inputCorrelated) {
        Log::Write("READER", "  no recent input — staying silent (animation noise)");
        return;
    }

    // Query text events in the window.
    auto events = TextCapture::RecentEvents(snap.menuObj, lookbackStart, MAX_QUERY_EVENTS);

    if (events.empty()) {
        Log::Write("READER", "  no text events in window — staying silent");
        TextCapture::DumpRingToLog("focus-text resolve failed (empty window)");
        return;
    }

    // Log every candidate for diagnostic.
    {
        char line[80];
        snprintf(line, sizeof(line), "  candidates (%zu):", events.size());
        Log::Write("READER", line);
        for (size_t i = 0; i < events.size(); i++) {
            const auto& ev = events[i];
            char prefix[96];
            snprintf(prefix, sizeof(prefix),
                     "    [%zu] t=%llums caller=0x%X text=",
                     i, (unsigned long long)ev.timestampMs, ev.callerRva);
            LogWide(prefix, ev.text);
        }
    }

    // Picker — current heuristic: speak the LATEST captured event in the
    // window. This is wrong for many menus (z-order, background labels) but
    // gives us observable behavior to iterate from. Refinement (caller-RVA
    // filtering, position matching) lands after Stream A G-A3 / G-A4 hand
    // us the wrapper-arg semantics.
    const auto& chosen = events.back();
    LogWide("  speaking: ", chosen.text);
    Speech::Output(chosen.text, /*interrupt=*/true);
}

} // namespace

namespace MenuReader {

bool Init() {
    if (g_initialized) {
        Log::Write("READER", "MenuReader::Init called twice — ignoring");
        return true;
    }
    MenuObserver::SetFocusChangeCallback(&OnFocusChanged);
    g_initialized = true;
    Log::Write("READER", "MenuReader initialized; subscribed to focus changes. "
                         "Picker is temporal (latest text in window); refine after "
                         "Frida G-A3 confirms text-wrapper semantics.");
    return true;
}

void Shutdown() {
    if (!g_initialized) return;
    MenuObserver::SetFocusChangeCallback(nullptr);
    g_initialized = false;
    Log::Write("READER", "MenuReader shut down");
}

} // namespace MenuReader

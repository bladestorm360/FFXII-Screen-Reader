#include "input/input_tracker.h"
#include "core/logger.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

std::atomic<uint64_t> g_lastInputMs{0};
HHOOK g_hook = nullptr;

// Called from the kernel-installed low-level keyboard hook thread.
// MUST be fast: do nothing that can block or call back into mod code.
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            g_lastInputMs.store(GetTickCount64(), std::memory_order_relaxed);
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

} // namespace

namespace InputTracker {

bool Init() {
    if (g_hook) {
        Log::Write("INPUT", "InputTracker::Init called twice — ignoring");
        return true;
    }
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                               GetModuleHandleW(nullptr), 0);
    if (!g_hook) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "SetWindowsHookEx(WH_KEYBOARD_LL) failed: GetLastError=%lu",
                 (unsigned long)GetLastError());
        Log::Write("INPUT", msg);
        return false;
    }
    Log::Write("INPUT", "InputTracker installed (WH_KEYBOARD_LL). "
                       "Keyboard only; gamepad input does not currently update the timestamp.");
    return true;
}

void Shutdown() {
    if (!g_hook) return;
    UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    g_lastInputMs.store(0, std::memory_order_relaxed);
    Log::Write("INPUT", "InputTracker shut down");
}

uint64_t LastInputTimestampMs() {
    return g_lastInputMs.load(std::memory_order_relaxed);
}

uint64_t MsSinceLastInput() {
    uint64_t last = g_lastInputMs.load(std::memory_order_relaxed);
    if (last == 0) return UINT64_MAX;
    uint64_t now = GetTickCount64();
    return (now >= last) ? (now - last) : 0;
}

bool WasRecentInput(uint64_t windowMs) {
    return MsSinceLastInput() <= windowMs;
}

} // namespace InputTracker

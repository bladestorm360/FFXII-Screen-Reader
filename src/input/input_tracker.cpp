#include "input/input_tracker.h"
#include "core/logger.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

// Deferred hotkey messages posted from the hook proc to the input thread's own
// message loop, so speech never runs inside the low-level hook callback.
constexpr UINT WM_DESCRIBE = WM_APP + 1;
constexpr UINT WM_REREAD   = WM_APP + 2;

std::atomic<uint64_t> g_lastInputMs{0};
HHOOK   g_hook = nullptr;
HANDLE  g_thread = nullptr;
DWORD   g_threadId = 0;
std::atomic<bool> g_oDown{false};      // edge-detect for the 'o' key (ignore auto-repeat)
std::atomic<bool> g_tDown{false};      // edge-detect for the 't' key (ignore auto-repeat)
InputTracker::HotkeyCallback g_describeCb = nullptr;
InputTracker::HotkeyCallback g_rereadCb = nullptr;

bool GameIsForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Low-level keyboard hook. Runs on the input thread (below) while it pumps
// messages. MUST stay fast: it only stamps the timestamp and posts a message.
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            g_lastInputMs.store(GetTickCount64(), std::memory_order_relaxed);
            if (kb && kb->vkCode == 'O') {   // 'o' = describe (i/j/k/l are alt arrow keys)
                // Edge-triggered (skip auto-repeat) + only when the game is focused.
                if (!g_oDown.exchange(true) && GameIsForeground())
                    PostThreadMessageW(g_threadId, WM_DESCRIBE, 0, 0);
            } else if (kb && kb->vkCode == 'T') {   // 't' = re-read last spoken line
                if (!g_tDown.exchange(true) && GameIsForeground())
                    PostThreadMessageW(g_threadId, WM_REREAD, 0, 0);
            }
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            if (kb && kb->vkCode == 'O') g_oDown.store(false);
            else if (kb && kb->vkCode == 'T') g_tDown.store(false);
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

DWORD WINAPI InputThread(LPVOID) {
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                               GetModuleHandleW(nullptr), 0);
    if (!g_hook) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "SetWindowsHookEx(WH_KEYBOARD_LL) failed: GetLastError=%lu",
                 (unsigned long)GetLastError());
        Log::Write("INPUT", msg);
        return 0;
    }
    Log::Write("INPUT", "InputTracker installed (WH_KEYBOARD_LL on a dedicated "
                        "message-loop thread). Keyboard only; gamepad does not "
                        "update the timestamp. 'o' = read focused item's description; "
                        "'t' = re-read last spoken line.");

    MSG m;
    BOOL r;
    while ((r = GetMessageW(&m, nullptr, 0, 0)) > 0) {
        if (m.message == WM_DESCRIBE) {
            InputTracker::HotkeyCallback cb = g_describeCb;
            if (cb) cb();
        } else if (m.message == WM_REREAD) {
            InputTracker::HotkeyCallback cb = g_rereadCb;
            if (cb) cb();
        } else {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    Log::Write("INPUT", "InputTracker thread exiting");
    return 0;
}

} // namespace

namespace InputTracker {

bool Init() {
    if (g_thread) {
        Log::Write("INPUT", "InputTracker::Init called twice — ignoring");
        return true;
    }
    g_thread = CreateThread(nullptr, 0, InputThread, nullptr, 0, &g_threadId);
    if (!g_thread) {
        char msg[128];
        snprintf(msg, sizeof(msg), "InputTracker: CreateThread failed: GetLastError=%lu",
                 (unsigned long)GetLastError());
        Log::Write("INPUT", msg);
        return false;
    }
    return true;
}

void Shutdown() {
    if (!g_thread) return;
    if (g_threadId) PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 2000);   // bounded so DLL detach can't hang
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_threadId = 0;
    g_lastInputMs.store(0, std::memory_order_relaxed);
    Log::Write("INPUT", "InputTracker shut down");
}

void SetDescribeCallback(HotkeyCallback cb) { g_describeCb = cb; }
void SetRereadCallback(HotkeyCallback cb) { g_rereadCb = cb; }

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

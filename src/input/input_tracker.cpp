#include "input/input_tracker.h"
#include "core/logger.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

// Deferred hotkey messages posted from the hook proc to the input thread's own
// message loop, so speech never runs inside the low-level hook callback.
constexpr UINT WM_DESCRIBE  = WM_APP + 1;
constexpr UINT WM_REREAD    = WM_APP + 2;
constexpr UINT WM_CONFIRM   = WM_APP + 6;   // game's Confirm (Space/Enter) -- observed, never sent
constexpr UINT WM_NAVKEY    = WM_APP + 3;   // wParam = vk, lParam = shift (0/1)
constexpr UINT WM_DIAG      = WM_APP + 4;   // wParam = vk, lParam = foreground(0/1) — input diagnostic
constexpr UINT WM_UNHOOK_LL = WM_APP + 5;   // retire the WH_KEYBOARD_LL hook once DInput owns input

// Input diagnostics (LL-hook key probe + the [ vs ] check). Input is confirmed
// working via the DirectInput path, so these are OFF; flip to true to re-diagnose.
constexpr bool DIAG_KEYS = false;
std::atomic<int> g_diagCount{0};

std::atomic<uint64_t> g_lastInputMs{0};
HHOOK   g_hook = nullptr;
HANDLE  g_thread = nullptr;
DWORD   g_threadId = 0;
std::atomic<bool> g_oDown{false};      // edge-detect for the 'o' key (ignore auto-repeat)
std::atomic<bool> g_tDown{false};      // edge-detect for the 't' key (ignore auto-repeat)
InputTracker::HotkeyCallback g_describeCb = nullptr;
InputTracker::HotkeyCallback g_rereadCb = nullptr;
InputTracker::HotkeyCallback g_confirmCb = nullptr;
InputTracker::NavKeyCallback g_navKeyCb = nullptr;

// Navigation keys (edge-detected independently so auto-repeat is suppressed).
constexpr DWORD kNavVks[4] = { VK_OEM_5 /*\*/, VK_OEM_4 /*[*/, VK_OEM_6 /*]*/, VK_OEM_3 /*`*/ };
std::atomic<bool> g_navDown[4]{};
int NavIdx(DWORD vk) {
    for (int i = 0; i < 4; ++i) if (kNavVks[i] == vk) return i;
    return -1;
}

bool GameIsForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// ---- DirectInput keyboard path (primary; fed by the dinput8 proxy) ----------
// Once the game's GetDeviceState feed is active, it OWNS key dispatch; the LL hook
// (which the game starves anyway) drops to diagnostics only, so keys never double-fire.
std::atomic<bool> g_dinputActive{false};

// DIK scan codes (dinput.h) for the mod's keys.
constexpr int DIK_O = 0x18, DIK_T = 0x14, DIK_LBRACKET = 0x1A, DIK_RBRACKET = 0x1B,
              DIK_GRAVE = 0x29, DIK_BACKSLASH = 0x2B, DIK_LSHIFT = 0x2A, DIK_RSHIFT = 0x36,
              DIK_MINUS = 0x0C, DIK_EQUALS = 0x0D, DIK_SEMICOLON = 0x27, DIK_APOSTROPHE = 0x28,
              DIK_SLASH = 0x35, DIK_P = 0x19;
// Party-status keys. DIK number row is 1..0 == 0x02..0x0B, so 4/5/6/7 = 0x05/0x06/0x07/0x08.
// Free in this game: it binds 1/2/3 to Game Speed and nothing to 4-7 (Docs/Controls.md).
// 7 reads roster slot 3, the GUEST slot (list 3 has nine slots: 0-2 active, 3 guest, 4-8 reserve).
constexpr int DIK_4 = 0x05, DIK_5 = 0x06, DIK_6 = 0x07, DIK_7 = 0x08;
// Combat-log navigation. Shift is deliberately NOT used: the game binds Left Shift to Toggle
// Walk/Run and the mod cannot swallow keys, so a Shift chord would silently flip walk/run on every
// press. Home/End are unbound and have no side effects.
constexpr int DIK_COMMA = 0x33, DIK_PERIOD = 0x34, DIK_HOME = 0xC7, DIK_END = 0xCF;
// F4: diagnostic A/B toggle for the menu-text painter interception. The game binds F1/F2/F3 to game
// speed and nothing to F4 (Docs/Controls.md), and the struck F4 modal combat-log design was never
// built, so the key is genuinely free. Plain key, no chord -- see the Shift note above.
constexpr int DIK_F4 = 0x3E;
// The game's Confirm (Docs/Controls.md: Space / Enter / Left Mouse). Observed only -- the mod is
// read-only on input and never swallows these, so the game's own text box advances exactly as it
// always did; we just learn that it did. Mouse confirm is not observed (no hook for it), so a
// mouse-only player simply gets no page advance rather than a wrong one.
constexpr int DIK_SPACE = 0x39, DIK_RETURN = 0x1C;

// Extra hotkeys beyond the 4 original nav keys: - = ; ' / p 4 5 6 7 , . Home End F4 (no Shift).
// NOTE: indices here are just slots in this array; the dispatch token is the VK passed to DInputEdge.
// Growing this array was once suspected of breaking 4/5/6 -- it never was; that was a missing
// pointer dereference in party_status.cpp. Keep the bound in step with the entries below.
std::atomic<bool> g_extraDown[15]{};
std::atomic<bool> g_confirmDown[2]{};   // Space / Enter edge flags (observed Confirm)
std::atomic<int>  g_bracketDiag{0};   // targeted [ vs ] confirmation (capped)

// Edge-detect one key from the per-frame DIK state and post its action (on the
// input thread) on the rising edge. `down` is this frame's state.
// No `shift` parameter -- it was passed `false` by every caller and could never be honoured: the
// game binds Left Shift to Toggle Walk/Run and the mod cannot swallow keys, so a Shift chord would
// silently flip walk/run on every press. Diagnostic keys must be plain and unbound.
void DInputEdge(DWORD vk, std::atomic<bool>& downFlag, bool down, bool isNav) {
    if (down) {
        if (!downFlag.exchange(true) && GameIsForeground()) {
            if (isNav)              PostThreadMessageW(g_threadId, WM_NAVKEY, (WPARAM)vk, 0);
            else if (vk == 'O')     PostThreadMessageW(g_threadId, WM_DESCRIBE, 0, 0);
            else if (vk == 'T')     PostThreadMessageW(g_threadId, WM_REREAD, 0, 0);
            else if (vk == VK_SPACE || vk == VK_RETURN)
                                    PostThreadMessageW(g_threadId, WM_CONFIRM, 0, 0);
        }
    } else {
        downFlag.store(false);
    }
}

// Low-level keyboard hook. Runs on the input thread (below) while it pumps
// messages. MUST stay fast: it only stamps the timestamp and posts a message.
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            g_lastInputMs.store(GetTickCount64(), std::memory_order_relaxed);
            if (DIAG_KEYS && kb && g_diagCount.fetch_add(1) < 80) {
                // Fires FIRST, before any gate — proves the hook sees the key.
                PostThreadMessageW(g_threadId, WM_DIAG, static_cast<WPARAM>(kb->vkCode),
                                   static_cast<LPARAM>(GameIsForeground() ? 1 : 0));
            }
            // Dispatch via the LL hook ONLY while the DirectInput feed isn't active
            // (the game starves this hook; DInput is the real path).
            if (!g_dinputActive.load(std::memory_order_relaxed)) {
                if (kb && kb->vkCode == 'O') {   // 'o' = describe
                    if (!g_oDown.exchange(true) && GameIsForeground())
                        PostThreadMessageW(g_threadId, WM_DESCRIBE, 0, 0);
                } else if (kb && kb->vkCode == 'T') {   // 't' = re-read
                    if (!g_tDown.exchange(true) && GameIsForeground())
                        PostThreadMessageW(g_threadId, WM_REREAD, 0, 0);
                } else if (kb) {
                    int ni = NavIdx(kb->vkCode);        // nav keys: \ [ ] `
                    if (ni >= 0 && !g_navDown[ni].exchange(true) && GameIsForeground()) {
                        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                        PostThreadMessageW(g_threadId, WM_NAVKEY,
                                           static_cast<WPARAM>(kb->vkCode),
                                           static_cast<LPARAM>(shift ? 1 : 0));
                    }
                }
            }
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            if (!g_dinputActive.load(std::memory_order_relaxed) && kb) {
                if (kb->vkCode == 'O') g_oDown.store(false);
                else if (kb->vkCode == 'T') g_tDown.store(false);
                else { int ni = NavIdx(kb->vkCode); if (ni >= 0) g_navDown[ni].store(false); }
            }
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
        } else if (m.message == WM_CONFIRM) {
            InputTracker::HotkeyCallback cb = g_confirmCb;
            if (cb) cb();
        } else if (m.message == WM_NAVKEY) {
            InputTracker::NavKeyCallback cb = g_navKeyCb;
            if (cb) cb(static_cast<int>(m.wParam));
        } else if (m.message == WM_DIAG) {
            char msg[96];
            snprintf(msg, sizeof(msg), "LL keydown vk=0x%02X fg=%d",
                     static_cast<unsigned>(m.wParam), static_cast<int>(m.lParam));
            Log::Write("INPUT-DIAG", msg);
        } else if (m.message == WM_UNHOOK_LL) {
            // DirectInput now owns key dispatch (the LL block below g_dinputActive is dead;
            // the recent-input timestamp is stamped by FeedDInputKeyboard). Retire the
            // system-wide WH_KEYBOARD_LL hook — from its OWNING thread — so a global keyboard
            // hook can't interfere with input. The thread stays alive to dispatch hotkeys.
            if (g_hook) {
                UnhookWindowsHookEx(g_hook);
                g_hook = nullptr;
                Log::Write("INPUT", "WH_KEYBOARD_LL hook retired (DirectInput feed owns input)");
            }
        } else {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if (g_hook) UnhookWindowsHookEx(g_hook);   // may already be retired on DInput-active
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
void SetConfirmCallback(HotkeyCallback cb) { g_confirmCb = cb; }
void SetRereadCallback(HotkeyCallback cb) { g_rereadCb = cb; }
void SetNavKeyCallback(NavKeyCallback cb) { g_navKeyCb = cb; }

void FeedDInputKeyboard(const unsigned char* dik) {
    if (!dik || !g_threadId) return;
    if (!g_dinputActive.exchange(true)) {
        Log::Write("INPUT", "DirectInput keyboard feed active — hotkeys via the game's own poll");
        // DInput now owns dispatch — retire the redundant global WH_KEYBOARD_LL hook
        // (removed on its owning thread; the input thread keeps running for hotkeys).
        PostThreadMessageW(g_threadId, WM_UNHOOK_LL, 0, 0);
    }

    // Stamp the "recent input" time on ANY key's rising edge (the menu reader gates
    // animation false-positives on this) — NOT on every per-frame poll.
    static unsigned char lastDik[256] = {};
    bool anyRising = false;
    for (int i = 0; i < 256; ++i) {
        if ((dik[i] & 0x80) && !(lastDik[i] & 0x80)) {
            anyRising = true;
            // Only [ (0x1A) and ] (0x1B), capped — confirms the game reports the [ key.
            if (DIAG_KEYS && (i == 0x1A || i == 0x1B) && g_bracketDiag.fetch_add(1) < 20) {
                char m[32];
                snprintf(m, sizeof(m), "DIK 0x%02X down (%s)", i, i == 0x1A ? "[" : "]");
                Log::Write("INPUT-DIAG", m);
            }
        }
        lastDik[i] = dik[i];
    }
    if (anyRising) g_lastInputMs.store(GetTickCount64(), std::memory_order_relaxed);

    // All hotkeys are standalone (no Shift — the game binds Left Shift to Walk/Run).
    DInputEdge('O',           g_oDown,       (dik[DIK_O]          & 0x80) != 0, false);
    DInputEdge('T',           g_tDown,       (dik[DIK_T]          & 0x80) != 0, false);
    DInputEdge(VK_SPACE,      g_confirmDown[0],(dik[DIK_SPACE]    & 0x80) != 0, false);  // Confirm (observed)
    DInputEdge(VK_RETURN,     g_confirmDown[1],(dik[DIK_RETURN]   & 0x80) != 0, false);  // Confirm (observed)
    DInputEdge(VK_OEM_5,      g_navDown[0],  (dik[DIK_BACKSLASH]  & 0x80) != 0, true);  // \  route
    DInputEdge(VK_OEM_4,      g_navDown[1],  (dik[DIK_LBRACKET]   & 0x80) != 0, true);  // [  prev object
    DInputEdge(VK_OEM_6,      g_navDown[2],  (dik[DIK_RBRACKET]   & 0x80) != 0, true);  // ]  next object
    DInputEdge(VK_OEM_3,      g_navDown[3],  (dik[DIK_GRAVE]      & 0x80) != 0, true);  // `  rescan
    DInputEdge(VK_F4,         g_extraDown[14],(dik[DIK_F4]         & 0x80) != 0, true);  // F4 text-capture A/B
    DInputEdge(VK_OEM_MINUS,  g_extraDown[0],(dik[DIK_MINUS]      & 0x80) != 0, true);  // -  prev category
    DInputEdge(VK_OEM_PLUS,   g_extraDown[1],(dik[DIK_EQUALS]     & 0x80) != 0, true);  // =  next category
    DInputEdge(VK_OEM_7,      g_extraDown[3],(dik[DIK_APOSTROPHE] & 0x80) != 0, true);  // '  diagnostic
    DInputEdge(VK_OEM_2,      g_extraDown[4],(dik[DIK_SLASH]      & 0x80) != 0, true);  // /  describe
    DInputEdge(VK_OEM_1,      g_extraDown[2],(dik[DIK_SEMICOLON]  & 0x80) != 0, true);  // ;  target status
    DInputEdge('P',           g_extraDown[5],(dik[DIK_P]          & 0x80) != 0, true);  // p  route to locked target
    DInputEdge('4',           g_extraDown[6],(dik[DIK_4]          & 0x80) != 0, true);  // 4  party slot 1 status
    DInputEdge('5',           g_extraDown[7],(dik[DIK_5]          & 0x80) != 0, true);  // 5  party slot 2 status
    DInputEdge('6',           g_extraDown[8],(dik[DIK_6]          & 0x80) != 0, true);  // 6  party slot 3 status
    DInputEdge('7',           g_extraDown[9],(dik[DIK_7]          & 0x80) != 0, true);  // 7  guest slot status
    DInputEdge(VK_OEM_COMMA,  g_extraDown[10],(dik[DIK_COMMA]     & 0x80) != 0, true);  // ,  log: older
    DInputEdge(VK_OEM_PERIOD, g_extraDown[11],(dik[DIK_PERIOD]    & 0x80) != 0, true);  // .  log: newer
    DInputEdge(VK_HOME,       g_extraDown[12],(dik[DIK_HOME]      & 0x80) != 0, true);  // Home log: oldest
    DInputEdge(VK_END,        g_extraDown[13],(dik[DIK_END]       & 0x80) != 0, true);  // End  log: newest
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

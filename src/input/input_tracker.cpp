#include "input/input_tracker.h"
#include "input/pad_hook.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "ui/text_prompt.h"
#include "ui/mod_menu.h"          // IsOpen -- the close-key mask below is scoped to it
#include "core/mem_read.h"
#include "speech/speech.h"
#include "speech/phrasebook.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

// Deferred hotkey messages posted from the hook proc to the input thread's own
// message loop, so speech never runs inside the low-level hook callback.
constexpr UINT WM_DESCRIBE  = WM_APP + 1;
constexpr UINT WM_REREAD    = WM_APP + 2;
// WM_APP + 6 was WM_CONFIRM (the game's Space/Enter, observed to page dialogue). Retired with the
// keypress pagination path -- see the note in input_tracker.h. Left unused rather than recycled so
// a stale PostThreadMessage from an old build can't land on a live handler.
constexpr UINT WM_NAVKEY    = WM_APP + 3;   // wParam = vk, lParam = shift (0/1)
constexpr UINT WM_DIAG      = WM_APP + 4;   // wParam = vk, lParam = foreground(0/1) — input diagnostic
constexpr UINT WM_UNHOOK_LL = WM_APP + 5;   // retire the WH_KEYBOARD_LL hook once DInput owns input
constexpr UINT WM_LICENSEPTS = WM_APP + 7;  // `U` -> read License Points (license board only)
constexpr UINT WM_MENUNAV   = WM_APP + 8;   // arrows + Home/End -> virtual-buffer nav (status screen).
                                            // wParam = VK; lParam = 1 if a combat-log fallback applies
                                            // (Home/End), 0 otherwise (arrows).
constexpr UINT WM_GIL       = WM_APP + 9;   // `g` -> speak party gil total (field / shop / menus)
constexpr UINT WM_PADSAY    = WM_APP + 10;  // wParam = Phrase::Id -> speak it from THIS thread. The
                                            // pad poll must not speak on the game's input thread;
                                            // see InputTracker::DispatchSpeakPhrase in the header.
constexpr UINT WM_PADSET    = WM_APP + 11;  // wParam = ModMenu::SettingId -> flip that row and speak
                                            // its name + new value. Its own message rather than a
                                            // DispatchModKey VK: these rows have no keyboard
                                            // shortcut, so reusing the key table would mean
                                            // inventing a phantom key to name each of them by.

// Input diagnostics (LL-hook key probe + the [ vs ] check). Input is confirmed
// working via the DirectInput path, so these are OFF; flip to true to re-diagnose.
constexpr bool DIAG_KEYS = false;
std::atomic<int> g_diagCount{0};

std::atomic<uint64_t> g_lastInputMs{0};
std::atomic<bool>     g_moveHeld{false};   // W/A/S/D, for the navigation stuck detector
HHOOK   g_hook = nullptr;
HANDLE  g_thread = nullptr;
DWORD   g_threadId = 0;
std::atomic<bool> g_oDown{false};      // edge-detect for the 'o' key (ignore auto-repeat)
std::atomic<bool> g_tDown{false};      // edge-detect for the 't' key (ignore auto-repeat)
std::atomic<bool> g_uDown{false};      // edge-detect for the 'U' key (License Points)
std::atomic<bool> g_gDown{false};      // edge-detect for the 'g' key (gil total)
InputTracker::HotkeyCallback g_describeCb = nullptr;
InputTracker::HotkeyCallback g_rereadCb = nullptr;
InputTracker::HotkeyCallback g_lpCb = nullptr;
InputTracker::HotkeyCallback g_gilCb = nullptr;
InputTracker::SettingToggleCallback g_settingToggleCb = nullptr;
InputTracker::NavKeyCallback g_navKeyCb = nullptr;
InputTracker::MenuNavCallback g_menuNavCb = nullptr;
// First refusal on arrows/Home/End and on `o` — see the header. Both decline while the mod menu is
// closed, which is almost always, so the normal paths are untouched.
InputTracker::MenuNavCallback g_modMenuNavCb = nullptr;
// Clan Primer page walk. Its own slot for the same reason the mod menu has one: StatusReader
// already owns the single MenuNav slot, and arbitrating here keeps both intact rather than one
// displacing the other. Declines whenever no primer entry is open.
InputTracker::MenuNavCallback g_primerNavCb = nullptr;
InputTracker::DescribeInterceptCallback g_modMenuDescribeCb = nullptr;

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
              DIK_SLASH = 0x35, DIK_P = 0x19, DIK_U = 0x16, DIK_G = 0x22;
// Party-status keys. DIK number row is 1..0 == 0x02..0x0B, so 4/5/6/7 = 0x05/0x06/0x07/0x08.
// Free in this game: it binds 1/2/3 to Game Speed and nothing to 4-7 (Docs/Controls.md).
// 7 reads roster slot 3, the GUEST slot (list 3 has nine slots: 0-2 active, 3 guest, 4-8 reserve).
constexpr int DIK_4 = 0x05, DIK_5 = 0x06, DIK_6 = 0x07, DIK_7 = 0x08;
// 8 and 9 join them for the shop's per-character equipment comparison, which can show six columns.
// Outside a shop, 8 reads the SUMMONED ESPER (S148) -- not a roster slot, which is why it needed its
// own key: an Esper is absent from roster list 3 entirely, so 4-7 can never address it.
//
// THEIR FREEDOM IS NOT PROVEN. Session 112's lesson is that the game's Controls screen omits
// bindings it really has (it never listed F9 = Hide On-Screen Keyboard), and probe_equip_compare's
// pad-word test could not settle these two: its pass condition was "no new pad value appears",
// which is indistinguishable from "the key was never pressed". So instead of claiming they are
// free, the poll below WATCHES for a collision and logs it -- see the note there.
constexpr int DIK_8 = 0x09, DIK_9 = 0x0A;
// Combat-log navigation. Shift is deliberately NOT used: the game binds Left Shift to Toggle
// Walk/Run and the mod cannot swallow keys, so a Shift chord would silently flip walk/run on every
// press. Home/End are unbound and have no side effects.
constexpr int DIK_COMMA = 0x33, DIK_PERIOD = 0x34, DIK_HOME = 0xC7, DIK_END = 0xCF;
// Bhujerba shout minigame: B reads the infamy meter, N reads the NPCs nearest the player.
//
// The tester first proposed `,` and `.` for these. Those are the COMBAT LOG's timeline navigation
// (older/newer), documented in Controls.md and README.md, and the log's whole point is that it
// works everywhere including mid-fight and in menus -- so taking them, even map-gated, would break
// reading the log back while standing on a Bhujerba street. B and N were chosen instead: neither
// appears in Docs/Controls.md's game bindings and neither is claimed anywhere in the mod.
//
// THEIR FREEDOM IS NO MORE PROVEN THAN 8/9's, and for the same Session 112 reason -- the game's
// Controls screen omits bindings it really has. They join the pad-word collision watch below.
constexpr int DIK_B = 0x30, DIK_N = 0x31;
// Virtual-buffer navigation (status screen). Arrow keys are READ, never swallowed -- the game still
// gets them. They only DO anything in the mod while a status buffer is active; elsewhere the mod
// ignores them. DIK extended-key scan codes (dinput.h): Up 0xC8, Down 0xD0, Left 0xCB, Right 0xCD.
constexpr int DIK_UP = 0xC8, DIK_DOWN = 0xD0, DIK_LEFT = 0xCB, DIK_RIGHT = 0xCD;
// The game's movement keys (Docs/Controls.md: W/S/A/D). OBSERVED, never touched -- the buffer is
// const and stays that way. This exists so navigation can tell "jammed against a wall" apart from
// "standing still reading a menu", which is the difference between a useful re-plan and a nuisance.
constexpr int DIK_W = 0x11, DIK_A = 0x1E, DIK_S = 0x1F, DIK_D = 0x20;
// The game binds F1/F2/F3 to Game Speed and NOTHING above that (Docs/Controls.md), so F4 upward are
// all ours. Plain keys, no chords -- see the Shift note above.
// F4: combat verbosity, Normal <-> Verbose. REPURPOSED in S90 from the painter-interception A/B
// diagnostic, which disabled row-text capture and so silently killed menu reading if pressed by
// accident -- an unacceptable thing to leave on a bare function key for a blind player.
// F5: nav availability filter (All <-> Story-gated).
// F6: label the focused entity from the clipboard. Same reasoning as F4/F5 -- the game binds only
// F1/F2/F3. The handler, the clipboard read, the persistence and the apply-before-numbering pass were
// all built in Session 65 and BOTH Controls.md and README.md documented the key, but `DIK_F6` was
// never defined and no edge was ever registered, so `case VK_F6:` has been dead code ever since and
// pressing F6 did precisely nothing. Confirmed in play by the tester, and again by the label store:
// 127 records, zero of them named.
// F7: autodetail, Off <-> On (Session 147 -- the key was RESERVED for it from S90 and is now spent).
// F8: open/close the mod's own settings menu.
// F10 is NOT bound (Session 115). It carried the sneak-assist toggle from S107 to S114; that feature
// now acts automatically on the maps `path_danger.cpp` names and has no setting, so the key went back
// to the game.
//
// ⚠ "THE GAME BINDS ONLY F1/F2/F3" IS FALSE, and this comment used to repeat it (Session 112).
// That came from the game's Controls CONFIGURATION screen, which lists only REBINDABLE actions --
// and the game has bindings it never shows there. **`F9` is the game's "Hide On-Screen Keyboard"**,
// measured from the game's own on-screen-keyboard overlay (its footer reads `F9 Hide On-Screen
// Keyboard` / `Space Close`). The mod cannot swallow keys, so while the audio beacon sat on F9 every
// toggle also flipped that full-screen panel. The beacon moved to **F11**; DIK_F9 is now unused by
// the mod and left to the game. Absence from a rebinding UI is not evidence a key is free.
constexpr int DIK_F4 = 0x3E, DIK_F5 = 0x3F, DIK_F6 = 0x40, DIK_F7 = 0x41, DIK_F8 = 0x42,
              DIK_F11 = 0x57;
// Ctrl/Alt scan codes for the BARE-KEY guard below. (DIK_LSHIFT / DIK_RSHIFT are already declared
// with the movement keys above.)
constexpr int DIK_LCTRL = 0x1D, DIK_RCTRL = 0x9D, DIK_LALT = 0x38, DIK_RALT = 0xB8;
// Escape closes the mod's own menu (Session 185, user instruction). It is dispatched ONLY as a
// virtual-buffer nav key, and ModMenu is the only consumer that answers it -- with the menu shut
// every reader declines and the press does nothing on our side.
//
// THIS IS NOT A CLAIM THAT ESCAPE IS FREE (L-51). The game's Controls screen lists an action it
// calls "Escape" bound to Left Ctrl, which says nothing about the physical Esc key, and that screen
// has already been caught hiding a binding once (`F9`). The mod cannot swallow a key, so if the
// game does own Esc, both things happen -- which is the same accepted cost the arrow keys carry
// inside this menu, and it only applies while the menu is open.
constexpr int DIK_ESCAPE = 0x01;
// Backspace closes the menu too (S192, user instruction). Same claim, same scope: a way out of the
// mod menu, never a way in, so it is only ever taken while the menu is open.
constexpr int DIK_BACK = 0x0E;
// Windows keys, for the same guard: Win+F-key is a shell chord and belongs to the shell.
constexpr int DIK_LWIN = 0xDB, DIK_RWIN = 0xDC;
// DIK_SPACE / DIK_RETURN are gone with the Confirm observation. The mod has no reason to watch the
// game's own Confirm: the only consumer was dialogue pagination, and a keyboard scan code cannot
// answer "did the box advance" for a player on a pad.

// Extra hotkeys beyond the 4 original nav keys: - = ; ' / p 4 5 6 7 , . Home End F4 (no Shift).
// NOTE: indices here are just slots in this array; the dispatch token is the VK passed to DInputEdge.
// Growing this array was once suspected of breaking 4/5/6 -- it never was; that was a missing
// pointer dereference in party_status.cpp. Keep the bound in step with the entries below.
std::atomic<bool> g_extraDown[30]{};   // 0-15 + 20-29 the keys below; 16-19 the arrow keys (status buffer)
std::atomic<int>  g_bracketDiag{0};   // targeted [ vs ] confirmation (capped)

// Edge-detect one key from the per-frame DIK state and post its action (on the
// input thread) on the rising edge. `down` is this frame's state.
// No `shift` parameter -- it was passed `false` by every caller and could never be honoured: the
// game binds Left Shift to Toggle Walk/Run and the mod cannot swallow keys, so a Shift chord would
// silently flip walk/run on every press. Diagnostic keys must be plain and unbound.
// Collision watch for the two keys whose freedom was never measured (8 and 9).
//
// The game funnels keyboard input into its own pad words DAT_02f97368/6a/6c (RVA 0x2E77368/6a/6c,
// filled by FUN_002498b0), so if it binds one of these keys a bit moves there on the same poll.
// The mod cannot swallow keys, so a collision means the game's action fires too -- exactly the F9
// failure of Session 112, which went unnoticed because nothing was watching.
//
// Log-only and deduped per distinct pad value, so a held key writes one line, not thousands.
// Absence of a line is NOT proof of freedom (the key may simply not have been pressed) -- but a
// line IS proof of collision, which is the direction that matters.
// `slot` gives each watched key its own dedup cell, so one key being held cannot mask a collision
// on another. Extended from the original two-bool form when B and N joined the watch list.
void LogPadOnKey(const char* keyName, int slot, bool down) {
    static uint32_t s_lastLogged[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
    if (slot < 0 || slot >= 4) return;
    if (!down) { s_lastLogged[slot] = 0xFFFFFFFF; return; }

    // The three RVAs used to be written out here. They now live in ONE place (PadHook), because the
    // pad router's survey log wants the same read and two copies of an RVA is one copy too many.
    uint16_t w[3] = {};
    if (!PadHook::ReadGamePadWords(w)) return;
    const uint16_t w0 = w[0], w1 = w[1], w2 = w[2];
    if (w0 == 0 && w1 == 0 && w2 == 0) return;          // nothing moved: the key looks free

    const uint32_t sig = (static_cast<uint32_t>(w0) << 16) ^ (static_cast<uint32_t>(w1) << 8) ^ w2;
    if (sig == s_lastLogged[slot]) return;
    s_lastLogged[slot] = sig;

    char hdr[112];
    snprintf(hdr, sizeof(hdr),
             "COLLISION? key=%s pad=0x%04X/0x%04X/0x%04X -- the game moved a pad bit for this key",
             keyName, w0, w1, w2);
    Log::Write("INPUT", hdr);
}

void DInputEdge(DWORD vk, std::atomic<bool>& downFlag, bool down, bool isNav) {
    if (down) {
        if (!downFlag.exchange(true) && GameIsForeground()) {
            if (isNav)              PostThreadMessageW(g_threadId, WM_NAVKEY, (WPARAM)vk, 0);
            else if (vk == 'O')     PostThreadMessageW(g_threadId, WM_DESCRIBE, 0, 0);
            else if (vk == 'T')     PostThreadMessageW(g_threadId, WM_REREAD, 0, 0);
            else if (vk == 'U')     PostThreadMessageW(g_threadId, WM_LICENSEPTS, 0, 0);
            else if (vk == 'G')     PostThreadMessageW(g_threadId, WM_GIL, 0, 0);
        }
    } else {
        downFlag.store(false);
    }
}

// Edge-detect a virtual-buffer nav key (arrow / Home / End) and post WM_MENUNAV on the rising edge.
// `navFallback` is passed through in lParam: true for Home/End (fall through to the combat log if the
// buffer doesn't consume them), false for arrows (no fallback). Read-only, never swallowed.
void DInputMenuNavEdge(DWORD vk, std::atomic<bool>& downFlag, bool down, bool navFallback) {
    if (down) {
        if (!downFlag.exchange(true) && GameIsForeground())
            PostThreadMessageW(g_threadId, WM_MENUNAV, static_cast<WPARAM>(vk),
                               static_cast<LPARAM>(navFallback ? 1 : 0));
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
            // Mod menu first: while it is open, `o` describes the focused SETTING. It declines when
            // closed, so the menu reader's item-description handler is reached exactly as before.
            InputTracker::DescribeInterceptCallback mcb = g_modMenuDescribeCb;
            if (!(mcb && mcb())) {
                InputTracker::HotkeyCallback cb = g_describeCb;
                if (cb) cb();
            }
        } else if (m.message == WM_REREAD) {
            InputTracker::HotkeyCallback cb = g_rereadCb;
            if (cb) cb();
        } else if (m.message == WM_LICENSEPTS) {
            InputTracker::HotkeyCallback cb = g_lpCb;
            if (cb) cb();
        } else if (m.message == WM_GIL) {
            InputTracker::HotkeyCallback cb = g_gilCb;
            if (cb) cb();
        } else if (m.message == WM_PADSAY) {
            // A phrasebook line the pad router asked for (mod mode entered / cancelled). Interrupts,
            // because it is an answer to a button the player pressed a moment ago.
            Speech::Output(Phrase::Get(static_cast<Phrase::Id>(m.wParam)), true);
        } else if (m.message == WM_PADSET) {
            // A pad thumb-click asking for one of the mod's own settings to flip -- the reachability
            // filter, the audio beacon, or the intercept's own kill switch. A callback like every
            // other handler here: this file knows no reader and no menu, and it names the setting by
            // an int so it does not have to agree with ModMenu about the enum.
            InputTracker::SettingToggleCallback cb = g_settingToggleCb;
            if (cb) cb(static_cast<int>(m.wParam));
        } else if (m.message == WM_MENUNAV) {
            // Arrows + Home/End -> virtual-buffer nav (status screen). Offer to the buffer first; for
            // Home/End (lParam != 0) fall through to the combat-log nav path when it declines.
            const int vk = static_cast<int>(m.wParam);
            bool consumed = false;
            // The mod's own menu first: while it is open these keys are its, and it declines
            // otherwise so the status buffer keeps them the rest of the time.
            InputTracker::MenuNavCallback mcb = g_modMenuNavCb;
            if (mcb) consumed = mcb(vk);
            InputTracker::MenuNavCallback cb = g_menuNavCb;
            if (!consumed && cb) consumed = cb(vk);
            // Clan Primer entry body. Offered after the status buffer; the two are never open at
            // once, so the order between them is arbitrary -- what matters is that both get a look.
            InputTracker::MenuNavCallback pcb = g_primerNavCb;
            if (!consumed && pcb) consumed = pcb(vk);
            if (!consumed && m.lParam != 0) {
                InputTracker::NavKeyCallback ncb = g_navKeyCb;
                if (ncb) ncb(vk);
            }
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
void SetLicensePointsCallback(HotkeyCallback cb) { g_lpCb = cb; }
void SetGilCallback(HotkeyCallback cb) { g_gilCb = cb; }
void SetSettingToggleCallback(SettingToggleCallback cb) { g_settingToggleCb = cb; }
void SetRereadCallback(HotkeyCallback cb) { g_rereadCb = cb; }
void SetNavKeyCallback(NavKeyCallback cb) { g_navKeyCb = cb; }
void SetMenuNavCallback(MenuNavCallback cb) { g_menuNavCb = cb; }
void SetModMenuNavCallback(MenuNavCallback cb) { g_modMenuNavCb = cb; }
void SetPrimerNavCallback(MenuNavCallback cb) { g_primerNavCb = cb; }
void SetModMenuDescribeCallback(DescribeInterceptCallback cb) { g_modMenuDescribeCb = cb; }

// See the header. The routing below is deliberately the SAME shape as DInputEdge/DInputMenuNavEdge:
// if a key ever changes handler, both devices change with it because there is only one table.
void DispatchModKey(int vk) {
    if (!g_threadId) return;
    switch (vk) {
        case 'O': PostThreadMessageW(g_threadId, WM_DESCRIBE,   0, 0); break;
        case 'T': PostThreadMessageW(g_threadId, WM_REREAD,     0, 0); break;
        case 'U': PostThreadMessageW(g_threadId, WM_LICENSEPTS, 0, 0); break;
        case 'G': PostThreadMessageW(g_threadId, WM_GIL,        0, 0); break;
        // Arrows: virtual-buffer nav with NO fallback -- outside a buffer the mod does nothing.
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
            PostThreadMessageW(g_threadId, WM_MENUNAV, static_cast<WPARAM>(vk), 0);
            break;
        // Home/End: the buffer first, then the combat log when it declines (lParam = 1).
        case VK_HOME: case VK_END:
            PostThreadMessageW(g_threadId, WM_MENUNAV, static_cast<WPARAM>(vk), 1);
            break;
        // Backspace: the mod menu's "back out one level, close at the top" (S192). It MUST go to the
        // menu-nav route rather than falling through to the nav-key switch below, because that switch
        // does not answer it and the press would simply vanish -- which is what happened the first
        // time the pad's Back button was pointed at this key. Same shape as the keyboard's own
        // DInputMenuNavEdge(VK_BACK, ...), which is the point of this table.
        case VK_BACK:
            PostThreadMessageW(g_threadId, WM_MENUNAV, static_cast<WPARAM>(vk), 0);
            break;
        // Everything else is a nav-key: NavCommands::OnNavKey owns the mod's whole key switch.
        default:
            PostThreadMessageW(g_threadId, WM_NAVKEY, static_cast<WPARAM>(vk), 0);
            break;
    }
}

void DispatchSpeakPhrase(int phraseId) {
    if (!g_threadId) return;
    PostThreadMessageW(g_threadId, WM_PADSAY, static_cast<WPARAM>(phraseId), 0);
}

void DispatchToggleSetting(int settingId) {
    if (!g_threadId) return;
    PostThreadMessageW(g_threadId, WM_PADSET, static_cast<WPARAM>(settingId), 0);
}

void FeedDInputKeyboard(const unsigned char* dik) {
    if (!dik || !g_threadId) return;

    // A PROMPT IS A KEYBOARD (Session 185). While one of the mod's own dialogs is up the player is
    // typing into a Win32 edit field, and the buffer below is whatever the game last polled before it
    // lost focus. Run the whole edge pass against ZEROS rather than returning early: returning would
    // leave a key that was down when the box opened still armed, so it would fire the instant the box
    // closed. Zeros release every edge cleanly and dispatch nothing.
    static const unsigned char kNoKeys[256] = {};
    if (TextPrompt::Busy()) dik = kNoKeys;
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

    // Movement-key state for the navigation stuck detector. A plain relaxed store of four bits.
    g_moveHeld.store(((dik[DIK_W] | dik[DIK_A] | dik[DIK_S] | dik[DIK_D]) & 0x80) != 0,
                     std::memory_order_relaxed);

    // All hotkeys are standalone (no Shift — the game binds Left Shift to Walk/Run).
    DInputEdge('O',           g_oDown,       (dik[DIK_O]          & 0x80) != 0, false);
    DInputEdge('T',           g_tDown,       (dik[DIK_T]          & 0x80) != 0, false);
    DInputEdge('U',           g_uDown,       (dik[DIK_U]          & 0x80) != 0, false);  // U  License Points
    DInputEdge('G',           g_gDown,       (dik[DIK_G]          & 0x80) != 0, false);  // g  party gil total
    DInputEdge(VK_OEM_5,      g_navDown[0],  (dik[DIK_BACKSLASH]  & 0x80) != 0, true);  // \  route
    DInputEdge(VK_OEM_4,      g_navDown[1],  (dik[DIK_LBRACKET]   & 0x80) != 0, true);  // [  prev object
    DInputEdge(VK_OEM_6,      g_navDown[2],  (dik[DIK_RBRACKET]   & 0x80) != 0, true);  // ]  next object
    DInputEdge(VK_OEM_3,      g_navDown[3],  (dik[DIK_GRAVE]      & 0x80) != 0, true);  // `  rescan
    // ---- EVERY F-KEY IS BARE-PRESS ONLY (Session 155, user instruction) -------------------------
    //
    // A chord belongs to whatever owns the chord, never to us. **Alt+F4 is the operating system
    // closing the game** -- and until this session the mod read it as a bare F4 and flipped combat
    // verbosity on the way out. Ctrl+F4, Shift+F7 and the rest were the same class of bug: the mod
    // cannot swallow a key, so an unguarded F-key fires IN ADDITION to whatever the chord already
    // does, and the player gets a silent state change they never asked for.
    //
    // This guard used to be LOCAL to F11 (S112, because Shift+F11 is an NVDA command the tester
    // needs while playing). That reasoning was never specific to F11 -- it is the general rule, and
    // scoping it to one key just meant the other five kept the defect.
    //
    // Holding a modifier makes the F-key read as UP rather than suppressing the dispatch, so a
    // modifier pressed mid-hold registers a clean release and cannot leave an edge armed.
    const bool fkeyModifierHeld =
        ((dik[DIK_LSHIFT] | dik[DIK_RSHIFT] | dik[DIK_LCTRL] | dik[DIK_RCTRL] |
          dik[DIK_LALT]   | dik[DIK_RALT]   | dik[DIK_LWIN]  | dik[DIK_RWIN]) & 0x80) != 0;
    const auto bareF = [&](int scan) { return !fkeyModifierHeld && (dik[scan] & 0x80) != 0; };

    DInputEdge(VK_F4,         g_extraDown[14], bareF(DIK_F4),  true);  // F4 combat verbosity
    DInputEdge(VK_F5,         g_extraDown[15], bareF(DIK_F5),  true);  // F5 all/story-gated
    DInputEdge(VK_F6,         g_extraDown[20], bareF(DIK_F6),  true);  // F6 name/clear the selection
    DInputEdge(VK_F7,         g_extraDown[27], bareF(DIK_F7),  true);  // F7 autodetail
    DInputEdge(VK_F8,         g_extraDown[21], bareF(DIK_F8),  true);  // F8 mod menu
    DInputEdge(VK_F11,        g_extraDown[22], bareF(DIK_F11), true);  // F11 audio beacon on/off
    // Escape -> the mod menu's close. Bare press only, on the same guard as the F-keys: Alt+Esc
    // and Ctrl+Esc belong to the shell. `false` = no combat-log fallback, so outside the menu it
    // reaches the virtual buffers, is declined by all of them, and ends there.
    DInputMenuNavEdge(VK_ESCAPE, g_extraDown[28],
                      !fkeyModifierHeld && (dik[DIK_ESCAPE] & 0x80) != 0, false);
    // Backspace -> the same close. Identical treatment to Escape, including the bare-press guard.
    DInputMenuNavEdge(VK_BACK,   g_extraDown[29],
                      !fkeyModifierHeld && (dik[DIK_BACK] & 0x80) != 0, false);
    DInputEdge(VK_OEM_MINUS,  g_extraDown[0],(dik[DIK_MINUS]      & 0x80) != 0, true);  // -  prev category
    DInputEdge(VK_OEM_PLUS,   g_extraDown[1],(dik[DIK_EQUALS]     & 0x80) != 0, true);  // =  next category
    // `'` (the NavProbe diagnostic dump) IS NO LONGER BOUND -- removed for the public 1.0 at the
    // user'''s instruction. It was a development key: it dumps the route/mesh probe to the log and says
    // nothing a player can use, so on a public build it is a key that can only confuse. The handler
    // and the probe itself are untouched (NavProbe::Request, nav_commands.cpp) -- only the edge that
    // fires it is gone, so re-binding it for a dev session is one line.
    DInputEdge(VK_OEM_2,      g_extraDown[4],(dik[DIK_SLASH]      & 0x80) != 0, true);  // /  describe
    DInputEdge(VK_OEM_1,      g_extraDown[2],(dik[DIK_SEMICOLON]  & 0x80) != 0, true);  // ;  target status
    DInputEdge('P',           g_extraDown[5],(dik[DIK_P]          & 0x80) != 0, true);  // p  route to the active target
    DInputEdge('4',           g_extraDown[6],(dik[DIK_4]          & 0x80) != 0, true);  // 4  party slot 1 status
    DInputEdge('5',           g_extraDown[7],(dik[DIK_5]          & 0x80) != 0, true);  // 5  party slot 2 status
    DInputEdge('6',           g_extraDown[8],(dik[DIK_6]          & 0x80) != 0, true);  // 6  party slot 3 status
    DInputEdge('7',           g_extraDown[9],(dik[DIK_7]          & 0x80) != 0, true);  // 7  guest slot status
    DInputEdge('8',           g_extraDown[23],(dik[DIK_8]         & 0x80) != 0, true);  // 8  equip column 5 / summoned Esper
    DInputEdge('9',           g_extraDown[24],(dik[DIK_9]         & 0x80) != 0, true);  // 9  equip column 6
    // COLLISION WATCH for 8/9 (see their DIK note above). The mod cannot swallow a key, so if the
    // game also binds one, BOTH happen and the player gets a surprise action. Rather than assert
    // they are free, record the game's own pad words on the frame the key goes down: the pad is
    // where the game funnels keyboard input, so a bit moving here names the collision. Log-only,
    // deduped per distinct pad value so a held key cannot flood the file.
    LogPadOnKey("8", 0, (dik[DIK_8] & 0x80) != 0);
    LogPadOnKey("9", 1, (dik[DIK_9] & 0x80) != 0);
    // Shout minigame keys. Both are silent no-ops off a shout map; the dispatcher decides, so the
    // edge is registered unconditionally here.
    DInputEdge('B',           g_extraDown[25],(dik[DIK_B]         & 0x80) != 0, true);  // B  infamy meter
    DInputEdge('N',           g_extraDown[26],(dik[DIK_N]         & 0x80) != 0, true);  // N  nearest NPCs
    LogPadOnKey("B", 2, (dik[DIK_B] & 0x80) != 0);
    LogPadOnKey("N", 3, (dik[DIK_N] & 0x80) != 0);
    DInputEdge(VK_OEM_COMMA,  g_extraDown[10],(dik[DIK_COMMA]     & 0x80) != 0, true);  // ,  log: older
    DInputEdge(VK_OEM_PERIOD, g_extraDown[11],(dik[DIK_PERIOD]    & 0x80) != 0, true);  // .  log: newer
    // Home/End: status buffer first (top/bottom of stats), else the combat log (oldest/newest). The
    // buffer's OnMenuNavKey returns false when the status page isn't active, so the fallback fires.
    DInputMenuNavEdge(VK_HOME, g_extraDown[12],(dik[DIK_HOME]     & 0x80) != 0, /*navFallback=*/true);
    DInputMenuNavEdge(VK_END,  g_extraDown[13],(dik[DIK_END]      & 0x80) != 0, /*navFallback=*/true);
    // Arrow keys: status-buffer nav only (no fallback -- the game owns them elsewhere). Up/Down = stat,
    // Left/Right = group.
    DInputMenuNavEdge(VK_UP,    g_extraDown[16],(dik[DIK_UP]      & 0x80) != 0, /*navFallback=*/false);
    DInputMenuNavEdge(VK_DOWN,  g_extraDown[17],(dik[DIK_DOWN]    & 0x80) != 0, /*navFallback=*/false);
    DInputMenuNavEdge(VK_LEFT,  g_extraDown[18],(dik[DIK_LEFT]    & 0x80) != 0, /*navFallback=*/false);
    DInputMenuNavEdge(VK_RIGHT, g_extraDown[19],(dik[DIK_RIGHT]   & 0x80) != 0, /*navFallback=*/false);
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

bool MovementHeld() { return g_moveHeld.load(std::memory_order_relaxed); }

// The tracker's own dispatch gate, exposed (Session 100) -- see the header.
bool GameForeground() { return GameIsForeground(); }

// ---- THE MOD MENU'S CLOSE KEYS ARE TAKEN FROM THE GAME WHILE IT IS OPEN (S192) ------------------
//
// THE MOD'S ONE CHARTERED KEY SWALLOW, and it is a new category: CLAUDE.md's read-only-input rule
// says the mod "never swallows a key". The user asked for exactly this, in as many words: *"when
// closing the mod menu, escape key is not intercepted so it pauses the game. fix that"*. Escape
// closed the menu and then reached the game as well, so every exit from the mod menu also opened
// the game's pause screen -- and for a blind player that is two screens deep with no warning.
//
// A CLAIM IS A LEVEL, NOT AN EDGE (`L-99`, and it applies to the keyboard exactly as it did to the
// pad). Clearing the byte only while the menu is open is NOT enough: the close is dispatched to the
// input thread, so within a poll or two the menu is already shut while the player is still holding
// the key -- and that poll would hand the game the press after all, which is the very defect being
// fixed, just later. `s_latched` is what makes the claim outlive the close; it lifts on release.
//
// SCOPE, and nothing wider: these two keys, while the menu is open or still held from when it was.
// With the menu shut and nothing latched, this function does not write a byte -- the buffer the
// game receives is bit-identical to the one it would receive with no mod installed.
//
// NOT the arrow keys. They are claimed by the menu too and they still reach the game, which is why
// the readme warns that the character walks while the menu is open. Widening this mask to them is a
// bigger behavioural change than was asked for and would need its own instruction.
void MaskModMenuKeys(unsigned char* dik) {
    if (!dik) return;
    static const int  kScan[2] = { DIK_ESCAPE, DIK_BACK };
    static bool       s_latched[2] = {};
    const bool open = ModMenu::IsOpen();
    for (int i = 0; i < 2; ++i) {
        if ((dik[kScan[i]] & 0x80) == 0) { s_latched[i] = false; continue; }   // released: claim ends
        if (open) s_latched[i] = true;                                          // claimed on this press
        if (s_latched[i]) dik[kScan[i]] = 0;                                    // ...and stays claimed
    }
}


} // namespace InputTracker

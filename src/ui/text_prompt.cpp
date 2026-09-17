#include "ui/text_prompt.h"
#include "core/logger.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int kIdEdit  = 1000;
constexpr int kIdLabel = 1001;

// Dialog-template class atoms for the Win32 predefined controls.
constexpr uint16_t kAtomButton = 0x0080, kAtomEdit = 0x0081, kAtomStatic = 0x0082;

std::atomic<bool> g_busy{false};
HANDLE            g_thread = nullptr;   // the live prompt's thread; owned by Shutdown
std::atomic<HWND> g_dlg{nullptr};       // the live prompt's window, for Shutdown to close

struct Request {
    bool                 yesNo = false;
    std::wstring         title;
    std::wstring         label;
    std::wstring         initial;
    TextPrompt::Callback cb    = nullptr;
    void*                ctx   = nullptr;
    HWND                 owner = nullptr;
    std::wstring         result;        // what the edit field held on OK
};

// ---- the in-memory DLGTEMPLATE ------------------------------------------------------------------
// A dialog template is a packed byte stream, not a struct: variable-length strings sit between the
// fixed parts and every item has to start on a DWORD boundary. Building it by hand is the price of
// having no .rc file, and it buys a genuine #32770 dialog -- which is what makes a screen reader
// read the whole box on open rather than only the control that happens to hold focus.
struct Tmpl {
    std::vector<uint8_t> b;
    void Align()               { while (b.size() & 3) b.push_back(0); }
    void W16(uint16_t v)       { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
    void W32(uint32_t v)       { W16(uint16_t(v)); W16(uint16_t(v >> 16)); }
    void Str(const wchar_t* s) { for (; s && *s; ++s) W16(uint16_t(*s)); W16(0); }

    void Item(uint32_t style, int x, int y, int cx, int cy, uint16_t id,
              uint16_t atom, const wchar_t* text) {
        Align();
        W32(style);
        W32(0);                                 // dwExtendedStyle
        W16(uint16_t(x)); W16(uint16_t(y)); W16(uint16_t(cx)); W16(uint16_t(cy));
        W16(id); W16(0);                        // the id field is a DWORD in DLGITEMTEMPLATE
        W16(0xFFFF); W16(atom);                 // class, in its atom form
        Str(text);
        W16(0);                                 // no creation data
    }
};

INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            Request* rq = reinterpret_cast<Request*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(rq));
            g_dlg.store(dlg, std::memory_order_release);
            SetDlgItemTextW(dlg, kIdEdit, rq->initial.c_str());
            // Pre-filled text starts selected, so typing replaces it instead of appending -- and a
            // screen reader reads the selection as it announces the field, which is how the player
            // hears what is already in there.
            SendDlgItemMessageW(dlg, kIdEdit, EM_SETSEL, 0, -1);
            // The game holds the foreground and will not give it up on its own. Without this the
            // dialog exists, owns nothing, and the player types into the game.
            SetForegroundWindow(dlg);
            SetFocus(GetDlgItem(dlg, kIdEdit));
            return FALSE;                        // focus was set by hand, just above
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK: {
                    Request* rq = reinterpret_cast<Request*>(GetWindowLongPtrW(dlg, DWLP_USER));
                    if (rq) {
                        wchar_t buf[256] = {};
                        GetDlgItemTextW(dlg, kIdEdit, buf, 256);
                        rq->result = buf;
                    }
                    EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
                default:
                    break;
            }
            return FALSE;
        case WM_CLOSE:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        case WM_DESTROY:
            g_dlg.store(nullptr, std::memory_order_release);
            return FALSE;
        default:
            return FALSE;
    }
}

// The module the mod is loaded as, taken from this function's own address rather than plumbed down
// from DllMain -- so this file needs nothing from the proxy.
HMODULE SelfModule() {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SelfModule), &h);
    return h;
}

bool ShowText(Request* rq) {
    Tmpl t;
    // DS_SETFONT gets the shell dialog font instead of the 1980s system font; DS_CENTER puts the box
    // where a sighted helper can find it. WS_EX_TOPMOST is on the template so the box is not painted
    // behind a game that owns the whole screen.
    t.W32(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | DS_CENTER);
    t.W32(WS_EX_TOPMOST);
    t.W16(4);                                    // cdit: four controls follow
    t.W16(0); t.W16(0); t.W16(264); t.W16(78);   // x, y, cx, cy, in dialog units
    t.W16(0);                                    // no menu
    t.W16(0);                                    // the default dialog class
    t.Str(rq->title.c_str());
    t.W16(9); t.Str(L"Segoe UI");                // DS_SETFONT: point size, then typeface

    // The static comes FIRST on purpose: a screen reader labels an edit control from the static that
    // precedes it in z-order, so swapping these two silently costs the field its name.
    t.Item(WS_CHILD | WS_VISIBLE | SS_LEFT, 8, 8, 248, 24, kIdLabel, kAtomStatic, rq->label.c_str());
    t.Item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
           8, 36, 248, 14, kIdEdit, kAtomEdit, L"");
    t.Item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 150, 56, 50, 16,
           IDOK, kAtomButton, L"OK");
    t.Item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 206, 56, 50, 16,
           IDCANCEL, kAtomButton, L"Cancel");

    const INT_PTR r = DialogBoxIndirectParamW(
        SelfModule(), reinterpret_cast<LPCDLGTEMPLATEW>(t.b.data()), rq->owner,
        DlgProc, reinterpret_cast<LPARAM>(rq));
    if (r == -1) {
        char m[96];
        snprintf(m, sizeof(m), "TextPrompt: DialogBoxIndirectParam failed, GetLastError=%lu",
                 (unsigned long)GetLastError());
        Log::Write("PROMPT", m);
        return false;
    }
    return r == IDOK;
}

DWORD WINAPI PromptThread(LPVOID param) {
    Request* rq = static_cast<Request*>(param);
    bool ok = false;

    if (rq->yesNo) {
        // MessageBox rather than a template of our own: it is the box every screen reader already
        // reads perfectly, and there is nothing about a two-button question worth hand-rolling.
        // MB_DEFBUTTON2 defaults to No -- this box only ever asks about THROWING WORK AWAY, so the
        // press that costs nothing is the one that should happen if the player just hits Enter.
        const int r = MessageBoxW(rq->owner, rq->label.c_str(), rq->title.c_str(),
                                  MB_YESNO | MB_ICONQUESTION | MB_TASKMODAL | MB_SETFOREGROUND |
                                  MB_TOPMOST | MB_DEFBUTTON2);
        ok = (r == IDYES);
    } else {
        ok = ShowText(rq);
    }

    // Hand the foreground back before the callback speaks, so the confirmation is heard with the
    // game already focused and the player can carry straight on.
    if (rq->owner && IsWindow(rq->owner)) SetForegroundWindow(rq->owner);

    TextPrompt::Callback cb   = rq->cb;
    void*                ctx  = rq->ctx;
    const std::wstring   text = rq->result;
    const bool           yesNo = rq->yesNo;
    delete rq;

    // Cleared BEFORE the callback runs: a callback may want to raise the next prompt, and it must
    // not be refused by the interlock of the prompt it was called from.
    g_busy.store(false, std::memory_order_release);

    char m[64];
    snprintf(m, sizeof(m), "%s closed: %s", yesNo ? "yes/no" : "edit", ok ? "ok" : "cancel");
    Log::Write("PROMPT", m);

    if (cb) cb(ok ? TextPrompt::Result::Ok : TextPrompt::Result::Cancel, text, ctx);
    return 0;
}

// The window to own the box: whatever this process has in the foreground when the key is pressed.
// Every hotkey that reaches here is already gated on the game being foreground
// (InputTracker::GameForeground), so this is the game's own window or nothing.
HWND OwnerWindow() {
    HWND fg = GetForegroundWindow();
    if (!fg) return nullptr;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return (pid == GetCurrentProcessId()) ? fg : nullptr;
}

bool Launch(Request* rq) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        Log::Write("PROMPT", "a prompt is already open -- ignoring");
        delete rq;
        return false;
    }
    rq->owner = OwnerWindow();
    if (g_thread) { CloseHandle(g_thread); g_thread = nullptr; }
    g_thread = CreateThread(nullptr, 0, PromptThread, rq, 0, nullptr);
    if (!g_thread) {
        Log::Write("PROMPT", "TextPrompt: CreateThread failed");
        g_busy.store(false, std::memory_order_release);
        delete rq;
        return false;
    }
    return true;
}

} // namespace

namespace TextPrompt {

bool Busy() { return g_busy.load(std::memory_order_acquire); }

bool AskText(const std::wstring& title, const std::wstring& label, const std::wstring& initial,
             Callback cb, void* ctx) {
    Request* rq = new Request{};
    rq->yesNo   = false;
    rq->title   = title;
    rq->label   = label;
    rq->initial = initial;
    rq->cb      = cb;
    rq->ctx     = ctx;
    return Launch(rq);
}

bool AskYesNo(const std::wstring& title, const std::wstring& question, Callback cb, void* ctx) {
    Request* rq = new Request{};
    rq->yesNo = true;
    rq->title = title;
    rq->label = question;
    rq->cb    = cb;
    rq->ctx   = ctx;
    return Launch(rq);
}

void Shutdown() {
    if (HWND d = g_dlg.load(std::memory_order_acquire)) PostMessageW(d, WM_CLOSE, 0, 0);
    if (g_thread) {
        // Bounded, like InputTracker's: DLL detach must not hang on a window the player left open.
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

} // namespace TextPrompt

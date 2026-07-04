#include "ui/menu_reader.h"
#include "ui/text_capture.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "speech/speech.h"
#include "core/logger.h"
#include "input/input_tracker.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

// Universal focus signal: FUN_00247510(owner, msg, val). msg 0x8000 = "cursor
// moved to item <val>". It carries three distinct cases we care about, told
// apart by the owner object's handler pointer (obj[0]):
//   * a normal list menu  -> speak the focused row's name (+ its value), read
//     from TextCapture's per-item map keyed by the game's own index;
//   * a confirm/quit pop-up (obj[0]==FUN_00241d40) -> speak the body prompt
//     (once) and the focused Yes/No button (id 1000/1001, code-fixed by index);
//   * a settings row's value list (obj[0]==FUN_002a6190) -> a left/right value
//     toggle; announce just the new value.
// NO option-name strings live here — everything spoken is the game's own text.
constexpr uint32_t RVA_DISPATCH     = 0x127510;   // FUN_00247510(owner, msg, val)
constexpr uint32_t RVA_TITLE_WINDOW = 0x29CE4C8;  // DAT_02aee4c8 — TitleReader owns the title menu
constexpr uint32_t RVA_CONFIRM_WND  = 0x121D40;   // FUN_00241d40 — confirm/menu window handler (obj[0])
constexpr uint32_t RVA_SPINNER_ROW  = 0x186190;   // FUN_002a6190 — settings row / value spinner (obj[0])

constexpr uint32_t OFF_POPUP_BODY = 0x1B0;   // confirm window -> inline codec body prompt
constexpr uint32_t OFF_ROW_BLOB   = 0x3E0;   // spinner row -> packed option-string blob
constexpr uint32_t OFF_ROW_MASK   = 0x154;   // spinner row -> disabled-option bitmask (u32)
constexpr uint32_t OFF_ROW_SEL    = 0x3C6;   // spinner row -> selected logical option index (u8)
constexpr uint32_t OFF_ROW_COUNT  = 0x3C7;   // spinner row -> option count (u8)
constexpr uint32_t OFF_CHILD_FIRST = 0x18;   // scene-graph node -> first child
constexpr uint32_t OFF_SIBLING_NEXT= 0x20;   // scene-graph node -> next sibling
constexpr uint32_t OFF_MENU_SUB    = 0xC8;   // menu -> list-cursor sub-widget (fallback parent)

constexpr uint64_t MSG_FOCUS  = 0x8000;
constexpr uint64_t MSG_YES    = 0x8100;      // no-list 2-choice pop-up results (owner = parent)
constexpr uint64_t MSG_NO     = 0x8101;
constexpr uint64_t MSG_CANCEL = 0x8102;

constexpr uint64_t VALUE_GATE_MS = 700;      // input window for a value-toggle announce

typedef uintptr_t (*Pfn_Dispatch)(void*, uintptr_t, uintptr_t);
Pfn_Dispatch s_origDispatch = nullptr;

std::mutex g_mutex;
void* g_lastOwner = nullptr;
int   g_lastIndex = -1;
std::wstring g_lastText;
void* g_pendingOwner = nullptr;   // focus whose text wasn't painted yet (menu-entry replay)
int   g_pendingIndex = -1;
std::unordered_map<void*, std::wstring> g_rowLastValue;  // spinner row -> last seen value (toggle dedup)
bool  g_initialized = false;

void LogLine(const char* prefix, const std::wstring& text) {
    char utf8[512] = {};
    if (!text.empty())
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    char line[640];
    snprintf(line, sizeof(line), "%s\"%s\"", prefix, utf8);
    Log::Write("READER", line);
}

// ---- SEH-guarded raw reads (game objects can be destructed asynchronously) ---
bool SafeReadPtr(const void* at, void** out) {
    if (!at) return false;
    __try { *out = *reinterpret_cast<void* const*>(at); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* PtrAt(void* base, uint32_t off) {
    if (!base) return nullptr;
    void* v = nullptr;
    return SafeReadPtr(reinterpret_cast<char*>(base) + off, &v) ? v : nullptr;
}
void* Obj0(void* obj) { return PtrAt(obj, 0); }

bool IsTitleMenu(void* owner) {
    void* titleWin = nullptr;
    if (SafeReadPtr(reinterpret_cast<void*>(Hooks::ResolveRva(RVA_TITLE_WINDOW)), &titleWin))
        return owner != nullptr && owner == titleWin;
    return false;
}
bool IsConfirmWindow(void* owner) { return owner && Obj0(owner) == Hooks::ResolveRva(RVA_CONFIRM_WND); }
bool IsSpinnerRow(void* owner)    { return owner && Obj0(owner) == Hooks::ResolveRva(RVA_SPINNER_ROW); }

// If `owner` is a confirm window, decode its inline body prompt at +0x1b0.
std::wstring ReadPopupBody(void* owner) {
    if (!IsConfirmWindow(owner)) return std::wstring();
    std::wstring s = GameText::Decode(
        reinterpret_cast<const uint8_t*>(reinterpret_cast<char*>(owner) + OFF_POPUP_BODY), 0x200);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// Copy the currently-selected option's codec bytes out of a spinner row's blob.
// Mirrors FUN_002b2d90: physical index = entry position; entry enabled iff
// (mask>>phys)&1==0; pick the (selected-logical)-th enabled entry.
bool ReadRowValueBytes(void* row, uint8_t* out, size_t cap, size_t* outLen) {
    __try {
        const uint8_t* p = *reinterpret_cast<const uint8_t* const*>(
            reinterpret_cast<char*>(row) + OFF_ROW_BLOB);
        if (!p) return false;
        uint32_t mask = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(row) + OFF_ROW_MASK);
        int sel   = *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(row) + OFF_ROW_SEL);
        int count = *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(row) + OFF_ROW_COUNT);
        uint32_t phys = 0;
        int enabled = 0;
        while (*p != 0) {
            if (((mask >> (phys & 0x1f)) & 1) == 0) {
                if (enabled == sel) {
                    uint32_t len = *p & 0x7f;
                    size_t n = (len < cap - 1) ? len : cap - 1;
                    for (size_t i = 0; i < n; ++i) out[i] = p[1 + i];
                    out[n] = 0;
                    *outLen = n;
                    return true;
                }
                ++enabled;
            }
            if (phys == static_cast<uint32_t>(count)) break;
            ++phys;
            p += (*p & 0x7f) + 1;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// A spinner row carries a value blob only if it's a setting-with-value row.
std::wstring DecodeRowValue(void* row) {
    if (!row) return std::wstring();
    uint8_t buf[256];
    size_t len = 0;
    if (!ReadRowValueBytes(row, buf, sizeof(buf), &len) || len == 0) return std::wstring();
    std::wstring s = GameText::Decode(buf, len);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// The N-th settings-row (FUN_002a6190) child under `parent`, in scene-graph
// order (== the list's item order). null if fewer than N+1 row children.
void* NthRowChild(void* parent, int index) {
    if (!parent || index < 0) return nullptr;
    void* spinnerHandler = Hooks::ResolveRva(RVA_SPINNER_ROW);
    void* child = PtrAt(parent, OFF_CHILD_FIRST);
    int count = 0;
    int guard = 0;
    while (child && guard++ < 512) {
        if (Obj0(child) == spinnerHandler) {
            if (count == index) return child;
            ++count;
        }
        child = PtrAt(child, OFF_SIBLING_NEXT);
    }
    return nullptr;
}

// Resolve focus index N -> its row object (the menu owns the rows, or its list
// sub-widget does). Used only to fetch a row's value on focus.
void* FindRowByIndex(void* owner, int index) {
    void* r = NthRowChild(owner, index);
    if (r) return r;
    return NthRowChild(PtrAt(owner, OFF_MENU_SUB), index);
}

// Value toggle: the focused settings row re-selected a value (left/right). The
// signal arrives with the ROW as owner. Announce only the new value, and only
// on a genuine change driven by recent input (silently prime on first sight so
// menu-entry value inits don't speak).
void OnValueToggle(void* row) {
    std::wstring val = DecodeRowValue(row);
    if (val.empty()) return;

    bool changed;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_rowLastValue.find(row);
        changed = (it != g_rowLastValue.end()) && (it->second != val);
        if (g_rowLastValue.size() > 128) g_rowLastValue.clear();
        g_rowLastValue[row] = val;
    }
    if (!changed) return;                                        // first sight / redraw same value
    if (InputTracker::MsSinceLastInput() > VALUE_GATE_MS) return; // not user-driven
    LogLine("  value: ", val);
    Speech::Output(val, /*interrupt=*/true);
}

void OnFocus(void* owner, int index) {
    if (index < 0) return;
    if (IsTitleMenu(owner)) return;               // TitleReader handles the title command menu
    if (IsSpinnerRow(owner)) { OnValueToggle(owner); return; }

    const bool isPopup = IsConfirmWindow(owner);

    // Build what we'll speak: pop-up button label (code-fixed by index), or the
    // focused row's "name" / "name: value".
    std::wstring text;
    if (isPopup) {
        text = TextCapture::StringById(1000 + (index != 0 ? 1 : 0));   // 1000=Yes, 1001=No
    } else {
        text = TextCapture::FocusedItemText(owner, index);            // row name
        if (!text.empty()) {
            void* row = FindRowByIndex(owner, index);
            std::wstring val = DecodeRowValue(row);
            if (!val.empty()) {
                {   // prime the toggle baseline so the user's first left/right speaks
                    std::lock_guard<std::mutex> lk(g_mutex);
                    g_rowLastValue[row] = val;
                }
                text += L": ";
                text += val;
            }
        }
    }

    bool ownerChanged, dup;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        ownerChanged = (owner != g_lastOwner);
        dup = (!ownerChanged && index == g_lastIndex && text == g_lastText);
        if (!dup) { g_lastOwner = owner; g_lastIndex = index; g_lastText = text; }
    }
    if (dup) return;

    char hdr[160];
    snprintf(hdr, sizeof(hdr), "focus owner=%p index=%d%s%s",
             owner, index, ownerChanged ? " (new surface)" : "", isPopup ? " [popup]" : "");
    Log::Write("READER", hdr);

    // Pop-up body prompt: announce once on entry, before the button.
    bool preambleSpoken = false;
    if (ownerChanged && isPopup) {
        std::wstring body = ReadPopupBody(owner);
        if (!body.empty()) {
            LogLine("  body: ", body);
            Speech::Output(body, /*interrupt=*/true);
            preambleSpoken = true;
        }
    }

    if (text.empty()) {
        // Menu entry can fire the first focus before the painter fills the item
        // map (or before the button id is cached). Stash it; TextCapture's
        // paint callback replays this focus once the text is available.
        if (ownerChanged) {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_pendingOwner = owner;
            g_pendingIndex = index;
        }
        Log::Write("READER", "  (text not ready — awaiting paint)");
        TextCapture::DumpRingToLog("focus text empty");
        return;
    }

    LogLine("  item: ", text);
    Speech::Output(text, /*interrupt=*/!preambleSpoken);
}

// Fired right after the painter fills an owner's item map — replay a menu-entry
// focus whose text wasn't ready yet.
void OnMenuPainted(void* owner) {
    void* pend; int idx;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        pend = g_pendingOwner; idx = g_pendingIndex;
    }
    if (owner != pend || idx < 0) return;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_pendingOwner = nullptr; g_pendingIndex = -1;
    }
    OnFocus(owner, idx);
}

uintptr_t HookedDispatch(void* owner, uintptr_t msg, uintptr_t val) {
    if (msg == MSG_FOCUS) {
        OnFocus(owner, static_cast<int>(static_cast<intptr_t>(val)));
    } else if (msg == MSG_YES || msg == MSG_NO || msg == MSG_CANCEL) {
        // No-list 2-choice pop-up result path (owner = parent). Logged for now;
        // the tested quit pop-up is the list variant handled via 0x8000 above.
        char hdr[96];
        snprintf(hdr, sizeof(hdr), "popup result msg=0x%llx owner=%p",
                 (unsigned long long)msg, owner);
        Log::Write("READER", hdr);
    }
    return s_origDispatch ? s_origDispatch(owner, msg, val) : 0;
}

} // namespace

namespace MenuReader {

bool Init() {
    if (g_initialized) {
        Log::Write("READER", "MenuReader::Init called twice — ignoring");
        return true;
    }
    TextCapture::SetMenuPaintedCallback(&OnMenuPainted);
    bool ok = Hooks::InstallTyped(RVA_DISPATCH, &HookedDispatch, &s_origDispatch);
    g_initialized = true;
    Log::Write("READER", ok
        ? "MenuReader initialized (0x8000 -> row name+value / pop-up Yes-No / value toggle; title skipped)."
        : "MenuReader: focus hook failed to install — see Hooks log.");
    return ok;
}

void Shutdown() {
    if (!g_initialized) return;
    TextCapture::SetMenuPaintedCallback(nullptr);
    Hooks::Uninstall(RVA_DISPATCH);
    g_initialized = false;
    Log::Write("READER", "MenuReader shut down");
}

} // namespace MenuReader

#include "ui/menu_reader.h"
#include "ui/text_capture.h"
#include "ui/ingame_menu_reader.h"
#include "ui/battle_target_reader.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "speech/speech.h"
#include "core/logger.h"
#include "input/input_tracker.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace {

// Universal focus signal: FUN_00247510(owner, msg, val). msg 0x8000 = "cursor
// moved to item <val>". It carries distinct cases we tell apart by the owner
// object's handler pointer (obj[0]):
//   * a config screen (obj[0]==FUN_0023fbe0) -> speak the focused row's name and,
//     for a value row (FUN_0023d6b0), append its current value (the highlighted
//     option child's label);
//   * a confirm/quit pop-up (obj[0]==FUN_00241d40) -> speak the body prompt
//     (once) and the focused Yes/No button (id 1000/1001, code-fixed by index);
//   * any other list menu -> speak the focused row's name.
// A config value change (left/right) sends NO focus message; it writes the store
// via FUN_00240750, which we hook to announce the new value.
// NO option-name strings live here — everything spoken is the game's own text.
constexpr uint32_t RVA_DISPATCH     = 0x127510;   // FUN_00247510(owner, msg, val)
constexpr uint32_t RVA_TITLE_WINDOW = 0x29CE4C8;  // DAT_02aee4c8 — TitleReader owns the title menu
constexpr uint32_t RVA_CONFIRM_WND  = 0x121D40;   // FUN_00241d40 — confirm/menu window handler (obj[0])
// Active-pane isolation: DAT_0208ebc0 holds the window the input pump routes the D-pad to (the
// focused pane); the `owner` of a 0x8000 IS that pane's controller, so owner == *DAT_0208ebc0
// means "this pane holds the cursor". FUN_00244830(old,new,flag) sets it (DAT_0208ebc0 = new) and
// fires just AFTER a pane's entry 0x8000, so we hook it to replay the just-entered focus item.
constexpr uint32_t RVA_FOCUS_WINDOW = 0x1F6EBC0;  // DAT_0208ebc0 — global input-focus window ptr
constexpr uint32_t RVA_FOCUS_SET    = 0x124830;   // FUN_00244830(old, new, flag)

// New-game / config screen (probe- + log-confirmed 2026-07-06). Controller =
// FUN_0023fbe0; the row array is at ctrl+0xE8 (stride 0x18). Value-setting rows come
// in two layouts (per FUN_0023ed80's type switch): types 1/2/8 (On/Off etc.) =
// FUN_0023e770, type 3 = FUN_0023d6b0. A left/right change writes FUN_00240750 with
// no focus message, so that write is the on-change announce trigger.
constexpr uint32_t RVA_CONFIG_CTRL   = 0x11FBE0; // FUN_0023fbe0 — main config controller (rows at +0xE8)
constexpr uint32_t RVA_GFX_CTRL      = 0x11BD40; // FUN_0023bd40 — Graphics sub-screen (rows at +0x4E0)
constexpr uint32_t RVA_CONTROLS_CTRL = 0x11CE10; // FUN_0023ce10 — Controls sub-screen (rows at +0xD8)
constexpr uint32_t RVA_VALROW_E770 = 0x11E770;  // FUN_0023e770 — enum value row types 1/2/8 (obj[0])
constexpr uint32_t RVA_VALROW_D6B0 = 0x11D6B0;  // FUN_0023d6b0 — enum value row type 3 (main screen)
constexpr uint32_t RVA_VALROW_DB40 = 0x11DB40;  // FUN_0023db40 — enum value row type 3 (Controls) — same layout as D6B0
constexpr uint32_t RVA_VALROW_EBE0 = 0x11EBE0;  // FUN_0023ebe0 — slider/gauge value row (numeric)
constexpr uint32_t RVA_VALROW_B330 = 0x11B330;  // FUN_0023b330 — Graphics slider (gauge child) — numeric
constexpr uint32_t RVA_VALROW_B6F0 = 0x11B6F0;  // FUN_0023b6f0 — Graphics enum (fmt buf at row+0xCC)
constexpr uint32_t RVA_VALROW_C5C0 = 0x11C5C0;  // FUN_0023c5c0 — Controls key-binding row (codes only, no name)
// Controls key-binding row (FUN_0023c5c0): caches DIK key CODES only, not the display name.
constexpr uint32_t OFF_BINDROW_CODE0  = 0xD0;   // *(u32)(row+0xd0 + col*4) = live key code (col 0 = primary keyboard)
constexpr uint32_t OFF_BINDROW_REBIND = 0xEC;   // *(u32)(row+0xec) != 0 => mid-rebind ("press a key")
constexpr int      BIND_ID_REBIND     = 0x46dd; // the game's "press a key" prompt string id
constexpr uint32_t OFF_CTRL_ROWARR_GFX = 0x4E0; // FUN_0023bd40 row array
constexpr uint32_t OFF_CTRL_ROWARR_CTL = 0xD8;  // FUN_0023ce10 row array
// Active-instance globals (set on open, cleared on close) — used to skip reads when a
// controller isn't the live menu, so we never dereference a closed/freed menu's widgets.
constexpr uint32_t RVA_INST_FBE0 = 0x1F6E6D8;   // DAT_0208e6d8 — active FUN_0023fbe0 instance
constexpr uint32_t RVA_INST_CE10 = 0x1F6E6D0;   // DAT_0208e6d0 — active FUN_0023ce10 instance
constexpr uint32_t RVA_STORE_WRITE = 0x120750;  // FUN_00240750(configId, &newValue) — config-store change
constexpr uint32_t RVA_GFX_WRITE   = 0x5DB90;   // FUN_0017db90(configId, curVal, dir) -> newVal — Graphics change
constexpr uint32_t OFF_GFX_ROW_CFGID = 0xC0;    // Graphics value row -> config id (int)

constexpr uint32_t OFF_POPUP_BODY  = 0x1B0;   // confirm window -> inline codec body prompt
constexpr uint32_t OFF_CTRL_ROWARR = 0xE8;    // controller -> row widget at ctrl+0xE8 + N*0x18
constexpr uint32_t ROW_STRIDE      = 0x18;    // controller row-array slot stride
constexpr uint32_t OFF_ROW_CFGID   = 0xC0;    // value row -> config id (u8)
constexpr uint32_t OFF_ROW_CHILDS  = 0x60;    // value row -> option-child pointer array
constexpr uint32_t OFF_ROW_CBASE   = 0xD0;    // value row -> base child index (u8, = 8)
constexpr uint32_t OFF_ROW_CCOUNT  = 0xD2;    // value row -> option count (u8, = 6)
constexpr uint32_t OFF_CHILD_FLAGS = 0x08;    // option child -> base flags; bit0 = selected
constexpr uint32_t OFF_CHILD_LABEL = 0x18;    // option child -> *(child+0x18) = codec label bytes

constexpr uint64_t MSG_FOCUS  = 0x8000;
constexpr uint64_t MSG_YES    = 0x8100;      // no-list 2-choice pop-up results (owner = parent)
constexpr uint64_t MSG_NO     = 0x8101;
constexpr uint64_t MSG_CANCEL = 0x8102;

typedef uintptr_t (*Pfn_Dispatch)(void*, uintptr_t, uintptr_t);
Pfn_Dispatch s_origDispatch = nullptr;

typedef void (*Pfn_StoreWrite)(uintptr_t, void*);   // FUN_00240750(configId, &newDisplayIdx)
Pfn_StoreWrite s_origStoreWrite = nullptr;

typedef uint32_t (*Pfn_GfxWrite)(uint32_t, uint32_t, uint32_t);  // FUN_0017db90(configId, curVal, dir)
Pfn_GfxWrite s_origGfxWrite = nullptr;

typedef void (*Pfn_FocusSet)(void*, void*, int);   // FUN_00244830(old, new, flag)
Pfn_FocusSet s_origFocusSet = nullptr;

std::mutex g_mutex;
// The CURRENTLY-focused row. NOT a dedup key — the config value-change hooks
// (HookedStoreWrite / HookedGfxWrite) get no focus message of their own, so they read these to
// learn which row the value belongs to.
void* g_focusOwner = nullptr;
int   g_focusIndex = -1;
void* g_pendingOwner = nullptr;   // focus whose text wasn't painted yet (menu-entry replay)
int   g_pendingIndex = -1;
void* g_valueChangeOwner = nullptr;   // Graphics value change awaiting a settled paint to announce
int   g_valueChangeIndex = -1;
// Last 0x8000 focus, stashed so the FUN_00244830 focus-change hook can replay the entry item
// once DAT_0208ebc0 flips to the newly-entered pane (the entry 0x8000 fires just before that).
void* g_stashOwner = nullptr;
int   g_stashIndex = -1;
uint32_t g_stashRowOff = 0;
void* g_diagOwner = nullptr;       // active-pane diagnostic dedup (owner, focus) pair
void* g_diagFocus = nullptr;
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
// The guard logic lives once in core/mem_read.h (shared with the message reader).
using MemRead::SafeReadPtr;
using MemRead::PtrAt;
using MemRead::Obj0;
using MemRead::SafeReadU8;
using MemRead::SafeReadU32;
using MemRead::SafeReadInt;

bool IsTitleMenu(void* owner) {
    void* titleWin = nullptr;
    if (SafeReadPtr(reinterpret_cast<void*>(Hooks::ResolveRva(RVA_TITLE_WINDOW)), &titleWin))
        return owner != nullptr && owner == titleWin;
    return false;
}
bool IsConfirmWindow(void* owner) { return owner && Obj0(owner) == Hooks::ResolveRva(RVA_CONFIRM_WND); }
bool IsConfigController(void* owner) {
    if (!owner) return false;
    void* cls = Obj0(owner);
    return cls == Hooks::ResolveRva(RVA_CONFIG_CTRL) ||
           cls == Hooks::ResolveRva(RVA_GFX_CTRL) ||
           cls == Hooks::ResolveRva(RVA_CONTROLS_CTRL);
}
bool IsConfigValueRow(void* row) {
    if (!row) return false;
    void* cls = Obj0(row);
    return cls == Hooks::ResolveRva(RVA_VALROW_E770) ||
           cls == Hooks::ResolveRva(RVA_VALROW_D6B0) ||
           cls == Hooks::ResolveRva(RVA_VALROW_DB40) ||
           cls == Hooks::ResolveRva(RVA_VALROW_EBE0) ||
           cls == Hooks::ResolveRva(RVA_VALROW_B330) ||
           cls == Hooks::ResolveRva(RVA_VALROW_B6F0) ||
           cls == Hooks::ResolveRva(RVA_VALROW_C5C0);
}

// True only when `owner` is the CURRENTLY-ACTIVE instance of its config controller.
// Guards value reads so we never dereference a closed/freed menu's widgets (which is
// unsafe even under SEH). Controllers without a known instance global fall through to
// IsConfigController + SEH (Graphics; its reads were already stable in testing).
bool IsActiveConfig(void* owner) {
    if (!owner) return false;
    void* cls = Obj0(owner);
    uint32_t instRva = 0;
    if (cls == Hooks::ResolveRva(RVA_CONFIG_CTRL))        instRva = RVA_INST_FBE0;
    else if (cls == Hooks::ResolveRva(RVA_CONTROLS_CTRL)) instRva = RVA_INST_CE10;
    else return IsConfigController(owner);   // Graphics etc.: obj[0] identity + SEH
    void* active = nullptr;
    return SafeReadPtr(Hooks::ResolveRva(instRva), &active) && active == owner;
}

// If `owner` is a confirm window, decode its inline body prompt at +0x1b0.
std::wstring ReadPopupBody(void* owner) {
    if (!IsConfirmWindow(owner)) return std::wstring();
    std::wstring s = GameText::Decode(
        reinterpret_cast<const uint8_t*>(reinterpret_cast<char*>(owner) + OFF_POPUP_BODY), 0x200);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// ---- config value read (new-game screen: FUN_0023fbe0) -----------------------
// Row array at ctrl+0xE8 (stride 0x18). Two value-row layouts (FUN_0023ed80 switch):
//   FUN_0023e770 (types 1/2/8): selected index at row+0xd0; option cells inline at
//     row+0xd8 + i*8; the selected cell's codec label = *(*(*(cell+0x60)+8)+0x18).
//   FUN_0023d6b0 (type 3): option children in the ptr-array at row+0x60 (base
//     row+0xd0, count row+0xd2); selected = child with (child+8)&1; label *(child+0x18).
// SEH-guarded raw reads use POD buffers only; decoding happens outside the guard.

// The N-th row widget on a config controller. Row-array base differs by controller:
// FUN_0023fbe0 -> +0xE8, FUN_0023ce10 (Controls) -> +0xD8, FUN_0023bd40 (Graphics) -> +0x4E0. Stride 0x18.
void* ConfigRowWidget(void* ctrl, int index) {
    if (!ctrl || index < 0) return nullptr;
    void* cls = Obj0(ctrl);
    uint32_t base = OFF_CTRL_ROWARR;                                              // FUN_0023fbe0
    if (cls == Hooks::ResolveRva(RVA_GFX_CTRL))           base = OFF_CTRL_ROWARR_GFX;
    else if (cls == Hooks::ResolveRva(RVA_CONTROLS_CTRL)) base = OFF_CTRL_ROWARR_CTL;
    return PtrAt(ctrl, base + static_cast<uint32_t>(index) * ROW_STRIDE);
}

// Copy the codec label bytes of the option at display index `idx`, per row class.
bool ReadOptionBytes(void* row, void* cls, int idx, uint8_t* out, size_t cap, size_t* outLen) {
    if (!row || idx < 0 || cap == 0) return false;
    __try {
        char* r = reinterpret_cast<char*>(row);
        const uint8_t* codec = nullptr;
        if (cls == Hooks::ResolveRva(RVA_VALROW_E770)) {
            void* cell = *reinterpret_cast<void* const*>(r + 0xd8 + static_cast<size_t>(idx) * sizeof(void*));
            void* arr  = cell ? *reinterpret_cast<void* const*>(reinterpret_cast<char*>(cell) + 0x60) : nullptr;
            void* leaf = arr  ? *reinterpret_cast<void* const*>(reinterpret_cast<char*>(arr) + 8)     : nullptr;
            if (leaf) codec = *reinterpret_cast<const uint8_t* const*>(reinterpret_cast<char*>(leaf) + 0x18);
        } else if (cls == Hooks::ResolveRva(RVA_VALROW_D6B0) || cls == Hooks::ResolveRva(RVA_VALROW_DB40)) {
            void* arr = *reinterpret_cast<void* const*>(r + OFF_ROW_CHILDS);
            int base  = *reinterpret_cast<uint8_t*>(r + OFF_ROW_CBASE);
            void* child = arr ? *reinterpret_cast<void* const*>(
                reinterpret_cast<char*>(arr) + static_cast<size_t>(base + idx) * sizeof(void*)) : nullptr;
            if (child) codec = *reinterpret_cast<const uint8_t* const*>(
                reinterpret_cast<char*>(child) + OFF_CHILD_LABEL);
        }
        if (!codec) return false;
        size_t k = 0;
        for (; k + 1 < cap; ++k) { uint8_t b = codec[k]; out[k] = b; if (!b) break; }
        out[k] = 0;
        *outLen = k;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Current selected display index of a value row (per class), or -1.
int SelectedIndex(void* row, void* cls) {
    if (!row) return -1;
    __try {
        char* r = reinterpret_cast<char*>(row);
        if (cls == Hooks::ResolveRva(RVA_VALROW_E770)) {
            return *reinterpret_cast<uint8_t*>(r + 0xd0);          // stored directly
        }
        if (cls == Hooks::ResolveRva(RVA_VALROW_D6B0) || cls == Hooks::ResolveRva(RVA_VALROW_DB40)) {
            void* arr = *reinterpret_cast<void* const*>(r + OFF_ROW_CHILDS);
            if (!arr) return -1;
            int base  = *reinterpret_cast<uint8_t*>(r + OFF_ROW_CBASE);
            int count = *reinterpret_cast<uint8_t*>(r + OFF_ROW_CCOUNT);
            for (int i = 0; i < count && i < 32; ++i) {
                void* child = *reinterpret_cast<void* const*>(
                    reinterpret_cast<char*>(arr) + static_cast<size_t>(base + i) * sizeof(void*));
                if (child && (*reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(child) + OFF_CHILD_FLAGS) & 1u))
                    return i;
            }
        }
        return -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Decode the option label of a value row at display index `idx`. Empty if unreadable.
std::wstring OptionLabel(void* row, int idx) {
    if (!IsConfigValueRow(row) || idx < 0) return std::wstring();
    uint8_t buf[256];
    size_t len = 0;
    if (!ReadOptionBytes(row, Obj0(row), idx, buf, sizeof(buf), &len) || len == 0) return std::wstring();
    std::wstring s = GameText::Decode(buf, len);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// FUN_0023ebe0 slider: the gauge child (*(*(row+0x60))) holds the current value at
// +0x18 and the range at +0x1c. We speak a number: raw when the range is <= 100
// (already a 0-100-ish scale), a rounded percentage when the range is larger.
bool ReadSlider(void* row, uint32_t* valOut, uint32_t* maxOut) {
    if (!row) return false;
    __try {
        void* arr = *reinterpret_cast<void* const*>(reinterpret_cast<char*>(row) + OFF_ROW_CHILDS);
        if (!arr) return false;
        void* gauge = *reinterpret_cast<void* const*>(arr);   // child[0]
        if (!gauge) return false;
        *valOut = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(gauge) + 0x18);
        *maxOut = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(gauge) + 0x1c);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::wstring FormatSlider(uint32_t val, uint32_t max) {
    if (max == 0) return std::wstring();
    int n = (max > 100) ? static_cast<int>((static_cast<uint64_t>(val) * 100 + max / 2) / max)
                        : static_cast<int>(val);
    return std::to_wstring(n);
}

// Graphics value rows (FUN_0023b330/FUN_0023b6f0) format their display value into an
// inline codec buffer on the row (row+0xD0 / row+0xCC) via FUN_0023b530. Copy + decode.
bool ReadInlineBytes(void* row, uint32_t off, uint8_t* out, size_t cap, size_t* outLen) {
    if (!row || cap == 0) return false;
    __try {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(reinterpret_cast<char*>(row) + off);
        size_t k = 0;
        for (; k + 1 < cap; ++k) { uint8_t b = p[k]; out[k] = b; if (!b) break; }
        out[k] = 0;
        *outLen = k;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::wstring InlineCodecValue(void* row, uint32_t off) {
    uint8_t buf[256];
    size_t len = 0;
    if (!ReadInlineBytes(row, off, buf, sizeof(buf), &len) || len == 0) return std::wstring();
    std::wstring s = GameText::Decode(buf, len);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// Replicate FUN_001e0b00(code): a DIK key code -> localized key-name string id. Pure
// arithmetic; the base depends on the active keyboard layout, matching the game's own
// GetKeyboardLayout switch (US default, FR 0x40c, DE 0x407). Keyboard keys are code >= 0x1c.
int KeyCodeToStringId(int code) {
    int base = 0x46e1;                                            // US / default
    uintptr_t lang = reinterpret_cast<uintptr_t>(GetKeyboardLayout(0)) & 0xfff;
    if (lang == 0x40c)      base = 0x4749;                        // FR
    else if (lang == 0x407) base = 0x47b1;                        // DE
    if (code >= 0x1c) return code - 0x1c + base;
    if (code >= 1)    return code + 0x46dd;
    return 0x46dc;
}

// Controls key-binding row (FUN_0023c5c0): the row caches only DIK CODES (col 0 = primary
// keyboard), never the name — the game resolves the name transiently at draw via
// FUN_001e0b00(code) -> id -> FUN_002f9860. We replicate the arithmetic and read the NAME
// from TextCapture's id cache, which the game's own draw of this row just populated (no game
// call). Keyboard column only; controller bindings are a later pass.
std::wstring ControlsBindingValue(void* row) {
    uint32_t rebind = 0;
    if (SafeReadU32(row, OFF_BINDROW_REBIND, &rebind) && rebind != 0)
        return TextCapture::StringById(BIND_ID_REBIND);           // "press a key" — the game's own prompt
    uint32_t code = 0;
    if (!SafeReadU32(row, OFF_BINDROW_CODE0, &code) || code == 0) return std::wstring();   // unbound
    return TextCapture::StringById(KeyCodeToStringId(static_cast<int>(code)));
}

// Current value of a config value row: enum option label, slider number, Graphics formatted
// string, or Controls key-binding name.
std::wstring ConfigRowValue(void* row) {
    if (!IsConfigValueRow(row)) return std::wstring();
    void* cls = Obj0(row);
    // Numeric-gauge sliders: FUN_0023ebe0 (audio volume) and FUN_0023b330 (Graphics).
    if (cls == Hooks::ResolveRva(RVA_VALROW_EBE0) || cls == Hooks::ResolveRva(RVA_VALROW_B330)) {
        uint32_t val = 0, max = 0;
        return ReadSlider(row, &val, &max) ? FormatSlider(val, max) : std::wstring();
    }
    if (cls == Hooks::ResolveRva(RVA_VALROW_B6F0)) return InlineCodecValue(row, 0xCC);   // Graphics enum
    if (cls == Hooks::ResolveRva(RVA_VALROW_C5C0)) return ControlsBindingValue(row);     // Controls key binding
    return OptionLabel(row, SelectedIndex(row, cls));                     // E770 / D6B0 / DB40 enums
}

// Value a row shows given the new store value `nv` — used on change, where the
// widget's cached state isn't updated yet: enum option `nv`, or slider value `nv`.
std::wstring ConfigRowValueAtNewValue(void* row, int nv) {
    if (!IsConfigValueRow(row) || nv < 0) return std::wstring();
    void* cls = Obj0(row);
    if (cls == Hooks::ResolveRva(RVA_VALROW_EBE0)) {
        uint32_t val = 0, max = 0;
        if (!ReadSlider(row, &val, &max)) return std::wstring();
        return FormatSlider(static_cast<uint32_t>(nv), max);   // nv is the new gauge value
    }
    return OptionLabel(row, nv);   // nv is the new option index
}

// On-demand describe key ('i'): speak the focused item's help/description that the
// game placed in the description bar (captured in TextCapture). Silent if the
// current item has none (silence beats a wrong or invented string). Runs on the
// input thread.
void DescribeHotkey() {
    std::wstring desc = TextCapture::CurrentHelpText();
    if (desc.empty()) return;
    LogLine("  describe: ", desc);
    Speech::Output(desc, /*interrupt=*/true);
}

// True when `owner` is the window the input pump currently routes the cursor to (DAT_0208ebc0).
// Companion panes that merely repaint on the same keypress are NOT it, so this isolates the one
// pane the player is actually in. SEH-guarded; a fault reads as "not focused".
bool IsFocusedPane(void* owner) {
    if (!owner) return false;
    void* fw = nullptr;
    return SafeReadPtr(Hooks::ResolveRva(RVA_FOCUS_WINDOW), &fw) && owner == fw;
}

void OnFocus(void* owner, int index, bool fromPaint) {
    if (index < 0) return;
    if (IsTitleMenu(owner)) return;               // TitleReader handles the title command menu

    // 1-frame settle: defer config-row speech to the next paint so a scrolled-in row
    // has settled text + value (fixes the occasional missed/stale read on fast scroll).
    // Non-config surfaces (pop-ups, etc.) still speak immediately.
    if (!fromPaint && IsConfigController(owner)) {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_pendingOwner = owner;
        g_pendingIndex = index;
        return;
    }

    const bool isPopup = IsConfirmWindow(owner);

    // Active-pane gate: a plain in-game content pane (not a pop-up, not a config controller)
    // speaks only when it currently holds the cursor — this is what stops the inventory "mixed"
    // reading (items/magicks/equipment from several panes at once). Pop-ups and config own focus
    // and are exempt. Entering a pane is handled by the FUN_00244830 replay, which re-invokes this
    // (fromPaint) once DAT_0208ebc0 has flipped to the entered pane.
    if (!isPopup && !IsConfigController(owner) && !IsFocusedPane(owner)) return;

    // Build what we'll speak: pop-up button label (code-fixed by index), or the
    // focused row's "name" / "name: value".
    std::wstring text;
    if (isPopup) {
        text = TextCapture::StringById(1000 + (index != 0 ? 1 : 0));   // 1000=Yes, 1001=No
    } else {
        text = TextCapture::FocusedItemText(owner, index);            // row name
        // Append the setting's value — only when this is the ACTIVE config menu, so we
        // never dereference a closed/freed menu's row widgets.
        if (!text.empty() && IsActiveConfig(owner)) {
            void* row = ConfigRowWidget(owner, index);
            std::wstring val = ConfigRowValue(row);
            if (!val.empty()) { text += L": "; text += val; }
        }
    }

    // Record the focus and speak it. NO dedup: this used to drop a focus matching the cached
    // (owner, index, text), which made leaving a pane and returning to the same row SILENT — the
    // pane gate above returns early without updating the cache, so the stale entry survived the
    // excursion and swallowed the re-entry. `ownerChanged` is kept only to gate the pop-up body
    // preamble below, never to suppress the row itself.
    bool ownerChanged;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        ownerChanged = (owner != g_focusOwner);
        g_focusOwner = owner;
        g_focusIndex = index;
    }

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
    // Focus-pending replay (deferred focus speech, incl. the 1-frame settle).
    void* pend; int idx;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        pend = g_pendingOwner; idx = g_pendingIndex;
    }
    if (owner == pend && idx >= 0) {
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_pendingOwner = nullptr; g_pendingIndex = -1;
        }
        OnFocus(owner, idx, /*fromPaint=*/true);
    }

    // Graphics value-change replay: announce just the new value once the row's display
    // text has settled on this draw (FUN_0017db90 marked the change).
    void* vcOwner; int vcIdx;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        vcOwner = g_valueChangeOwner; vcIdx = g_valueChangeIndex;
    }
    if (owner == vcOwner && vcIdx >= 0) {
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_valueChangeOwner = nullptr; g_valueChangeIndex = -1;
        }
        if (IsActiveConfig(owner)) {
            std::wstring val = ConfigRowValue(ConfigRowWidget(owner, vcIdx));
            if (!val.empty()) {
                LogLine("  value: ", val);
                Speech::Output(val, /*interrupt=*/true);
            }
        }
    }
}

uintptr_t HookedDispatch(void* owner, uintptr_t msg, uintptr_t val) {
    if (msg == MSG_FOCUS) {
        // Bump the tooltip focus generation BEFORE the game handles the focus, so
        // the description it sets (FUN_00291d80) during s_origDispatch is attributed
        // to this focus for the `o` key.
        TextCapture::NotifyFocusChanged();
        const int index = static_cast<int>(static_cast<intptr_t>(val));
        const uint32_t rowOff = IngameMenuReader::RowChainOff(owner);

        // Stash this focus so the FUN_00244830 focus-change hook can replay the entry item once
        // DAT_0208ebc0 flips to the entered pane (the entry 0x8000 fires just before that flip,
        // so it would otherwise be gated out). Also emit a deduped active-pane diagnostic.
        void* focusWin = nullptr;
        SafeReadPtr(reinterpret_cast<void*>(Hooks::ResolveRva(RVA_FOCUS_WINDOW)), &focusWin);
        bool diag;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_stashOwner = owner; g_stashIndex = index; g_stashRowOff = rowOff;
            diag = (owner != g_diagOwner || focusWin != g_diagFocus);
            if (diag) { g_diagOwner = owner; g_diagFocus = focusWin; }
        }
        if (diag) {
            char d[128];
            snprintf(d, sizeof(d), "pane owner=%p focus=%p focused=%d rowOff=0x%X",
                     owner, focusWin, owner == focusWin ? 1 : 0, rowOff);
            Log::Write("READER", d);
        }

        if (IngameMenuReader::IsBattleCommandOwner(owner)) {
            // Battle command menu (Attack / Magicks & Technicks / Items / ...). A SEPARATE system —
            // NOT gated by the field-menu IsFocusedPane pane isolation. `index` = highlighted command.
            IngameMenuReader::OnBattleCommandFocus(owner, index);
        } else if (rowOff) {
            // Row-chain in-game menu (field pause menu + submenus): `val` is the focused row index.
            // Speak only if this pane holds the cursor.
            if (IsFocusedPane(owner))
                IngameMenuReader::OnRowChainFocus(owner, rowOff, index);
        } else {
            OnFocus(owner, index, /*fromPaint=*/false);   // gates the content path internally
        }
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

// FUN_00244830(old, new, flag): sets DAT_0208ebc0 = new (the pane gaining the cursor). It fires
// just AFTER the entered pane's first 0x8000 (which was gated out because the focus pointer hadn't
// flipped yet), so we replay that stashed focus now that IsFocusedPane(new) is true — this is what
// makes the first item on entering a submenu speak.
void HookedFocusSet(void* oldWin, void* newWin, int flag) {
    if (s_origFocusSet) s_origFocusSet(oldWin, newWin, flag);
    void* o; int idx; uint32_t rowOff;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        o = g_stashOwner; idx = g_stashIndex; rowOff = g_stashRowOff;
    }
    if (o && o == newWin && idx >= 0) {
        if (rowOff) IngameMenuReader::OnRowChainFocus(o, rowOff, idx);
        else        OnFocus(o, idx, /*fromPaint=*/true);
    }
}

// FUN_00240750(configId, &newDisplayIdx): the config store is written when a value
// row changes (left/right). No focus message fires for an in-place value change, so
// this write is the on-change announce trigger. Speak only when the CURRENTLY-
// focused row is the value row whose config id matches — so a "Restore Defaults"
// batch write (many configs; focused row is a button) never speaks, and no per-row
// dedup is needed (FUN_0023d6b0 calls this only on a genuine change).
void HookedStoreWrite(uintptr_t configId, void* pIdx) {
    void* owner; int idx;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        owner = g_focusOwner;
        idx   = g_focusIndex;
    }
    if (IsActiveConfig(owner) && idx >= 0) {
        void* row = ConfigRowWidget(owner, idx);
        uint8_t rowCfg = 0xff;
        if (IsConfigValueRow(row) && SafeReadU8(row, OFF_ROW_CFGID, &rowCfg) &&
            rowCfg == static_cast<uint8_t>(configId & 0xff)) {
            int newVal = -1;
            SafeReadInt(pIdx, &newVal);                  // new option index OR new slider value
            std::wstring val = ConfigRowValueAtNewValue(row, newVal);
            if (val.empty()) val = ConfigRowValue(row);  // fallback to current state
            if (!val.empty()) {
                LogLine("  value: ", val);
                Speech::Output(val, /*interrupt=*/true);
            }
        }
    }
    if (s_origStoreWrite) s_origStoreWrite(configId, pIdx);
}

// FUN_0017db90(configId, curVal, dir): the Graphics subsystem's value setter, called on
// left/right in FUN_0023b330/b6f0. The row's display text only refreshes on the next
// draw, so we mark the change (gated to the focused Graphics row's config id) and
// announce it from OnMenuPainted once it has settled.
uint32_t HookedGfxWrite(uint32_t configId, uint32_t curVal, uint32_t dir) {
    uint32_t newVal = s_origGfxWrite ? s_origGfxWrite(configId, curVal, dir) : 0;
    void* owner; int idx;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        owner = g_focusOwner;
        idx   = g_focusIndex;
    }
    if (idx >= 0 && IsActiveConfig(owner) && Obj0(owner) == Hooks::ResolveRva(RVA_GFX_CTRL)) {
        void* row = ConfigRowWidget(owner, idx);
        int rowCfg = -1;
        if (IsConfigValueRow(row) &&
            SafeReadInt(reinterpret_cast<char*>(row) + OFF_GFX_ROW_CFGID, &rowCfg) &&
            rowCfg == static_cast<int>(configId)) {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_valueChangeOwner = owner;
            g_valueChangeIndex = idx;
        }
    }
    return newVal;
}

} // namespace

namespace MenuReader {

bool Init() {
    if (g_initialized) {
        Log::Write("READER", "MenuReader::Init called twice — ignoring");
        return true;
    }
    TextCapture::SetMenuPaintedCallback(&OnMenuPainted);
    InputTracker::SetDescribeCallback(&DescribeHotkey);
    bool ok = Hooks::InstallTyped(RVA_DISPATCH,    &HookedDispatch,   &s_origDispatch);
    ok     &= Hooks::InstallTyped(RVA_STORE_WRITE, &HookedStoreWrite, &s_origStoreWrite);
    ok     &= Hooks::InstallTyped(RVA_GFX_WRITE,   &HookedGfxWrite,   &s_origGfxWrite);
    ok     &= Hooks::InstallTyped(RVA_FOCUS_SET,   &HookedFocusSet,   &s_origFocusSet);  // active-pane entry replay
    ok     &= IngameMenuReader::Init();   // battle command + target-reticle name hooks
    ok     &= BattleTargetReader::Init(); // battle target-selection readout (FUN_00329220 + ctx+0xde0)
    g_initialized = true;
    Log::Write("READER", ok
        ? "MenuReader initialized (0x8000 -> row name+value; config value-on-change via "
          "FUN_00240750; 'i' -> item description; pop-up Yes/No; title skipped)."
        : "MenuReader: a hook failed to install — see Hooks log.");
    return ok;
}

void Shutdown() {
    if (!g_initialized) return;
    TextCapture::SetMenuPaintedCallback(nullptr);
    InputTracker::SetDescribeCallback(nullptr);
    IngameMenuReader::Shutdown();
    BattleTargetReader::Shutdown();
    Hooks::Uninstall(RVA_FOCUS_SET);
    Hooks::Uninstall(RVA_GFX_WRITE);
    Hooks::Uninstall(RVA_STORE_WRITE);
    Hooks::Uninstall(RVA_DISPATCH);
    g_initialized = false;
    Log::Write("READER", "MenuReader shut down");
}

} // namespace MenuReader

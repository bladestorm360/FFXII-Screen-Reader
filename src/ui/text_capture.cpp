#include "ui/text_capture.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/stall_probe.h"
#include "speech/speech.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

// The three UI-text draw functions + the universal list painter + the id->string
// resolver (abs = RVA + 0x120000).
constexpr uint32_t RVA_TEXT_IMM  = 0x190280;  // FUN_002b0280(struct)   immediate glyph sink
constexpr uint32_t RVA_TEXT_OBJ1 = 0x18BEC0;  // FUN_002abec0(obj)      object/scene draw
constexpr uint32_t RVA_TEXT_OBJ2 = 0x18BF20;  // FUN_002abf20(obj)      object/scene draw
constexpr uint32_t RVA_PAINTER   = 0x1B28E0;  // FUN_002d28e0(p1,p2,subwidget) list painter
constexpr uint32_t RVA_RESOLVE   = 0x1D9860;  // FUN_002f9860(id) -> codec byte* (localized string)
constexpr uint32_t RVA_DESC_SET  = 0x171D80;  // FUN_00291d80(codecText, flag) description-bar setter
// Item/ability/equipment/battle-command DESCRIPTIONS use a different sink than FUN_00291d80: the
// display notifier FUN_00293170 formats the focused entry's description via FUN_00292b70, which
// writes the finished codec to (outBuf+8) and returns 1. We hook the formatter and capture that
// codec — but only while inside FUN_00293170 (the on-highlight display call), so the off-screen
// width-measurement callers of FUN_00292b70 don't pollute the `o`-key description.
constexpr uint32_t RVA_ITEMDESC_DISPLAY = 0x173170;  // FUN_00293170 (on-highlight desc display)
constexpr uint32_t RVA_ITEMDESC_FMT     = 0x172B70;  // FUN_00292b70(outBuf, params) -> outBuf+8 = codec
constexpr uint32_t OFF_ITEMDESC_TEXT    = 0x08;      // outBuf+8 = formatted description codec

constexpr uint32_t OFF_CODEC_STR = 0x28;      // imm text struct -> codec byte*
constexpr uint32_t OFF_SUB_OWNER = 0xC8;      // subwidget -> owner (== the 0x8000 focus owner)
constexpr uint32_t OFF_SUB_CB    = 0x120;     // subwidget -> per-item cell callback (== subwidget[0x24])

constexpr size_t   RING_MAX = 256;
constexpr size_t   OWNER_MAP_CAP = 64;        // bound owner-map growth over a session

// Per-item cell callback: (context, rowDrawCtx, &geom, itemIndex) -> visible flag.
typedef int64_t     (*Pfn_Cell)(void*, int64_t, void*, int64_t);
typedef void        (*Pfn_Painter)(void*, int64_t, void*);
typedef void        (*Pfn_TextDraw)(void*);
typedef const uint8_t* (*Pfn_Resolve)(int);
typedef void        (*Pfn_DescSet)(void*, uintptr_t);
typedef void        (*Pfn_ItemDescDisplay)(int, uint32_t, int, int);  // FUN_00293170
typedef uint64_t    (*Pfn_ItemDescFmt)(void*, void*);                 // FUN_00292b70(outBuf, params)

Pfn_TextDraw s_origImm  = nullptr;
Pfn_TextDraw s_origObj1 = nullptr;
Pfn_TextDraw s_origObj2 = nullptr;
Pfn_Painter  s_origPainter = nullptr;
Pfn_Resolve  s_origResolve = nullptr;
Pfn_DescSet  s_origDescSet = nullptr;
Pfn_ItemDescDisplay s_origItemDescDisplay = nullptr;
Pfn_ItemDescFmt     s_origItemDescFmt     = nullptr;
thread_local bool s_inItemDesc = false;   // true only while inside FUN_00293170 (the display path)

std::mutex g_mutex;

// Diagnostic ring (framing text = every non-list draw; newest overwrites oldest).
struct RingEntry { bool imm; std::wstring text; };
std::array<RingEntry, RING_MAX> g_ring;
size_t g_head = 0, g_count = 0;

// Live owner -> (index -> row fields), rebuilt each paint. Keyed by owner so a
// pop-up drawn over a menu can't clobber the menu's rows.
std::unordered_map<void*, std::unordered_map<int, std::vector<std::wstring>>> g_itemsByOwner;
void* g_paintOwner = nullptr;     // owner of the paint in progress
int   g_curIdx = -1;              // item index being painted right now (sticky), -1 = none
bool  g_intercepting = false;     // re-entrancy guard for the painter swap
Pfn_Cell g_realCb = nullptr;      // the menu's real cell callback (during interception)

// Localized UI strings captured from FUN_002f9860 (id -> decoded), for pop-up
// button labels (ids 1000/1001) AND the Controls key-binding NAME block. Persists
// for the session. The key-binding names are the ids FUN_001e0b00 maps DIK codes to:
// a contiguous block (base 0x46e1 US .. 0x47b1 DE, + per-code offset up to ~0xD1, plus
// the 0x46dc/0x46dd specials). The Controls value reader can't call the game to resolve
// a code, so we cache the block as the game draws the focused binding, then look it up.
constexpr int BIND_ID_LO = 0x46dc;
constexpr int BIND_ID_HI = 0x4882;
std::unordered_map<int, std::wstring> g_idCache;

// Focused-item description (FUN_00291d80), gated to the current focus generation
// so the `i` hotkey never speaks a previous item's description.
std::wstring g_helpText;
uint32_t g_helpGen = 0;
uint32_t g_helpTextGen = 0xffffffffu;   // != g_helpGen until a description is set for a focus

TextCapture::MenuPaintedCallback g_paintedCb = nullptr;

bool g_initialized = false;
std::atomic<bool> g_interceptEnabled{true};   // Shift+` A/B; see TextCapture::ToggleInterception

std::wstring JoinFields(const std::vector<std::wstring>& fields) {
    std::wstring out;
    for (const auto& f : fields) {
        if (f.empty()) continue;
        if (!out.empty()) out += L": ";
        out += f;
    }
    return out;
}

// ---- SEH-guarded raw reads/writes (object-free so they can use __try) --------
bool ReadStrPtr(void* structPtr, const uint8_t** out) {
    if (!structPtr) return false;
    __try {
        *out = *reinterpret_cast<const uint8_t* const*>(
            reinterpret_cast<const char*>(structPtr) + OFF_CODEC_STR);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadSubwidget(void* sub, void** ownerOut, void*** slotOut, void** cbOut) {
    __try {
        *ownerOut = *reinterpret_cast<void* const*>(reinterpret_cast<char*>(sub) + OFF_SUB_OWNER);
        void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(sub) + OFF_SUB_CB);
        *slotOut = slot;
        *cbOut = *slot;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WriteSlot(void** slot, void* val) {
    __try { *slot = val; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- codec capture -----------------------------------------------------------
// listCapable: only the immediate glyph sink (FUN_002b0280) draws list-row
// labels; object draws are always framing (never attributed to an item).
void Capture(void* structPtr, bool listCapable) {
    STALL_SCOPE("TextCapture::Capture");
    const uint8_t* strp = nullptr;
    if (!ReadStrPtr(structPtr, &strp) || !strp) return;
    std::wstring text = GameText::Decode(strp);
    if (!GameText::IsMostlyPrintable(text)) return;

    // Once per STRING DRAWN, on the game's paint path. Decode already happened above, off the lock;
    // the evicted ring entry is carried out and destroyed off it too.
    std::wstring evicted;
    {
        StallProbe::TimedLock<std::mutex> lk(g_mutex, "lock:textcapture");
        if (listCapable && g_curIdx >= 0 && g_paintOwner) {
            g_itemsByOwner[g_paintOwner][g_curIdx].push_back(std::move(text));  // list row: read on focus
        } else {
            evicted.swap(g_ring[g_head].text);                                  // framing (diagnostics)
            g_ring[g_head] = RingEntry{ listCapable, std::move(text) };
            g_head = (g_head + 1) % RING_MAX;
            if (g_count < RING_MAX) ++g_count;
        }
    }
}

void HookImm(void* p1)  { Capture(p1, /*listCapable=*/true);  if (s_origImm)  s_origImm(p1);  }
void HookObj1(void* p1) { Capture(p1, /*listCapable=*/false); if (s_origObj1) s_origObj1(p1); }
void HookObj2(void* p1) { Capture(p1, /*listCapable=*/false); if (s_origObj2) s_origObj2(p1); }

const uint8_t* HookResolve(int id) {
    const uint8_t* ret = s_origResolve ? s_origResolve(id) : nullptr;
    STALL_SCOPE("TextCapture::HookResolve");
    if (ret && (id == 1000 || id == 1001 || (id >= BIND_ID_LO && id <= BIND_ID_HI))) {
        std::wstring s = GameText::Decode(ret);               // SEH-guarded inside
        if (GameText::IsMostlyPrintable(s)) {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_idCache[id] = std::move(s);
        }
    }
    return ret;
}

// FUN_00291d80(codecText, flag): the game sets the description-bar text here on
// each focus — config rows resolve their per-row help id, and the New Game+/- mode
// buttons feed their descriptions the same way. Cache it, tagged with the current
// focus generation so the `i` hotkey attributes it to exactly this focus.
void HookedDesc(void* codecText, uintptr_t flag) {
    STALL_SCOPE("TextCapture::HookedDesc");
    if (codecText) {
        std::wstring s = GameText::Decode(reinterpret_cast<const uint8_t*>(codecText));  // SEH-guarded inside
        if (GameText::IsMostlyPrintable(s)) {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_helpText = std::move(s);
            g_helpTextGen = g_helpGen;
        }
    }
    if (s_origDescSet) s_origDescSet(codecText, flag);
}

// FUN_00293170: the on-highlight item/ability/equipment/command description DISPLAY call. It formats
// the focused entry's description via FUN_00292b70. We flag "inside display" around it so only that
// formatter call (not the off-screen width-measurement callers) captures into g_helpText.
void HookedItemDescDisplay(int p1, uint32_t p2, int p3, int p4) {
    const bool prev = s_inItemDesc;
    s_inItemDesc = true;
    if (s_origItemDescDisplay) s_origItemDescDisplay(p1, p2, p3, p4);
    s_inItemDesc = prev;
}

// FUN_00292b70(outBuf, params): the shared description formatter — writes the finished codec to
// outBuf+8 and returns 1 on success. When invoked from the display path, capture that codec as the
// focused item's description (for the `o` key), tagged with the current focus generation.
uint64_t HookedItemDescFmt(void* outBuf, void* params) {
    uint64_t r = s_origItemDescFmt ? s_origItemDescFmt(outBuf, params) : 0;
    STALL_SCOPE("TextCapture::HookedItemDescFmt");
    if (s_inItemDesc && r && outBuf) {
        const uint8_t* codec = reinterpret_cast<const uint8_t*>(outBuf) + OFF_ITEMDESC_TEXT;
        std::wstring s = GameText::Decode(codec);   // SEH-guarded inside
        if (GameText::IsMostlyPrintable(s)) {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_helpText = std::move(s);
            g_helpTextGen = g_helpGen;
        }
    }
    return r;
}

// ---- per-item interception ---------------------------------------------------
// Called by the game's painter in place of the real callback (we swapped the
// pointer). Attributes this row's draws to `index`, then runs the real callback.
int64_t CellWrapper(void* ctx, int64_t rowDrawCtx, void* geom, int64_t indexArg) {
    int index = static_cast<int>(indexArg);
    // This runs once per ROW of every menu paint, on the game's own paint path. Anything held here
    // delays the menu's own drawing, so the critical section is kept to pointer swaps: the old
    // row's strings are moved out under the lock and FREED after releasing it (clear() used to run
    // every wstring destructor while the lock was held).
    std::vector<std::wstring> discard;
    {
        STALL_SCOPE("TextCapture::CellWrapper");
        StallProbe::TimedLock<std::mutex> lk(g_mutex, "lock:textcapture");
        g_curIdx = index;              // sticky: stays set through the post-callback glyph draw
        if (g_paintOwner) g_itemsByOwner[g_paintOwner][index].swap(discard);   // fresh for this paint
    }
    discard.clear();                   // destructors run off the lock
    Pfn_Cell real = g_realCb;          // read outside the lock (single game thread)
    if (!real) {
        // Also silent before. Returning 0 tells the menu "row not visible", which could make it
        // re-paint or skip rows -- worth knowing about rather than guessing at later.
        static bool s_warned = false;
        if (!s_warned) { s_warned = true; Log::Write("TEXT", "CellWrapper: g_realCb NULL -> returning 0"); }
        return 0;
    }
    return real(ctx, rowDrawCtx, geom, indexArg);
}

// Runs after the full paint: reset state, then notify the reader that `owner`'s
// item map is now populated (drives the menu-entry focus replay).
void FinishPaint(void* owner) {
    // Runs at the end of every menu paint, on the game's paint path. The eviction used to call
    // g_itemsByOwner.clear() while holding g_mutex -- destroying every cached string of every
    // surface inside the critical section the paint itself contends for. Swap the map out and let
    // it destruct after the lock is released.
    std::unordered_map<void*, std::unordered_map<int, std::vector<std::wstring>>> discard;
    {
        STALL_SCOPE("TextCapture::FinishPaint");
        StallProbe::TimedLock<std::mutex> lk(g_mutex, "lock:textcapture");
        g_curIdx = -1;
        g_intercepting = false;
        if (g_itemsByOwner.size() > OWNER_MAP_CAP) {
            std::unordered_map<int, std::vector<std::wstring>> saved;
            auto keep = g_itemsByOwner.find(owner);
            if (keep != g_itemsByOwner.end()) saved = std::move(keep->second);
            g_itemsByOwner.swap(discard);
            if (!saved.empty()) g_itemsByOwner[owner] = std::move(saved);
        }
    }
    discard.clear();                   // destructors run off the lock
    if (g_paintedCb) g_paintedCb(owner);
}

void HookedPainter(void* param_1, int64_t param_2, void* subwidget) {
    StallProbe::NoteThread("TextCapture::HookedPainter");
    // Second anchor, on the menu's OWN paint path: if the field sim pauses while a menu is up, the
    // field-frame anchor goes quiet legitimately and cannot tell a pause from a freeze. This one
    // only ticks while something is being drawn, so a gap here means drawing itself stopped.
    StallProbe::GapTick("anchor:painter", /*gapWarnMs=*/150.0);
    void* owner = nullptr; void** slot = nullptr; void* realCb = nullptr;
    bool intercept = false;
    if (g_interceptEnabled.load(std::memory_order_relaxed) &&
        subwidget && ReadSubwidget(subwidget, &owner, &slot, &realCb) && realCb) {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_intercepting) {
            g_intercepting = true;
            g_realCb = reinterpret_cast<Pfn_Cell>(realCb);
            g_paintOwner = owner;
            intercept = true;
        }
    }
    { STALL_SCOPE("TextCapture::PainterSwap");
      if (intercept && !WriteSlot(slot, reinterpret_cast<void*>(&CellWrapper))) {
          // Was silent before. A failed swap leaves the game's own callback in place, so rows go
          // uncaptured with no trace -- indistinguishable from "the menu drew nothing".
          static bool s_warned = false;
          if (!s_warned) { s_warned = true; Log::Write("TEXT", "painter swap FAILED (WriteSlot)"); }
          intercept = false;
      } }
    if (s_origPainter) s_origPainter(param_1, param_2, subwidget);
    if (intercept) {
        WriteSlot(slot, realCb);   // restore the game's callback
        StallProbe::NoteFirstPaint();   // closes the announce -> first-paint bracket
        FinishPaint(owner);
    }
}

} // namespace

namespace TextCapture {

bool Init() {
    if (g_initialized) return true;
    bool ok = true;
    ok &= Hooks::InstallTyped(RVA_TEXT_IMM,  &HookImm,       &s_origImm);
    ok &= Hooks::InstallTyped(RVA_TEXT_OBJ1, &HookObj1,      &s_origObj1);
    ok &= Hooks::InstallTyped(RVA_TEXT_OBJ2, &HookObj2,      &s_origObj2);
    ok &= Hooks::InstallTyped(RVA_PAINTER,   &HookedPainter, &s_origPainter);
    ok &= Hooks::InstallTyped(RVA_RESOLVE,   &HookResolve,   &s_origResolve);
    ok &= Hooks::InstallTyped(RVA_DESC_SET,  &HookedDesc,    &s_origDescSet);
    ok &= Hooks::InstallTyped(RVA_ITEMDESC_DISPLAY, &HookedItemDescDisplay, &s_origItemDescDisplay);
    ok &= Hooks::InstallTyped(RVA_ITEMDESC_FMT,     &HookedItemDescFmt,     &s_origItemDescFmt);
    g_initialized = true;
    Log::Write("TEXT", ok
        ? "TextCapture initialized (codec draws + FUN_002d28e0 per-item index map + id-string cache)."
        : "TextCapture: one or more hooks failed to install — see Hooks log.");
    return ok;
}

void Shutdown() {
    if (!g_initialized) return;
    Hooks::Uninstall(RVA_ITEMDESC_FMT);
    Hooks::Uninstall(RVA_ITEMDESC_DISPLAY);
    Hooks::Uninstall(RVA_DESC_SET);
    Hooks::Uninstall(RVA_RESOLVE);
    Hooks::Uninstall(RVA_PAINTER);
    Hooks::Uninstall(RVA_TEXT_IMM);
    Hooks::Uninstall(RVA_TEXT_OBJ1);
    Hooks::Uninstall(RVA_TEXT_OBJ2);
    std::lock_guard<std::mutex> lk(g_mutex);
    g_head = 0; g_count = 0; g_itemsByOwner.clear(); g_idCache.clear();
    g_paintOwner = nullptr; g_curIdx = -1; g_intercepting = false;
    g_helpText.clear(); g_helpGen = 0; g_helpTextGen = 0xffffffffu;
    g_initialized = false;
    Log::Write("TEXT", "TextCapture shut down");
}

std::wstring FocusedItemText(void* owner, int index) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto o = g_itemsByOwner.find(owner);
    if (o == g_itemsByOwner.end()) return std::wstring();
    auto it = o->second.find(index);
    if (it == o->second.end()) return std::wstring();
    return JoinFields(it->second);
}

std::wstring StringById(int id) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_idCache.find(id);
    return it != g_idCache.end() ? it->second : std::wstring();
}

std::wstring CurrentHelpText() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return (g_helpTextGen == g_helpGen) ? g_helpText : std::wstring();
}

void NotifyFocusChanged() {
    std::lock_guard<std::mutex> lk(g_mutex);
    ++g_helpGen;
}

void SetMenuPaintedCallback(MenuPaintedCallback cb) { g_paintedCb = cb; }

bool InterceptionEnabled() { return g_interceptEnabled.load(std::memory_order_relaxed); }

bool ToggleInterception() {
    const bool on = !g_interceptEnabled.load(std::memory_order_relaxed);
    g_interceptEnabled.store(on, std::memory_order_relaxed);
    Log::Write("TEXT", on ? "painter interception ENABLED (diagnostic toggle)"
                          : "painter interception DISABLED (diagnostic toggle) -- row text will not be captured");
    Speech::Output(on ? L"Menu text capture on" : L"Menu text capture off", true);
    return on;
}

// SNAPSHOT UNDER THE LOCK, LOG OUTSIDE IT. This used to hold g_mutex across ~257 Log::Write calls
// -- and g_mutex is the lock the game's own menu paint needs on every row (CellWrapper) and every
// string (Capture), so a dump stalled the paint for as long as the logging took. Even as a
// deliberate diagnostic that is unacceptable: a debug facility must never be able to slow the game.
// The copy costs one allocation per line, paid on the caller's thread, off the lock.
void DumpRingToLog(const char* reason) {
    struct Line { bool imm; std::wstring text; };
    std::vector<Line>                     framing;
    std::vector<std::pair<int, std::wstring>> items;
    void*  paintOwner = nullptr;
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        paintOwner = g_paintOwner;
        count      = g_count;
        framing.reserve(g_count);
        for (size_t k = 0; k < g_count; ++k) {
            const RingEntry& e = g_ring[(g_head + RING_MAX - g_count + k) % RING_MAX];
            framing.push_back(Line{ e.imm, e.text });
        }
        auto o = g_itemsByOwner.find(g_paintOwner);
        if (o != g_itemsByOwner.end()) {
            items.reserve(o->second.size());
            for (const auto& kv : o->second) items.emplace_back(kv.first, JoinFields(kv.second));
        }
    }

    char hdr[176];
    snprintf(hdr, sizeof(hdr), "dump (%s): %zu framing strings, paintOwner=%p %zu item slots",
             reason ? reason : "", count, paintOwner, items.size());
    Log::Write("TEXT", hdr);
    for (const auto& e : framing) {
        char utf8[400];
        Log::ToUtf8(e.text, utf8, sizeof(utf8));
        char line[512];
        snprintf(line, sizeof(line), "  framing[%s] \"%s\"", e.imm ? "imm" : "obj", utf8);
        Log::Write("TEXT", line);
    }
    for (const auto& kv : items) {
        char utf8[400];
        Log::ToUtf8(kv.second, utf8, sizeof(utf8));
        char line[512];
        snprintf(line, sizeof(line), "  item[%d] \"%s\"", kv.first, utf8);
        Log::Write("TEXT", line);
    }
}

} // namespace TextCapture

#include "ui/text_capture.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"

#include <Windows.h>
#include <array>
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

Pfn_TextDraw s_origImm  = nullptr;
Pfn_TextDraw s_origObj1 = nullptr;
Pfn_TextDraw s_origObj2 = nullptr;
Pfn_Painter  s_origPainter = nullptr;
Pfn_Resolve  s_origResolve = nullptr;
Pfn_DescSet  s_origDescSet = nullptr;

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
// button labels (ids 1000/1001). Persists for the session.
std::unordered_map<int, std::wstring> g_idCache;

// Focused-item description (FUN_00291d80), gated to the current focus generation
// so the `i` hotkey never speaks a previous item's description.
std::wstring g_helpText;
uint32_t g_helpGen = 0;
uint32_t g_helpTextGen = 0xffffffffu;   // != g_helpGen until a description is set for a focus

TextCapture::MenuPaintedCallback g_paintedCb = nullptr;

bool g_initialized = false;

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
    const uint8_t* strp = nullptr;
    if (!ReadStrPtr(structPtr, &strp) || !strp) return;
    std::wstring text = GameText::Decode(strp);
    if (!GameText::IsMostlyPrintable(text)) return;

    std::lock_guard<std::mutex> lk(g_mutex);
    if (listCapable && g_curIdx >= 0 && g_paintOwner) {
        g_itemsByOwner[g_paintOwner][g_curIdx].push_back(std::move(text));  // list row: read on focus
    } else {
        g_ring[g_head] = RingEntry{ listCapable, std::move(text) };          // framing (diagnostics)
        g_head = (g_head + 1) % RING_MAX;
        if (g_count < RING_MAX) ++g_count;
    }
}

void HookImm(void* p1)  { Capture(p1, /*listCapable=*/true);  if (s_origImm)  s_origImm(p1);  }
void HookObj1(void* p1) { Capture(p1, /*listCapable=*/false); if (s_origObj1) s_origObj1(p1); }
void HookObj2(void* p1) { Capture(p1, /*listCapable=*/false); if (s_origObj2) s_origObj2(p1); }

const uint8_t* HookResolve(int id) {
    const uint8_t* ret = s_origResolve ? s_origResolve(id) : nullptr;
    if (ret && (id == 1000 || id == 1001)) {
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

// ---- per-item interception ---------------------------------------------------
// Called by the game's painter in place of the real callback (we swapped the
// pointer). Attributes this row's draws to `index`, then runs the real callback.
int64_t CellWrapper(void* ctx, int64_t rowDrawCtx, void* geom, int64_t indexArg) {
    int index = static_cast<int>(indexArg);
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_curIdx = index;              // sticky: stays set through the post-callback glyph draw
        if (g_paintOwner) g_itemsByOwner[g_paintOwner][index].clear();  // fresh for this paint
    }
    Pfn_Cell real = g_realCb;          // read outside the lock (single game thread)
    return real ? real(ctx, rowDrawCtx, geom, indexArg) : 0;
}

// Runs after the full paint: reset state, then notify the reader that `owner`'s
// item map is now populated (drives the menu-entry focus replay).
void FinishPaint(void* owner) {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_curIdx = -1;
        g_intercepting = false;
        if (g_itemsByOwner.size() > OWNER_MAP_CAP) {
            auto keep = g_itemsByOwner.find(owner);
            std::unordered_map<int, std::vector<std::wstring>> saved;
            if (keep != g_itemsByOwner.end()) saved = std::move(keep->second);
            g_itemsByOwner.clear();
            if (!saved.empty()) g_itemsByOwner[owner] = std::move(saved);
        }
    }
    if (g_paintedCb) g_paintedCb(owner);
}

void HookedPainter(void* param_1, int64_t param_2, void* subwidget) {
    void* owner = nullptr; void** slot = nullptr; void* realCb = nullptr;
    bool intercept = false;
    if (subwidget && ReadSubwidget(subwidget, &owner, &slot, &realCb) && realCb) {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_intercepting) {
            g_intercepting = true;
            g_realCb = reinterpret_cast<Pfn_Cell>(realCb);
            g_paintOwner = owner;
            intercept = true;
        }
    }
    if (intercept) WriteSlot(slot, reinterpret_cast<void*>(&CellWrapper));  // swap in our wrapper
    if (s_origPainter) s_origPainter(param_1, param_2, subwidget);
    if (intercept) {
        WriteSlot(slot, realCb);   // restore the game's callback
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
    g_initialized = true;
    Log::Write("TEXT", ok
        ? "TextCapture initialized (codec draws + FUN_002d28e0 per-item index map + id-string cache)."
        : "TextCapture: one or more hooks failed to install — see Hooks log.");
    return ok;
}

void Shutdown() {
    if (!g_initialized) return;
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

void DumpRingToLog(const char* reason) {
    std::lock_guard<std::mutex> lk(g_mutex);
    size_t items = 0;
    auto o = g_itemsByOwner.find(g_paintOwner);
    if (o != g_itemsByOwner.end()) items = o->second.size();
    char hdr[176];
    snprintf(hdr, sizeof(hdr), "dump (%s): %zu framing strings, paintOwner=%p %zu item slots",
             reason ? reason : "", g_count, g_paintOwner, items);
    Log::Write("TEXT", hdr);
    for (size_t k = 0; k < g_count; ++k) {
        size_t idx = (g_head + RING_MAX - g_count + k) % RING_MAX;
        const RingEntry& e = g_ring[idx];
        char utf8[400] = {};
        WideCharToMultiByte(CP_UTF8, 0, e.text.c_str(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
        char line[512];
        snprintf(line, sizeof(line), "  framing[%s] \"%s\"", e.imm ? "imm" : "obj", utf8);
        Log::Write("TEXT", line);
    }
    if (o != g_itemsByOwner.end()) {
        for (const auto& kv : o->second) {
            std::wstring joined = JoinFields(kv.second);
            char utf8[400] = {};
            WideCharToMultiByte(CP_UTF8, 0, joined.c_str(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
            char line[512];
            snprintf(line, sizeof(line), "  item[%d] \"%s\"", kv.first, utf8);
            Log::Write("TEXT", line);
        }
    }
}

} // namespace TextCapture

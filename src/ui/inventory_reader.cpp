#include "ui/inventory_reader.h"
#include "ui/shop_reader.h"
#include "ui/primer_reader.h"
#include "ui/dialogue_reader.h"
#include "ui/text_capture.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "speech/speech.h"
#include "core/logger.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

using MemRead::PtrAt;
using MemRead::SafeReadU8;
using MemRead::SafeReadU16;
using MemRead::SafeReadU32;

// ---- hooks -------------------------------------------------------------------------------------
constexpr uint32_t RVA_REFRESH     = 0x4455F0;  // FUN_005655f0(container, tabState, filter)
constexpr uint32_t RVA_RESOLVE_MSG = 0x1D9860;  // FUN_002f9860(id) -> codec ptr (menu message books)

// ---- container offsets (one tabbed family: items lists, equipment list, shop) --------------------
constexpr uint32_t OFF_C_SCROLL = 0xD8;   // -> scroll/cursor grid widget
constexpr uint32_t OFF_C_ROWS   = 0xE0;   // -> row array (null when the list is empty)
constexpr uint32_t OFF_C_TABLE  = 0xE8;   // -> tab table
constexpr uint32_t OFF_C_TABST  = 0xF0;   // per-tab saved state, stride 8; srcIdx (s8) at +6
constexpr uint32_t OFF_C_180    = 0x180;  // bits[20:16] tab COUNT, bits[25:21] tab INDEX
constexpr uint32_t OFF_S_COUNT  = 0xE8;   // scroll widget: built row count (u16)
constexpr uint32_t OFF_TAB_TEXT = 0x08;   // tab table entry: category name text id (u32)

// ---- row record (stride 0x20) -------------------------------------------------------------------
constexpr uint32_t ROW_STRIDE = 0x20;
constexpr uint32_t OFF_R_NAME = 0x00;   // codec string ptr (built by FUN_002b58b0)
constexpr uint32_t OFF_R_ID   = 0x08;   // item id (u16); 0xFFFF = empty/mid-rebuild. 0x0 is VALID.
constexpr uint32_t OFF_R_QTY  = 0x0E;   // owned count (u16)
// (+0x0C exists but its meaning is unknown -- see the header. Never read, never spoken.)

constexpr int MAX_ROWS = 4096;

typedef void (*Pfn_Refresh)(void* container, void* tabState, int filter);
Pfn_Refresh s_origRefresh = nullptr;
typedef const uint8_t* (*Pfn_ResolveMsg)(int);   // FUN_002f9860(id)

// Set when a category was just announced, so the item that the game's own re-fired 0x8000 delivers
// microseconds later is QUEUED behind it instead of interrupting it. NOT a dedup -- nothing is
// suppressed; both utterances are spoken, in order. Consumed by the very next TryFocus and cleared
// unconditionally, so it can never leak into an unrelated announcement. Game thread only.
bool g_queueNextItem = false;

// Set alongside g_queueNextItem by OnCategoryRefresh -- the SAME event, but carrying WHICH surface
// the category was announced for. FUN_005655f0 serves the party-menu lists, the equipment list AND
// the shop, so a consumer outside this file (shop_reader) must be able to prove a pending category
// is its own before queueing behind it. The party-menu handshake above is deliberately left exactly
// as it is. Game thread only.
void* g_categoryOwner = nullptr;

// Decode an item/category name codec. Same two-step as shop_reader: these names come from the
// shared string pool (FUN_002b58b0), which decodes directly; fall back to the variant-prefix skip
// if the direct read is not printable. Empty on garbage/stale pointers -> the caller stays silent.
std::wstring DecodeName(const uint8_t* codec) {
    if (!codec) return std::wstring();
    std::wstring s = GameText::Decode(codec, 320);
    if (GameText::IsMostlyPrintable(s)) return s;
    s = GameText::Decode(GameText::SkipVariantPrefix(codec), 320);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// FUN_002f9860(id) -> codec ptr. Game call, pure message-book lookup (bank = id/1000, entry =
// id%1000) -- the same call FUN_005655f0 itself makes a few instructions later on this thread.
// Kept in its own function so no C++ object is live inside the __try scope (SEH rule).
const uint8_t* ResolveMsgCodec(int id) {
    auto fn = reinterpret_cast<Pfn_ResolveMsg>(Hooks::ResolveRva(RVA_RESOLVE_MSG));
    if (!fn) return nullptr;
    __try { return fn(id); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

struct ListInfo {
    void* rows;
    int   count;
};

// Claim a window by SHAPE, never by class: a member of this family carries a row array, a scroll
// widget, a tab table and a sane row count. Four classes were observed and one class serves several
// screens, so a class table would be both incomplete and wrong (see the header).
bool ReadList(void* w, ListInfo* out) {
    if (!w) return false;
    void* rows = PtrAt(w, OFF_C_ROWS);
    if (!rows) return false;                       // null => EMPTY category; see IsEmptyCategory
    void* scroll = PtrAt(w, OFF_C_SCROLL);
    if (!scroll) return false;
    if (!PtrAt(w, OFF_C_TABLE)) return false;
    uint16_t n = 0;
    if (!SafeReadU16(scroll, OFF_S_COUNT, &n)) return false;
    if (n == 0 || n > MAX_ROWS) return false;
    out->rows  = rows;
    out->count = static_cast<int>(n);
    return true;
}

// An EMPTY category -- SHIELDS with no shield owned -- is still OUR surface, and the ONLY thing that
// separates it from a populated one is a null row array. The game does this deliberately:
// FUN_0057cf20:253-257 frees its buffer and returns NULL when it builds zero rows, so
// FUN_005655f0:42 stores null into +0xE0; then FUN_005655f0:49-52 CLAMPS the count it hands the
// scroll widget from 0 to 1. So the widget still reports "one row at index 0", and the generic
// painted-cell path happily reads that cell -- getting whatever the painter last drew there, which
// is the PREVIOUS category's row. Measured: "SHIELDS" then "Leather Cap" (a helm), Session 89.
//
// Same shape test ReadList uses, minus the row array it is diagnosing the absence of.
bool IsEmptyCategory(void* w) {
    if (!w) return false;
    if (PtrAt(w, OFF_C_ROWS)) return false;        // has rows -> populated, not this case
    if (!PtrAt(w, OFF_C_SCROLL)) return false;
    if (!PtrAt(w, OFF_C_TABLE)) return false;
    return true;
}

// Every early exit from OnCategoryRefresh below used to be a bare `return` -- SIX of them, none of
// them logged. A category that failed to announce therefore left NOTHING behind, and the surface
// could not be diagnosed at all: the offhand/shield report of 2026-08-11 ("selecting a shield doesn't
// read them") could not even be told apart from "the mod never reached that screen". This names WHICH
// gate closed and carries the values that separate the candidates, in one line shape.
//
// This is a silent-if-wrong reader with no diagnostic, which is the bug choice_reader.cpp already
// learned the hard way (see LogReject in dialogue_reader.cpp for the same pattern).
//
// LOG-ONLY, and the (owner, gate) cache is log volume control, not speech dedup -- FUN_005655f0 is
// event-driven (screen open + tab change), never per-frame, so this cannot flood; the cache only
// stops a held tab key from repeating one identical line. Deliberately a CACHE and not a counter cap:
// a cap gets spent early in a session and is then dead for the one rejection that matters hours in,
// which is exactly what dialogue_reader.cpp:116-119 had to be repaired for.
void NoteCategoryGate(void* w, const char* gate, uint32_t raw180, int tabIdx, int tabCount,
                      int src, uint32_t textId) {
    static void*       s_lastOwner = nullptr;
    static const char* s_lastGate  = nullptr;
    if (w == s_lastOwner && gate == s_lastGate) return;   // same surface, same gate -> already said
    s_lastOwner = w;
    s_lastGate  = gate;                                   // literal pointers: identity compare is exact
    char m[256];
    snprintf(m, sizeof(m),
             "category declined at %s: owner=%p rows=%p scroll=%p table=%p raw180=0x%08X "
             "tab=%d/%d src=%d textId=%u",
             gate, w, PtrAt(w, OFF_C_ROWS), PtrAt(w, OFF_C_SCROLL), PtrAt(w, OFF_C_TABLE),
             raw180, tabIdx, tabCount, src, textId);
    Log::Write("INV", m);
}

// The four silent exits AFTER ReadList has succeeded. Together with NoteCategoryGate above and the
// `list declined` line in TryFocus, every path by which this reader can decline to speak now names
// itself -- which is what the 2026-08-11 offhand report needed and did not have.
//
// Worth stating plainly because it is the shape of the whole defect: `category: "SHIELDS"` announced
// three times and NOT ONE `item:` line followed, while the weapon slot announced its first candidate
// in the SAME millisecond as its category. Same window class, same owner, same code path -- so the
// question was never "which reader owns this surface", it was "which of these five returns fired".
void NoteRowGate(void* owner, const char* gate, int index, int count, unsigned id) {
    static void*       s_lastOwner = nullptr;
    static const char* s_lastGate  = nullptr;
    if (owner == s_lastOwner && gate == s_lastGate) return;
    s_lastOwner = owner;
    s_lastGate  = gate;
    char m[192];
    snprintf(m, sizeof(m), "row declined at %s: owner=%p index=%d count=%d id=0x%04X",
             gate, owner, index, count, id);
    Log::Write("INV", m);
}

// FUN_005655f0(container, ...): announce the category this refresh is switching to. Runs BEFORE the
// original, so the name is spoken ahead of the item the original's FUN_002d47c0 re-fire delivers.
void OnCategoryRefresh(void* w) {
    STALL_SCOPE("InventoryReader::OnCategoryRefresh");
    if (!w) return;

    uint32_t raw180 = 0;
    if (!SafeReadU32(w, OFF_C_180, &raw180)) {
        NoteCategoryGate(w, "tab-bitfield unreadable", 0, -1, -1, -1, 0);
        return;
    }
    const int32_t v = static_cast<int32_t>(raw180);
    const int tabIdx   = (v << 6)  >> 27;          // bits[25:21], sign-extended exactly as the game does
    const int tabCount = (v << 11) >> 27;          // bits[20:16]
    if (tabCount < 1 || tabIdx < 0 || tabIdx >= tabCount) {
        NoteCategoryGate(w, "tab index out of range", raw180, tabIdx, tabCount, -1, 0);
        return;
    }

    void* table = PtrAt(w, OFF_C_TABLE);
    if (!table) {
        NoteCategoryGate(w, "no tab table", raw180, tabIdx, tabCount, -1, 0);
        return;
    }

    // The tab's entry in the table is indirected through the per-tab saved state (FUN_005655f0:19).
    uint8_t srcRaw = 0;
    if (!SafeReadU8(w, OFF_C_TABST + static_cast<uint32_t>(tabIdx) * 8 + 6, &srcRaw)) {
        NoteCategoryGate(w, "per-tab state unreadable", raw180, tabIdx, tabCount, -1, 0);
        return;
    }
    int src = static_cast<int8_t>(srcRaw);
    if (src < 0) src = 0;                          // the game clamps -1 to 0 the same way

    uint32_t textId = 0;
    if (!SafeReadU32(table, static_cast<uint32_t>(src) * 8 + OFF_TAB_TEXT, &textId)) {
        NoteCategoryGate(w, "tab text id unreadable", raw180, tabIdx, tabCount, src, 0);
        return;
    }

    // Prefer the id cache TextCapture already fills from the game's own resolver -- no game call on
    // the common path. Fall back to the getter the first time an id is seen this session.
    std::wstring name = TextCapture::StringById(static_cast<int>(textId));
    if (name.empty()) name = DecodeName(ResolveMsgCodec(static_cast<int>(textId)));
    if (name.empty()) {                            // no readable text -> silent, never fabricated
        NoteCategoryGate(w, "category name did not decode", raw180, tabIdx, tabCount, src, textId);
        return;
    }

    Log::WriteW("INV", "category:", w, name);
    Speech::Output(name, /*interrupt=*/true);
    g_queueNextItem = true;                        // let the item that follows queue behind this
    g_categoryOwner = w;                           // ...and let shop_reader prove that item is ITS row
}

// FUN_005655f0 is the unified list refresh, and the game runs it on screen OPEN as well as on every
// category change -- which makes it the closest thing the mod has to "a tabbed list screen just
// appeared". Two readers outside this file need that edge and neither had an event of its own:
//
//   ShopReader   -- had no shop-open hook at all (only the cursor-move handler FUN_0056e5d0), so
//                   entering a shop said nothing until a direction key was pressed;
//   DialogueReader -- its page guard is re-armed by an end-of-message latch that is never observed
//                   when a list screen tears the box down, so the clerk went quiet on re-entry.
//
// Both are called unconditionally and both stand down on their own if the surface is not theirs.
void HookedRefresh(void* container, void* tabState, int filter) {
    // BEFORE the original: whatever conversation was on screen is being covered or torn down right
    // now, and its page keys must not survive to silence it when it comes back.
    DialogueReader::ForgetLivePages();

    OnCategoryRefresh(container);
    if (s_origRefresh) s_origRefresh(container, tabState, filter);

    // AFTER the original: this is the first instant the rows exist (FUN_0056e410:54 copies
    // container+0xE0 into panel+0xC8), so a shop can finally read the row it opened on.
    ShopReader::OnListRefreshed(container);
}

} // namespace

namespace InventoryReader {

bool ConsumeCategoryAnnounce(void* owner) {
    if (!owner || owner != g_categoryOwner) return false;
    g_categoryOwner = nullptr;
    g_queueNextItem = false;      // this announcement was `owner`'s, so it is not a party list's
    return true;
}

bool TryFocus(void* owner, int index) {
    STALL_SCOPE("InventoryReader::TryFocus");
    if (!owner || index < 0) return false;

    // The shop's container is the same shape, but shop_reader already speaks those rows with their
    // price and inventory. Without this both readers would announce the same row.
    if (ShopReader::OwnsSurface(owner)) return false;

    // ...and the same for the Clan Primer. IsEmptyCategory below matches on SHAPE (null row array,
    // live scroll, live table) and the primer's entry list has exactly that shape, so this reader was
    // claiming Traveller's Tips and then deliberately saying nothing -- the log line
    // "empty category -- claimed and SILENT" was printing on a list that was not empty at all and was
    // not even ours. A shape test cannot tell two structs apart; only identity can.
    if (PrimerReader::OwnsSurface(owner)) return false;

    ListInfo li{};
    if (!ReadList(owner, &li)) {
        // Not one of ours, or a shape we cannot read -> let the generic path try, as before.
        if (!IsEmptyCategory(owner)) {
            // THE LAST SILENT DECLINE ON THIS SURFACE, and the one the 2026-08-11 offhand log
            // narrowed the shield defect down to. That log proves the mod REACHES the screen --
            // `category: owner=…BF63DC0 "SHIELDS"` is announced, and the description decoder reads
            // each shield's stats correctly -- but no `item:` line ever follows, and neither the
            // `empty category` claim nor any `category declined at` gate fired. So the row read is
            // failing HERE, and this was the one exit with nothing to say about why.
            //
            // The three pointers are the whole answer: `ReadList` wants all of rows/scroll/table,
            // `IsEmptyCategory` wants rows NULL but the other two live. A shield list that reads
            // rows=null scroll=null means the container is mid-rebuild; rows=live with a bad count
            // means `FUN_0057cf20`'s two-pool `0x41` branch built something this reader mis-sizes.
            // Same log-only (owner, gate) cache discipline as NoteCategoryGate above.
            static void* s_lastDeclined = nullptr;
            if (owner != s_lastDeclined) {
                s_lastDeclined = owner;
                void*    scroll = PtrAt(owner, OFF_C_SCROLL);
                uint16_t n = 0;
                if (scroll) SafeReadU16(scroll, OFF_S_COUNT, &n);
                char m[208];
                snprintf(m, sizeof(m),
                         "list declined (not ours / unreadable shape): owner=%p index=%d rows=%p "
                         "scroll=%p table=%p count=%u",
                         owner, index, PtrAt(owner, OFF_C_ROWS), scroll,
                         PtrAt(owner, OFF_C_TABLE), static_cast<unsigned>(n));
                Log::Write("INV", m);
            }
            return false;
        }
        // An empty category IS ours. CLAIM it and say NOTHING: returning false here would hand the
        // cell to the generic painted-cell path, which reads the previous category's stale paint.
        // Nothing to announce is not a reason to invent "empty" -- be silent (never-speak-filler).
        // The name itself was already spoken by OnCategoryRefresh, so the switch is still audible.
        ConsumeCategoryAnnounce(owner);            // nothing follows it; do not leave the flag pending
        // Carry the owner and index: this claim is the mod's ONLY path to deliberate silence on this
        // surface, so if it ever fires on a POPULATED list it is indistinguishable from the reader
        // being broken. FUN_005655f0 nulls +0xE0 at the top of every refresh before rebuilding it, so
        // a focus delivered inside that window reads null on a list that is about to have rows --
        // pairing this line with OnCategoryRefresh's owner is what tells the two apart.
        char m[160];
        snprintf(m, sizeof(m),
                 "empty category -- claimed and SILENT (row array null, scroll count clamped to 1): "
                 "owner=%p index=%d", owner, index);
        Log::Write("INV", m);
        return true;
    }
    if (index >= li.count) {
        NoteRowGate(owner, "index past row count", index, li.count, 0xFFFF);
        return false;
    }

    void* row = reinterpret_cast<char*>(li.rows) + static_cast<size_t>(index) * ROW_STRIDE;

    uint16_t id = 0xFFFF;
    if (!SafeReadU16(row, OFF_R_ID, &id)) {
        NoteRowGate(owner, "row id unreadable", index, li.count, 0xFFFF);
        return false;
    }
    if (id == 0xFFFF) {                            // empty / mid-rebuild -> let the generic path try
        NoteRowGate(owner, "row id 0xFFFF (empty / mid-rebuild)", index, li.count, id);
        return false;
    }

    std::wstring name = DecodeName(reinterpret_cast<const uint8_t*>(PtrAt(row, OFF_R_NAME)));
    if (name.empty()) {                            // unreadable -> fall through rather than invent
        NoteRowGate(owner, "name did not decode", index, li.count, id);
        return false;
    }

    // The count is spoken only above 1: a row exists only if you own at least one, so a bare name
    // already means exactly one. This is what keeps Key Items and Magicks from reading "... 1".
    uint16_t qty = 0;
    SafeReadU16(row, OFF_R_QTY, &qty);
    std::wstring line = name;
    if (qty > 1) { line += L" "; line += std::to_wstring(qty); }

    const bool interrupt = !g_queueNextItem;       // queue behind a category we just announced
    g_queueNextItem = false;

    Log::WriteW("INV", "item:", owner, line);
    Speech::Output(line, interrupt);
    return true;
}

bool Init() {
    const bool ok = Hooks::InstallTyped(RVA_REFRESH, &HookedRefresh, &s_origRefresh);
    Log::Write("INV", ok
        ? "InventoryReader initialized (item name+quantity on focus; category name on open/switch)"
        : "InventoryReader: the FUN_005655f0 hook FAILED to install -- see Hooks log.");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_REFRESH);
    s_origRefresh   = nullptr;
    g_queueNextItem = false;
    g_categoryOwner = nullptr;
}

} // namespace InventoryReader

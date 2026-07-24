#include "ui/inventory_reader.h"
#include "ui/shop_reader.h"
#include "ui/text_capture.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "speech/speech.h"
#include "core/logger.h"

#include <Windows.h>
#include <cstdint>
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
    if (!rows) return false;                       // null => empty list (does not occur in play)
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

// FUN_005655f0(container, ...): announce the category this refresh is switching to. Runs BEFORE the
// original, so the name is spoken ahead of the item the original's FUN_002d47c0 re-fire delivers.
void OnCategoryRefresh(void* w) {
    STALL_SCOPE("InventoryReader::OnCategoryRefresh");
    if (!w) return;

    uint32_t raw180 = 0;
    if (!SafeReadU32(w, OFF_C_180, &raw180)) return;
    const int32_t v = static_cast<int32_t>(raw180);
    const int tabIdx   = (v << 6)  >> 27;          // bits[25:21], sign-extended exactly as the game does
    const int tabCount = (v << 11) >> 27;          // bits[20:16]
    if (tabCount < 1 || tabIdx < 0 || tabIdx >= tabCount) return;

    void* table = PtrAt(w, OFF_C_TABLE);
    if (!table) return;

    // The tab's entry in the table is indirected through the per-tab saved state (FUN_005655f0:19).
    uint8_t srcRaw = 0;
    if (!SafeReadU8(w, OFF_C_TABST + static_cast<uint32_t>(tabIdx) * 8 + 6, &srcRaw)) return;
    int src = static_cast<int8_t>(srcRaw);
    if (src < 0) src = 0;                          // the game clamps -1 to 0 the same way

    uint32_t textId = 0;
    if (!SafeReadU32(table, static_cast<uint32_t>(src) * 8 + OFF_TAB_TEXT, &textId)) return;

    // Prefer the id cache TextCapture already fills from the game's own resolver -- no game call on
    // the common path. Fall back to the getter the first time an id is seen this session.
    std::wstring name = TextCapture::StringById(static_cast<int>(textId));
    if (name.empty()) name = DecodeName(ResolveMsgCodec(static_cast<int>(textId)));
    if (name.empty()) return;                      // no readable text -> silent, never fabricated

    Log::WriteW("INV", "category:", w, name);
    Speech::Output(name, /*interrupt=*/true);
    g_queueNextItem = true;                        // let the item that follows queue behind this
}

void HookedRefresh(void* container, void* tabState, int filter) {
    OnCategoryRefresh(container);
    if (s_origRefresh) s_origRefresh(container, tabState, filter);
}

} // namespace

namespace InventoryReader {

bool TryFocus(void* owner, int index) {
    STALL_SCOPE("InventoryReader::TryFocus");
    if (!owner || index < 0) return false;

    // The shop's container is the same shape, but shop_reader already speaks those rows with their
    // price and inventory. Without this both readers would announce the same row.
    if (ShopReader::OwnsSurface(owner)) return false;

    ListInfo li{};
    if (!ReadList(owner, &li)) return false;
    if (index >= li.count) return false;

    void* row = reinterpret_cast<char*>(li.rows) + static_cast<size_t>(index) * ROW_STRIDE;

    uint16_t id = 0xFFFF;
    if (!SafeReadU16(row, OFF_R_ID, &id)) return false;
    if (id == 0xFFFF) return false;                // empty / mid-rebuild -> let the generic path try

    std::wstring name = DecodeName(reinterpret_cast<const uint8_t*>(PtrAt(row, OFF_R_NAME)));
    if (name.empty()) return false;                // unreadable -> fall through rather than invent

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
}

} // namespace InventoryReader

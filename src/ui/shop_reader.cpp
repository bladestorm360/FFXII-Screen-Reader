#include "ui/shop_reader.h"
#include "ui/inventory_reader.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "speech/speech.h"
#include "speech/phrasebook.h"
#include "core/logger.h"

#include <cstdint>
#include <string>

namespace {

using MemRead::Obj0;
using MemRead::PtrAt;
using MemRead::SafeReadU8;
using MemRead::SafeReadU16;
using MemRead::SafeReadU32;
using MemRead::SafeReadS16;

// ---- hook + class identities (all CONFIRMED live, Session 68 probe_shop) ------------------------
constexpr uint32_t RVA_HILITE    = 0x44E5D0;  // FUN_0056e5d0(container, mode) -- highlight/refresh handler
constexpr uint32_t RVA_CONTAINER = 0x44E140;  // FUN_0056e140 -- shop list container class (obj[0])
constexpr uint32_t RVA_PANEL     = 0x44D370;  // FUN_0056d370 -- item panel class (obj[0])

// ---- container / panel offsets ------------------------------------------------------------------
constexpr uint32_t OFF_C_PANEL  = 0xC0;   // container+0xC0 -> item panel
constexpr uint32_t OFF_C_SCROLL = 0xD8;   // container+0xD8 -> scroll/cursor grid widget
constexpr uint32_t OFF_P_ROWS   = 0xC8;   // panel+0xC8 -> row array

// ---- scroll grid: highlighted index = ed + (f4 + f2)*ec + ee  (from FUN_0056e5d0:25-30) ---------
constexpr uint32_t OFF_S_EC = 0xEC;  // u8  columns
constexpr uint32_t OFF_S_ED = 0xED;  // s8
constexpr uint32_t OFF_S_EE = 0xEE;  // s8
constexpr uint32_t OFF_S_F2 = 0xF2;  // s16
constexpr uint32_t OFF_S_F4 = 0xF4;  // s16

// ---- row record (stride 0x20) -------------------------------------------------------------------
constexpr uint32_t ROW_STRIDE = 0x20;
constexpr uint32_t OFF_R_NAME  = 0x00;  // codec string ptr (built by FUN_002b58b0)
constexpr uint32_t OFF_R_ID    = 0x08;  // item id (u16); 0xFFFF on an empty/mid-rebuild row
constexpr uint32_t OFF_R_INV   = 0x0E;  // owned count (u16) -- the "INVENTORY" column
constexpr uint32_t OFF_R_PRICE = 0x10;  // price (u32 & 0x7FFFFFFF); sell price is already halved here

// ---- quantity selector (panel FUN_0056d370, active after you pick an item) -----------------------
constexpr uint32_t OFF_P_QTY  = 0xDC;   // u16 quantity being bought/sold
constexpr uint32_t OFF_P_E4   = 0xE4;   // u32 mode flags
constexpr uint32_t OFF_P_SEL  = 0xD0;   // ptr -> selected item row (id@+8, price@+0x10)
constexpr uint32_t QTY_MODE_BIT = 0x2;        // panel+0xE4 bit1 -> quantity selector is active
constexpr uint32_t STEP_10X_BIT = 0x400000;   // panel+0xE4 bit22 -> +10 step (else +1); Left-arrow toggle.
                                              // PROVISIONAL: one live sample + user-described behavior --
                                              // confirmed by hearing "1x"/"10x" in play, easy to flip.

typedef void (*Pfn_Hilite)(void* container, uint32_t mode);
Pfn_Hilite s_origHilite = nullptr;
typedef uint64_t (*Pfn_Panel)(void* panel, void* msg);   // FUN_0056d370(panel, msg*) -- item panel proc
Pfn_Panel s_origPanel = nullptr;

// Change-guard for the no-dedup exception (see header). Game-thread only; no lock needed.
void* g_lastContainer = nullptr;
int   g_lastItemId    = -1;

// Quantity-selector state (game-thread only). The panel proc is per-frame, so we speak ONLY on a real
// change -- the sanctioned no-dedup exception, guarding FUN_0056d370.
bool g_qtyActive = false;
int  g_lastQty   = -1;
int  g_lastStep  = -1;

// Decode a shop item-name codec. Shop names come from the shared string pool (FUN_002b58b0); the live
// probe decoded them fine without the variant-prefix skip, but fall back to the skip form if the direct
// decode is not printable. Returns empty on garbage/stale pointer -> caller stays silent.
std::wstring DecodeName(const uint8_t* codec) {
    if (!codec) return std::wstring();
    std::wstring s = GameText::Decode(codec, 320);
    if (GameText::IsMostlyPrintable(s)) return s;
    s = GameText::Decode(GameText::SkipVariantPrefix(codec), 320);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

void OnShopHighlight(void* container) {
    STALL_SCOPE("ShopReader::OnShopHighlight");
    if (!container || Obj0(container) != Hooks::ResolveRva(RVA_CONTAINER)) return;
    void* panel = PtrAt(container, OFF_C_PANEL);
    if (!panel || Obj0(panel) != Hooks::ResolveRva(RVA_PANEL)) return;
    void* scroll = PtrAt(container, OFF_C_SCROLL);
    void* rows   = PtrAt(panel, OFF_P_ROWS);
    if (!scroll || !rows) return;

    // Highlighted index, exactly as the game computes it from the scroll grid.
    uint8_t ec = 0, ed = 0, ee = 0; int16_t f2 = 0, f4 = 0;
    if (!SafeReadU8(scroll, OFF_S_EC, &ec) || !SafeReadU8(scroll, OFF_S_ED, &ed) ||
        !SafeReadU8(scroll, OFF_S_EE, &ee) || !SafeReadS16(scroll, OFF_S_F2, &f2) ||
        !SafeReadS16(scroll, OFF_S_F4, &f4)) return;
    const int idx = static_cast<int8_t>(ed) + (static_cast<int>(f4) + static_cast<int>(f2)) * ec
                  + static_cast<int8_t>(ee);
    if (idx < 0 || idx > 4096) return;
    void* row = reinterpret_cast<char*>(rows) + static_cast<size_t>(idx) * ROW_STRIDE;

    uint16_t itemId = 0xFFFF;
    SafeReadU16(row, OFF_R_ID, &itemId);
    if (itemId == 0xFFFF) return;                        // empty / mid-rebuild refresh -> silent

    const uint8_t* codec = reinterpret_cast<const uint8_t*>(PtrAt(row, OFF_R_NAME));
    std::wstring name = DecodeName(codec);
    if (name.empty()) return;                            // no readable name -> silent (never fabricate)

    // The shop's tabs route through the SAME FUN_005655f0 refresh the party-menu item lists use, so
    // InventoryReader has already spoken the category name by the time we get here: FUN_0056ded0:82
    // -> FUN_0056e410:50 -> FUN_005655f0 (its hook), then FUN_0056ded0:83 -> FUN_0056e5d0 -> us,
    // ~0.2 ms later. Queue behind that name instead of interrupting it -- the fix those lists got at
    // birth and this reader never did -- and drop the item guard so the new tab's row always speaks,
    // even in the rare case it repeats the previous tab's item id.
    const bool afterCategory = InventoryReader::ConsumeCategoryAnnounce(container);
    if (afterCategory) g_lastItemId = -1;

    // The one sanctioned change-check: FUN_0056e5d0 is a redraw handler (~2x per focus). Speak only when
    // the highlighted item actually changes. What re-arms it is OnListRefreshed below, on the game's own
    // list-rebuild event -- it clears BOTH halves of the key, so a shop re-entered into a recycled
    // container still speaks. NOT a dedup of distinct focuses.
    if (container == g_lastContainer && itemId == g_lastItemId) return;
    g_lastContainer = container;
    g_lastItemId    = itemId;

    uint32_t price = 0; SafeReadU32(row, OFF_R_PRICE, &price); price &= 0x7FFFFFFF;
    uint16_t inv   = 0; SafeReadU16(row, OFF_R_INV, &inv);

    std::wstring line = name + L", " + std::to_wstring(price) + Phrase::Get(Phrase::Id::GilSuffix) + L", "
                      + std::to_wstring(inv) + Phrase::Get(Phrase::Id::InInventorySuffix);
    Log::WriteW("SHOP", afterCategory ? "item (queued):" : "item:", container, line);
    Speech::Output(line, /*interrupt=*/!afterCategory);
}

// FUN_0056e5d0(container, mode): run the game's refresh first (so the highlight/rows are current), then
// read and announce the highlighted row.
void HookedHilite(void* container, uint32_t mode) {
    if (s_origHilite) s_origHilite(container, mode);
    OnShopHighlight(container);
}

void SpeakQty(uint16_t qty, uint32_t total) {
    std::wstring line = std::to_wstring(qty) + L", " + std::to_wstring(total) + Phrase::Get(Phrase::Id::GilSuffix);
    Log::WriteW("SHOP", "qty:", line);
    Speech::Output(line, /*interrupt=*/true);
}

// Quantity selector: after you pick an item, panel+0xE4 bit1 turns on and the panel holds the quantity
// (+0xDC) and the +1/+10 step (bit 0x400000). FUN_0056d370 is a per-frame proc, so this speaks ONLY on a
// real change -- entering the selector, the quantity changing, or the step toggling (Left arrow -> "1x" /
// "10x"). Resets on leaving so re-entering re-announces. NOT a dedup of distinct events.
void OnPanelQuantity(void* panel) {
    STALL_SCOPE("ShopReader::OnPanelQuantity");
    if (!panel || Obj0(panel) != Hooks::ResolveRva(RVA_PANEL)) return;
    uint32_t e4 = 0;
    if (!SafeReadU32(panel, OFF_P_E4, &e4)) return;
    if (!(e4 & QTY_MODE_BIT)) {                    // back on the item list -> reset; the item hook owns it
        g_qtyActive = false; g_lastQty = -1; g_lastStep = -1;
        return;
    }
    uint16_t qty = 0; SafeReadU16(panel, OFF_P_QTY, &qty);
    const int step = (e4 & STEP_10X_BIT) ? 10 : 1;
    uint32_t price = 0;
    if (void* selRow = PtrAt(panel, OFF_P_SEL)) { SafeReadU32(selRow, OFF_R_PRICE, &price); price &= 0x7FFFFFFF; }
    const uint32_t total = price * qty;

    if (!g_qtyActive) {                            // just opened the quantity selector
        g_qtyActive = true; g_lastQty = qty; g_lastStep = step;
        SpeakQty(qty, total);
        return;
    }
    if (qty != g_lastQty) {                        // quantity changed (Up/Down, +1 or +10)
        g_lastQty = qty; g_lastStep = step;
        SpeakQty(qty, total);
        return;
    }
    if (step != g_lastStep) {                      // pure step toggle (Left arrow) -> "1x" / "10x"
        g_lastStep = step;
        std::wstring line = std::to_wstring(step) + Phrase::Get(Phrase::Id::TimesSuffix);
        Log::WriteW("SHOP", "step:", panel, line);
        Speech::Output(line, /*interrupt=*/true);
        return;
    }
    // no change -> silent (per-frame guard: FUN_0056d370)
}

// FUN_0056d370(panel, msg*): the item panel proc, called per-frame. Run the game's handler first, then
// read the quantity state.
uint64_t HookedPanel(void* panel, void* msg) {
    const uint64_t ret = s_origPanel ? s_origPanel(panel, msg) : 0;
    OnPanelQuantity(panel);
    return ret;
}

} // namespace

namespace ShopReader {

bool OwnsSurface(void* w) {
    if (!w) return false;
    void* cls = Obj0(w);
    return cls == Hooks::ResolveRva(RVA_CONTAINER) || cls == Hooks::ResolveRva(RVA_PANEL);
}

// The borrowed open event -- see the header. InventoryReader calls this from its FUN_005655f0 hook
// AFTER the original has run, which is the first moment the rows exist: FUN_0056e410:54 copies
// container+0xE0 into panel+0xC8, and until it does OnShopHighlight bails on a null row array.
void OnListRefreshed(void* container) {
    if (!container || Obj0(container) != Hooks::ResolveRva(RVA_CONTAINER)) return;

    // BOTH guards reset, not just the item id. A list rebuild is a genuine new event, so whatever is
    // highlighted now is new information by definition and the redraw guard must not speak for it.
    // Clearing g_lastContainer is the half that was missing: it was only ever reset at DLL unload, so
    // a container the engine pooled and handed back at the SAME ADDRESS kept the old key and silenced
    // the first row of the next visit. That is not hypothetical -- menu_reader.cpp:102 records the
    // engine recycling pop-up window addresses, which is the same allocator behaviour, and the old
    // comment here claimed "the guard resets when the container changes" while testing an address
    // that had not changed.
    g_lastContainer = nullptr;
    g_lastItemId    = -1;
    OnShopHighlight(container);
}

bool Init() {
    bool ok  = Hooks::InstallTyped(RVA_HILITE, &HookedHilite, &s_origHilite);  // item name+price+inventory
    ok      &= Hooks::InstallTyped(RVA_PANEL,  &HookedPanel,  &s_origPanel);   // quantity selector + step
    Log::Write("SHOP", ok
        ? "ShopReader initialized (item name+price+inventory on highlight; quantity+total+step in the selector)"
        : "ShopReader: a hook FAILED to install -- see Hooks log.");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_PANEL);
    Hooks::Uninstall(RVA_HILITE);
    g_lastContainer = nullptr;
    g_lastItemId    = -1;
    g_qtyActive     = false;
    g_lastQty       = -1;
    g_lastStep      = -1;
}

} // namespace ShopReader

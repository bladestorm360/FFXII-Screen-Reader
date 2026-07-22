#include "ui/menu_state.h"
#include "core/hooks.h"
#include "core/mem_read.h"

namespace {

using MemRead::Obj0;
using MemRead::PtrAt;
using MemRead::SafeReadPtr;

// ---- surface-identity RVAs (abs = RVA + 0x120000) ----------------------------------------------
// The single home for "which class is this menu object". Everything below is an obj[0] handler
// pointer except the two globals, which hold live instances.
constexpr uint32_t RVA_TITLE_WINDOW = 0x29CE4C8;  // DAT_02aee4c8 — live title window (TitleReader owns it)
constexpr uint32_t RVA_CONFIRM_WND  = 0x121D40;   // FUN_00241d40 — confirm / quit pop-up
constexpr uint32_t RVA_FOCUS_WINDOW = 0x1F6EBC0;  // DAT_0208ebc0 — global input-focus window ptr

constexpr uint32_t RVA_CONFIG_CTRL   = 0x11FBE0;  // FUN_0023fbe0 — main config controller (rows at +0xE8)
constexpr uint32_t RVA_GFX_CTRL      = 0x11BD40;  // FUN_0023bd40 — Graphics sub-screen (rows at +0x4E0)
constexpr uint32_t RVA_CONTROLS_CTRL = 0x11CE10;  // FUN_0023ce10 — Controls sub-screen (rows at +0xD8)

constexpr uint32_t RVA_LICENSE_BOARD = 0x43CD40;  // FUN_0055cd40 — license-board node grid
constexpr uint32_t RVA_JOBSEL_RING   = 0x437DB0;  // FUN_00557db0 — job-select ring (12 jobs)

constexpr uint32_t RVA_CHOICE_POPUP  = 0x1ADF20;  // FUN_002cdf20 — generic Yes/No prompt
constexpr uint32_t RVA_MENU_CTX      = 0x1F7AC30; // DAT_0209ac30 (ptr) — menu context
constexpr uint32_t OFF_CTX_CHOICE    = 0x2E8;     // ctx+0x2e8 = the live Yes/No prompt
constexpr uint32_t OFF_POPUP_LIST    = 0xC0;      // prompt+0xc0 = the list widget it owns

constexpr uint32_t RVA_VALROW_E770 = 0x11E770;    // FUN_0023e770 — enum value row types 1/2/8
constexpr uint32_t RVA_VALROW_D6B0 = 0x11D6B0;    // FUN_0023d6b0 — enum value row type 3 (main screen)
constexpr uint32_t RVA_VALROW_DB40 = 0x11DB40;    // FUN_0023db40 — enum value row type 3 (Controls)
constexpr uint32_t RVA_VALROW_EBE0 = 0x11EBE0;    // FUN_0023ebe0 — slider/gauge value row (numeric)
constexpr uint32_t RVA_VALROW_B330 = 0x11B330;    // FUN_0023b330 — Graphics slider (gauge child)
constexpr uint32_t RVA_VALROW_B6F0 = 0x11B6F0;    // FUN_0023b6f0 — Graphics enum (fmt buf at row+0xCC)
constexpr uint32_t RVA_VALROW_C5C0 = 0x11C5C0;    // FUN_0023c5c0 — Controls key-binding row

// Active-instance globals (set on open, cleared on close) — used to skip reads when a controller
// isn't the live menu, so we never dereference a closed/freed menu's widgets.
constexpr uint32_t RVA_INST_FBE0 = 0x1F6E6D8;     // DAT_0208e6d8 — active FUN_0023fbe0 instance
constexpr uint32_t RVA_INST_CE10 = 0x1F6E6D0;     // DAT_0208e6d0 — active FUN_0023ce10 instance

// Row-array bases per controller class, stride shared.
constexpr uint32_t OFF_CTRL_ROWARR     = 0xE8;    // FUN_0023fbe0
constexpr uint32_t OFF_CTRL_ROWARR_GFX = 0x4E0;   // FUN_0023bd40
constexpr uint32_t OFF_CTRL_ROWARR_CTL = 0xD8;    // FUN_0023ce10
constexpr uint32_t ROW_STRIDE          = 0x18;

} // namespace

namespace MenuState {

bool IsTitleMenu(void* owner) {
    void* titleWin = nullptr;
    if (SafeReadPtr(reinterpret_cast<void*>(Hooks::ResolveRva(RVA_TITLE_WINDOW)), &titleWin))
        return owner != nullptr && owner == titleWin;
    return false;
}

bool IsConfirmWindow(void* owner) {
    return owner && Obj0(owner) == Hooks::ResolveRva(RVA_CONFIRM_WND);
}

bool IsConfigController(void* owner) {
    if (!owner) return false;
    void* cls = Obj0(owner);
    return cls == Hooks::ResolveRva(RVA_CONFIG_CTRL) ||
           cls == Hooks::ResolveRva(RVA_GFX_CTRL) ||
           cls == Hooks::ResolveRva(RVA_CONTROLS_CTRL);
}

bool IsGraphicsConfig(void* owner) {
    return owner && Obj0(owner) == Hooks::ResolveRva(RVA_GFX_CTRL);
}

bool IsLicenseBoard(void* owner) {
    return owner && Obj0(owner) == Hooks::ResolveRva(RVA_LICENSE_BOARD);
}

bool IsJobSelectRing(void* owner) {
    return owner && Obj0(owner) == Hooks::ResolveRva(RVA_JOBSEL_RING);
}

void* ChoicePopup() {
    void* ctx = nullptr;
    if (!SafeReadPtr(Hooks::ResolveRva(RVA_MENU_CTX), &ctx) || !ctx) return nullptr;
    void* pop = PtrAt(ctx, OFF_CTX_CHOICE);
    return (pop && Obj0(pop) == Hooks::ResolveRva(RVA_CHOICE_POPUP)) ? pop : nullptr;
}

bool IsChoicePopup(void* owner) {
    if (!owner) return false;
    if (Obj0(owner) == Hooks::ResolveRva(RVA_CHOICE_POPUP)) return true;
    void* pop = ChoicePopup();
    return pop && owner == PtrAt(pop, OFF_POPUP_LIST);
}

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

void* FocusedOwner() {
    void* fw = nullptr;
    return SafeReadPtr(Hooks::ResolveRva(RVA_FOCUS_WINDOW), &fw) ? fw : nullptr;
}

bool IsFocusedPane(void* owner) {
    return owner && owner == FocusedOwner();
}

bool IsAnyMenuOpen() {
    return FocusedOwner() != nullptr;
}

ValueRow ClassifyValueRow(void* row) {
    if (!row) return ValueRow::None;
    void* cls = Obj0(row);
    if (cls == Hooks::ResolveRva(RVA_VALROW_E770)) return ValueRow::EnumE770;
    if (cls == Hooks::ResolveRva(RVA_VALROW_D6B0)) return ValueRow::EnumD6B0;
    if (cls == Hooks::ResolveRva(RVA_VALROW_DB40)) return ValueRow::EnumDB40;
    if (cls == Hooks::ResolveRva(RVA_VALROW_EBE0)) return ValueRow::SliderEBE0;
    if (cls == Hooks::ResolveRva(RVA_VALROW_B330)) return ValueRow::SliderB330;
    if (cls == Hooks::ResolveRva(RVA_VALROW_B6F0)) return ValueRow::GfxEnumB6F0;
    if (cls == Hooks::ResolveRva(RVA_VALROW_C5C0)) return ValueRow::KeyBindC5C0;
    return ValueRow::None;
}

void* ConfigRowWidget(void* ctrl, int index) {
    if (!ctrl || index < 0) return nullptr;
    void* cls = Obj0(ctrl);
    uint32_t base = OFF_CTRL_ROWARR;                                              // FUN_0023fbe0
    if (cls == Hooks::ResolveRva(RVA_GFX_CTRL))           base = OFF_CTRL_ROWARR_GFX;
    else if (cls == Hooks::ResolveRva(RVA_CONTROLS_CTRL)) base = OFF_CTRL_ROWARR_CTL;
    return PtrAt(ctrl, base + static_cast<uint32_t>(index) * ROW_STRIDE);
}

} // namespace MenuState

#pragma once

#include <cstdint>

// Menu machine state: WHICH surface is on screen, which one holds the cursor, and what kind of
// widget a given row is. The third of the mod's three state modules, alongside
// PlayerState (field liveness) and BattleState (battle). It answers "where is the player, in menu
// terms" -- it never speaks, never formats, and never reads a row's VALUE (that is ui/config_reader).
//
// WHY THIS EXISTS. These predicates and the surface-identity RVAs behind them were private to
// menu_reader.cpp's anonymous namespace, so nothing else could ask them: ingame_menu_reader.cpp grew
// a parallel 28-constant set and title_reader.cpp a third, and menu_reader.cpp itself passed 660
// lines. Surface identity is shared knowledge and belongs in one place.
//
// CONTRACT (same as its two sibling modules): pure, SEH-guarded memory reads. It calls no game
// function, writes nothing, and every predicate reads FALSE on a fault -- so a freed or
// mid-teardown menu is reported as "not this", never dereferenced blindly.
namespace MenuState {

// ---- surface identity (obj[0] class-pointer tests) ---------------------------------------------
bool IsTitleMenu(void* owner);        // the title command menu -- TitleReader owns it, readers skip it
bool IsConfirmWindow(void* owner);    // FUN_00241d40 confirm / quit pop-up
bool IsConfigController(void* owner); // main config / Graphics / Controls screen
bool IsGraphicsConfig(void* owner);   // the Graphics sub-screen specifically (FUN_0023bd40) —
                                      // its value setter is a separate hook from the shared store

// True only when `owner` is the CURRENTLY-ACTIVE instance of its config controller. Guards value
// reads so we never dereference a closed/freed menu's widgets -- which is unsafe even under SEH,
// because the memory can be reallocated to something live. Controllers with no known instance
// global (Graphics) fall back to IsConfigController + SEH.
bool IsActiveConfig(void* owner);

// ---- the focused pane ---------------------------------------------------------------------------
// DAT_0208ebc0 holds the window the input pump routes the D-pad to. Companion panes that merely
// repaint on the same keypress are NOT it, so this isolates the one pane the player is actually in
// -- it is what stops the inventory reading items/magicks/equipment from several panes at once.
void* FocusedOwner();
bool  IsFocusedPane(void* owner);

// True when any menu surface currently holds the cursor. The ambient "is a menu open" query the
// codebase previously had no way to ask.
bool IsAnyMenuOpen();

// ---- config rows ---------------------------------------------------------------------------------
// The value-row layouts, as distinct classes. Callers switch on this instead of re-resolving row
// RVAs, so the class -> RVA mapping stays in exactly one file.
enum class ValueRow {
    None,          // not a value row
    EnumE770,      // FUN_0023e770 -- enum, types 1/2/8; option cells inline at row+0xd8
    EnumD6B0,      // FUN_0023d6b0 -- enum, type 3 (main screen); children via row+0x60
    EnumDB40,      // FUN_0023db40 -- enum, type 3 (Controls); same layout as D6B0
    SliderEBE0,    // FUN_0023ebe0 -- slider/gauge, numeric
    SliderB330,    // FUN_0023b330 -- Graphics slider (gauge child), numeric
    GfxEnumB6F0,   // FUN_0023b6f0 -- Graphics enum; formatted text inline at row+0xCC
    KeyBindC5C0,   // FUN_0023c5c0 -- Controls key binding; DIK codes only, no name
};
ValueRow ClassifyValueRow(void* row);
inline bool IsConfigValueRow(void* row) { return ClassifyValueRow(row) != ValueRow::None; }

// The N-th row widget on a config controller. The row-array base differs by controller
// (FUN_0023fbe0 -> +0xE8, Controls -> +0xD8, Graphics -> +0x4E0), stride 0x18.
void* ConfigRowWidget(void* ctrl, int index);

} // namespace MenuState

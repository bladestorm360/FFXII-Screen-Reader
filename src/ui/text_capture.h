#pragma once

#include <cstdint>
#include <string>

// Captures the codec text the game rasterizes, so the reader speaks the game's
// OWN strings verbatim — no option-name table lives here or anywhere.
//
// The reader must speak the item the cursor is ACTUALLY on, not the Nth string
// on screen. Every list menu paints its rows through the universal painter
// `FUN_002d28e0`, which calls a per-menu cell callback (`subwidget+0x120`) with
// the ABSOLUTE item index — the same index the `0x8000` focus signal carries.
// We intercept that callback so each drawn string is attributed to its true
// item index, producing a clean `owner -> index -> text` map of only the real
// list-row labels (values, help footers, and stale text are NOT list-row draws,
// so they land in a separate diagnostic ring instead).
namespace TextCapture {

bool Init();
void Shutdown();

// Text of the list row at `index` in the menu owned by `owner`, built live from
// the per-item paint callback. A row's captured fields are joined. Empty if
// `owner` isn't a menu we've painted or the row wasn't drawn (scrolled off).
std::wstring FocusedItemText(void* owner, int index);

// Localized UI string for a text id, captured passively from the game's own
// resolver `FUN_002f9860` (e.g. pop-up "Yes"/"No" = ids 1000/1001). Empty until
// the game has drawn that id at least once this session.
std::wstring StringById(int id);

// The help/description text currently shown for the focused item, captured from
// the game's description-bar setter (FUN_00291d80 @ RVA 0x171D80). Bound to the
// latest focus by a generation counter: returns empty if no description was set
// for the current focus, so a stale description from a previous item is never
// returned. Read on demand by the `i` hotkey.
std::wstring CurrentHelpText();

// Bump the focus generation. The reader calls this on each focus change so a
// description the game sets during that focus is attributed to it (and only it).
void NotifyFocusChanged();

// Fired (game thread) right after the painter finishes drawing `owner`'s rows,
// i.e. when `owner`'s item map is freshly populated. The reader uses this to
// replay a focus announce whose first `0x8000` arrived before the paint (the
// menu-entry "no item text" case).
typedef void (*MenuPaintedCallback)(void* owner);
void SetMenuPaintedCallback(MenuPaintedCallback cb);

// Fired from the per-string draw the moment ANY UI text is drawn -- the earliest proof that a menu
// has actually started rendering, as opposed to merely having been handed the input focus.
//
// Why this and not SetMenuPaintedCallback: that one is driven from FinishPaint, i.e. the list
// painter FUN_002d28e0, and the FIELD MENU never goes through it. Measured: PainterSwap/CellWrapper/
// FinishPaint appeared in ONE dump window of a session where Capture appeared in 29. So the painter
// callback cannot time the field menu, and this can.
//
// Carries the DECODED string that was just drawn. "Some text was drawn" is useless on its own --
// the field HUD draws text every frame, so a bare notification fires on the very next frame and is
// indistinguishable from announcing at key-press. The caller matches this against the row it is
// waiting for, which turns it into "the menu has actually put that row on screen".
typedef void (*DrawCallback)(const std::wstring& drawn);
void SetDrawCallback(DrawCallback cb);

// Diagnostic: dump the framing ring + the per-owner item map to the log.
void DumpRingToLog(const char* reason);

// DIAGNOSTIC A/B (Shift+`). Turns off ONLY the painter callback swap -- the one place the mod
// writes into a game structure -- while leaving every hook installed and every other reader
// working. Row text stops being captured while it is off, which is the point: if the field menu
// then opens instantly, the swap is what stalls it.
// Returns the new state. Announces itself, so it is usable without sight.
bool ToggleInterception();
bool InterceptionEnabled();

} // namespace TextCapture

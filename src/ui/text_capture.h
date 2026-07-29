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
// the game's description-bar setters — FUN_00291d80 (RVA 0x171D80) for field
// menus, FUN_0028fcb0 (RVA 0x16FCB0) for the battle menu. Both write the same
// window; only the wrapper differs, which is why the battle menu was silent
// until S87 added the second one. Bound to the
// latest focus by a generation counter: returns empty if no description was set
// for the current focus, so a stale description from a previous item is never
// returned. Read on demand by the `i` hotkey.
std::wstring CurrentHelpText();

// Bump the focus generation. The reader calls this on each focus change so a
// description the game sets during that focus is attributed to it (and only it).
void NotifyFocusChanged();

// Directly supply the help/description text for the CURRENT focus. For surfaces
// whose description the game does NOT feed to the description-bar setter
// (FUN_00291d80) -- the license-board node effect text and job descriptions --
// the reader resolves the text itself and hands it here. Stamps the current focus
// generation, so `o` (CurrentHelpText) returns it only for this focus, exactly
// like the captured path. Call on the game thread AFTER NotifyFocusChanged has
// bumped the generation for this focus.
void ProvideHelpText(const std::wstring& text);

// Fired (game thread) right after the painter finishes drawing `owner`'s rows,
// i.e. when `owner`'s item map is freshly populated. The reader uses this to
// replay a focus announce whose first `0x8000` arrived before the paint (the
// menu-entry "no item text" case).
typedef void (*MenuPaintedCallback)(void* owner);
void SetMenuPaintedCallback(MenuPaintedCallback cb);

// (There was a per-string DrawCallback here, used to time the menu-entry announce off "the row we
// are waiting for was drawn". Measurement killed it: the row is rasterized 31ms after the focus
// event. Chasing a later "menu is ready" signal then failed three more ways -- the menu's own
// window proc turned out to be running per-frame 32ms after the focus-set, with no activation
// event at all, so there was never anything to wait for. The announce is immediate again; see
// MenuReader::HookedFocusSet. Do not reintroduce a draw-based readiness signal.)

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

#pragma once

#include <cstdint>

// Bridges the FUN_00247510 msg-0x8000 focus event to TextCapture lookups, then to
// Speech::Output. By design contains no option-name strings — speaks only
// the bytes captured from the game's text-wrapper layer.
//
// Surface identity and the focused-pane test live in ui/menu_state.h; config row
// values in ui/config_reader.h.
namespace MenuReader {

bool Init();
void Shutdown();

// Called (game thread) for EVERY message the field pause menu's own window proc receives, from
// IngameMenuReader's observe-only FUN_00280de0 hook.
//
// Logs the menu's open sequence with a delta from the entry arm, so the event that coincides with
// the menu actually appearing can be READ off the log instead of guessed -- three different
// "readiness" signals have already been refuted by measurement. Bounded: only while an entry is
// armed, only briefly after it, and capped, because this runs on the game thread.
//
// Returns true while an entry announce is still pending, so the caller can skip further work.
bool NoteWindowMessage(void* window, uint32_t cat, uint64_t msg, uint32_t state);

// The window reported ACTIVATE (cat 0x11f / msg 0x8000) -- the game's own "this menu is now live".
// Speaks the entry row that HookedFocusSet held back.
void OnMenuActivated(void* window);

} // namespace MenuReader

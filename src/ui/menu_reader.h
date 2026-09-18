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

// AUTO DETAIL (S192): speak the `o` description for the CURRENT focus, if the setting is on and it
// has not already been said for this focus. Call AFTER the focused row has been announced -- it
// declines while nothing has been spoken for the focus yet, precisely so it cannot get in front of
// that line and be cut off by it. Safe to call from anywhere on the game thread and safe to call
// more than once: at most one description goes out per focus.
//
// Most surfaces need no call at all -- the focus dispatch and the paint already cover them. It is
// exported for the DEFERRED entry announcements, which speak from another file some milliseconds
// after the focus that owns them (ingame_menu_reader.cpp's SHOW release).
void VolunteerDetail();

} // namespace MenuReader

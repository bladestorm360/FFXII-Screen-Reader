#pragma once

#include <cstdint>

// "Did the user just press a key" timestamp tracker + the mod's on-demand
// hotkeys. Owns a WH_KEYBOARD_LL hook on a DEDICATED thread that pumps messages
// (a low-level hook only fires while its installing thread runs a message loop —
// the previous inline install on the short-lived init thread never fired).
// Keyboard ONLY — gamepad input is a known limitation (separate TODO).
namespace InputTracker {

bool Init();
void Shutdown();

// Callback fired (on the input thread) when the user presses the on-demand
// "describe / read tooltip" key (`o`; i/j/k/l are alt arrow keys), while the game
// window is foregrounded. The reader registers a handler that speaks the
// focused item's description.
typedef void (*HotkeyCallback)();
void SetDescribeCallback(HotkeyCallback cb);

// Callback fired (on the input thread) when the user presses the "re-read last line"
// key (`t`), while the game window is foregrounded. The message reader registers a
// handler that repeats the last spoken dialogue/panel line.
void SetRereadCallback(HotkeyCallback cb);

// Wall-clock milliseconds (GetTickCount64) of the last key-down event.
// 0 if no event has been observed since Init.
uint64_t LastInputTimestampMs();

// Convenience: how long ago the last input event was. UINT64_MAX if never.
uint64_t MsSinceLastInput();

// True if there has been a key-down event within the last `windowMs`.
bool WasRecentInput(uint64_t windowMs);

} // namespace InputTracker

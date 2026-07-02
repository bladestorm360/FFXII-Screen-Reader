#pragma once

#include <cstdint>

// Minimal "did the user just press a key" timestamp tracker for gating
// menu_reader speech. Uses WH_KEYBOARD_LL — fires on every key event
// regardless of game focus, so we capture even pre-window-activation
// presses. Keyboard ONLY — gamepad input is a known limitation and is
// tracked as a separate TODO (see project_menu_reader_scaffolding.md).
namespace InputTracker {

bool Init();
void Shutdown();

// Wall-clock milliseconds (GetTickCount64) of the last key-down event.
// 0 if no event has been observed since Init.
uint64_t LastInputTimestampMs();

// Convenience: how long ago the last input event was. UINT64_MAX if never.
uint64_t MsSinceLastInput();

// True if there has been a key-down event within the last `windowMs`.
bool WasRecentInput(uint64_t windowMs);

} // namespace InputTracker

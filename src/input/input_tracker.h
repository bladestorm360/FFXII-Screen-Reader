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

// Navigation hotkeys. Fired (on the input thread) when the user presses a nav key
// while the game window is foregrounded: `\` (VK_OEM_5), `[` (VK_OEM_4),
// `]` (VK_OEM_6), or `` ` `` (VK_OEM_3). `vk` is the virtual-key code; `shift` is
// the Shift state at press time (so Shift+[ / Shift+] / Shift+\ resolve). Keys are
// edge-triggered (auto-repeat suppressed) and passed through to the game.
typedef void (*NavKeyCallback)(int vk, bool shift);
void SetNavKeyCallback(NavKeyCallback cb);

// Fed by the dinput8 proxy each frame with the game's own 256-byte DirectInput
// keyboard state (DIK scan-code buffer, bit 0x80 = down). This is the primary key
// path — the game acquires the keyboard exclusively, starving OS-level hooks, so we
// read the mod's hotkeys from the same buffer the game polls. Edge-detected;
// dispatches describe/reread/nav on rising edges (game-foreground only).
void FeedDInputKeyboard(const unsigned char* dikState);

// Wall-clock milliseconds (GetTickCount64) of the last key-down event.
// 0 if no event has been observed since Init.
uint64_t LastInputTimestampMs();

// Convenience: how long ago the last input event was. UINT64_MAX if never.
uint64_t MsSinceLastInput();

// True if there has been a key-down event within the last `windowMs`.
bool WasRecentInput(uint64_t windowMs);

} // namespace InputTracker

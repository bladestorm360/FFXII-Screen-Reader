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

// Callback fired (on the input thread) when the user presses the "read License
// Points" key (`U`), while the game window is foregrounded. The license reader
// registers a handler that speaks the current LP when the license board is open
// (silent otherwise). `U` is free in this game's bindings (Docs/Controls.md).
void SetLicensePointsCallback(HotkeyCallback cb);

// Callback fired (on the input thread) when the user presses the "read gil" key
// (`g`), while the game window is foregrounded. GilReader registers a handler that
// speaks the party's gil total (silent when no save is loaded). Memory-only read, so
// it is safe off the game thread. `g` is free in this game's bindings (Docs/Controls.md).
void SetGilCallback(HotkeyCallback cb);

// Callback fired (on the input thread) when the user presses the "re-read last line"
// key (`t`), while the game window is foregrounded. The message reader registers a
// handler that repeats the last spoken dialogue/panel line.
void SetRereadCallback(HotkeyCallback cb);

// Fired when the player presses the game's own CONFIRM key (Space / Enter). PURELY OBSERVED: the
// mod never swallows or injects it, the game still receives it exactly as before. It exists so the
// dialogue reader can advance its page pointer on the same press that advances the game's text box.
void SetConfirmCallback(HotkeyCallback cb);

// Navigation hotkeys. Fired (on the input thread) when the user presses a nav key
// while the game window is foregrounded: `\` (VK_OEM_5), `[` (VK_OEM_4),
// `]` (VK_OEM_6), or `` ` `` (VK_OEM_3). `vk` is the virtual-key code; `shift` is
// the Shift state at press time (so Shift+[ / Shift+] / Shift+\ resolve). Keys are
// edge-triggered (auto-repeat suppressed) and passed through to the game.
// The `shift` argument was removed: it was passed `false` from every DInputEdge call site and could
// never be honoured (Left Shift is the game's Toggle Walk/Run and the mod cannot swallow keys).
typedef void (*NavKeyCallback)(int vk);
void SetNavKeyCallback(NavKeyCallback cb);

// Virtual-buffer navigation keys (status screen): the ARROW keys plus Home/End, fired on the input
// thread while the game is foregrounded. `vk` is VK_UP / VK_DOWN / VK_LEFT / VK_RIGHT / VK_HOME /
// VK_END. The callback returns TRUE if it consumed the key (e.g. the status buffer is active). The
// return matters only for Home/End: when the buffer does NOT consume them they fall through to the
// combat-log nav path (NavKeyCallback), so the combat-log Home/End keep working everywhere else. The
// arrow keys have no fallback -- outside a buffer they simply do nothing in the mod (the game owns
// them). Keys are edge-triggered (auto-repeat suppressed) and never swallowed from the game.
typedef bool (*MenuNavCallback)(int vk);
void SetMenuNavCallback(MenuNavCallback cb);

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

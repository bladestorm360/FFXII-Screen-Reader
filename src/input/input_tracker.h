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

// S174. Fired (input thread) when the pad's `L3` asks for the intercept off or on; ModMenu
// registers `ToggleController`. A callback like every other handler here: this file knows no menu.
void SetControllerToggleCallback(HotkeyCallback cb);

// Callback fired (on the input thread) when the user presses the "re-read last line"
// key (`t`), while the game window is foregrounded. The message reader registers a
// handler that repeats the last spoken dialogue/panel line.
void SetRereadCallback(HotkeyCallback cb);

// REMOVED: SetConfirmCallback (Space / Enter, "the game's own Confirm"). The dialogue reader used it
// to advance its page pointer, which made multi-page dialogue KEYBOARD-ONLY — this tracker sees the
// DirectInput keyboard buffer and nothing else, while the game reads pads through XInput, so a
// controller player heard page 1 and then silence. Pagination now comes off the game's own page
// cursor in `ui/dialogue_reader` and is blind to the input device. Do not add an input-side page
// signal back: observing a key that usually advances the box is not observing the box advancing.

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

// The mod's own menu (F8) gets FIRST REFUSAL on the keys above and on `o`, because while it is open
// it owns them; every other consumer is offered the key only after the mod menu declines. Two
// separate slots rather than chaining through StatusReader/MenuReader: those two already own the
// single MenuNav/Describe slots, and arbitrating here keeps both paths intact instead of one
// displacing the other (CLAUDE.md: keep both and arbitrate, never collapse). ModMenu is the only
// registrant; both callbacks return true only while the menu is open, so with it closed every key
// behaves exactly as it did before.
void SetModMenuNavCallback(MenuNavCallback cb);

// The Clan Primer's entry body walk (primer_reader.cpp). A third slot, for the reason given just
// above: StatusReader holds the MenuNav slot and neither should displace the other.
void SetPrimerNavCallback(MenuNavCallback cb);
typedef bool (*DescribeInterceptCallback)();
void SetModMenuDescribeCallback(DescribeInterceptCallback cb);

// Fed by the dinput8 proxy each frame with the game's own 256-byte DirectInput
// keyboard state (DIK scan-code buffer, bit 0x80 = down). This is the primary key
// path — the game acquires the keyboard exclusively, starving OS-level hooks, so we
// read the mod's hotkeys from the same buffer the game polls. Edge-detected;
// dispatches describe/reread/nav on rising edges (game-foreground only).
void FeedDInputKeyboard(const unsigned char* dikState);

// ---- DEVICE-INDEPENDENT DISPATCH (Session 173) -------------------------------------------------
//
// Post one of the mod's own hotkeys to this tracker's message thread from a device that is NOT the
// keyboard -- today that means `input/pad_router.cpp`. `vk` is the SAME virtual-key code the
// keyboard path posts, and it lands in the SAME handler, so a pad button and its key are the same
// action by construction: there is no second copy of "what `\` does" to drift.
//
// Routing matches `DInputEdge` / `DInputMenuNavEdge` exactly -- O/T/U/G to their own handlers, the
// arrows and Home/End to the virtual-buffer path (Home/End with the combat-log fallback), and
// everything else to `NavCommands::OnNavKey`, which is where the mod's key switch already lives.
//
// EDGE DETECTION IS THE CALLER'S JOB. The keyboard path suppresses auto-repeat before it gets here;
// the pad router does its own rising-edge work per pad index. This function fires every time it is
// called.
//
// Safe from any thread: it only posts. The foreground gate is the caller's too (the pad router
// checks `GameForeground()` before it reads anything).
void DispatchModKey(int vk);

// Speak one phrasebook line from THIS tracker's thread. `phraseId` is a `Phrase::Id` cast to int --
// passed as an int so the pad router does not have to agree with this header about the enum.
//
// It exists so the pad keeps the mod's rule that speech happens on a thread that already speaks.
// The pad poll runs on the game's input thread, which has never spoken and is not a place to start:
// the whole reason this tracker owns a message loop is that speech must not run inside an input
// callback. Phrasebook strings are static, so nothing is allocated or owned across the post.
void DispatchSpeakPhrase(int phraseId);

// S174: toggle the gamepad intercept, from the pad poll. Only posts, like `DispatchSpeakPhrase`.
// Not a `DispatchModKey` VK: the Controller row has no keyboard shortcut to name it by. The caller
// runs it ABOVE its own `ControllerOn()` gate, which would otherwise eat the button that undoes it.
void DispatchToggleController();

// Wall-clock milliseconds (GetTickCount64) of the last key-down event.
// 0 if no event has been observed since Init.
uint64_t LastInputTimestampMs();

// Convenience: how long ago the last input event was. UINT64_MAX if never.
uint64_t MsSinceLastInput();

// True if there has been a key-down event within the last `windowMs`.
bool WasRecentInput(uint64_t windowMs);

// Is the player currently holding a movement key (W/A/S/D — Docs/Controls.md)? Read from the GAME
// thread by the navigation stuck detector, written by the DirectInput feed; a relaxed atomic, so it
// is free to call per frame. Purely observational — the mod never writes the input buffer.
//
// It exists to tell "jammed against a wall" apart from "standing still in a menu". Without it a stuck
// detector would fire at anyone who stopped to listen.
bool MovementHeld();

// Is the game window the foreground window? This has always existed as the dispatch gate inside the
// tracker; exposed in Session 100 because AutoWalk must stop commanding movement the moment the game
// loses focus, and a second copy of the check would be a second place for it to be wrong.
bool GameForeground();

} // namespace InputTracker

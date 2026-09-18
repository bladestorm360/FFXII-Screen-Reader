#pragma once

#include <cstdint>
#include "input/pad_hook.h"

// THE PAD READ AUTHORITY -- SDL3's gamepad API, and the reason it is not XInput.
//
// WHAT WENT WRONG. S162 built the pad intercept as an IAT patch on `XInputGetState` and read the
// pad through it. **XInput is the Xbox protocol.** A DualSense, a DualShock 4, a Switch Pro pad or
// a generic HID stick does not speak it on Windows -- those enumerate as HID joysticks and reach a
// game through DirectInput / RawInput / HIDAPI. They appear on XInput ONLY when a translation layer
// (Steam Input, DS4Windows, ViGEm) is running. So every pad feature in the mod was unreachable for
// anyone on a PlayStation controller, which is the most common pad in the blind community.
//
// It shipped that way in V1.0. Lirin's log is the proof and it is worth keeping the exact shape:
//
//     [PAD] gamepad intercept installed (XInputGetState, IAT)
//     [PAD] non-keyboard DirectInput device created: idx=1 guid=B861A230-F85C-11EE
//     [PAD] non-keyboard DirectInput device polled:  idx=1 cbData=272 (JOYSTICK-SIZED ...)
//
// 272 bytes is `DIJOYSTATE2`: the game was reading that pad through DirectInput all session, while
// the XInput side never once returned a connected pad -- `controller CONNECTED on index N`, a
// one-shot line that IS present in the shipped binary, never fired.
//
// WHY SDL3 AND NOT "ALSO HOOK DIRECTINPUT". Hooking DirectInput as a second read path would work for
// Lirin and break on the next device, because a raw `DIJOYSTATE2` carries no button MEANING: it is
// `rgbButtons[128]` in the device's own HID report order, and "which index is Circle" differs per
// controller. There is no honest way to read it without a per-device table. SDL3 already holds that
// table -- thousands of device mappings -- and returns a NORMALIZED gamepad: SOUTH/EAST/WEST/NORTH,
// the D-pad, both shoulders, both stick clicks, Back and Start, in one layout for every pad. That is
// the whole reason the FFPR mods use it (`Core/GamepadManager.cs`), which is the design this file is
// ported from, and it is what the user asked for when controller support was first specified.
//
// SDL3 WAS ALREADY LINKED INTO THIS DLL when the XInput hook was written -- the navigation beacon
// pulls it in, and `CMakeLists.txt` line 28 says in as many words "SDL3 (audio output for the
// navigation beacon; later, controller support)". The gamepad subsystem cost nothing to reach.
//
// ---- ONE PATH: SDL3 READS, AND SDL3 IS WHAT THE GAME SEES (S188) --------------------------------
//
// THE MOD IS THE PAD'S ONLY READER, AND THE GAME'S PAD IS BUILT FROM WHAT THE MOD READ. This is the
// FFPR model ported whole, not half of it. There, `GamepadManager` opens the pad through SDL and
// `DisableUnityGamepad()` switches the engine's own gamepad devices OFF, after which
// `InputPassthroughPatches` hands the game back whatever the mod did not consume -- its own header
// says "Suppress all game input when mod is consuming / Inject SDL controller state for game
// passthrough when not suppressing".
//
// The FFXII equivalent, because this engine reads the OS directly rather than through an input
// system object:
//
//     * `pad_hook.cpp` SYNTHESISES the `XINPUT_STATE` the game reads, from this file's SDL state
//       minus whatever `PadRouter` claimed. The real `XInputGetState` result is discarded.
//     * `dinput8_proxy.cpp` BLANKS the game's DirectInput joystick, so the physical device cannot
//       reach the game a second time, un-routed, behind the mod's back.
//
// WHY THIS IS THE WHOLE POINT. It means there is exactly ONE code path for every controller. An Xbox
// pad, a DualSense, a Switch Pro pad, a ROG Ally's built-in sticks -- SDL's database normalises them
// all, and every one of them reaches the game as the same synthesised state built by the same lines.
// Testing on any pad tests the path all pads use; there is no second, device-shaped path left to go
// unexercised. The previous design read through SDL but let the game keep reading the hardware, so
// it needed a different suppressor per API and each had to be proven separately. That is gone, and
// with it went the `rgbButtons[]` index tables, the SDL binding lookups and the per-button
// confirmation they required -- none of that has anything to describe any more.
//
// IT ALSO FIXES SOMETHING THE OLD SHAPE COULD NOT. A DualSense used to reach the game as a raw
// DirectInput joystick with whatever quirks that device's HID descriptor carries. Now it reaches the
// game as a clean XInput pad, which is the path FFXII supports best.
//
// THIS CROSSES THE CONSUMPTION/INJECTION LINE, DELIBERATELY AND ON INSTRUCTION. The mod no longer
// merely clears bits the player pressed; it constructs the pad state the game reads. That is a
// category change from the S162 charter and CLAUDE.md's second input-write exception is rewritten to
// match. The bound that replaces "never set a bit" is: EVERY BIT IN THE SYNTHESISED STATE CAME FROM
// A PHYSICAL CONTROL THE PLAYER IS TOUCHING, as SDL read it this poll. The mod cannot originate an
// input; it can only decline to forward one.
//
// OFF MEANS OFF, AND THAT IS UNCHANGED. With the `Controller` row off, or no pad open, this file
// drives nothing: the real `XInputGetState` result is returned untouched and the DirectInput
// joystick is left alone, so the game's input is byte-identical to an unmodded run. `L3` + `R3` still
// reaches the router, which is what lets the pad be handed back and taken again without a keyboard.
namespace GamepadSDL {

// Opens SDL's gamepad subsystem and the first gamepad present. Safe to call with no pad attached --
// hot-plug is handled in Poll. False (logged) when the subsystem will not start, in which case the
// mod behaves exactly as it does with no pad: nothing crashes, nothing speaks.
//
// Call AFTER ModMenu::Init (the router asks it whether the Controller setting is on) and it may be
// called before or after AudioEngine::Init -- both use subsystem-scoped init, so neither teardown
// takes the other's subsystem down.
bool Init();

// Closes the pad and drops SDL_INIT_GAMEPAD. Idempotent.
void Shutdown();

// True when SDL started AND a gamepad is currently open.
bool Available();

// SDL's name for the open pad's type ("PS5 Controller", "Xbox Series X Controller", ...), or
// "none" when nothing is open. Log/diagnostic use.
const char* TypeName();

// READS THE PAD AND RUNS THE ROUTER, at most once per frame however many hooks call it.
//
// WHY THE STALENESS GUARD. Three hook sites want the router's mask to be current when they run --
// the keyboard poll (which fires every frame, menus and loads included, and so is what guarantees
// the pad is read at all), and the two suppressors, which must not apply a mask computed for the
// previous frame. Letting any of them drive the poll, with a short staleness window collapsing the
// duplicates, removes the ordering question entirely: whoever runs first in a frame does the read,
// and the others see a fresh mask.
//
// INPUT-POLL THREAD. Cheap by construction -- an event drain plus ~27 cached reads -- but measured
// rather than assumed: the call site wraps it in STALL_SCOPE. See `L-88`; nothing here blocks.
void PollIfStale();

// ---- driving the game ---------------------------------------------------------------------------

// True when the mod should be the source of the game's pad state: a pad is open AND the `Controller`
// row is on. False hands the hardware back completely -- callers must then leave the game's own
// input path untouched rather than write a neutral state over it, because "off" has always meant
// byte-identical, not merely inert.
bool DriveGame();

// Fills `out` with the state the GAME should see this frame: what SDL read, minus what `PadRouter`
// claimed. `dwPacketNumber` advances only when the contents actually change, which is the contract
// XInput callers rely on to detect input.
//
// Every bit set here came from a physical control SDL reported as pressed on this poll. Nothing is
// originated -- the mod can decline to forward an input, never invent one.
//
// Returns false (leaving `out` untouched) when `DriveGame()` is false.
bool BuildGameState(PadHook::State* out);

} // namespace GamepadSDL

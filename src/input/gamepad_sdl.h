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
//     * `dinput8_proxy.cpp` SYNTHESISES the `DIJOYSTATE2` the game reads, from the same bytes.
//
// TWO ENCODERS, AND THE GAME PICKS WHICH ONE RUNS -- CORRECTED S193, AFTER IT SHIPPED WRONG. S188
// wrote only the XInput encoder and BLANKED the DirectInput road, on the belief that the pad reached
// the game through XInput and the DirectInput device was a second, redundant road to close. It is
// not redundant: for most controllers it is the ONLY road. `FUN_00797690` builds EITHER a
// `PInputDevicePadXInput` OR a `PInputDevicePadDirectInput` per device, decided by Microsoft's
// `IsXInputDevice()` WMI test for `IG_` in the PnP id, and only the XInput class ever calls
// `XInputGetState`. A DualSense has no `IG_`, so the game never created an XInput device for it,
// never called the import once, and the mod's whole hand-back was addressed to a reader that did not
// exist -- while the blanker sat on the road the pad actually had. The tester's report was exact:
// mod functions fine (they read SDL), game functions dead. See `dinput8_proxy.cpp` for the full
// decode table. An Xbox pad could not have shown it, because an Xbox pad takes the other branch.
//
// WHY THIS IS STILL ONE PATH. The device-shaped layer is what is gone, not the format-shaped one.
// One reader (SDL), one router, one consume model, one post-router state -- `GameButtons` below --
// and then a pack into whichever struct the game asked for. An Xbox pad, a DualSense, a Switch Pro
// pad and a handheld's built-in sticks are normalised by SDL's database and reach the game through
// the same lines; nothing branches on WHICH device it is. What branches is which struct the game
// reads, and the game decides that, not the mod. The `rgbButtons[]` index tables the old design
// needed are still gone: the mod writes the game's fixed slot order, so the device's own HID report
// order never enters into it.
//
// IT ALSO FIXES SOMETHING THE OLD SHAPE COULD NOT. A DualSense used to reach the game as a raw
// DirectInput joystick with whatever quirks that device's HID descriptor carries. Now what lands in
// that buffer is SDL's normalised layout, so the device's own quirks never reach the engine.
//
// THIS CROSSES THE CONSUMPTION/INJECTION LINE, DELIBERATELY AND ON INSTRUCTION. The mod no longer
// merely clears bits the player pressed; it constructs the pad state the game reads. That is a
// category change from the S162 charter and CLAUDE.md's second input-write exception is rewritten to
// match. The bound that replaces "never set a bit" is: EVERY BIT IN THE SYNTHESISED STATE CAME FROM
// A PHYSICAL CONTROL THE PLAYER IS TOUCHING, as SDL read it this poll. The mod cannot originate an
// input; it can only decline to forward one.
//
// NO PAD MEANS NO WRITES. With no pad open this file drives nothing: the real `XInputGetState`
// result is returned untouched and the DirectInput joystick is left alone, so the game's input is
// byte-identical to an unmodded run. (Until S194 the `Controller` row and the L3 + R3 chord could
// force the same state with a pad attached; both were removed at the user's instruction.)
//
// A CONTROLLER SDL HAS NO MAPPING FOR FALLS BACK THE SAME WAY, and that is the safety net under the
// whole design: `SDL_GetGamepads` lists only devices SDL can normalise, so an exotic stick is never
// opened, `DriveGame()` stays false, and the game reads its own hardware exactly as it would with no
// mod installed. Such a player loses the pad's MOD features; they do not lose the game.
namespace GamepadSDL {

// Opens SDL's gamepad subsystem and the first gamepad present. Safe to call with no pad attached --
// hot-plug is handled in Poll. False (logged) when the subsystem will not start, in which case the
// mod behaves exactly as it does with no pad: nothing crashes, nothing speaks.
//
// Call AFTER ModMenu::Init (the router reads the Right stick camera row) and it may be
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

// True when the mod should be the source of the game's pad state: a pad is open. False hands the
// hardware back completely -- callers must then leave the game's own input path untouched rather
// than write a neutral state over it, because "no pad" means byte-identical, not merely inert.
bool DriveGame();

// THE ONE POST-ROUTER PAD STATE: what SDL read this poll, minus what `PadRouter` claimed. Both of
// the encoders below are built from this and from nothing else, which is what keeps "one reader, one
// consume model" true now that the game reads two different wire formats.
//
// Every bit set here came from a physical control SDL reported as pressed on this poll. Nothing is
// originated -- the mod can decline to forward an input, never invent one.
//
// Returns false (leaving `out` untouched) when `DriveGame()` is false.
bool GameButtons(PadHook::Gamepad* out);

// Fills `out` with the state the GAME should see this frame, as an `XINPUT_STATE`.
// `dwPacketNumber` advances only when the contents actually change, which is the contract XInput
// callers rely on to detect input. A thin wrapper over `GameButtons`.
//
// Returns false (leaving `out` untouched) when `DriveGame()` is false.
bool BuildGameState(PadHook::State* out);

// The open pad's USB vendor and product ids, or 0/0 when nothing is open or SDL does not know them.
//
// WHY THE MOD NEEDS THESE AT ALL, given that the whole point is that SDL normalises devices: FFXII
// ITSELF carries one device-specific branch. `PInputDevicePadDirectInput::vfunction6` re-maps the
// four face buttons in reverse for product GUID `{00060079-...}` (VID 0x0079 / PID 0x0006, the
// DragonRise "Generic USB Joystick" adapter). When the mod writes that device's buffer it has to
// write what the game will decode, so this one id is checked -- see `dinput8_proxy.cpp`. It is the
// game's branch, mirrored; the mod has none of its own.
void VendorProduct(uint16_t* vid, uint16_t* pid);

} // namespace GamepadSDL

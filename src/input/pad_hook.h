#pragma once

#include <cstdint>

// THE GAMEPAD INTERCEPT -- the pad twin of the keyboard hook in proxy/dinput8_proxy.cpp.
//
// WHY THIS EXISTS. `InputTracker` reads the game's DirectInput KEYBOARD buffer and nothing else
// (`dinput8_proxy.cpp` records only GUID_SysKeyboard devices), so the pad has always been invisible
// to the mod. That gap is recorded twice already: the dialogue page-advance was struck for it
// (input_tracker.h), and the navigation stuck detector's own init line says "Keyboard only; gamepad
// does not update the timestamp." A controller player could reach none of the mod's features.
//
// WHERE WE INTERCEPT, AND WHY IT IS HERE AND NOT LOWER. `FFXII_TZA.exe` statically imports
// `XInputGetState` / `XInputSetState` from **XINPUT9_1_0.DLL** (2 imports; see the exe's import
// table). We patch the exe's IAT entry for `XInputGetState`, call the original, hand the true state
// to PadRouter, and apply whatever the router consumed on the way out.
//
// The obvious-looking alternative -- the game's own unified pad words at RVA 0x2E77368/6A/6C, which
// `input_tracker.cpp`'s collision watch already reads -- is WRONG for this job, and not by a little:
// `FUN_002498b0` OR's every physical sub-device bound to a logical pad into those same 16 bits, so
// by the time input arrives there the D-PAD AND THE LEFT STICK ARE THE SAME BITS. "Take the D-pad
// for party slots but leave the left stick driving menus" is not expressible at that layer. In
// XINPUT_GAMEPAD they are separate fields. Same argument for taking the right stick alone.
//
// WHY AN IAT PATCH RATHER THAN MinHook. A tester's session once ran MinHook out of trampoline slots
// at hook 63 of 66 and silently lost the three combat hooks (see Hooks::LogInstallCensus). An IAT
// patch costs zero slots, and it is the same technique class as the vtable swap dinput8_proxy.cpp
// already performs -- one VirtualProtect over one pointer. `XInputGetState` is also a documented
// TWO-argument API, so L-20 (a detour's arity must match) is unpayable here.
//
// THIS IS THE SECOND SANCTIONED WRITE TO INPUT (see CLAUDE.md; the first is Auto-walk, S100). Since
// S188 it does not merely consume -- it BUILDS the `XINPUT_STATE` the game reads, from the SDL state
// in `gamepad_sdl.cpp` minus what `PadRouter` claimed. The bound is that every bit came from a
// physical control the player is touching on that poll; the mod can decline to forward an input, and
// can never originate one.
//
// IT IS ONE OF TWO ENCODERS, AND ON MOST CONTROLLERS IT IS THE ONE THAT NEVER RUNS (S193). FFXII
// builds EITHER a `PInputDevicePadXInput` or a `PInputDevicePadDirectInput` for a pad, decided by
// Microsoft's `IsXInputDevice()` WMI test, and only the first ever calls `XInputGetState`. A
// DualSense, a Switch Pro pad or any generic HID stick takes the other branch, so for those players
// this file is installed, correct, and never called -- their pad is built in `dinput8_proxy.cpp`
// instead, from the same bytes. Do not read a silent XInput hook as a broken one.
namespace PadHook {

// XInput's gamepad state, declared here rather than pulled from <Xinput.h> -- the same reason
// dinput8_proxy.cpp declares its own DirectInput types: the mod links against neither library and
// only ever reaches these through the trampoline. Layout is the documented XINPUT_GAMEPAD /
// XINPUT_STATE and must not be reordered.
struct Gamepad {
    uint16_t buttons;
    uint8_t  leftTrigger;
    uint8_t  rightTrigger;
    int16_t  thumbLX;
    int16_t  thumbLY;
    int16_t  thumbRX;
    int16_t  thumbRY;
};
struct State {
    uint32_t packet;
    Gamepad  pad;
};

// Documented XINPUT_GAMEPAD_* bits. Note DPAD_* and the thumbs are distinct here and are NOT
// distinct one layer down -- that distinction is the whole reason this file hooks where it does.
constexpr uint16_t kDpadUp    = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008;
constexpr uint16_t kStart     = 0x0010, kBack     = 0x0020;
constexpr uint16_t kLeftThumb = 0x0040, kRightThumb = 0x0080;
constexpr uint16_t kLeftShoulder = 0x0100, kRightShoulder = 0x0200;
constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

// Patches the exe's IAT entry for XInputGetState. False (logged) if the import is absent or the
// patch could not be applied -- in which case the mod behaves exactly as it did before this file
// existed. Call AFTER ModMenu::Init: the router asks it whether the feature is switched on.
bool Init();

// Restores the original IAT entry. Idempotent.
void Shutdown();

// True once the patch is in and has not faulted itself off. Log/diagnostic use.
bool Active();

// TRUE ONCE THE GAME HAS ACTUALLY ASKED THIS HOOK FOR A PAD STATE -- i.e. it built a
// `PInputDevicePadXInput`, and this encoder is the live one. It stays FALSE for the whole session on
// every controller the game classed as DirectInput, because there the import is never called at all.
//
// `dinput8_proxy.cpp` needs it to settle a mixed setup: an Xbox pad on the XInput road with a wheel
// or a second stick also enumerated. That joystick is NOT the pad, so it must be blanked rather than
// handed the pad's state. Exactly one encoder ever feeds.
bool DrivingXInput();

// Short human-readable name for a single button bit ("A", "D-pad Up", ...); "?" for anything else.
// Lives here because the bit constants do, and both the router and its log want the same words.
const char* ButtonName(uint16_t bit);

// The GAME's own unified pad words (RVA 0x2E77368/6A/6C -- pad 0's +0x08/+0x0A/+0x0C, rebuilt every
// frame by FUN_002498b0). This is the layer BELOW the intercept, where keyboard and pad have already
// been merged, so it cannot drive behaviour -- but it is exactly what answers "did the game react to
// this input", which is what the collision watch and the Phase-1 scheme survey both need.
//
// Centralized here because two callers now want it: input_tracker.cpp's 8/9/B/N collision watch
// (which owned this read first) and the pad router's survey log. Memory-only + guarded, so it is
// safe from the input thread. False when any of the three reads fails.
bool ReadGamePadWords(uint16_t out[3]);

} // namespace PadHook

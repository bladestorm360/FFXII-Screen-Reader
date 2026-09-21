#include "input/gamepad_sdl.h"

#include "input/pad_router.h"
#include "core/logger.h"
#include "ui/mod_menu.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace GamepadSDL {
namespace {

bool         g_sdlUp   = false;
SDL_Gamepad* g_pad     = nullptr;
SDL_JoystickID g_padId = 0;
char         g_typeName[64] = "none";

// The router's claim, LATCHED UNTIL RELEASE. Read from the suppressor hooks, which run on the same
// input-poll thread as the poll that writes them -- atomics anyway, because `PadHook::Active()` and
// the XInput hook are reachable from whichever thread the game polls XInput on, and that has never
// been proven to be the same one.
//
// WHY LATCHED, AND WHY THIS WAS A REAL BUG (S187). The router claims a button on its RISING EDGE
// (`consume |= rising`), which is true for exactly one poll. This file polls every ~4 ms; the game
// reads its pad once a FRAME, ~33 ms. So the edge almost never coincided with the game's read: by
// the time the game looked, the mask had been overwritten with 0 and the button reached the game
// anyway. Start paused the game, Back opened the map, and only the right stick behaved -- because
// `eatStick = onField` is a LEVEL, so it was still true whenever the game happened to look.
//
// This was introduced by moving the read off the XInput hook. Before, `PadRouter::OnPoll` ran FROM
// that hook, so edge detection and consumption were the same event on the same buffer and an
// edge-shaped mask was exactly right. Decoupling the read from the write made a one-poll mask
// useless, and the fix belongs here rather than in the router: a claimed button stays claimed for
// as long as the player holds it, so the mask is correct whenever the game reads, at any rate.
std::atomic<uint16_t> g_consumed{0};
std::atomic<bool>     g_eatStick{false};

// Buttons the router has claimed that are still physically held. Poll-thread only.
uint16_t g_heldConsume = 0;

// ---- staleness guard ----------------------------------------------------------------------------
// Three hook sites call PollIfStale and only the first one in a frame should do the work. A
// millisecond stamp rather than a frame counter because none of the three shares a frame number --
// 4 ms is below one frame at any rate this game runs at (it logs 30 fps = 33 ms) and above the
// spacing of two hooks firing within the same frame.
constexpr uint64_t kPollIntervalMs = 4;
uint64_t g_lastPollMs = 0;

// ---- priming ------------------------------------------------------------------------------------
// A button (or a deflected stick) that is already down when the pad OPENS must not read as a fresh
// press. PadRouter keeps its own edge state and has no reset entry point, so the mask is applied
// HERE, on the way in: a primed button is reported as up until the player actually releases it, at
// which point it leaves the mask and behaves normally. Same idea as the FFPR GamepadManager's
// `gamepadPrimed`, moved one layer out because our router owns the edges.
uint16_t g_primeMask  = 0;
bool     g_primeStick = false;

// XInput bit  <-  SDL gamepad button. This table IS the portability: SDL's mapping database has
// already turned whatever the physical device reports into these logical buttons, so a DualSense,
// a Switch Pro pad and an Xbox pad all arrive here in one layout and the scheme in pad_router.cpp
// needs no per-device branch.
//
// SOUTH/EAST/WEST/NORTH are POSITIONS, not letters. On a PlayStation pad SOUTH is Cross, EAST is
// Circle, WEST is Square, NORTH is Triangle -- the same physical positions the docs call A/B/X/Y.
struct BtnMap { SDL_GamepadButton sdl; uint16_t bit; };
constexpr BtnMap kButtons[] = {
    { SDL_GAMEPAD_BUTTON_SOUTH,          PadHook::kA },
    { SDL_GAMEPAD_BUTTON_EAST,           PadHook::kB },
    { SDL_GAMEPAD_BUTTON_WEST,           PadHook::kX },
    { SDL_GAMEPAD_BUTTON_NORTH,          PadHook::kY },
    { SDL_GAMEPAD_BUTTON_BACK,           PadHook::kBack },
    { SDL_GAMEPAD_BUTTON_START,          PadHook::kStart },
    { SDL_GAMEPAD_BUTTON_LEFT_STICK,     PadHook::kLeftThumb },
    { SDL_GAMEPAD_BUTTON_RIGHT_STICK,    PadHook::kRightThumb },
    { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  PadHook::kLeftShoulder },
    { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PadHook::kRightShoulder },
    { SDL_GAMEPAD_BUTTON_DPAD_UP,        PadHook::kDpadUp },
    { SDL_GAMEPAD_BUTTON_DPAD_DOWN,      PadHook::kDpadDown },
    { SDL_GAMEPAD_BUTTON_DPAD_LEFT,      PadHook::kDpadLeft },
    { SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     PadHook::kDpadRight },
};
constexpr int kButtonCount = static_cast<int>(sizeof(kButtons) / sizeof(kButtons[0]));

// THE STATE THE GAME WILL SEE, rebuilt every poll from SDL minus the router's claim. Guarded by a
// spinless copy under the poll thread's ownership: only PollIfStale writes it, and the XInput hook
// reads it through BuildGameState on the same input thread.
PadHook::Gamepad g_gameState{};
uint32_t         g_gamePacket = 0;

// A one-line capability note for the log. The mod no longer needs SDL's raw joystick bindings for
// anything -- it does not write the device's own report format any more, it builds an XINPUT_STATE --
// but knowing whether SDL has a full mapping for this pad is still the difference between "your
// controller is fully understood" and "SDL is guessing at this device", which is worth one line when
// someone reports an odd button.
void LogMappingCoverage() {
    if (!g_pad) return;
    const char* map = SDL_GetGamepadMapping(g_pad);
    Log::Write("PAD", map ? "SDL has a full mapping for this controller"
                          : "SDL has NO mapping for this controller -- buttons may be wrong; "
                            "report the controller model");
    if (map) SDL_free(const_cast<char*>(map));
}

void ClosePad() {
    if (g_pad) {
        SDL_CloseGamepad(g_pad);
        g_pad = nullptr;
    }
    g_padId = 0;
    std::snprintf(g_typeName, sizeof(g_typeName), "none");
    g_gameState = PadHook::Gamepad{};
    g_consumed.store(0, std::memory_order_relaxed);
    g_eatStick.store(false, std::memory_order_relaxed);
    g_heldConsume = 0;
    g_primeMask  = 0;
    g_primeStick = false;
}

void OpenPad(SDL_JoystickID id) {
    if (g_pad) return;                      // one pad at a time; the scheme is single-player
    g_pad = SDL_OpenGamepad(id);
    if (!g_pad) {
        char m[160];
        snprintf(m, sizeof(m), "SDL_OpenGamepad(%u) failed: %s", (unsigned)id, SDL_GetError());
        Log::Write("PAD", m);
        return;
    }
    g_padId = id;

    const char* name = SDL_GetGamepadName(g_pad);
    std::snprintf(g_typeName, sizeof(g_typeName), "%s", name ? name : "Unknown");

    char m[256];
    snprintf(m, sizeof(m), "CONTROLLER CONNECTED via SDL3: \"%s\" (SDL type %d, id %u, "
                           "VID_%04X PID_%04X) -- pad lines below are real data",
             g_typeName, (int)SDL_GetGamepadType(g_pad), (unsigned)id,
             SDL_GetGamepadVendor(g_pad), SDL_GetGamepadProduct(g_pad));
    Log::Write("PAD", m);

    LogMappingCoverage();

    // Prime on the next poll: whatever is held right now is masked until released.
    g_primeMask  = 0xFFFF;
    g_primeStick = true;
}

void OpenFirstGamepad() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) return;
    if (count > 0) OpenPad(ids[0]);
    SDL_free(ids);
}

// Drain SDL's event queue. Nothing else in the mod uses it, so this both serves hot-plug and stops
// the queue growing without bound -- SDL_PumpEvents would otherwise keep filling it forever.
void DrainEvents() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_GAMEPAD_ADDED) {
            if (!g_pad) OpenPad(ev.gdevice.which);
        } else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
            if (g_pad && ev.gdevice.which == g_padId) {
                Log::Write("PAD", "controller disconnected -- pad features inactive until one is "
                                  "plugged back in");
                ClosePad();
                OpenFirstGamepad();     // a second pad may still be attached
            }
        }
    }
}

} // namespace

bool Init() {
    if (g_sdlUp) return true;

    // HINTS, set before the subsystem starts.
    //
    // ENHANCED REPORTS OFF. On a DualSense this is what stops SDL putting the pad into its extended
    // report mode. That mode is a WRITE to the device, it changes what every OTHER reader sees, and
    // the game is another reader -- turning it on here could alter how FFXII's own DirectInput read
    // behaves. The mod does not need rumble, the touchpad or the gyro, so it does not pay that.
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "0");

    // A separate thread for device detection and RawInput messages (SDL's own default, set
    // explicitly because it matters here): device arrival must never be discovered on the game's
    // input-poll thread. See `L-88`.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");

    // BACKGROUND EVENTS DELIBERATELY LEFT OFF (the default). SDL gates them on
    // `SDL_HasWindows() && !focus`, and this DLL never creates an SDL window, so SDL_HasWindows() is
    // false and events arrive regardless. The focus decision is therefore ours alone and it is
    // already made where it belongs: PadRouter::OnPoll returns on InputTracker::GameForeground().

    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        char m[192];
        snprintf(m, sizeof(m), "SDL_InitSubSystem(GAMEPAD) failed: %s -- controller support "
                               "inactive this session", SDL_GetError());
        Log::Write("PAD", m);
        return false;
    }
    g_sdlUp = true;

    OpenFirstGamepad();
    if (!g_pad)
        Log::Write("PAD", "SDL3 gamepad subsystem up; no controller attached yet (hot-plug is "
                          "handled -- plug one in and it will be picked up)");
    return true;
}

void Shutdown() {
    if (!g_sdlUp) return;
    ClosePad();
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    g_sdlUp = false;
}

bool Available() { return g_sdlUp && g_pad != nullptr; }

const char* TypeName() { return g_typeName; }

bool DriveGame() {
    return g_sdlUp && g_pad != nullptr && ModMenu::ControllerOn();
}

void VendorProduct(uint16_t* vid, uint16_t* pid) {
    if (vid) *vid = g_pad ? SDL_GetGamepadVendor(g_pad)  : 0;
    if (pid) *pid = g_pad ? SDL_GetGamepadProduct(g_pad) : 0;
}

// THE STATE THE GAME READS. Built from SDL, minus the router's claim -- see the header.
//
// THE ONE SOURCE BOTH ENCODERS DRAW FROM. `BuildGameState` packs this into an `XINPUT_STATE` and
// `dinput8_proxy.cpp` packs the same bytes into a `DIJOYSTATE2`; neither reads a device and neither
// makes its own consumption decision. If a third wire format ever appears it hangs off here too.
bool GameButtons(PadHook::Gamepad* out) {
    if (!out || !DriveGame()) return false;

    PadHook::Gamepad g = g_gameState;
    g.buttons = static_cast<uint16_t>(g.buttons & ~g_consumed.load(std::memory_order_relaxed));
    if (g_eatStick.load(std::memory_order_relaxed)) { g.thumbRX = 0; g.thumbRY = 0; }

    *out = g;
    return true;
}

bool BuildGameState(PadHook::State* out) {
    if (!out) return false;

    PadHook::Gamepad g{};
    if (!GameButtons(&g)) return false;

    // XInput callers detect "something changed" from the packet number, so it must move when the
    // contents do and hold still when they do not. FFXII relies on exactly this: its XInput device
    // (`FUN_007a2140`) unpacks the state ONLY when the packet differs from the one it kept last
    // frame. Two callers a frame is harmless -- the second finds the contents unchanged and does not
    // advance it.
    static PadHook::Gamepad s_last{};
    if (memcmp(&g, &s_last, sizeof(g)) != 0) { s_last = g; ++g_gamePacket; }

    out->packet = g_gamePacket;
    out->pad    = g;
    return true;
}

void PollIfStale() {
    if (!g_sdlUp) return;

    const uint64_t now = SDL_GetTicks();
    if (now - g_lastPollMs < kPollIntervalMs) return;
    g_lastPollMs = now;

    SDL_PumpEvents();
    DrainEvents();
    if (!g_pad) return;

    // ---- read the pad, in the mod's own XInput-shaped struct ------------------------------------
    // PadRouter has always taken a PadHook::State and it still does. What changed is where the
    // struct comes from: this is a snapshot the MOD owns, not the game's buffer, so writing the
    // consume mask into it touches nothing the game will read. The clearing that used to happen
    // here now happens in the suppressor hooks, against whichever buffer the game actually polls.
    PadHook::State st{};

    uint16_t buttons = 0;
    for (int i = 0; i < kButtonCount; ++i)
        if (SDL_GetGamepadButton(g_pad, kButtons[i].sdl)) buttons |= kButtons[i].bit;

    // Priming: drop anything still held from the moment the pad opened, and retire it from the mask
    // the instant it comes up.
    if (g_primeMask) {
        g_primeMask &= buttons;          // released bits leave the mask for good
        buttons = static_cast<uint16_t>(buttons & ~g_primeMask);
    }

    st.pad.buttons = buttons;

    const short lx = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX);
    const short ly = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY);
    short       rx = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTX);
    short       ry = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTY);

    // SDL's Y axis is positive-DOWN; XINPUT_GAMEPAD's is positive-UP, and pad_router.cpp reads the
    // XInput convention (`ry > 0` is Up). Negate so the router's own thresholds keep their meaning.
    const short lyx = static_cast<short>(ly == -32768 ? 32767 : -ly);
    const short ryx = static_cast<short>(ry == -32768 ? 32767 : -ry);

    // Stick priming: a right stick already deflected when the pad opened is reported centred until
    // it comes back to near-centre once, so an open mid-deflection fires no direction.
    if (g_primeStick) {
        const int ax = rx < 0 ? -rx : rx, ay = ry < 0 ? -ry : ry;
        if (ax < 8000 && ay < 8000) g_primeStick = false;
        else { rx = 0; ry = 0; }
    }

    st.pad.thumbLX = lx;
    st.pad.thumbLY = lyx;
    st.pad.thumbRX = g_primeStick ? 0 : rx;
    st.pad.thumbRY = g_primeStick ? 0 : ryx;

    st.pad.leftTrigger  = static_cast<uint8_t>(
        SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    st.pad.rightTrigger = static_cast<uint8_t>(
        SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);

    // ---- what the GAME will be given ------------------------------------------------------------
    // Captured BEFORE the router runs, so it is the player's real input. `BuildGameState` subtracts
    // the router's claim from this when the XInput hook asks for it. Storing the post-router copy
    // instead would work for a button claimed on this very poll and fail for one still being held,
    // which is the whole reason the claim is latched -- keep the two concerns apart.
    g_gameState = st.pad;

    // ---- run the scheme -------------------------------------------------------------------------
    const uint16_t before  = st.pad.buttons;
    const short    beforeX = st.pad.thumbRX, beforeY = st.pad.thumbRY;

    PadRouter::OnPoll(0, &st);

    // Whatever the router cleared from its own copy is what the suppressors must clear from the
    // game's. Derived by comparison rather than by a new router out-parameter, so the router keeps
    // the single consume path it already has and cannot claim one thing and report another.
    const uint16_t claimedNow = static_cast<uint16_t>(before & ~st.pad.buttons);

    // Latch it, then drop anything the player has let go of. `buttons` is the post-priming state the
    // router actually saw, so a primed button can never enter the latch. The published mask is the
    // union, which is what makes it correct at the game's read rate rather than only at ours.
    g_heldConsume |= claimedNow;
    g_heldConsume &= buttons;
    g_consumed.store(g_heldConsume, std::memory_order_relaxed);
    g_eatStick.store((beforeX != 0 || beforeY != 0) && st.pad.thumbRX == 0 && st.pad.thumbRY == 0,
                     std::memory_order_relaxed);
}

} // namespace GamepadSDL

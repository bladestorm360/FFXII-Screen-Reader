#include "input/pad_normal.h"
#include "input/pad_hook.h"
#include "ui/mod_menu.h"

#include <Windows.h>

namespace PadRouter {

namespace {

struct DpadBind { uint16_t bit; int vk; const char* name; };

// THE PARTY. Clockwise from Up: 1, 2, 3, then the guest, one rule rather than four positions.
const DpadBind kParty[] = {
    { PadHook::kDpadUp,    '4', "party 1 (4)" },
    { PadHook::kDpadRight, '5', "party 2 (5)" },
    { PadHook::kDpadDown,  '6', "party 3 (6)" },
    { PadHook::kDpadLeft,  '7', "guest (7)"   },
};

// THE PATHFINDER ON THE D-PAD (S194, right-stick camera row on). The right stick's own field layout,
// direction for direction, so a player who switches the row on keeps the map they already know.
const DpadBind kPathfinder[] = {
    { PadHook::kDpadUp,    VK_OEM_MINUS, "previous category (-)" },
    { PadHook::kDpadDown,  VK_OEM_PLUS,  "next category (=)"     },
    { PadHook::kDpadLeft,  VK_OEM_4,     "previous object ([)"   },
    { PadHook::kDpadRight, VK_OEM_6,     "next object (])"       },
};

// Dispatch and CLAIM -- the D-pad on a surface the mod owns.
uint16_t Claim(const DpadBind (&table)[4], uint16_t rising, Context ctx) {
    uint16_t consume = 0;
    for (const DpadBind& m : table) {
        if (rising & m.bit) {
            Act(PadHook::ButtonName(m.bit), m.vk, m.name, ctx);
            consume |= m.bit;
        }
    }
    return consume;
}

} // namespace

uint16_t NormalBindings(Context ctx, uint16_t rising, const bool (&stickRising)[DIR_COUNT],
                        bool* eatStick) {
    uint16_t consume = 0;
    const bool onField = (ctx == Context::Field);
    const bool fight   = (ctx == Context::Battle);
    const bool live    = onField || fight;   // the player is driving a body

    // Read once per poll: a relaxed atomic load, safe on this thread. See pad_normal.h.
    const bool camera = ModMenu::SettingOn(ModMenu::SettingId::RightStickCamera);

    // ---- right stick ----------------------------------------------------------------------------
    // WITH THE CAMERA ROW ON, a live surface gets NOTHING from the stick: no dispatch, no swallow.
    // Dispatching while the player turns would read out a category on every sweep of the camera, and
    // swallowing it is the very thing the row exists to undo.
    if (!(camera && live)) {
        // THE RIGHT STICK IS THE PATHFINDER, with ONE context-gated direction. Up is `o` wherever a
        // description could be read -- a menu, a message box, a battle with a target under the
        // cursor -- and falls back to the category cycle on a plain idle field, which is the only
        // surface where `o` has nothing to say and the pathfinder has everything. The user's call,
        // and the reason this direction is the only one that moves.
        const int   upVk   = onField ? VK_OEM_MINUS : 'O';
        const char* upName = onField ? "previous category (-)" : "describe / Libra (o)";
        const int   kStickVk[DIR_COUNT] = { upVk, VK_OEM_PLUS, VK_OEM_4, VK_OEM_6 };
        const char* kStickName[DIR_COUNT] = {
            upName, "next category (=)", "previous object ([)", "next object (])"
        };
        for (int d = 0; d < DIR_COUNT; ++d) {
            if (stickRising[d]) Act(StickDirName(d), kStickVk[d], kStickName[d], ctx);
        }
        // Consumption is unchanged from the shipped build: the field camera only. Off the field the
        // stick is read and passed through, so nothing the game does with it is taken away.
        *eatStick = onField;
    }

    // ---- L1 AND R1: ASK, AND GO (Session 185) ---------------------------------------------------
    //
    // L1 = `;`  -- what am I about to interact with. Out of combat that is the object the game's own
    //              interaction scorer has picked, which is not always the one the mod's list has
    //              focused; in a fight it is the enemy, with its HP.
    // R1 = `\`  -- route to the current selection, and start the beacon.
    //
    // R1 NO LONGER CHANGES MEANING IN A FIGHT, and that was the user's call with a reason behind it:
    // the thing a player most needs a route for mid-combat is a way OUT. Sending R1 to `p` in battle
    // meant the one context where escaping matters was the one context where the route key routed to
    // the enemy instead. `p` did not lose its pad home -- it moved to mod + Y, where asking for the
    // target's bearing is a deliberate question rather than the default.
    //
    // L1 IS TAKEN FROM THE GAME, KNOWINGLY. It was Speed mode (x2 / x4). The user's ruling: game speed
    // is reachable from the options menu and from the keyboard's `1`, pad buttons are scarce, and a
    // speed toggle is not what one should be spent on.
    //
    // BOTH ARE GATED ON `live`, WHICH IS WHAT KEEPS THE BATTLE TARGET LIST WORKING. With a targeting
    // cursor up the context is FieldBusy, not Battle, so neither shoulder is touched and the game keeps
    // L1 and R1 as the target list's group step -- the Foes / Party / Reserve / Allies switch S184
    // built the spoken titles for. Taking them there would have silenced a feature to feed another.
    if (live && (rising & PadHook::kLeftShoulder)) {
        Act("L1", VK_OEM_1, "target readout (;)", ctx);
        consume |= PadHook::kLeftShoulder;
    }
    if (live && (rising & PadHook::kRightShoulder)) {
        Act("R1", VK_OEM_5, "route + beacon (\\)", ctx);
        consume |= PadHook::kRightShoulder;
    }

    // ---- D-pad ----------------------------------------------------------------------------------
    if (onField) {
        // Row off: THE PARTY, and ONLY on a field the player is driving. S174 narrowed this from
        // `live` to `onField` at the user's instruction: the D-pad is how a pad moves a cursor, and
        // party slots are not worth costing the player a battle menu.
        // Row on: the pathfinder the stick gave up. The party moves to the fight, below.
        consume |= Claim(camera ? kPathfinder : kParty, rising, ctx);
    } else if (fight && camera) {
        // Row on, in a fight with NO menu up (a menu makes the context FieldBusy). There is no cursor
        // here for the D-pad to move, which is the only reason S174's ruling can bend for this.
        //
        // ESCAPE MODE OVERRIDES IT (S194, the user: *"escape mode must overwrite the combat switch for
        // the d-pad when in combat and fall back to pathfinding"*). A fleeing party needs somewhere to
        // run TO, not its HP list -- the rule S179 already applies to the beacon, which always resumes
        // the route in escape mode. With the row off nothing changes: the stick is the pathfinder then.
        consume |= Claim(Escaping() ? kPathfinder : kParty, rising, ctx);
    } else {
        // In a menu the D-pad is the GAME'S, and it is also an arrow key. Dispatched and NOT consumed:
        // the Status attributes buffer and the Clan Primer page walk hear it exactly as they hear the
        // keyboard's arrows, and the game's own cursor is untouched. Where no buffer is open the mod
        // does nothing at all with it.
        static const DpadBind kArrow[] = {
            { PadHook::kDpadUp,    VK_UP,    "buffer arrow" },
            { PadHook::kDpadDown,  VK_DOWN,  "buffer arrow" },
            { PadHook::kDpadLeft,  VK_LEFT,  "buffer arrow" },
            { PadHook::kDpadRight, VK_RIGHT, "buffer arrow" },
        };
        for (const DpadBind& m : kArrow) {
            if (rising & m.bit) Act(PadHook::ButtonName(m.bit), m.vk, m.name, ctx);
        }
    }
    return consume;
}

} // namespace PadRouter

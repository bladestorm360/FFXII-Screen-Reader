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

// THE TARGET GATE (S196): a fight with no menu up, and the party NOT fleeing. It is the one test that
// flips R1 to the target and the camera-row D-pad to the party; escape mode fails it, so both fall
// back to the pathfinder -- a party running away needs somewhere to run TO. One predicate, so the two
// controls cannot drift apart (`L-71`).
bool TargetGate(Context ctx) { return ctx == Context::Battle && !Escaping(); }

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
    // R1 = `\`  -- on the field, route to the current selection and start the beacon.
    //      `p`  -- in a fight with no menu up, directions to the target the party is on.
    //      `\`  -- in a fight in ESCAPE MODE: the route again, so a fleeing party can pick where to.
    //
    // R1 CHANGES MEANING IN A FIGHT AGAIN (S196, the user's: *"in combat, it should announce
    // directions to target ... if escape mode is toggled, it should fall back to pathfinder. so
    // effectively, same context gate as the d-pad with right stick camera on."*). S185 had pinned it
    // to `\` everywhere because the route a player most needs mid-combat is a way OUT; escape mode is
    // what answers that now -- the game's own flee flag says when the player wants out. That was also
    // why `p` lived on mod + Y, and it does not any more: R1 is its pad home. The gate is TargetGate
    // above, and it applies whichever way the camera row is set.
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
        if (TargetGate(ctx)) Act("R1", 'P', "directions to target (p)", ctx);
        else                 Act("R1", VK_OEM_5, "route + beacon (\\)", ctx);
        consume |= PadHook::kRightShoulder;
    }

    // ---- D-pad ----------------------------------------------------------------------------------
    // THE NORMAL D-PAD ROW (S195, the user's) hands it back to the game on a live surface -- the game
    // chooses the party leader with it -- and it wins over the camera row: nothing dispatched, nothing
    // consumed, so the game reads the D-pad exactly as it would unmodded. Menus are the `else` below
    // and already pass it through, so the row changes nothing there.
    const bool gameDpad = ModMenu::SettingOn(ModMenu::SettingId::NormalDpad);
    if (live && gameDpad) {
        // The game's. Deliberately no arrow dispatch either: no buffer the arrows feed is open on a
        // live field or in a fight, and "the mod does nothing with it" is what the row promises.
    } else if (live && !camera) {
        // Camera row off: THE PARTY, on the field AND in a fight with no menu up (S195, the user: *"if
        // it is off and the right stick is used for pathfinding, the combat context does not apply and
        // it is always to be used for checking vitals"*). The field/fight switch and escape mode's
        // override below belong to the camera row only, because only there does the D-pad carry the
        // pathfinder. S174's narrowing to the field alone was about a battle MENU's cursor, and a
        // menu makes the context FieldBusy -- `live` is never true while one is up.
        consume |= Claim(kParty, rising, ctx);
    } else if (onField) {
        // Camera row on, open field: the pathfinder the stick gave up. The party moves to the fight.
        consume |= Claim(kPathfinder, rising, ctx);
    } else if (fight) {
        // Camera row on, in a fight with NO menu up (a menu makes the context FieldBusy). There is no
        // cursor here for the D-pad to move.
        //
        // ESCAPE MODE OVERRIDES IT (S194, the user: *"escape mode must overwrite the combat switch for
        // the d-pad when in combat and fall back to pathfinding"*). A fleeing party needs somewhere to
        // run TO, not its HP list -- the rule S179 already applies to the beacon, which always resumes
        // the route in escape mode. With the camera row off none of this applies: the D-pad is the
        // party and the stick is the pathfinder. R1 makes the same switch through the same gate.
        consume |= Claim(TargetGate(ctx) ? kParty : kPathfinder, rising, ctx);
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

#include "input/pad_router.h"
#include "input/input_tracker.h"
#include "input/pad_survey.h"
#include "core/logger.h"
#include "ui/mod_menu.h"
#include "ui/dialogue_reader.h"
#include "ui/ingame_menu_reader.h"
#include "navigation/player_state.h"
#include "battle/battle_state.h"
#include "speech/phrasebook.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>

namespace {

// ---- the published, stamped context (written game thread, read input thread) --------------------
std::atomic<uint8_t>  g_ctx{static_cast<uint8_t>(PadRouter::Context::Unknown)};
std::atomic<uint64_t> g_ctxStampMs{0};
std::atomic<uint8_t>  g_state{static_cast<uint8_t>(PadRouter::State::Normal)};

// ---- mod mode ----------------------------------------------------------------------------------
// A LATCH, not a hold. Press Back and the mod says "Mod"; the NEXT button is a mod command and the
// mode ends. It is not a held modifier because holding one button while pressing another is an
// awkward grip on a pad whose thumbs are already on two sticks, and a latch is one button at a time,
// which is the whole ergonomic argument for a pad in the first place.
//
// S174 moved the latch from L3 to Back. A stick click cannot be reached without taking the thumb off
// the stick it is steering with, which is why the entire mode went a session without ever being
// tried -- the player simply never pressed it. Back costs the game's map toggle; that trade was
// taken deliberately, and L3 now buys the whole pad back in one press instead.
//
// EVERY EXIT SPEAKS. "Mod" on arming, "Cancelled" on the modifier pressed twice, on an unmapped
// button, and on the timeout. A silent mode is a mode a blind player is stuck in without knowing it,
// which is exactly the failure Controls.md's "no mode to get stuck in" rule was written against.
std::atomic<uint64_t> g_modArmedMs{0};
constexpr uint64_t kModModeMs = 5000;   // generous: the player may be listening to something first

// A verdict older than this is not a verdict. The field tick stopping IS the signal that the player
// is no longer on a live field -- map change, pause, stall -- so consumption must end on its own
// rather than waiting for something to notice. Same 250 ms AutoWalk expires its injection mask on.
constexpr uint64_t kCtxStaleMs = 250;

// ---- right-stick cardinal isolation -------------------------------------------------------------
// A stick is analogue and a category cycle is not, so the stick has to become four edge-triggered
// directions. Thresholds are the FFPR mods', play-proven on this same job: cross 16000 to arm, and
// beat the other axis by 8000 so a diagonal picks ONE direction instead of firing both.
//
// Release is at a LOWER value than arm on purpose. Without that hysteresis a stick resting near the
// threshold chatters, and every chatter here is a spoken line.
constexpr int kStickArm       = 16000;
constexpr int kStickRelease   = 10000;
constexpr int kStickDominance = 8000;

enum Dir { DIR_UP = 0, DIR_DOWN, DIR_LEFT, DIR_RIGHT, DIR_COUNT };

const char* const kDirName[DIR_COUNT] = {
    "R-stick-Up", "R-stick-Down", "R-stick-Left", "R-stick-Right"
};

struct PadEdges {
    uint16_t prevButtons = 0;
    bool     prevStick[DIR_COUNT] = {};
};
// XInput reports up to 4 pads. Per-index edges, so a second connected pad cannot alias the first
// one's history.
PadEdges g_edges[4];

// Fresh context, or Unknown when the game thread has gone quiet.
PadRouter::Context FreshContext() {
    const uint64_t stamp = g_ctxStampMs.load(std::memory_order_acquire);
    if (stamp == 0) return PadRouter::Context::Unknown;
    const uint64_t now = GetTickCount64();
    if (now >= stamp && (now - stamp) > kCtxStaleMs) return PadRouter::Context::Unknown;
    return static_cast<PadRouter::Context>(g_ctx.load(std::memory_order_relaxed));
}


// The survey log lives in `pad_survey.cpp` -- it answers what the GAME does with a control, which
// is a different question from what the mod makes it mean, and this file was over 500 lines.

// ---- dispatch ----------------------------------------------------------------------------------
//
// EVERY pad action goes through here, and here does exactly one thing: hand a VIRTUAL-KEY CODE to
// `InputTracker::DispatchModKey`. The pad reimplements no feature and knows no handler -- it names
// the same key the keyboard names, and the tracker routes both into the same callback. That is
// CLAUDE.md's centralization rule, and it is also why a future mod key is reachable from the pad by
// adding one line to a table here rather than by touching any reader.
void Act(const char* padInput, int vk, const char* action, PadRouter::Context ctx) {
    InputTracker::DispatchModKey(vk);
    char m[128];
    snprintf(m, sizeof(m), "%s -> %s ctx=%s", padInput, action, PadRouter::ContextName(ctx));
    Log::Write("PAD", m);
}

void Say(Phrase::Id id) { InputTracker::DispatchSpeakPhrase(static_cast<int>(id)); }

// MOD MODE'S TABLE. Returns the virtual key for a button pressed while the mode is armed, or 0 for
// "not mapped" -- which cancels, rather than doing nothing, so an unmapped press is never silent.
//
// These are the actions with no home in Normal mode: the ones that would need a face button, and
// the face buttons are the game's core verbs.
//
// A SETTING WITH A MOD-MENU ROW GETS NO PAD BUTTON. Every entry below ASKS the mod something or
// MOVES somewhere -- a readout, a route, a step through the log. Nothing here flips a switch,
// because a switch is already two presses away through Start, and the menu says what it changed and
// what the new value means. `F4` combat verbosity and `F11` audio beacon were bound to L1 and R1 for
// exactly one session; the user's rule (2026-08-28) struck both. The keyboard keeps its shortcuts --
// they cost nothing there -- but a pad button is scarce and a duplicate route is not what to spend
// one on. The same rule pre-emptively excludes `F5`, `F7` and the volumes.
//
// R1 and both stick clicks are deliberately left UNMAPPED rather than refilled. An unmapped button
// in mod mode says "Cancelled", which is a truthful answer; inventing a use for a free button is how
// a scheme grows bindings nobody asked for.
//
// S174 moved two entries, and neither was a free choice:
//   * `Back` used to be the target readout. It is now the LATCH itself, so in here it has to mean
//     cancel -- the "press the modifier twice to back out" gesture that was L3's. The readout moved
//     to L1, the only button left that a mod-mode press can reach.
//   * `R3` used to be route-to-target. It is gone from this table because the route now has a
//     one-press home in Normal, where it is actually wanted: `R1` in a fight. A player mid-combat
//     was never going to reach it through a two-press latch, which is what made burying it wrong.
int ModModeKeyFor(uint16_t bit, const char** nameOut) {
    switch (bit) {
        case PadHook::kStart:         *nameOut = "mod menu (F8)";        return VK_F8;
        case PadHook::kA:             *nameOut = "describe / Libra (o)"; return 'O';
        case PadHook::kB:             *nameOut = "re-read line (t)";     return 'T';
        case PadHook::kX:             *nameOut = "rescan + area (`)";    return VK_OEM_3;
        case PadHook::kY:             *nameOut = "describe target (/)";  return VK_OEM_2;
        case PadHook::kDpadUp:        *nameOut = "License Points (U)";   return 'U';
        case PadHook::kDpadDown:      *nameOut = "gil (g)";              return 'G';
        case PadHook::kDpadLeft:      *nameOut = "combat log older (,)"; return VK_OEM_COMMA;
        case PadHook::kDpadRight:     *nameOut = "combat log newer (.)"; return VK_OEM_PERIOD;
        case PadHook::kLeftShoulder:  *nameOut = "target readout (;)";   return VK_OEM_1;
        default:                      *nameOut = nullptr;                return 0;
    }
}

} // namespace

namespace PadRouter {

// Log-facing names, shared by this file's dispatch lines and by `pad_survey.cpp`. One spelling of
// each, so a context or a direction never reads two ways across two files.
const char* ContextName(Context c) {
    switch (c) {
        case Context::OffField:  return "offfield";
        case Context::Field:     return "field";
        case Context::FieldBusy: return "fieldbusy";
        case Context::Battle:    return "battle";
        default:                 return "unknown";
    }
}

const char* StickDirName(int dir) {
    return (dir >= 0 && dir < DIR_COUNT) ? kDirName[dir] : "?";
}

void OnGameFrame() {
    Context c;
    if (!PlayerState::IsFieldActive() || !PlayerState::IsFieldNavSafe()) {
        // Either off the field entirely, or flagged live but not settled (still fading in, walkmap
        // not up). Neither is a surface to start consuming input on.
        c = Context::OffField;
    } else if (IngameMenuReader::BattleCommandActive() || DialogueReader::IsBoxLive()) {
        // The two gates the audio beacon already trusts. Deliberately NOT
        // MenuState::IsAnyMenuOpen(): it reads a global the decompile writes once and never clears,
        // so it answers "open" forever -- a gate built on it once killed the field object scan for
        // an entire fight.
        c = Context::FieldBusy;
    } else if (BattleState::PartyEngagement().engaged) {
        c = Context::Battle;
    } else {
        c = Context::Field;
    }
    g_ctx.store(static_cast<uint8_t>(c), std::memory_order_relaxed);
    g_ctxStampMs.store(GetTickCount64(), std::memory_order_release);
}

Context CurrentContext() { return FreshContext(); }
State   CurrentState()   { return static_cast<State>(g_state.load(std::memory_order_relaxed)); }

void OnPoll(uint32_t userIndex, PadHook::State* state) {
    if (!state || userIndex >= 4) return;

    // The mod never acts on input aimed at another window. THIS is now the first gate, above the off
    // switch: `L3` must not flip the intercept because the player pressed a stick click in another
    // application while the game sat in the background.
    if (!InputTracker::GameForeground()) return;

    PadEdges& e = g_edges[userIndex];
    const uint16_t buttons = state->pad.buttons;
    const Context  ctx     = FreshContext();

    // ---- edges, computed ABOVE the off switch (S174) --------------------------------------------
    // Edge history is kept current whether or not the intercept is on. It used to be computed after
    // the gates, so `prevButtons` went stale while the intercept was off or the game was in the
    // background -- and a button still held on the way back in then read as a fresh press.
    const uint16_t rising = static_cast<uint16_t>(buttons & ~e.prevButtons);
    e.prevButtons = buttons;

    const int rx = state->pad.thumbRX;
    const int ry = state->pad.thumbRY;
    const int ax = rx < 0 ? -rx : rx;
    const int ay = ry < 0 ? -ry : ry;
    const int vArm = kStickArm, vRel = kStickRelease;

    bool stick[DIR_COUNT] = {};
    const bool vHeld = e.prevStick[DIR_UP]   || e.prevStick[DIR_DOWN];
    const bool hHeld = e.prevStick[DIR_LEFT] || e.prevStick[DIR_RIGHT];
    if (ay >= (vHeld ? vRel : vArm) && ay > ax - kStickDominance) {
        if (ry > 0) stick[DIR_UP] = true; else stick[DIR_DOWN] = true;
    }
    if (ax >= (hHeld ? vRel : vArm) && ax > ay - kStickDominance) {
        if (rx > 0) stick[DIR_RIGHT] = true; else stick[DIR_LEFT] = true;
    }
    bool stickRising[DIR_COUNT] = {};
    for (int d = 0; d < DIR_COUNT; ++d) {
        stickRising[d] = stick[d] && !e.prevStick[d];
        e.prevStick[d] = stick[d];
    }

    // ---- L3: the intercept's own kill switch (S174) ---------------------------------------------
    //
    // WHY IT IS ABOVE THE OFF SWITCH. A kill switch that lives below its own gate can only be thrown
    // once: with the Controller row off, `OnPoll` used to return on its first line, so no pad button
    // could ever turn it back on and the player had to reach the keyboard. The escape hatch has to
    // work in BOTH directions, so the check runs here -- above the gate, below nothing else.
    //
    // WHAT "OFF" STILL GUARANTEES. Off means the game's pad state is untouched, and that is intact:
    // this prologue only READS, and the consume below is skipped entirely when the intercept is
    // already off. So while off, the mod costs the game one bit test per poll and changes nothing it
    // reads -- L3 passes straight through and the game toggles its area map, which is the honest
    // price of the button. CLAUDE.md's "returns on its first line" wording is now the gate three
    // lines down, not this one, and Docs/GameArchitecture.md records the amendment.
    //
    // L3 is the right button for a toggle and the wrong one for anything else: the user's own
    // verdict is that stick clicks are too awkward for normal play, which is exactly why nothing
    // time-critical lives here.
    if (rising & PadHook::kLeftThumb) {
        const bool wasOn = ModMenu::ControllerOn();
        InputTracker::DispatchToggleController();
        // A pad the mod is handing back must not leave a latch armed behind it. The spoken
        // "Controller, Off" is the feedback, so this needs no separate "Cancelled".
        g_modArmedMs.store(0, std::memory_order_relaxed);
        Log::Write("PAD", wasOn ? "L3 -> intercept OFF (the pad is the game's again)"
                                : "L3 -> intercept ON");
        if (wasOn) state->pad.buttons = static_cast<uint16_t>(buttons & ~PadHook::kLeftThumb);
        return;
    }

    // THE OFF SWITCH. With the Controller setting off this returns having read the pad and written
    // nothing, so what the game reads is byte-identical to the mod as it shipped with no pad hook at
    // all. Same bound Auto-walk carries, and the only line of it that moved is this one.
    if (!ModMenu::ControllerOn()) return;

    // ---- survey ---------------------------------------------------------------------------------
    // It measures the controls the mod does NOT take, and it runs on everything -- including what
    // this poll is about to claim, so a binding can be checked against what the game wanted the
    // button for. See `pad_survey.cpp`, which also explains why its lines are a poll behind.
    PadSurvey::OnPoll(userIndex, buttons, rising, stickRising, ctx);

    // ---- THE SCHEME ----------------------------------------------------------------------------
    //
    // Three surfaces, checked in this order, and only one of them ever runs: the mod's own menu owns
    // the pad while it is open; mod mode owns the NEXT button once armed; otherwise Normal.
    //
    // WHAT GETS CONSUMED IS DECIDED BY CONTEXT, NOT BY BUTTON. On a live field the mod takes the
    // right stick, the D-pad and R1 outright, because there it is confident what they mean. In a
    // menu it takes NOTHING and merely listens -- the D-pad still reaches the game's cursor, and the
    // mod's virtual buffers hear the same arrow the keyboard would have sent. That mirrors the
    // keyboard exactly, which cannot swallow a key at all (Docs/Controls.md), so the two devices
    // behave the same way on the same screen.
    //
    // WHY A MENU NEEDS NO DETECTOR. `OnGameFrame` stops being called when the field tick stops, and
    // the party menu is one of the places it stops (S157). The 250 ms stamp therefore expires by
    // itself and `ctx` falls to Unknown -- so "we are in a game menu" arrives for free, without
    // MenuState::IsAnyMenuOpen(), which is unusable (1 write, 0 clears).
    uint16_t consume  = 0;        // button bits cleared from what the game is about to read
    bool     eatStick = false;    // right stick zeroed for the game

    const uint64_t now         = GetTickCount64();
    const bool     modMenuOpen = ModMenu::IsOpen();
    const uint64_t armedAt     = g_modArmedMs.load(std::memory_order_relaxed);
    bool           modArmed    = armedAt != 0;

    if (modArmed && (now - armedAt) > kModModeMs) {
        g_modArmedMs.store(0, std::memory_order_relaxed);
        modArmed = false;
        Say(Phrase::Id::ModCancelled);
        Log::Write("PAD", "mod mode expired unused");
    }

    if (modMenuOpen) {
        // ---- the mod's own menu ----------------------------------------------------------------
        // It is a modal overlay drawn over whatever the player was doing, so here the pad IS taken:
        // the game must not act on the same press that moved the menu. `o` reads the focused
        // setting's description; B and Start both close, because the two habits are equally common
        // and neither costs the menu anything.
        g_state.store(static_cast<uint8_t>(State::ModMenu), std::memory_order_relaxed);
        struct MenuBind { uint16_t bit; int vk; const char* name; };
        static const MenuBind kMenu[] = {
            { PadHook::kDpadUp,    VK_UP,    "mod menu up"      },
            { PadHook::kDpadDown,  VK_DOWN,  "mod menu down"    },
            { PadHook::kDpadLeft,  VK_LEFT,  "mod menu left"    },
            { PadHook::kDpadRight, VK_RIGHT, "mod menu right"   },
            { PadHook::kA,         'O',      "read description" },
            { PadHook::kB,         VK_F8,    "close mod menu"   },
            { PadHook::kStart,     VK_F8,    "close mod menu"   },
            // Back closes as well. Everywhere else it is now the mod-mode latch, so a player will
            // press it here expecting the mod to answer; letting it fall through would open the
            // game's map behind the overlay instead.
            { PadHook::kBack,      VK_F8,    "close mod menu"   },
        };
        for (const MenuBind& m : kMenu) {
            if (rising & m.bit) {
                Act(PadHook::ButtonName(m.bit), m.vk, m.name, ctx);
                consume |= m.bit;
            }
        }
        // The right stick drives it too, so a thumb already resting there never has to move.
        static const int kMenuStickVk[DIR_COUNT] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
        for (int d = 0; d < DIR_COUNT; ++d) {
            if (stickRising[d]) { Act(kDirName[d], kMenuStickVk[d], "mod menu", ctx); eatStick = true; }
        }
    } else if (modArmed) {
        // ---- mod mode: the next button, then out ------------------------------------------------
        // The stick is deliberately ignored here rather than treated as a cancel -- a thumb resting
        // against a live stick would end the mode before the player pressed anything.
        g_state.store(static_cast<uint8_t>(State::ModMode), std::memory_order_relaxed);
        if (rising) {
            // Lowest set bit wins, so a two-button fumble resolves to one action rather than several.
            const uint16_t bit = static_cast<uint16_t>(rising & (~rising + 1));
            const char* action = nullptr;
            const int vk = ModModeKeyFor(bit, &action);
            if (vk) {
                Act(PadHook::ButtonName(bit), vk, action, ctx);
            } else {
                Say(Phrase::Id::ModCancelled);
                char m[96];
                snprintf(m, sizeof(m), "mod mode cancelled on %s (not mapped)",
                         PadHook::ButtonName(bit));
                Log::Write("PAD", m);
            }
            // Consume the WHOLE rising set, not just the bit that resolved: every button of a fumble
            // was aimed at the mod, and letting the others through would fire a game verb the player
            // never meant. Back cancelling itself falls out of this too -- it maps to nothing.
            consume |= rising;
            g_modArmedMs.store(0, std::memory_order_relaxed);
        }
    } else {
        // ---- Normal ------------------------------------------------------------------------------
        g_state.store(static_cast<uint8_t>(State::Normal), std::memory_order_relaxed);
        const bool onField = (ctx == Context::Field);
        const bool live    = onField || (ctx == Context::Battle);   // the player is driving a body

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
            if (stickRising[d]) Act(kDirName[d], kStickVk[d], kStickName[d], ctx);
        }
        // Consumption is unchanged from the shipped build: the field camera only. Off the field the
        // stick is read and passed through, so nothing the game does with it is taken away.
        eatStick = onField;

        // R1 IS THE ROUTE KEY, and the SECOND context-gated control after the stick's Up. Out of
        // combat it routes to whatever the stick has selected and starts the beacon; in a fight it
        // routes to the ACTIVE TARGET instead -- the one `p` and `;` already speak for, which needs
        // no battle menu open. (Not the game's lock-on: that is L2, held, and the mod never takes it.)
        //
        // The beacon needs no branch here. Entering combat already stops the route beacon and, with
        // the Target beacon row on, hands the audio to the in-combat target ping -- which is why `p`
        // is the one route call that does not seed a beacon of its own.
        //
        // WHY R1 AND NOT A SHOULDER THAT LOOKED FREER. FFXII spends every other one: L1 is Speed
        // mode, L2 is zoom and then lock-on, R2 is map zoom and then flee, and both stick clicks the
        // player has ruled out for anything time-critical. R1 has no field job at all, and its only
        // battle job -- switching the target list to Reserve -- happens with a targeting cursor up,
        // which is `FieldBusy` here and passes through untouched.
        if (live && (rising & PadHook::kRightShoulder)) {
            const bool fighting = (ctx == Context::Battle);
            Act("R1", fighting ? 'P' : VK_OEM_5,
                      fighting ? "route to active target (p)" : "route + beacon (\\)", ctx);
            consume |= PadHook::kRightShoulder;
        }

        if (onField) {
            // THE D-PAD IS THE PARTY, and ONLY on a field the player is driving -- never in combat.
            // Clockwise from Up: 1, 2, 3, then the guest, one rule rather than four positions.
            //
            // S174 narrowed this from `live` to `onField` at the user's instruction: the D-pad is
            // how a pad moves a cursor, and a fight is one command menu away at all times. Party
            // slots are worth having; they are not worth costing the player a battle menu. In every
            // other context it falls through to the arrow passthrough below.
            struct PartyBind { uint16_t bit; int vk; const char* name; };
            static const PartyBind kParty[] = {
                { PadHook::kDpadUp,    '4', "party 1 (4)" },
                { PadHook::kDpadRight, '5', "party 2 (5)" },
                { PadHook::kDpadDown,  '6', "party 3 (6)" },
                { PadHook::kDpadLeft,  '7', "guest (7)"   },
            };
            for (const PartyBind& m : kParty) {
                if (rising & m.bit) {
                    Act(PadHook::ButtonName(m.bit), m.vk, m.name, ctx);
                    consume |= m.bit;
                }
            }
        } else {
            // In a menu the D-pad is the GAME'S, and it is also an arrow key. Dispatched and NOT
            // consumed: the Status attributes buffer and the Clan Primer page walk hear it exactly
            // as they hear the keyboard's arrows, and the game's own cursor is untouched. Where no
            // buffer is open the mod does nothing at all with it.
            struct ArrowBind { uint16_t bit; int vk; };
            static const ArrowBind kArrow[] = {
                { PadHook::kDpadUp,    VK_UP    }, { PadHook::kDpadDown,  VK_DOWN  },
                { PadHook::kDpadLeft,  VK_LEFT  }, { PadHook::kDpadRight, VK_RIGHT },
            };
            for (const ArrowBind& m : kArrow) {
                if (rising & m.bit) Act(PadHook::ButtonName(m.bit), m.vk, "buffer arrow", ctx);
            }
        }

        // BACK/SELECT ARMS MOD MODE, in every context including a menu -- the mod menu has to be
        // reachable from wherever the player is, which is the whole point of putting F8 behind it.
        //
        // S174 moved it off L3. The latch has to be reachable without the thumb leaving the stick it
        // is steering with, and a stick click is not that button -- which is why the whole mode went
        // untested for a session. Back costs the game's map toggle, and the user took that trade
        // knowingly: the map has other routes, and L3 now hands the entire pad back in one press.
        if (rising & PadHook::kBack) {
            g_modArmedMs.store(now ? now : 1, std::memory_order_relaxed);
            Say(Phrase::Id::ModMode);
            consume |= PadHook::kBack;
            Log::Write("PAD", "mod mode armed (Back)");
        }
    }

    // ---- apply ---------------------------------------------------------------------------------
    // Bits are only ever CLEARED and the axes only ever zeroed -- see CLAUDE.md's second
    // input-write exception. Nothing here can set a bit the player did not press.
    //
    // One of TWO write statements in this function since S174; the other is the L3 prologue's,
    // which clears its own bit and returns before reaching here. Between them they are still the
    // only writes to an XINPUT_STATE anywhere in the mod, which is the bound that matters.
    if (consume)  state->pad.buttons = static_cast<uint16_t>(buttons & ~consume);
    if (eatStick) { state->pad.thumbRX = 0; state->pad.thumbRY = 0; }
}

} // namespace PadRouter

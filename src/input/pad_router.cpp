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
#include "ui/text_prompt.h"

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
    // ---- the L3 + R3 chord (S185) --------------------------------------------------------------
    // `together` latches the instant both thumb-clicks are down at once and stays latched until BOTH
    // are back up, so it does not matter which one the player presses first or lets go of first.
    // `fired` stops the second release from firing the chord a second time.
    bool     thumbsTogether = false;
    bool     chordFired     = false;
    // The thumb bits as of the previous poll. Kept apart from `prevButtons` because the chord is
    // resolved on the FALLING edge and everything else in this file on the rising one.
    uint16_t prevThumbs     = 0;
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
// FOUR FACE BUTTONS AND START, AND NOTHING ELSE (Session 185, the user's scheme). The D-pad, the
// shoulders and the sticks were all stripped out of here: the D-pad and the right stick keep one
// meaning everywhere now, and L1 and R1 earned permanent Normal-mode homes, so a second meaning
// behind a latch would be a button that does two things depending on a mode the player has to
// remember they are in. An unmapped press says "Cancelled", which is the honest answer.
//
// TWO OF THE FOUR CHANGE MEANING IN A FIGHT, and both changes are the same idea: the button keeps
// the question and the context picks which subject it is about.
//   X  out of combat is the party's gil. In a fight it is the enemy readout, `;` -- the name and HP
//      of what you are up against, which is the only "how much of it is there" that matters mid-fight.
//   Y  out of combat rescans and says the area. In a fight it is `p`, the directions to the target
//      you are already acting on.
// A and B do not move, because neither question has a combat form: the Esper gauge is the Esper
// gauge, and the settings menu is the settings menu.
//
// A IS THE SUMMONED ESPER (`8`), AND IT IS SILENT WHEN THERE IS NONE. That silence is the game's own
// answer, not a dropped press -- an Esper absent from the field has no HP to read. `8` is the same
// key the keyboard uses, so the two devices cannot drift.
int ModModeKeyFor(uint16_t bit, bool fighting, const char** nameOut) {
    switch (bit) {
        case PadHook::kStart: *nameOut = "mod menu (F8)"; return VK_F8;
        // A IS NOT IN THIS TABLE, AND ITS ABSENCE IS THE POINT. Mod + A falls through to `default`,
        // which speaks "Cancelled" and ends the mode, so the modifier has exactly one opener (Start)
        // and one canceller instead of two openers and no clean way out (S187).
        //
        // WHICH BUTTON CANCELS WAS SWAPPED AT S189, to the user's instruction: it is the one the GAME
        // cancels with, so backing out of mod mode feels like backing out of anything else in FFXII.
        // A also CLOSES the mod menu, from the `modMenuOpen` branch -- same button, same meaning, two
        // contexts. The pair to it is below: B is the game's confirm, so B is what asks a question.
        case PadHook::kB:     *nameOut = "summoned Esper (8)"; return '8';
        case PadHook::kX:
            *nameOut = fighting ? "enemy name and HP (;)" : "gil (g)";
            return fighting ? VK_OEM_1 : 'G';
        case PadHook::kY:
            *nameOut = fighting ? "directions to target (p)" : "rescan + area (`)";
            return fighting ? 'P' : VK_OEM_3;
        default:              *nameOut = nullptr; return 0;
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

    // Button bits cleared from what the game is about to read. Declared HERE, above the off switch,
    // because the thumb-clicks are resolved up there and they are consumed like anything else. It is
    // only ever applied below the off switch, so a poll with the intercept off returns having
    // accumulated nothing and written nothing.
    uint16_t consume = 0;

    // ---- a prompt owns the keyboard, so it owns the pad too (S185) ------------------------------
    // The edges above are kept current on purpose -- a button still held when the box opened must not
    // read as a fresh press when it closes -- but nothing below this line may run while one of the
    // mod's own dialogs has the foreground. Same shape as the keyboard feed's zeroed buffer.
    //
    // The thumb state is carried forward first, for the same reason the button edges are computed
    // above: the chord resolves on a FALLING edge, so leaving `prevThumbs` stale across a dialog
    // would turn a click the player released while typing into a toggle the moment the box closed.
    if (TextPrompt::Busy()) {
        e.prevThumbs = static_cast<uint16_t>(buttons & (PadHook::kLeftThumb | PadHook::kRightThumb));
        e.thumbsTogether = false;
        e.chordFired = false;
        return;
    }

    // ---- THE TWO THUMB-CLICKS, AND THE CHORD THEY MAKE (S174, rebuilt S185) ----------------------
    //
    // L3 = the reachability filter. R3 = the audio beacon. BOTH AT ONCE = the intercept's own kill
    // switch, which is what L3 alone used to be.
    //
    // WHY THE SINGLES FIRE ON RELEASE. A chord and its two singles cannot all be edge-triggered on
    // the press: whichever button went down first would already have spoken by the time the second
    // one arrived. Acting on RELEASE resolves that with no timer and no guess window -- hold both,
    // let go, and the chord is what you get; click one, and that one is what you get. The player
    // feels no delay because the action lands when the thumb comes off the stick.
    //
    // THE USER'S OWN RULE MADE THIS THE RIGHT PLACE. Stick clicks are too awkward for anything
    // time-critical, so a settings toggle -- which nothing is waiting on -- is exactly what belongs
    // here, and the release-edge latency that would be unacceptable on a route key costs nothing.
    //
    // WHY IT IS ALL ABOVE THE OFF SWITCH. A kill switch that lives below its own gate can only be
    // thrown once: with the intercept off, `OnPoll` used to return on its first line, so no pad
    // button could turn it back on and the player had to reach the keyboard. The escape hatch has to
    // work in BOTH directions, so the whole block runs here.
    //
    // WHAT "OFF" STILL GUARANTEES. Off means the game's pad state is untouched, and that is intact:
    // the singles are skipped, nothing is consumed, and L3 and R3 pass straight through to the game's
    // area map and camera recentre. Only the chord answers while off, because only the chord is the
    // way back. That is the honest price of the two buttons, and it is the same bargain S174 struck
    // for L3 alone. Docs/GameArchitecture.md records the amendment.
    {
        const bool l3 = (buttons & PadHook::kLeftThumb)  != 0;
        const bool r3 = (buttons & PadHook::kRightThumb) != 0;
        if (l3 && r3) e.thumbsTogether = true;

        const uint16_t thumbFalling = static_cast<uint16_t>(
            (e.prevThumbs & ~buttons) & (PadHook::kLeftThumb | PadHook::kRightThumb));

        if (thumbFalling) {
            if (e.thumbsTogether) {
                if (!e.chordFired) {
                    e.chordFired = true;
                    const bool wasOn = ModMenu::ControllerOn();
                    InputTracker::DispatchToggleSetting(static_cast<int>(ModMenu::SettingId::Controller));
                    // A pad the mod is handing back must not leave a latch armed behind it. The
                    // spoken "Controller, Off" is the feedback, so this needs no "Cancelled".
                    g_modArmedMs.store(0, std::memory_order_relaxed);
                    Log::Write("PAD", wasOn ? "L3+R3 -> intercept OFF (the pad is the game's again)"
                                            : "L3+R3 -> intercept ON");
                }
            } else if (ModMenu::ControllerOn()) {
                // A single click, and the intercept is on. One button, one row, spoken by name.
                if (thumbFalling & PadHook::kLeftThumb) {
                    InputTracker::DispatchToggleSetting(
                        static_cast<int>(ModMenu::SettingId::UnreachableFilter));
                    Log::Write("PAD", "L3 -> reachability filter");
                }
                if (thumbFalling & PadHook::kRightThumb) {
                    InputTracker::DispatchToggleSetting(
                        static_cast<int>(ModMenu::SettingId::AudioBeacon));
                    Log::Write("PAD", "R3 -> audio beacon");
                }
            }
        }
        if (!l3 && !r3) { e.thumbsTogether = false; e.chordFired = false; }
        e.prevThumbs = static_cast<uint16_t>(buttons & (PadHook::kLeftThumb | PadHook::kRightThumb));

        // Consumed for as long as they are HELD, not on an edge: an XInput button is a level, so
        // clearing the bit every poll is what keeps the game from seeing it at all. Only while the
        // intercept is on -- see the guarantee above.
        //
        // IT GOES THROUGH `consume`, NOT THROUGH A WRITE OF ITS OWN. The apply at the bottom of this
        // function rebuilds the button word from the ORIGINAL `buttons`, so a second writer up here
        // would be silently undone on every poll that also consumed something else -- which is most
        // of them. One accumulator, one write; see the apply.
        if (ModMenu::ControllerOn()) {
            consume |= static_cast<uint16_t>(buttons & (PadHook::kLeftThumb | PadHook::kRightThumb));
        }
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
    bool eatStick = false;        // right stick zeroed for the game

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
            // B INSPECTS, A CLOSES -- the game's own confirm/cancel pair, not the Xbox letters
            // (S189, the user's call). The mod menu is a menu; the button the player confirms with
            // everywhere else in FFXII is the button that should read a row out, and the one they
            // back out with everywhere else is the one that should shut it. Getting this backwards
            // costs a blind player the muscle memory the rest of the game just taught them.
            { PadHook::kB,         'O',      "read description" },
            { PadHook::kA,         VK_F8,    "close mod menu"   },
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
            const int vk = ModModeKeyFor(bit, ctx == Context::Battle, &action);
            if (vk) {
                Act(PadHook::ButtonName(bit), vk, action, ctx);
            } else if (bit == PadHook::kBack) {
                // Back, Back -- see the consume note below. The map speaks for itself.
                Log::Write("PAD", "mod mode ended on Back; the map press passes through to the game");
            } else {
                Say(Phrase::Id::ModCancelled);
                char m[96];
                snprintf(m, sizeof(m), "mod mode cancelled on %s (not mapped)",
                         PadHook::ButtonName(bit));
                Log::Write("PAD", m);
            }
            // Consume the WHOLE rising set, not just the bit that resolved: every button of a fumble
            // was aimed at the mod, and letting the others through would fire a game verb the player
            // never meant.
            //
            // BACK IS THE ONE EXCEPTION, AND IT IS DELIBERATE (S187, the user's ruling). Back is the
            // modifier, so pressing it twice is the natural "I did not mean that" -- and the game's
            // own map toggle is what Back costs the player everywhere else. Letting the second press
            // through makes that cost recoverable and gives the map a pad route again: **Back, Back
            // opens the map.** It used to be consumed, and the map opened anyway one frame later
            // because the mask was edge-shaped (see gamepad_sdl.cpp) -- so the behaviour the player
            // saw was right by accident. Now it is right on purpose.
            //
            // It does NOT speak "Cancelled" here: the map screen opening is the feedback, and
            // announcing a cancel over a screen the player just deliberately opened would be filler
            // that contradicts what happened.
            consume |= static_cast<uint16_t>(rising & ~PadHook::kBack);
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

        // ---- L1 AND R1: ASK, AND GO (Session 185) -----------------------------------------------
        //
        // L1 = `;`  -- what am I about to interact with. Out of combat that is the object the game's
        //              own interaction scorer has picked, which is not always the one the mod's list
        //              has focused; in a fight it is the enemy, with its HP.
        // R1 = `\`  -- route to the current selection, and start the beacon.
        //
        // R1 NO LONGER CHANGES MEANING IN A FIGHT, and that was the user's call with a reason behind
        // it: the thing a player most needs a route for mid-combat is a way OUT. Sending R1 to `p`
        // in battle meant the one context where escaping matters was the one context where the route
        // key routed to the enemy instead. `p` did not lose its pad home -- it moved to mod + Y,
        // where asking for the target's bearing is a deliberate question rather than the default.
        //
        // L1 IS TAKEN FROM THE GAME, KNOWINGLY. It was Speed mode (x2 / x4). The user's ruling: game
        // speed is reachable from the options menu and from the keyboard's `1`, pad buttons are
        // scarce, and a speed toggle is not what one should be spent on. That leaves the mod holding
        // L1, R1, Back, both thumb-clicks, and -- on the field only -- the D-pad and the right stick.
        //
        // BOTH ARE GATED ON `live`, WHICH IS WHAT KEEPS THE BATTLE TARGET LIST WORKING. With a
        // targeting cursor up the context is FieldBusy, not Battle, so neither shoulder is touched
        // and the game keeps L1 and R1 as the target list's group step -- the Foes / Party / Reserve
        // / Allies switch S184 built the spoken titles for. Taking them there would have silenced a
        // feature to feed another.
        if (live && (rising & PadHook::kLeftShoulder)) {
            Act("L1", VK_OEM_1, "target readout (;)", ctx);
            consume |= PadHook::kLeftShoulder;
        }
        if (live && (rising & PadHook::kRightShoulder)) {
            Act("R1", VK_OEM_5, "route + beacon (\\)", ctx);
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

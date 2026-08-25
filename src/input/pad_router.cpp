#include "input/pad_router.h"
#include "input/input_tracker.h"
#include "core/logger.h"
#include "ui/mod_menu.h"
#include "ui/dialogue_reader.h"
#include "ui/ingame_menu_reader.h"
#include "navigation/player_state.h"
#include "battle/battle_state.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>

namespace {

// ---- the published, stamped context (written game thread, read input thread) --------------------
std::atomic<uint8_t>  g_ctx{static_cast<uint8_t>(PadRouter::Context::Unknown)};
std::atomic<uint64_t> g_ctxStampMs{0};
std::atomic<uint8_t>  g_state{static_cast<uint8_t>(PadRouter::State::Normal)};

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

const char* ContextName(PadRouter::Context c) {
    switch (c) {
        case PadRouter::Context::OffField:  return "offfield";
        case PadRouter::Context::Field:     return "field";
        case PadRouter::Context::FieldBusy: return "fieldbusy";
        case PadRouter::Context::Battle:    return "battle";
        default:                            return "unknown";
    }
}

// ---- the Phase-1 survey log ---------------------------------------------------------------------
//
// WHAT IT IS FOR. There is no documented pad scheme for this game. S112 established that the game's
// Controls config screen is not evidence a control is free -- it lists only what is REBINDABLE, and
// the pad is not rebindable at all. Reading the game's own binding banks confirmed it: they hold
// keyboard DIK codes across the Main/Alt1/Alt2 columns and carry no pad column. So the only way to
// learn what the game does with a pad control is to press it and watch the game's own pad words.
//
// One line per rising edge: the input, the game's pad words on that same poll, and the context. A
// word that moves means the game reacted and that control is SPOKEN FOR.
//
// READ IT THE RIGHT WAY (L-04). The dedup below is per DISTINCT VALUE, so an absent line means that
// value never occurred, NOT that the control was never pressed. Presence is evidence; absence is not.
//
// Deduped per input slot so one held control cannot mask another, exactly as the keyboard collision
// watch is. 16 button bits + 4 stick directions.
constexpr int kSurveySlots = 20;
uint32_t g_lastSurveySig[kSurveySlots] = {};   // 0 = nothing logged yet for this slot

void SurveyLog(int slot, const char* inputName, PadRouter::Context ctx) {
    if (slot < 0 || slot >= kSurveySlots) return;

    uint16_t w[3] = {};
    const bool haveWords = PadHook::ReadGamePadWords(w);

    // The signature folds the three pad words AND the context, so the same button pressed in a menu
    // and on the field are two lines rather than one line and a silently dropped second case.
    const uint32_t sig = (static_cast<uint32_t>(w[0]) << 16)
                       ^ (static_cast<uint32_t>(w[1]) << 8)
                       ^  static_cast<uint32_t>(w[2])
                       ^ (static_cast<uint32_t>(ctx) << 28)
                       ^ 0x80000000u;                       // never 0, so "unset" stays distinct
    if (sig == g_lastSurveySig[slot]) return;
    g_lastSurveySig[slot] = sig;

    char m[160];
    if (haveWords) {
        snprintf(m, sizeof(m), "survey %s ctx=%s pad=0x%04X/0x%04X/0x%04X %s",
                 inputName, ContextName(ctx), w[0], w[1], w[2],
                 (w[0] || w[1] || w[2]) ? "GAME-REACTED" : "no-reaction");
    } else {
        snprintf(m, sizeof(m), "survey %s ctx=%s pad=unreadable", inputName, ContextName(ctx));
    }
    Log::Write("PAD", m);
}

} // namespace

namespace PadRouter {

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

    // THE OFF SWITCH IS THE FIRST LINE, on purpose. With the Controller setting off this returns
    // before it has read or written anything, so the input path is byte-identical to the mod as it
    // shipped with no pad hook at all -- not merely "skipped". Same bound Auto-walk carries.
    if (!ModMenu::ControllerOn()) return;

    // The mod never acts on input aimed at another window.
    if (!InputTracker::GameForeground()) return;

    PadEdges& e = g_edges[userIndex];
    const uint16_t buttons = state->pad.buttons;
    const Context  ctx     = FreshContext();

    // ---- edges ---------------------------------------------------------------------------------
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

    // ---- survey (Phase 1 diagnostic; DELETE once Docs/Controls.md carries the scheme) -----------
    if (rising) {
        for (int b = 0; b < 16; ++b) {
            const uint16_t bit = static_cast<uint16_t>(1u << b);
            if (rising & bit) SurveyLog(b, PadHook::ButtonName(bit), ctx);
        }
    }
    static const char* const kDirName[DIR_COUNT] = {
        "R-stick-Up", "R-stick-Down", "R-stick-Left", "R-stick-Right"
    };
    for (int d = 0; d < DIR_COUNT; ++d) {
        if (stickRising[d]) SurveyLog(16 + d, kDirName[d], ctx);
    }

    // ---- consumption ---------------------------------------------------------------------------
    //
    // PHASE 1 CLAIMS EXACTLY ONE INPUT: the right stick, and only on a live idle field. It is the
    // one the approved scheme takes anyway, and it is the only proof of the swallow that needs
    // neither the log nor sight -- on the field the camera stops answering the right stick, and
    // everywhere else it still answers.
    //
    // The D-pad, the face buttons and mod mode are deliberately NOT claimed yet: which physical
    // controls are free is exactly what the survey above is still measuring, and assigning one
    // before that lands is the S112 failure repeated.
    if (ctx == Context::Field) {
        state->pad.thumbRX = 0;
        state->pad.thumbRY = 0;
    }
}

} // namespace PadRouter

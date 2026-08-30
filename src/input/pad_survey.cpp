#include "input/pad_survey.h"

#include "input/pad_hook.h"
#include "core/logger.h"

#include <cstdio>

namespace {

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

// THE WORDS ARE READ ONE POLL LATE (S174). `ReadGamePadWords` reads state the game rebuilds FROM the
// buffer this very call is still returning, so sampling on the rising edge asks the game what it
// thought before it had been told. An edge is therefore REMEMBERED here and emitted on the next
// poll, by which time the game has acted. The cost is that a control pressed on the very last poll
// before a disconnect goes unlogged.
//
// THIS IS AN A-PRIORI FIX, NOT A MEASURED ONE, and the distinction is worth keeping. It was briefly
// justified by a log line -- `R1 ctx=unknown` reporting `GAME-REACTED pad=0x0080` -- which does not
// survive reading: `0x0080` in the game's word space is LEFT (S70), not R1 (`0x0800`), so that line
// was a direction being held, not a reaction to the shoulder. **Whether the old sampling was early
// has never actually been measured.**
uint32_t           g_surveyPending    = 0;      // one bit per slot
uint32_t           g_pendingIndex     = 0;
uint16_t           g_pendingRaw       = 0;
PadRouter::Context g_surveyPendingCtx = PadRouter::Context::Unknown;

// Slot -> name. Slots 0..15 are button bits, 16..19 the four stick directions.
const char* SurveySlotName(int slot) {
    if (slot < 0 || slot >= kSurveySlots) return "?";
    if (slot < 16) return PadHook::ButtonName(static_cast<uint16_t>(1u << slot));
    return PadRouter::StickDirName(slot - 16);
}

// `raw` is the WHOLE XInput button word at the moment of the edge, and `idx` the pad index. Both
// were missing until S174 and both were needed:
//
//   * A survey line naming one button cannot say what ELSE was down. The first real log has `L1` and
//     `R1` edges the player is certain they never pressed, and nothing in the line can tell a stray
//     bit from a chord from a second device.
//   * `pad=` is the GAME's word, in a bit layout this project still only has at 0.95 -- and the
//     first real log CONTRADICTS it (D-pad presses produced `0x0100`/`0x0200`/`0x0400`/`0x0800`,
//     which that layout calls L2/R2/L1/R1). Printing `raw` beside `pad` turns every press into a
//     direct XInput-bit -> game-word-bit correspondence, which is the only honest way to promote or
//     kill that layout. Guessing it from the button NAME is what produced the 0.95 in the first place.
void SurveyLog(int slot, const char* inputName, PadRouter::Context ctx, uint32_t idx, uint16_t raw) {
    if (slot < 0 || slot >= kSurveySlots) return;

    uint16_t w[3] = {};
    const bool haveWords = PadHook::ReadGamePadWords(w);

    // The signature folds the three pad words AND the context, so the same button pressed in a menu
    // and on the field are two lines rather than one line and a silently dropped second case.
    const uint32_t sig = (static_cast<uint32_t>(w[0]) << 16)
                       ^ (static_cast<uint32_t>(w[1]) << 8)
                       ^  static_cast<uint32_t>(w[2])
                       ^ (static_cast<uint32_t>(ctx) << 28)
                       ^ (static_cast<uint32_t>(raw) << 4)
                       ^ (idx << 24)
                       ^ 0x80000000u;                       // never 0, so "unset" stays distinct
    if (sig == g_lastSurveySig[slot]) return;
    g_lastSurveySig[slot] = sig;

    char m[192];
    if (haveWords) {
        snprintf(m, sizeof(m), "survey %s ctx=%s idx=%u raw=0x%04X pad=0x%04X/0x%04X/0x%04X %s",
                 inputName, PadRouter::ContextName(ctx), idx, raw, w[0], w[1], w[2],
                 (w[0] || w[1] || w[2]) ? "GAME-REACTED" : "no-reaction");
    } else {
        snprintf(m, sizeof(m), "survey %s ctx=%s idx=%u raw=0x%04X pad=unreadable",
                 inputName, PadRouter::ContextName(ctx), idx, raw);
    }
    Log::Write("PAD", m);
}

} // namespace

namespace PadSurvey {

void OnPoll(uint32_t userIndex, uint16_t buttons, uint16_t rising, const bool* stickRising,
            PadRouter::Context ctx) {
    // Last poll's edges first. The context, index and raw word stamped are the ones the PRESS
    // happened in, carried over with it -- a button pressed on a live field must not be reported as
    // a menu press just because the next poll landed after the menu opened.
    if (g_surveyPending) {
        for (int slot = 0; slot < kSurveySlots; ++slot) {
            if (g_surveyPending & (1u << slot))
                SurveyLog(slot, SurveySlotName(slot), g_surveyPendingCtx, g_pendingIndex, g_pendingRaw);
        }
        g_surveyPending = 0;
    }

    uint32_t pending = 0;
    for (int b = 0; b < 16; ++b) {
        if (rising & static_cast<uint16_t>(1u << b)) pending |= (1u << b);
    }
    if (stickRising) {
        for (int d = 0; d < 4; ++d) if (stickRising[d]) pending |= (1u << (16 + d));
    }
    if (pending) {
        g_surveyPending    = pending;
        g_surveyPendingCtx = ctx;
        g_pendingIndex     = userIndex;
        g_pendingRaw       = buttons;
    }
}

} // namespace PadSurvey

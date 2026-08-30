#pragma once

#include <cstdint>
#include "input/pad_router.h"

// THE PAD SURVEY -- the instrument that answers "does the GAME do anything with this control?".
//
// It is not part of the scheme and owns none of it. It watches every rising edge the router sees,
// including the ones the router is about to claim, and writes one line per distinct outcome. Split
// out of `pad_router.cpp` in Session 174, when that file crossed the 500-line rule: the router
// decides what a control MEANS, this decides what the GAME already thinks it means, and the two
// questions were only ever sharing a file.
//
// READ THE LOG THE RIGHT WAY (L-04). The dedup is per DISTINCT VALUE, so an absent line means that
// value never occurred -- NOT that the control was never pressed. Presence is evidence; absence is
// not. Session 174 paid for that lesson twice in one hour: an absent `L3` line was read as "the
// button does not reach us", when the player had simply not pressed it.
namespace PadSurvey {

// INPUT-POLL THREAD, once per poll, from `PadRouter::OnPoll` after the Controller gate.
//
// Does two things in this order: emits the PREVIOUS poll's edges -- by which time the game has had a
// poll to react to them -- and then remembers this poll's for the next call. `stickRising` is four
// bools, Up/Down/Left/Right, matching `PadRouter::StickDirName`.
//
// `buttons` is the WHOLE XInput word, not just the edge. It is logged as `raw=` so a line can be
// read for what else was down and which device it came from, and so the game's own pad words can be
// mapped bit-for-bit against XInput's from real presses rather than inferred from a button's name.
void OnPoll(uint32_t userIndex, uint16_t buttons, uint16_t rising, const bool* stickRising,
            PadRouter::Context ctx);

} // namespace PadSurvey

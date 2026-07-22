#pragma once

#include <string>

// Reads the game's message text — NPC dialogue + in-engine cutscene captions (the
// e5f0 message window) and informational panels (item acquired, treasure, battle
// system lines — the FUN_0057c480 system-message surface). Speaks each new line as
// it appears, with the speaker name, and repeats the last line on the `t` hotkey.
//
// By design contains NO hard-coded UI strings: everything spoken is decoded from the
// game's own codec bytes. Yes/no confirm dialogs on the panel surface are a distinct
// mechanism from the already-handled title/new-game confirms and are deliberately
// NOT spoken (kept classified + logged behind a mute flag), so nothing double-speaks
// or regresses.
namespace MessageReader {

bool Init();
void Shutdown();

// Advance to the next page of the message currently on screen and speak it. Called on the input
// thread when the player presses the game's own Confirm key -- the same press that advances the
// game's text box -- so our page pointer tracks the box instead of running ahead of it.
//
// A multi-page message arrives from the content setter as ONE string containing every page, which
// is why it used to be read out in a single breath. Pages are split on the codec's 0x03 break.
//
// Silent when no page remains (the box is closing) and when no message is active: announcing
// anything there would be filler.
void NextPage();

// Body text of the most recent yes/no confirm surface ("Obtain Accessories 1?", "Choose this
// license board?"), captured at its case-1 BIRTH — the only moment the composed string, with its
// substituted parameter, is readable. Returns AND clears it, so one prompt speaks once.
//
// This reader stays muted for confirms (kSpeakSurfaceConfirms) and hands the text over instead:
// MenuReader speaks it as the pop-up preamble, which keeps the proven body-then-button ordering
// (speaking it here would be cut off by the Yes/No focus that fires immediately after).
// Empty when no confirm is pending.
std::wstring TakeConfirmPrompt();

} // namespace MessageReader

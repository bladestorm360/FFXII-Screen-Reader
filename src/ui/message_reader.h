#pragma once

#include <string>

// Reads the game's NON-PAGINATED message surfaces: the "obtained <item>" treasure/loot toast and the
// menu system-message panel ("cannot equip", "sold"). Speaks each as it appears and repeats the last
// line on the `t` hotkey.
//
// FIELD DIALOGUE IS NOT HERE — it lives in `ui/dialogue_reader`, which drives it off the game's own
// page cursor. This module used to own it through the whole-message content setter FUN_002e16b0 and
// split the string into pages itself, advancing on an observed Space/Enter; that made dialogue
// keyboard-only. Both the setter hook and the page list are gone. The `t` store stays here because
// all three surfaces share it — `DialogueReader` feeds it through NoteSpoken().
//
// By design contains NO hard-coded UI strings: everything spoken is decoded from the game's own
// codec bytes. Yes/no confirm dialogs on the panel surface are a distinct mechanism from the
// already-handled title/new-game confirms and are deliberately NOT spoken (kept classified + logged
// behind a mute flag), so nothing double-speaks or regresses.
namespace MessageReader {

bool Init();
void Shutdown();

// Record `text` as the most recent spoken line, so `t` repeats it. Called by every reader that owns
// a message surface — this module's own toast/panel paths and DialogueReader's pages — so the
// re-read key has ONE store behind it rather than one per surface. Game thread.
void NoteSpoken(const std::wstring& text);

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

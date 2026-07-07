#pragma once

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

} // namespace MessageReader

#pragma once

// The titled REWARD PANEL -- a hunt's payout ("Antlion Infestation / 4300 gil / Bubble Belt x1 /
// Sickle-Blade x1"), and anything else the game routes through the same window. Speaks the title
// and every row once, when the panel is built, and feeds MessageReader's `t` store so the line can
// be re-read while the panel is still up.
//
// NOT the "You obtain <item>!" toast (`FUN_0035e070`, message_reader). The two are separate windows;
// the key items of a hunt reward are the only part that goes through the toast -- see the .cpp.
namespace RewardPanelReader {

bool Init();
void Shutdown();

// Is the panel on screen? Set at its build, cleared at its destroy message. Any thread.
bool IsLive();

} // namespace RewardPanelReader

#pragma once

// The PARTY MENU's shared character chooser -- the "select a character" portrait grid.
//
// ONE surface, four commands. `0x4b3` Party, `0x4b4` Status, `0x4b6` Equipment and `0x4b9` Gambits all
// activate the SAME controller (FUN_00285290, parked at menuCtx+0xf8); only the mode the field pane arms
// it in differs. That is what makes this one choke point rather than four readers, and it is why the
// code moved out of ingame_menu_reader.cpp (Session 93) instead of growing it -- that file is 739 lines
// against a 500 cap, and the chooser was the self-contained part.
//
// WHAT IT SPEAKS depends on the command the pane is on, because the same highlight means different
// things. On Status/Equipment it is a stat readout; on Party the screen is a MEMBERSHIP TOGGLE and the
// stats are noise -- the tester heard "Vaan, Level 99, HP 17026/8513, MP 648/648" one second after the
// row "Party" was spoken, which was the reported defect verbatim.
namespace CharSelectReader {

// Installs two hooks: the chooser's cursor-set (per highlight) and the party toggle (per accepted
// press, because a toggle does not move the cursor and would otherwise be silent).
bool Init();
void Shutdown();

} // namespace CharSelectReader

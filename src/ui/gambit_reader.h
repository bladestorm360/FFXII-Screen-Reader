#pragma once

// Gambit setup screen (field menu -> Gambits -> a character).
//
// The screen is a 13-record display array on one panel class, FUN_005691e0 (RVA 0x4491E0), pause
// command 0x4B9. Record 0 is the character header carrying the gambit MASTER toggle; records 1..N are
// the gambit rows, each holding a CONDITION and an ACTION name plus its own on/off bit. A second
// cursor selects which COLUMN of the focused row the player is on.
//
// NO NEW HOOK. Every focus on this surface arrives as msg 0x8000 through FUN_00247510, which
// menu_reader.cpp already owns -- the dispatch branch there hands it straight here, the same way it
// hands the field dialogue window to ChoiceReader.
//
// CONFIRMED LIVE, 2026-07-30 (probe_gambit_menu_output.log), all four pass criteria:
//   * `owner` IS the panel object, so the existing hook sees this surface.
//   * `val` IS the record index -- 0 for the header, 1..panel+0x126 for the rows. No off-by-one.
//   * cond/action ids read 0xFFFF on exactly the rows whose class byte is 2, and nowhere else.
//   * popcount(panel+0x124) == the number of rows whose own on-bit is set, so the mask's bit i-1
//     belongs to display row i.
//
// The PICKER (the condition/action chooser this screen opens on Confirm) is NOT handled here. The
// probe run never confirmed on a row, so it captured no picker messages at all and its row layout is
// unmeasured -- shipping a reader for it would be a guess, and a wrong one would claim the surface
// and silence whatever covers it today.
namespace GambitReader {

// Is this dispatch owner the gambit panel? Discriminated by CLASS (obj[0]), never by address: menu
// objects are pooled and the live log caught this very panel reusing the allocation the field pane
// window had held minutes earlier.
bool IsGambitPanel(void* owner);

// Speak the focused record. `recIndex` is the dispatch's own `val`. Returns true when this surface is
// ours -- claimed either way, so the generic painted-row path can never also speak these rows. A read
// failure stays SILENT and says why in the log.
bool OnFocus(void* owner, int recIndex);

} // namespace GambitReader

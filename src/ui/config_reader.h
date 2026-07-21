#pragma once

#include <string>

// Reads the CURRENT VALUE shown by a config-screen row: an enum option label, a slider number, a
// Graphics formatted string, or a Controls key-binding name. Split out of menu_reader.cpp, which
// was carrying the hook plumbing and ~180 lines of value plumbing in one 660-line file.
//
// Row IDENTITY is not decided here -- callers get a MenuState::ValueRow and this module switches on
// it, so the row-class -> RVA mapping stays in ui/menu_state.cpp alone.
//
// Every value is the GAME's own text: option labels decode the row's codec bytes, key names come
// from TextCapture's id cache (populated by the game's own draw of that row). Nothing is invented,
// and an unreadable value returns EMPTY so the caller stays silent rather than guessing.
namespace ConfigReader {

// Current value of `row`. Empty when the row has no value, or it could not be read.
std::wstring RowValue(void* row);

// The value the row WILL show given a new store value `nv` -- used on a left/right change, where
// the widget's own cached state has not been updated yet. `nv` is the new option index for enums,
// the new gauge value for sliders.
std::wstring RowValueAtNewValue(void* row, int nv);

} // namespace ConfigReader

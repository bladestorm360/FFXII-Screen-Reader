#pragma once
#include <string>

// Expands what the game's own equipment detail panel HIDES.
//
// Past three statuses FFXII stops listing them and prints "Immune: Various status effects" instead
// (FUN_00293310:157-212 — it popcounts the mask and only enumerates when the count is under 4).
// Sighted players lose the detail too, but it lands hardest on accessories, where the whole point
// of the item is which ailments it blocks: "Various status effects" tells a buyer nothing.
//
// The mask is still right there in the item record, so the names are recoverable. Every word used
// is the game's own — the row label, the separator and each status name all come from game data.
namespace EquipDetail {

// If `descParams` describes a piece of equipment whose status list the game collapsed, rewrite that
// phrase in `text` with the full list. Returns true if it changed anything.
//
// `descParams` is FUN_00292b70's second argument, verbatim.
// GAME THREAD ONLY — it resolves the item record through the game's own getter.
bool ExpandCollapsedStatuses(const void* descParams, std::wstring& text);

} // namespace EquipDetail

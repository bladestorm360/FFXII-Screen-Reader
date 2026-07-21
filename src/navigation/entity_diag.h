#pragma once

// The backtick object-dump diagnostic, split out of entity_list.cpp.
namespace EntityDiag {

// Walk the handle table and log every object with its position, flags, name key and category.
// File-only -- it never speaks. The CALLER must hold EntityList's mutex: this reads live game
// tables and must not race a rescan.
void DumpLocked();

} // namespace EntityDiag

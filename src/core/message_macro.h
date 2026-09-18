#pragma once

#include <cstdint>

// MESSAGE MACROS — the numbers FFXII substitutes into its own dialogue.
//
// The game writes lines like "<n> Bhujerbans heed your words" with the count as an inline MACRO, and
// the mod used to read them with the number simply missing: "Bhujerbans heed your words", or "Only
// [gap] Bhujerban heeds your words". The decoder was never at fault — it framed the escape correctly
// and performed no substitution, because nothing knew what to substitute.
//
// ---- HOW THE GAME DOES IT (traced, then confirmed by a Ghidra xref pass) ------------------------
//
//   script `setmesmacro`  = native 0x1A8 -> FUN_0034CF20 -> FUN_002E1B70(slot, index, valA, valB)
//   FUN_002E1B70 clamps slot to [0,7] and stores the pair at
//       DAT_0215F540 + (slot*0x20 + index)*8      (valA)   ABS 0x0215F540, RVA 0x203F540
//       DAT_0215F544 + (slot*0x20 + index)*8      (valB)
//   FUN_002E16B0, the message-show path, hands `&DAT_0215F540 + slot*0x100` to the renderer, and
//   the escape dispatcher FUN_002AC5F0's **case 0x2E** reads the table back out of its render
//   context at `param_1[0xE]`.
//
// **SELECTOR 0x2E IS THE MACRO PRINTER**, and that is why the xref pass found only three functions
// touching the table — an initialiser, the show path and the writer. The printer never names the
// global; it receives a pointer.
//
// ---- WHY THE MOD CAPTURES THE WRITE INSTEAD OF READING THE TABLE -------------------------------
//
// The decoder sees codec BYTES, not the render context, so it cannot reach `param_1[0xE]` and has
// no slot to index the global with. Hooking the writer sidesteps the whole question: the script
// sets the macro immediately before showing the line that uses it, so the most recent write IS the
// value that line will print. Cheap, and it needs no part of the escape's parameter encoding.
//
// ---- WHY ONE CACHED VALUE WAS NOT ENOUGH (S190) -------------------------------------------------
//
// The old note here said: "this holds one value, so a line carrying TWO macros would print the same
// number twice. No line the mod reads today does that." **The Pharos Subterra orb pedestal does**,
// and it carries FIVE. Its prompt is, in shipped data (`rbl_g02.ebp` @0x1c324):
//
//     "How many black orbs will you set in the pedestal?
//      (Black orbs: <0f2e 81 90>    Already set: <0f2e 82 90>)"
//     0x0E block: row0 `0f2e8390`  row1 `0f2e8490`  row2 `0f2e8590`  row3 "Cancel"
//
// -- two macros in the page (indices 1 and 2) and THREE MORE as the option rows themselves
// (indices 3, 4, 5), which are the quantities the player picks between. With one cached value all
// five print the same number. In the log that read as "(Black orbs: 10    Already set: 10)", which
// looked correct only because this save happened to have 10 orbs and 10 already set.
//
// So the value is now resolved BY THE ESCAPE'S OWN INDEX, which is what the game does: case 0x2E
// indexes the table it was handed. The writer hook already sees `slot` and `index`, so the table is
// simply kept instead of collapsed to its last write.
//
// **THE SLOT IS THE ONE ASSUMPTION, AND IT IS LOGGED.** The decoder sees codec bytes and never
// learns which slot FUN_002E16B0 passed, so this tracks the slot most recently WRITTEN -- the same
// reasoning the one-value cache already rested on (the script fills a slot immediately before
// showing the line that reads it). `Unresolved()` counts the indices a decode asked for and did not
// find, which is what a wrong slot would look like; it is logged once rather than papered over with
// a fallback to "some other number we happen to hold".
namespace MessageMacro {

// Installs the writer hook. Non-fatal: without it macros stay blank, exactly as before.
bool Init();

// The most recent macro VALUE, or false when none has been written this session.
//
// **IT IS THE SECOND WORD, MEASURED.** The decompile does not say which of the pair the printer
// uses, so the first build shipped `valA` and logged both. One play pass settled it: the game showed
// "2 Bhujerbans heed your words" while the log read `valA=0 valB=2`, and the mod spoke the 0. So the
// pair is (kind, value) -- `valA` is a type selector, 0 for a plain integer, and `valB` is the
// number. `FUN_002E1B70` storing them at +0 and +4 respectively fits that reading exactly.
//
// `outKind` optionally returns the first word, which is still logged on every first write so a
// non-zero kind (a different macro TYPE the mod has not seen) shows up rather than printing wrong.
bool Latest(int32_t* outValue, int32_t* outKind = nullptr);

// The value the macro at `index` was last given, within the slot most recently written. False when
// that index has never been written -- the caller then prints NOTHING, because the alternative is
// putting some other row's number in the player's ear.
//
// `index` is the escape's own parameter byte masked with 0x7F: `0f 2e 83 90` is index 3.
bool ValueAt(int index, int32_t* outValue, int32_t* outKind = nullptr);

} // namespace MessageMacro

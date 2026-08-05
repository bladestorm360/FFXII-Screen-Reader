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
// LIMIT, stated rather than buried: this holds one value, so a line carrying TWO macros would
// print the same number twice. No line the mod reads today does that -- the shout messages carry a
// single 0x2E -- and the log line below is what would show it if one ever did.
namespace MessageMacro {

// Installs the writer hook. Non-fatal: without it macros stay blank, exactly as before.
bool Init();

// The most recent macro value, or false when none has been written this session.
// `outOther` optionally receives the pair's second word -- WHICH OF THE TWO the printer uses is not
// settled by the decompile, so both are logged on the first write and one play pass decides it.
bool Latest(int32_t* outValue, int32_t* outOther = nullptr);

} // namespace MessageMacro

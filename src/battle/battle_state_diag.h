#pragma once

#include <cstdint>

// The two BattleState diagnostics. Split out of battle_state.h alongside battle_state_diag.cpp
// (Session 93) -- the header was 25 lines over its 150 cap and these declare a separate translation
// unit's contents, so they belong beside it.
//
// Both are LOG-ONLY. They exist because the no-filler rule sends "why did that key say nothing" to the
// log rather than to speech: DiagnoseSlot is what `4`/`5`/`6`/`7` write instead of announcing an empty
// slot, and DiagnoseCommitment is what `;` writes when there is no committed target to describe.
namespace BattleState {

// Why did BtlChrForSlot return null? Empty slots are SILENT by design, so a genuine failure and an
// empty slot sound identical -- this puts the distinguishing detail in the log instead of leaving
// the next debugging round to guesswork.
struct SlotDiag {
    void*    globalValue = nullptr;  // the pointer stored AT the global (the correct W)
    uint32_t magic       = 0;        // *(u32*)W -- must be 0x5071901
    bool     magicOk     = false;
    uint16_t rosterEntry = 0xFFFF;   // list-3 entry for this slot (>= 0x28 means empty)
    bool     rosterRead  = false;
};
SlotDiag DiagnoseSlot(int slot);

// Why did the commitment lookup fail? `;` resolves BtlWork -> leader index -> leader BtlChr ->
// actor-pool scan -> the active/queued fields, and today ANY broken link collapses to one silent
// boolean, so a user pressing the key after confirming an attack just gets nothing. This logs every
// link with its raw value, so a single press names the one that failed instead of leaving the whole
// chain suspect. Log-only; speaks nothing and changes no state.
void DiagnoseCommitment();

} // namespace BattleState

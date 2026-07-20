#pragma once
#include <cstdint>
#include <string>

// Party status readout — the first piece of combat support.
//
// Reads the game's own party roster + BtlChr records directly from memory. Works on the FIELD and in
// BATTLE alike: the roster list we use is the unmasked master copy, so it is not affected by the HUD
// visibility gate or the game mode.
//
// Source of truth: the seven `btlAtel*FromPartySlot` script natives (btlAtelGetCharIdFromPartySlot,
// ...HpNow/HpMax/MpNow/MpMax/NowStatus/CharacterKind), resolved offline at FUN_0050fd00..FUN_0050ff20.
// We do NOT call them — they are athena-VM natives that pop their argument off the VM stack (and abort
// on underflow) and push their result through the VM context, so calling one from outside the VM is not
// legitimate. Instead we read exactly what their bodies read.
namespace PartyStatus {

// Roster list 3 holds NINE slots: 0-2 the active party, 3 the guest, 4-8 the reserve. The game's
// own bound check for this list (FUN_00322c50 category 0x17) is literally `slot < 9`, so every one
// is a legal read; occupancy is decided by the >= 0x28 sentinel per slot.
constexpr int kMaxSlots = 9;

struct SlotVitals {
    bool     present = false;   // false = empty slot (speak nothing at all)
    uint8_t  charId  = 0;
    int32_t  curHP   = 0;
    int32_t  maxHP   = 0;
    int16_t  curMP   = 0;
    int16_t  maxMP   = 0;
    bool     haveMP  = false;   // MP gauge disabled for this character
    uint32_t status  = 0;       // status bitfield (statusA | statusB)
    std::wstring name;
    std::wstring statusNames;   // e.g. "Poison, Slow" — resolved from the game's own status table
};

// Read one party slot (0-based). Returns false if the slot is empty or unreadable.
bool ReadSlot(int slot, SlotVitals& out);

// Speak one party slot's vitals. ORDER IS: "<name>, <statuses>, HP <cur>/<max>, MP <cur>/<max>" --
// statuses sit right after the name because in real-time combat you must know you are poisoned or
// stopped before you need the exact numbers (user, 2026-07-20).
//
// AN EMPTY OR UNREADABLE SLOT IS **SILENT** -- it speaks nothing at all, like every other mod key
// with nothing to report. It must NEVER announce "Empty slot" or any other filler; that is a
// standing user instruction, not a preference. (combat_system.md 8.2 once argued for speaking it so
// a broken mod could not masquerade as an empty slot, but that reasoning came from a session where
// the mod WAS broken, and announcing an absent guest on every press is just noise.) The
// distinguishing detail goes to the LOG instead, via BattleState::DiagnoseSlot -- see SpeakSlot's
// body, and note PARTY is in the logger's flush list so it survives a hard exit.
//
// `slot` is 0-based: key 4 -> 0, 5 -> 1, 6 -> 2, 7 -> 3 (the guest, usually absent and therefore
// usually silent -- that is correct behaviour, not a bug).
void SpeakSlot(int slot);

} // namespace PartyStatus

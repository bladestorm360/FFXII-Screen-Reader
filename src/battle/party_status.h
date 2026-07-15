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

// Active party slots (3 active + 1 guest). The game bounds this at 4 in six independent functions;
// we still test every slot for emptiness rather than trusting the constant.
constexpr int kMaxSlots = 4;

struct SlotVitals {
    bool     present = false;   // false = empty slot (speak nothing at all)
    uint8_t  charId  = 0;
    int32_t  curHP   = 0;
    int32_t  maxHP   = 0;
    int16_t  curMP   = 0;
    int16_t  maxMP   = 0;
    bool     haveMP  = false;   // MP gauge disabled for this character
    uint32_t status  = 0;       // status bitfield (statusA | statusB) — unused for now
    std::wstring name;
};

// Read one party slot (0-based). Returns false if the slot is empty or unreadable — in which case the
// caller speaks NOTHING (an empty slot is silent, not "empty slot").
bool ReadSlot(int slot, SlotVitals& out);

// Speak one party slot's vitals: "<name>, HP <cur> of <max>, MP <cur> of <max>". Silent when the slot
// is empty. `slot` is 0-based (key 4 -> 0, 5 -> 1, 6 -> 2).
void SpeakSlot(int slot);

} // namespace PartyStatus

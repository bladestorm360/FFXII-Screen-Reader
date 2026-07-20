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

// Speak one party slot's vitals: "<name>, HP <cur> of <max>, MP <cur> of <max>, <statuses>".
// An empty slot now says "Empty slot" rather than staying silent -- silence made a broken mod
// indistinguishable from an empty party slot, which is what hid the missing-dereference bug for
// two sessions. `slot` is 0-based (key 4 -> 0, 5 -> 1, 6 -> 2).
void SpeakSlot(int slot);

} // namespace PartyStatus

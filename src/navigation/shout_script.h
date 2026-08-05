#pragma once

#include <cstdint>

#include "navigation/shout_table.h"

// SCRIPT-VM ACCESS for the shout minigame: find the running map script, and turn one of its
// variable indices into an address.
//
// This is a READ layer plus one narrowly-scoped write helper. It knows nothing about speech, keys
// or the gauge; `shout_meter` and `shout_fill` are its only callers, and the write helper is only
// ever reached from `shout_fill` under that file's charter.
//
// ---- THE MODULE RECORD (all CONFIRMED from the loader FUN_0026C8C0 and the resolver FUN_00262440)
//
// Script modules live in a five-slot array at `NavRva::HANDLE_TABLE_BASE` (0x1F78E10) with stride
// 0x288 -- the SAME array the entity scan already walks for field objects, because a slot is a
// controller: a script VM instance plus its actors. Per slot, as longlong indices:
//     mod[0x00] (+0x00)  the loaded EBP2 image base
//     mod[0x08] (+0x40)  storage-class 1 base
//     mod[0x09] (+0x48)  storage-class 0 base
//     mod[0x0B] (+0x58)  storage-class 4 base  (the loader assigns 0x02099DF0 to every module --
//                        the cross-script global int work array)
//     mod[0x0C] (+0x60)  storage-class 5 base  (0x02099FF0, the float globals)
//     mod[0x0F] (+0x78)  the variable DESCRIPTOR table
// Class 3 is module-local: `ebpBase + *(u32*)(ebpBase + 0x40)`. Class 2 is per-actor and is
// resolved by CALLING a function pointer at mod[0x13] with a VM context we do not have, so it is
// deliberately UNSUPPORTED here -- a class-2 meter fails open rather than being guessed at.
//
// The bases are read from the RECORD, never from the hardcoded globals the loader happens to store
// there. Same values, but the record is what the engine's own resolver reads, so the two cannot
// drift apart.
//
// ---- THE DESCRIPTOR (FUN_00262440) ----
//     desc      = *(u32*)(mod[0x0F] + 4 + varIdx * 8)
//     elemType  = desc >> 28        0=u8 1=s8 2=u16 3=s16 4=u32 5=float (strides 1/1/2/2/4/4)
//     class     = (desc >> 24) & 7
//     byteOff   = desc & 0xFFFFFF
//     address   = classBase + byteOff
//
// ---- IDENTITY ----
// A module names itself: three NUL-terminated strings at `ebpBase + 0x110` are the build stamp,
// the author, and `<module>.src`. Verified on all 809 EBP2 files in the game. See shout_table.h.
namespace ShoutScript {

// A resolved shout-script module. `row` is never null when `valid` is true.
struct Module {
    bool              valid   = false;
    void*             record  = nullptr;   // the module record (base + slot * 0x288)
    void*             ebpBase = nullptr;
    int               slot    = -1;
    const ShoutTable::Row* row = nullptr;
    char              srcName[32] = {};
};

// Scan the five controller slots for one whose `.src` name matches a ShoutTable row.
// GAME THREAD only (it dereferences live engine pointers, all SEH-guarded).
// Returns a Module with `valid == false` when no slot is a shout script -- the normal case
// everywhere in the game except the Bhujerba streets during this sequence.
//
// `outNames`, when non-null, receives a human-readable census of ALL five slots' names for the
// log: it is the falsifier for a table that names no live module, and it is what makes a missing
// row a one-line fix instead of another guessing round.
Module FindShoutModule(char* outNames, int outNamesCap);

// Decode one of the module's variables to an absolute address.
// Returns false when the descriptor is unreadable, its storage class is unsupported (2), or the
// resulting address is null. `outElemType` receives the descriptor's element type.
bool VarAddress(const Module& m, uint8_t varIdx, void** outAddr, uint8_t* outElemType,
                uint32_t* outRawDesc);

// SEH-guarded read of a script variable, honoring `elemType`. False on fault.
bool ReadVar(void* addr, uint8_t elemType, int32_t* out);

// SEH-guarded write of a script variable, honoring `elemType`. False on fault.
//
// ⚠ THIS IS A GAME-MEMORY WRITE. It exists for exactly one caller, `ShoutFill`, under the charter
// in shout_fill.h. Nothing else may call it.
bool WriteVar(void* addr, uint8_t elemType, int32_t value);

} // namespace ShoutScript

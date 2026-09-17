#pragma once

#include <cstdint>

#include "navigation/shout_table.h"

// SCRIPT-VM ACCESS for the shout minigame: find the running map script, and turn one of its
// variable indices into an address.
//
// This is a READ layer plus one narrowly-scoped write helper. It knows nothing about speech, keys
// or the gauge. The write helper is reached only from `shout_fill` and `sochen_doors`, each under
// its own file's charter.
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

// THE EXECUTING module, read from the engine's own "current module" global (`DAT_02099D70`, RVA
// 0x1F79D70 — the loader and every module entry point save/set/restore it). Called from INSIDE a
// script native, this is the record of the script that made the call, with no scanning and no
// guessing about which slot holds what.
//
// Added in S134 because the five-slot scan came back empty on a live shout sequence: the tester's
// log has the sequence's own dialogue and `script modules: [0]=- [1]=- [2]=- [3]=- [4]=-` on the
// same map, so `record[0]` is not an EBP2 image the way the loader's decompile reads. Until
// `DumpRecords` says what those records really hold, this is the resolution path that does not
// depend on the layout being what we thought.
Module FromCurrentModule();

// Build a Module from a record pointer, whatever produced it. `valid` is false unless the record
// yields an EBP2 image whose `.src` name matches a ShoutTable row.
Module FromRecord(void* record);

// RAW DIAGNOSTIC (log-only). Dumps the first quadwords of all five slot records, the value of the
// current-module global, and the first bytes of whatever each candidate pointer addresses — so the
// question "what is actually in these records" is answered by bytes rather than by another reading
// of the decompile.
void DumpRecords();

// ---- THE GENERIC LAYER (Session 154) -----------------------------------------------------------
//
// Everything below is true of ANY loaded map script, not just a shout one, and the statue-puzzle
// reader in `statue_diag` / `statue_guide` is its second caller. It lives here rather than in a new
// file because the descriptor decode must exist exactly once: `VarAddress` above now delegates to
// `VarAddressRaw`, so a `Module` and a `RawModule` can never disagree about where a variable is.
//
// THE NAMESPACE NAME IS HISTORICAL. `ShoutScript` was named when the gauge was its only client; the
// half below knows nothing about shouting. If a third client appears, lift this half into
// `script_vm.{h,cpp}` and leave `Module` / `FindShoutModule` behind -- they are the only genuinely
// shout-specific things here, because they resolve through `ShoutTable`.

// A live script module with NO table lookup attached -- the raw fact that slot N holds a script
// whose authoring name is this.
struct RawModule {
    bool  valid   = false;
    void* record  = nullptr;
    void* ebpBase = nullptr;
    int   slot    = -1;
    char  srcName[32] = {};
};

// Every live module whose `.src` name starts with `prefix` (exact ASCII, case-sensitive), written
// into `out` up to `cap`. Returns how many were written. GAME THREAD only.
//
// A PREFIX rather than an exact name because a dungeon's rooms are separate scripts sharing one
// authoring prefix (`mrm_` is the whole Stilshrine of Miriam), and the caller wants "am I anywhere
// in this dungeon" without enumerating every room.
int FindModulesBySrcPrefix(const char* prefix, RawModule* out, int cap);

// How many variables the module's descriptor table declares (its word 0). 0 when unreadable.
// Bounded by the caller -- a torn record can report anything.
uint32_t VarCount(void* record);

// The base address of one storage class on a raw record, or null. Classes 4 and 5 are the
// CROSS-SCRIPT global arrays every module shares, which is what makes them worth reading directly:
// a flag written by a script whose descriptor table we are not sweeping still lands there.
// Class 3 needs `ebpBase`; class 2 is unsupported and always returns null.
void* ClassBaseRaw(void* record, void* ebpBase, uint8_t cls);

// The descriptor decode, on a raw record. `VarAddress` below is a thin wrapper on this.
bool VarAddressRaw(void* record, void* ebpBase, uint8_t varIdx, void** outAddr,
                   uint8_t* outElemType, uint32_t* outRawDesc);

// Decode one of the module's variables to an absolute address.
// Returns false when the descriptor is unreadable, its storage class is unsupported (2), or the
// resulting address is null. `outElemType` receives the descriptor's element type.
bool VarAddress(const Module& m, uint8_t varIdx, void** outAddr, uint8_t* outElemType,
                uint32_t* outRawDesc);

// SEH-guarded read of a script variable, honoring `elemType`. False on fault.
bool ReadVar(void* addr, uint8_t elemType, int32_t* out);

// SEH-guarded write of a script variable, honoring `elemType`. False on fault.
//
// ⚠ THIS IS A GAME-MEMORY WRITE. It has exactly two callers, each under its own user-authorized
// charter: `ShoutFill` (shout_fill.h) and, since S180, `SochenDoors` (sochen_doors.h). Nothing else
// may call it.
bool WriteVar(void* addr, uint8_t elemType, int32_t value);

} // namespace ShoutScript

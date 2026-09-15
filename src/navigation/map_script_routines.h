#pragma once

#include <cstdint>
#include <string>
#include <vector>

// PER-ROUTINE FACTS about the loaded map-global script (container 0), read live -- Session 179.
//
// Two facts, both about what a routine DOES rather than what it is called:
//
//  * WHERE ITS ENTRY TABLE LIVES. A scene object's `+0x48` IS its routine's entry table (record
//    `+0x10`, blob-relative) -- GameArchitecture.md "A scene object's +0x48 IS its routine's ENTRY
//    TABLE". So object -> routine is a POINTER IDENTITY: no authoring order, no slot arithmetic, no
//    name. (The slot index also equals the routine index on every map measured; the identity is what
//    this joins on, and the diagnostic line in door_binding.cpp counts how often the two agree.)
//
//  * WHICH FLOOR IDS IT OPENS. `setmapidfloor(id, class, state)` (native 0x00FE) writes the walkmap
//    MATERIAL override bank entry `id`: class 0 -> bit 23, the leader's floor-refusal bit; state 0
//    forces the bit ON (closed), state 1 forces it OFF (open). Decompile: the handler's class switch
//    {0,1,2,3,7,9} -> bits {23,25,26,27,24,12} matches the script's own class list exactly, and the
//    live effective flags on Mirror of the Soul's doors 3 and 4 read material 3 / 4 with bits 23-27
//    set (`0x0FA07000`, `0x0FA09000`). A routine that calls `setmapidfloor(N, 0, 1)` can OPEN floor N
//    for the party -- a door, a fake wall, a magic wall, a rock, a gate.
namespace MapScript {

struct RoutineFacts {
    int         index          = -1;
    uint64_t    entryTable     = 0;   // live address of the routine's entry table; 0 when unreadable
    uint32_t    opensFloorMask = 0;   // bit N set: the routine calls setmapidfloor(N, class 0, open)
    bool        codeRead       = false;
    std::string name;                 // printable-ASCII form, for the log only (most are Shift-JIS)
};

// Read every container-0 routine's facts. False when no field script is loaded or the table is
// unreadable; `out` is then empty. Memory-only and SEH-guarded, like ReadExitDests.
bool ReadRoutineFacts(std::vector<RoutineFacts>& out);

// A cheap identity for the script CURRENTLY resident: routine-table offset, name-pool offset and
// routine count. 0 when no script is loaded. A cache keyed on the MAP ID alone can be filled from the
// previous map's blob -- the id flips at the leading edge of a transition while the old script is
// still in place (S93) -- and the blob ADDRESS is reused between maps (Sochen 185 and 186 share it),
// so neither is a safe key on its own.
uint64_t ScriptFingerprint();

// Index into `facts` of the routine whose entry table `sceneObj+0x48` points at, or -1. Exact
// pointer equality only -- a near miss is not a binding.
int RoutineIndexOfObject(const std::vector<RoutineFacts>& facts, void* sceneObj);

} // namespace MapScript

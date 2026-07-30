#pragma once

#include <cstdint>
#include <string>

// Master-data plumbing shared between battle_state.cpp and battle_state_diag.cpp.
//
// These are NOT public API. They exist as a header only because the diagnostics were split into their
// own translation unit (Session 93) to bring battle_state.cpp back under the 500-line cap, and
// DiagnoseCommitment inspects exactly this plumbing -- that is most of what it is FOR. Same idiom as
// entity_list_internal.h and map_script_internal.h.
namespace BattleState {
namespace Internal {

// DAT_02ebf138 -- the ability/action table. The diagnostics print its header fields directly, so the RVA
// has to be visible to both files; two copies of it would be two things to get wrong.
constexpr uint32_t RVA_ACTIONTBL = 0x2D9F138;

// Every "pointer" in master data is a u32 offset needing this relocation (FUN_0020e600, literally
// `return off + _DAT_01f83530`). Returns null on a failed read or a zero offset.
//
// S87: this used to bail when the base read back as ZERO, conflating "the read failed" with "the addend
// is 0" -- and 0 is a legitimate addend. DiagnoseCommitment is what prints whether it actually is zero.
void* Reloc(uint32_t off);

// Shared-pool lookup: chunk = pool[1 + (idx >> 11)], string = chunk[idx & 0x7FF]. Both levels are
// [u32 count][u32 offset...] with offsets relative to their own table base, and the string carries
// FUN_002b58b0's 00 00 variant prefix.
std::wstring PoolString(uint16_t idx);

// One record out of an st2e table, with the count guard that keeps a non-ability id (the 0x4000+
// AI-opcode band) from indexing outside the table.
void* MasterRecord(uint32_t tableRva, uint32_t index);

} // namespace Internal
} // namespace BattleState

#pragma once

#include <cstdint>
#include <string>

// Item id -> the game's own display name.
//
// ⚠ THIS IS A GAME CALL. Every other name lookup in the mod is a read-only reimplementation
// (BattleState::AbilityName, EntityScan::ResolveObjectName), because the standing rule is
// memory-reads-only from mod threads. This one is the exception, for a reason worth stating so it
// is not "fixed" later:
//
//   FUN_00272cb0 dispatches through FUN_003588b0, which decodes its argument as a pooled record
//   handle (pool selector, index, and a generation check against rec+0x16) and then reads the name
//   via FUN_00263990 (rec+0x102 npcdic index, or rec+0xf8 for a custom string). Ghidra dropped the
//   register-passthrough argument on that inner call, so the exact input semantics are NOT
//   established offline -- but the call itself is play-confirmed in the battle item sublist. A
//   reimplementation would have to reverse the 14-class dispatch behind PTR_FUN_01eebd08 on an
//   inference this project's confidence bar does not accept.
//
// CALL ONLY FROM THE GAME THREAD, and only from an event hook -- never from the input thread and
// never on a per-frame path. Callers cache the finished wstring, exactly as the combat log renders
// its text at append time.
namespace ItemNames {

// Raw codec pointer, or nullptr. SEH-guarded.
const uint8_t* ResolveCodec(uint32_t itemId);

// Decoded display name; empty when unresolvable (callers must stay SILENT rather than invent one).
std::wstring Resolve(uint32_t itemId);

// NOT WRAPPED, on purpose (Session 146): FUN_00272c80 (RVA 0x152C80) is this resolver's count
// sibling -- same FUN_003588b0 lookup, then FUN_00263a10 -> the stack size at rec+0x100. It is what
// FUN_0027e530 draws its count from. The battle item list does NOT go through that draw (it shares
// FUN_0027ce70 with the magick list and shows the count from panel+0x512), so a wrapper here would
// have no caller and no play evidence behind its number. Recorded in GameArchitecture.md instead.

} // namespace ItemNames

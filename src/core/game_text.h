#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Decoder for FFXII:TZA's custom text codec (NOT UTF-16). Validated offline
// against real game data (tools/ebp_msg_decode.py + st2e_decode.py): menu
// strings, item/ability names, and NPC dialogue all decode to correct English.
// The mod NEVER stores option-name strings; it decodes what the game drew.
namespace GameText {

// Decode a NUL-terminated codec byte string into a wide string. Reads at most
// `maxBytes` codec bytes. SEH-guarded (the source struct can be transient);
// returns an empty string on fault or null input.
std::wstring Decode(const uint8_t* p, size_t maxBytes = 512);

// Apply FUN_002b58b0's variant selector before decoding a SHARED-POOL string.
// Pool strings (DAT_02ebf170 / word.bin -- ability, status, item and combatant
// names) are stored with a two-byte 00 00 prefix; decoding one without this skip
// reads the 00 as a terminator and returns an EMPTY string. Returns `p` unchanged
// when there is no prefix. SEH-guarded.
//
// Do NOT apply to actor+0x18 -- the binder stores that already variant-selected.
const uint8_t* SkipVariantPrefix(const uint8_t* p);

// Cheap heuristic: is `s` mostly printable text (>=60% ASCII 0x20-0x7e, and at
// least one letter)? Rejects decoded garbage from stale/non-text pointers.
bool IsMostlyPrintable(const std::wstring& s);

} // namespace GameText

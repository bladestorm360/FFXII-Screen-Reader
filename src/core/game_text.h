#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Decoder for FFXII:TZA's custom text codec (NOT UTF-16). Validated offline
// against real game data (tools/ebp_msg_decode.py + st2e_decode.py): menu
// strings, item/ability names, and NPC dialogue all decode to correct English.
// The mod NEVER stores option-name strings; it decodes what the game drew.
namespace GameText {

// Decode a NUL-terminated codec byte string into a wide string. Reads at most
// `maxBytes` codec bytes. SEH-guarded (the source struct can be transient);
// returns an empty string on fault or null input.
std::wstring Decode(const uint8_t* p, size_t maxBytes = 512);

// Decode into PAGES, split on the codec's page-break control byte 0x03.
//
// A multi-page message arrives as ONE string: the telop content setter hands over every page at
// once, so decoding it flat concatenates them with nothing between ("...miss out on the
// bounty.You gotta talk to...") and speaking it reads the whole conversation in a single breath.
//
// 0x03 is the page break, from the game's own codec handler FUN_002ac5f0. It is the only control
// case that RETURNS 0 -- ending the draw pass -- after storing the resume position in *param_2 and
// calling FUN_0017fae0(0); every other control falls through and keeps drawing. 0x02 does
// line-advance math and continues (a newline, not a page); 0x09 only skips itself; 0x0A does
// layout and continues.
//
// Empty pages are dropped. A message with no 0x03 yields exactly one page, so callers can treat
// single- and multi-page text identically.
void DecodePages(const uint8_t* p, size_t maxBytes, std::vector<std::wstring>& out);

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

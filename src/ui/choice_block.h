#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// THE 0x0E OPTION BLOCK, and nothing else.
//
// Split out of choice_reader.cpp so it can be exercised offline: it touches no Windows API, no
// GameText and no game memory -- it is a bounded walk over bytes the caller already copied. That is
// what lets the option-block parse be regression-tested without the game running, which is the only
// honest way to claim a change to the reader did not disturb the surfaces that already worked.
//
// Layout, with `m` pointing at the 0x0E byte (derived from FUN_002a5590 + FUN_003ffdf0, then
// CONFIRMED against a raw byte dump of a live notice board):
//
//     m+0            0x0E
//     m+1            u8 & 0x7F   option count
//     m+2, m+3       u8 & 0x7F   (default / selected indices)
//     m+4            u8 flags    bit0 = a per-option bit table follows
//     m+5 ...        that table, ceil(count/7) bytes, only when bit0 is set
//     then           length-prefixed strings: [len & 0x7F][len codec bytes] ..., 0x00 terminates
//
// The count byte is a CAPACITY (32 on a board with ~10 bills); the real list ends at the 0x00
// length terminator, which is what bounds the walk.
namespace ChoiceBlock {

constexpr uint8_t CTRL_OPTIONS = 0x0E;   // the option-block marker
constexpr uint8_t CTRL_PAGE    = 0x03;   // page break (GameText::DecodePages splits on it)
constexpr uint8_t CTRL_COL_A   = 0x0A;   // column separator inside a row
constexpr uint8_t CTRL_COL_B   = 0x05;   // column separator inside a row
constexpr int     MAX_OPTIONS  = 32;     // FUN_002b2ce0 bounds its walk at 0x20 slots

// Locate the 0x0E option block and return the codec bytes of option `slot`, or an empty span.
// Mirrors FUN_003ffdf0's header walk exactly.
//
// EVERY FALSE RETURN NAMES ITSELF. All of them used to report as "no 0x0E block", which is how a
// surface the reader had never met (the Draklor lift's numeric field) read as a parse failure for a
// whole session. `why` is a literal, so LogFail's pointer comparison still tells them apart.
inline bool OptionCodec(const std::vector<uint8_t>& buf, int slot, size_t pageOff,
                        const uint8_t** outPtr, size_t* outLen, const char** why) {
    *outPtr = nullptr; *outLen = 0; *why = "unknown";
    if (slot < 0) { *why = "negative slot"; return false; }

    // Start at the page the game says is on screen. Without this the search always returns the FIRST
    // block in the message -- the notice board's mark list -- even when the player is three pages
    // further on, looking at a Yes/No.
    size_t m = pageOff;
    if (m >= buf.size()) { *why = "page offset past the end of the cached message"; return false; }

    // The question text and any column headers precede the marker, so it is never the first byte.
    // Stop at the NEXT page break: a page without a block has no options, and borrowing the next
    // page's would be worse than silence.
    while (m < buf.size() && buf[m] != CTRL_OPTIONS) {
        if (buf[m] == 0x00 || buf[m] == CTRL_PAGE) {
            *why = "no 0x0E block on this page (terminator or page break reached first)";
            return false;
        }
        ++m;
    }
    if (m + 5 >= buf.size()) { *why = "0x0E block header runs past the cached message"; return false; }

    const int count = buf[m + 1] & 0x7F;
    if (count <= 0 || count > MAX_OPTIONS) { *why = "0x0E count byte out of range"; return false; }
    if (slot >= count) { *why = "slot past the block's own count"; return false; }
    const uint8_t flags = buf[m + 4];

    // FUN_003ffdf0: p = m+4; if (flags & 1) p += ceil(count/7); then p += 1.
    size_t p = m + 4;
    if (flags & 1) p += static_cast<size_t>((count + 6) / 7);
    p += 1;

    // Length-prefixed entries: [len & 0x7F][len bytes]. A 0x00 length ends the REAL list, which is
    // shorter than `count` -- that byte is a capacity.
    for (int i = 0; i <= slot; ++i) {
        if (p >= buf.size()) { *why = "entry walk ran past the cached message"; return false; }
        const size_t len = buf[p] & 0x7F;
        if (len == 0) { *why = "slot past the list terminator (count is a capacity)"; return false; }
        if (p + 1 + len > buf.size()) { *why = "entry length runs past the cached message"; return false; }
        if (i == slot) { *outPtr = &buf[p + 1]; *outLen = len; return true; }
        p += 1 + len;
    }
    *why = "entry walk ended before reaching the slot";
    return false;
}

} // namespace ChoiceBlock

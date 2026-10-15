#pragma once

#include <cstddef>
#include <cstdint>

// Codec framing shared by the guarded snapshot and the text decoder. Selector
// lengths match tools/ffxii_codec.py and the existing interpreter registry
// (FUN_002ac5f0 / FUN_002b1fb0); this does not assign words to unknown icons.
namespace GameText::Codec {

constexpr int kUnknown = -1;
constexpr int kIncomplete = -2;

inline int EscapeParamCount(const uint8_t* selector, size_t n) {
    const uint8_t s = selector[0];
    auto at = [&](size_t k) -> uint8_t { return k < n ? selector[k] : 0; };
    switch (s) {
        case 0x30: case 0x5F: return 0; // existing sparse-switch holes
        case 0x21: return 0;
        case 0x31: case 0x6C: return 3;
        case 0x29: {
            const int k = at(1) & 7;
            return k == 0 ? 2 : 1 + k;
        }
        case 0x2A: return 2 + (at(2) & 0x7F);
        case 0x38: case 0x6F: return (at(1) & 7) == 0 ? 2 : 3;
        default: break;
    }
    if (s >= 0x40 && s <= 0x6B && s != 0x56) return 1;
    if (s >= 0x20 && s <= 0x70) return 2;
    return kUnknown;
}

inline int ControlLength(uint8_t c) {
    switch (c) {
        case 0x00: case 0x0B: case 0x0C: case 0x0D: return 0;
        case 0x08: return 3;
        case 0x0E: return kUnknown;
        default: return 1;
    }
}

// Positive = complete token size; 0 = terminator at a TOKEN boundary.
// Incomplete tokens must not substitute a number or consume a numeric scope.
// Zero bytes INSIDE a token are payload, not string terminators.
inline int TokenLength(const uint8_t* token, size_t n) {
    if (n == 0) return kIncomplete;
    const uint8_t c = token[0];
    int length = 1;
    if (c == 0x0F) {
        if (n < 2) return kIncomplete;
        const uint8_t s = token[1];
        if ((s == 0x29 || s == 0x38 || s == 0x6F) && n < 3) return kIncomplete;
        if (s == 0x2A && n < 4) return kIncomplete;
        const int params = EscapeParamCount(token + 1, n - 1);
        if (params < 0) return kUnknown;
        length = 2 + params;
    } else if (c < 0x10) {
        length = ControlLength(c);
        if (length <= 0) return length;
    } else if (c <= 0x1F) {
        length = 2;
    }
    return static_cast<size_t>(length) <= n ? length : kIncomplete;
}

} // namespace GameText::Codec

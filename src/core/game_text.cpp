#include "core/game_text.h"

#include <Windows.h>

namespace GameText {
namespace {

// Copy up to maxBytes codec bytes (until a 0x00 terminator) out of a possibly-
// transient game buffer. SEH-guarded and object-free so it can use __try.
bool SafeCopy(const uint8_t* p, size_t maxBytes, uint8_t* out, size_t* outLen) {
    __try {
        for (size_t i = 0; i < maxBytes; ++i) {
            uint8_t b = p[i];
            out[i] = b;
            if (b == 0x00) { *outLen = i; return true; }
        }
        *outLen = maxBytes;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------------------------
// The COMPLETE control-code table, from the game's own interpreter FUN_002ac5f0 (RVA 0x18C5F0).
//
// Ported from `..\FFXII-Decompile\tools\ffxii_codec.py`, which is the reference implementation and
// carries the regression: re-decoding all 102 shipped battle messages leaves ZERO structurally
// unexplained bytes. Port changes from there, not the other way round.
//
// WHY THIS REPLACED THE OLD RULE. The previous decoder used "after 0x0f <sel>, consume every
// following byte >= 0x80". That is wrong on **75 of the 102** battle messages: digits (0x85-0x8e)
// and punctuation (0x99/0x9a/0xa0-0xaf) are all >= 0x80, so they were eaten as if they were escape
// parameters. Live capture of the shipped text made the damage concrete -- message 0x33 came out as
// "Vaans steal failed" with the trailing period gone, because the sentence ends with a 0x29 colour
// escape and the fallback swallowed whatever followed it. With the table below the same message
// decodes as "Vaan's steal failed!\nDire Rat C has nothing to steal." (confirmed live 2026-07-20).
//
// FUN_002b1fb0 is the game's own "how many bytes is this token" helper: it calls FUN_002ac5f0 and
// returns the advance, which is what proves each decoder's return value is the advance FROM THE
// SELECTOR byte -- hence params = ret - 1.
// ---------------------------------------------------------------------------------------------

// Parameter-byte count for a 0x0f escape whose SELECTOR sits at buf[sel].
// Returns -1 only for selectors outside the interpreter's range (treated as "stop").
int EscapeParamCount(const uint8_t* buf, size_t n, size_t sel) {
    const uint8_t s = buf[sel];
    // Bounds-safe peek at the k'th byte after the selector.
    auto at = [&](size_t k) -> uint8_t { return (sel + k < n) ? buf[sel + k] : 0; };

    switch (s) {
        // No case in FUN_002ac5f0 and no inner `default:` -- the 0x0f is consumed and the selector
        // falls through to render as an ordinary glyph. Neither occurs in any shipped message.
        // Confidence 0.97; deliberately left below the ship bar because nothing depends on it.
        case 0x30: case 0x5F: return 0;

        case 0x21: return 0;    // inline, no decoder
        case 0x31: return 3;    // FUN_003fff10 -- the codec-sprintf STRING substitution slot
        case 0x6C: return 3;    // FUN_003ffce0

        // --- the three VARIABLE-LENGTH escapes -------------------------------------------------
        case 0x29: {            // FUN_003ffbc0
            const int k = at(1) & 7;
            return (k == 0) ? 2 : 1 + k;
        }
        case 0x2A:              // FUN_003ffd70 -- ruby / annotation block
            return 2 + (at(2) & 0x7F);
        case 0x38: case 0x6F:   // FUN_003ffb00 -- 16-bit value reader
            return ((at(1) & 7) == 0) ? 2 : 3;

        default: break;
    }

    // Icon / glyph family: FUN_002aeb20 returns 2 on every path => ONE parameter byte, not a
    // high-bit run. 0x56 has its own 2-param case and 0x5F is a hole, both handled above/below.
    if (s >= 0x40 && s <= 0x6B && s != 0x56) return 1;

    // Everything else the interpreter handles in 0x20..0x70 takes 2 parameter bytes
    // (FUN_003ffab0 / 003ffc60 / 003ffda0 / 003ffee0 / 003ffb90, all returning 3).
    if (s >= 0x20 && s <= 0x70) return 2;

    return -1;
}

// Bytes consumed by a sub-0x10 control code, INCLUDING the control byte itself.
// 0 means "this terminates the string". -1 means "length unknown -- stop, do not guess".
int ControlLength(uint8_t c) {
    switch (c) {
        case 0x00: case 0x0B: case 0x0C: case 0x0D:
            return 0;           // hit `default:` in FUN_002ac5f0:355-358, which returns 0
        case 0x08:
            return 3;           // FUN_002ac5f0:277-279
        case 0x0E:
            return -1;          // FUN_003ffdf0, variable and NOT fully decoded -- never guessed
        default:
            return 1;           // 0x01-0x07, 0x09, 0x0A
    }
}

// Space-equivalent controls. FUN_002ac5f0:115-118 lists them as ONE fall-through group:
//     case '\x01': case '\x04': case '\x06': case '\a':
// 0x04 was already known to be a space, so all four render identically.
//
// This fixed a visible defect: the game separates an enemy's name from its instance letter with
// 0x06, so `31 3a 4d 06 21` is "Rat" + SPACE + "B". Mapping only 0x04 produced "Dire RatB" where
// the game draws "Dire Rat B".
//
// 0x05 is deliberately excluded -- it has its own case and FUN_002ac5f0's 0x0a handler scans for it
// as a structural delimiter, so it is a separator rather than plain whitespace.
inline bool IsSpaceControl(uint8_t c) {
    return c == 0x01 || c == 0x04 || c == 0x06 || c == 0x07;
}

} // namespace

std::wstring Decode(const uint8_t* p, size_t maxBytes) {
    if (!p) return std::wstring();
    if (maxBytes > 1024) maxBytes = 1024;

    uint8_t buf[1024];
    size_t n = 0;
    if (!SafeCopy(p, maxBytes, buf, &n)) return std::wstring();

    std::wstring out;
    out.reserve(n);

    size_t i = 0;
    while (i < n) {
        const uint8_t c = buf[i];

        if (c == 0x0F) {                                  // inline format escape
            if (i + 1 >= n) break;
            const int params = EscapeParamCount(buf, n, i + 1);
            if (params < 0) break;                        // unknown selector: stop, never guess
            i += 2 + static_cast<size_t>(params);
            continue;
        }

        if (c < 0x10) {                                   // control byte
            if (IsSpaceControl(c)) out.push_back(L' ');
            else if (c == 0x02)    out.push_back(L'\n');
            const int len = ControlLength(c);
            if (len <= 0) break;                          // terminator, or length unknown
            i += static_cast<size_t>(len);
            continue;
        }

        if (c <= 0x1F) {                                  // 2-byte extended glyph
            // FUN_002ac2f0:25-32 -- glyph = bank[c & 0xf] + (next - 0x20), advance 2. We cannot
            // map the extended banks to Unicode, but we MUST consume both bytes: the old decoder
            // dropped the lead byte and then rendered the trailing byte as a stray letter.
            i += 2;
            continue;
        }

        if (c >= 0x20 && c <= 0x39)      out.push_back(static_cast<wchar_t>(L'A' + (c - 0x20)));
        else if (c >= 0x3A && c <= 0x53) out.push_back(static_cast<wchar_t>(L'a' + (c - 0x3A)));
        else if (c >= 0x85 && c <= 0x8E) out.push_back(static_cast<wchar_t>(L'0' + (c - 0x85)));
        else {
            // Extended glyphs. The font atlas IS the character map (FUN_002ac2f0:75 computes the
            // glyph slot as `byte - 0x20`), so these can only ever be established empirically.
            // A-Z/a-z and ! ? , . ' are corroborated at 0.99 by the shipped battle messages; the
            // rest carry their prior observational status. Unmapped bytes are dropped, never
            // guessed.
            switch (c) {
                case 0x99: out.push_back(L'!');  break;
                case 0x9A: out.push_back(L'?');  break;
                case 0xA4: out.push_back(L'+');  break;   // "New Game+" glyph
                case 0xA5: out.push_back(L'-');  break;
                case 0xA7: out.push_back(L',');  break;
                case 0xA8: out.push_back(L'.');  break;
                case 0xAA: out.push_back(L':');  break;
                case 0xAC: out.push_back(L'\''); break;
                case 0xAE: out.push_back(L'(');  break;
                case 0xAF: out.push_back(L')');  break;
                case 0xA2: out.push_back(L'/');  break;
                case 0xA0: out.push_back(L'&');  break;   // "Magicks & Technicks"
                case 0x9E: out.push_back(L'%');  break;
                case 0x8F: out.push_back(L'-');  break;   // em-dash
                default:   break;                         // unmapped extended glyph: drop
            }
        }
        ++i;
    }
    return out;
}

const uint8_t* SkipVariantPrefix(const uint8_t* p) {
    // FUN_002b58b0's variant selector. Strings in the SHARED CODEC POOL (DAT_02ebf170, i.e.
    // word.bin) are stored with a two-byte 00 00 prefix:
    //     00 00 | 22 4e 4b 3e 00   ->  "Cure"
    // Decoding one without this skip reads the 00 as a terminator and yields an EMPTY string --
    // which is exactly what made a probe print "idx150" instead of "Attack" for action 0x96.
    //
    // NOTE actor+0x18 needs NO skip: the binder FUN_0023a570 calls FUN_002b58b0 before storing it,
    // which is why the existing combatant-name reads work as-is.
    if (!p) return p;
    __try {
        if (p[0] == 0x00 && p[1] == 0x00) return p + 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return p;
    }
    return p;
}

bool IsMostlyPrintable(const std::wstring& s) {
    if (s.empty()) return false;
    size_t ascii = 0, alpha = 0;
    for (wchar_t ch : s) {
        if (ch >= 0x20 && ch < 0x7f) ++ascii;
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z')) ++alpha;
    }
    return alpha >= 1 && ascii >= (s.size() * 3 + 4) / 5;  // >= 60%
}

} // namespace GameText

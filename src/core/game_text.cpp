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

// Parameter-byte count for a 0x0f escape selector; -1 = unknown.
//
// Sourced from the game's OWN control-code interpreter FUN_002ac5f0 (RVA 0x18C5F0), which switches
// on bytes < 0x10 and, for 0x0f, on the selector 0x20-0x6f. Each case calls a param decoder whose
// return value is how far to advance from the selector:
//   FUN_003ffab0 -> returns 3 => selector + 2 param bytes
//   FUN_003fff10 -> returns 4 => selector + 3 param bytes
//   0x21         -> inline, no decoder => 0 param bytes
// The 0x40-0x6b block is the icon/glyph family (shared handler -> FUN_002aeb20); its selectors take
// a variable number of params which we have NOT decoded, so they return -1 and use the fallback.
// This reconciles with tools/ebp_msg_decode.py's observed set (0x20/0x27/0x2f/0x37/0x3c = 2,
// 0x60 = 1, 0x21 = 0) and extends it — notably 0x31, the string-substitution slot used by the
// obtained-item template, takes 3.
int EscapeParamCount(uint8_t sel) {
    switch (sel) {
        case 0x21:
            return 0;
        case 0x20: case 0x27: case 0x2f: case 0x32: case 0x34: case 0x35:
        case 0x37: case 0x3a: case 0x3c: case 0x3d: case 0x3e: case 0x56:
            return 2;   // FUN_003ffab0
        case 0x31:
            return 3;   // FUN_003fff10 — the codec-sprintf "%s" slot
        default:
            return -1;  // unknown / icon family -> conservative high-bit fallback
    }
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
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = buf[i];
        if (c == 0x00) break;
        else if (c == 0x02) out.push_back(L'\n');                              // line-break
        else if (c == 0x04) out.push_back(L' ');                               // space
        else if (c >= 0x20 && c <= 0x39) out.push_back((wchar_t)(L'A' + (c - 0x20))); // A-Z
        else if (c >= 0x3a && c <= 0x53) out.push_back((wchar_t)(L'a' + (c - 0x3a))); // a-z
        else if (c >= 0x85 && c <= 0x8e) out.push_back((wchar_t)(L'0' + (c - 0x85))); // 0-9
        else if (c == 0x0f) {
            // Inline format escape: 0x0f <selector> <params...>. The parameter count is
            // SELECTOR-DEPENDENT (see EscapeParamCount) — it is NOT "every following byte with the
            // high bit set". That old rule is only an approximation: digits are 0x85-0x8e and
            // punctuation is 0x99/0x9a/0xa0-0xaf, all >= 0x80, so a zero-parameter escape (0x21)
            // followed by a number had the number eaten. "Obtained 3 Potions!" lost both the 3 and
            // the !, and the live tutorial text shows it as "The  command can" (the button glyph
            // and nothing else survived). Counts come from the game's own control-code interpreter
            // FUN_002ac5f0, whose per-selector param decoders are FUN_003ffab0 (2 params) and
            // FUN_003fff10 (3 params).
            if (i + 1 >= n) break;
            const uint8_t sel = buf[i + 1];
            ++i;                                      // consume selector
            int params = EscapeParamCount(sel);
            if (params < 0) {
                // Unknown selector: fall back to the old high-bit run so we never emit raw
                // parameter bytes as text. Conservative, and only reachable for selectors the
                // interpreter maps to decoders we have not read.
                while (i + 1 < n && buf[i + 1] >= 0x80) ++i;
            } else {
                for (int k = 0; k < params && i + 1 < n; ++k) ++i;
            }
        }
        else {
            switch (c) {
                case 0x99: out.push_back(L'!'); break;
                case 0x9a: out.push_back(L'?'); break;
                case 0xa4: out.push_back(L'+'); break;   // "New Game+" glyph
                case 0xa5: out.push_back(L'-'); break;
                case 0xa7: out.push_back(L','); break;
                case 0xa8: out.push_back(L'.'); break;
                case 0xaa: out.push_back(L':'); break;
                case 0xac: out.push_back(L'\''); break;
                case 0xae: out.push_back(L'('); break;
                case 0xaf: out.push_back(L')'); break;
                case 0xa2: out.push_back(L'/'); break;
                case 0xa0: out.push_back(L'&'); break;   // "Magicks & Technicks"
                case 0x9e: out.push_back(L'%'); break;
                case 0x8f: out.push_back(L'-'); break;   // em-dash
                default:   break;                        // unmapped extended glyph: drop
            }
        }
    }
    return out;
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

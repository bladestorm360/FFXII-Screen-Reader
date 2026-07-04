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
            // Variable/format escape: 0x0f <selector> <params...> where every
            // param byte has the high bit set (>=0x80); the run ends at the
            // first byte < 0x80 (matches tools/ebp_msg_decode.py).
            if (i + 1 < n) ++i;                       // skip selector
            while (i + 1 < n && buf[i + 1] >= 0x80) ++i;  // skip params
        }
        else {
            switch (c) {
                case 0x99: out.push_back(L'!'); break;
                case 0x9a: out.push_back(L'?'); break;
                case 0xa5: out.push_back(L'-'); break;
                case 0xa7: out.push_back(L','); break;
                case 0xa8: out.push_back(L'.'); break;
                case 0xaa: out.push_back(L':'); break;
                case 0xac: out.push_back(L'\''); break;
                case 0xae: out.push_back(L'('); break;
                case 0xaf: out.push_back(L')'); break;
                case 0xa2: out.push_back(L'/'); break;
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

#include "core/game_text.h"

#include <Windows.h>
#include <cstdint>
#include <string>

// THE ENGINE'S SECOND STRING FORMAT -- "ex00" + UTF-16LE (S184).
//
// Not every string FUN_002f9860 hands back is codec text. menu_expansion.bin (message section 19)
// stores some entries as the four ASCII bytes "ex00" followed by little-endian UTF-16 up to a 0x0000:
// ids 0x4A48..0x4A4D are "LEADER", "PARTY", "FOES", "RESERVE", "ALLIES", "TIME" (US data, decoded
// offline). Decoded as codec those bytes are two stray glyphs and "QQ" -- rubbish that can pass the
// printable test.
//
// The engine treats the prefix as a format switch, not as data, and does so in its common label path:
//   * FUN_00364980 (RVA 0x244980) is the whole test -- first byte 'e', second byte 'x';
//   * FUN_002d8690 / FUN_002d87d0 / FUN_002d8910 / FUN_002d89e0 / FUN_002d8b00 each call it on the
//     string and hand an "ex" string to the wide renderer FUN_003649a0 instead of the codec one;
//   * FUN_00365b50 copies the 4-byte header, then walks u16 characters, treating `{` as the start of
//     an insert (`{i...`) rather than a character;
//   * FUN_0029ccf0 WRITES one: "ex00" then L'+' or L'-', which is where the exact header comes from.
//
// This file matches the full "ex00" header -- stricter than the engine's two-byte test on purpose, so a
// codec string can never be taken for a wide one (the stricter test can only cost silence, which is
// what such a string produced before). Inserts are dropped: they carry a value, not text of their own.
namespace GameText {
namespace {

constexpr size_t kExHeader   = 4;
constexpr size_t kExMaxChars = 256;

// POD-only so it can sit under __try. The header test reads one byte at a time and stops at the first
// mismatch, so an ordinary codec string costs one compare; a short string that runs into an unmapped
// page faults into the handler and falls back to the codec path.
bool CopyExUnits(const uint8_t* p, size_t maxChars, wchar_t* out, size_t* outLen) {
    __try {
        if (p[0] != 'e' || p[1] != 'x' || p[2] != '0' || p[3] != '0') return false;
        size_t k = 0;
        for (; k < maxChars; ++k) {
            const uint16_t u = static_cast<uint16_t>(p[kExHeader + 2 * k] | (p[kExHeader + 2 * k + 1] << 8));
            if (u == 0) break;
            out[k] = static_cast<wchar_t>(u);
        }
        *outLen = k;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool DecodeExString(const uint8_t* p, size_t maxBytes, std::wstring& out) {
    if (!p || maxBytes < kExHeader) return false;   // the caller did not allow even the header
    size_t maxChars = (maxBytes - kExHeader) / 2;
    if (maxChars > kExMaxChars) maxChars = kExMaxChars;
    wchar_t units[kExMaxChars];
    size_t n = 0;
    if (!CopyExUnits(p, maxChars, units, &n)) return false;

    out.clear();
    out.reserve(n);
    bool inInsert = false;
    for (size_t k = 0; k < n; ++k) {
        const wchar_t ch = units[k];
        if (inInsert) { if (ch == L'}') inInsert = false; continue; }
        if (ch == L'{') { inInsert = true; continue; }
        out.push_back(ch);
    }
    return true;
}

} // namespace GameText

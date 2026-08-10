#include "core/game_text.h"
#include "core/message_macro.h"
#include "core/game_glyphs.h"
#include "core/game_glyphs_pl.h"
#include "core/hooks.h"      // ResolveRva -- the font manager + the glyph-record getter
#include "core/mem_read.h"   // guarded reads of the loaded font records
#include "core/logger.h"     // the detection verdict, and the bytes when it fails

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cwctype>

namespace GameText {
namespace {

SpriteNameFn g_spriteResolver = nullptr;

// ---- glyph variant ------------------------------------------------------------------------------
// The live byte -> character table. `kGlyph` (generated from the game's own font00.dat) is the base;
// a fan translation that repaints atlas slots patches a handful of entries over it.
//
// Built once per variant change rather than consulted per character: the decode loop runs per byte
// of every string the mod speaks, and a 16-entry search inside it would be paid on every letter of
// every line. One array, one index, no branch.
std::atomic<Variant> g_variant{Variant::Standard};
wchar_t g_glyph[256];
std::atomic<bool> g_glyphReady{false};

void BuildGlyphTable(Variant v) {
    for (int i = 0; i < 256; ++i) g_glyph[i] = kGlyph[i];
    if (v == Variant::PolishPatch) {
        for (const auto& o : kGlyphPolish) g_glyph[o.byte] = o.ch;
    }
    g_glyphReady.store(true, std::memory_order_release);
}

// ---- WHICH FONT ATLAS IS LOADED — detected, not asked (Session 147) ------------------------------
//
// STRIKES "autodetection is not available" (game_glyphs_pl.h, debug.md S130). That claim was about
// the DISK, and it is true there: the Polish patch repacks the VBF in place, leaves no loose file
// and no version marker, and its own font metadata still names the stock letters. But the thing the
// setting was describing was never a file on disk -- it was WHICH ATLAS THE GAME LOADED, and that is
// sitting in memory in the font manager.
//
// THE FINGERPRINT, measured by diffing the two `font00.dat` files byte for byte (both 46,876 bytes):
// they differ in EXACTLY 20 bytes, in 10 records, and every one of those 20 is an advance width
// (the file stores it twice per record, at +0x0C and +0x10). S130 saw this and wrote it down --
// "the patch adjusts ten advance widths" -- without noticing that ten adjusted widths ARE the marker.
//
//   slot  60  61  62   84  85  86   98  117 118  179     (record ordinal == slot; byte = slot+0x20)
//   stock 21  21  21   22  22  22   24  19  36   36
//   PL    20  24  24   17  18  18   20  11  11   11
//
// HOW THIS READS THEM WITHOUT KNOWING THE STRUCT. `FUN_0017f8c0(slot)` hands back the loaded record,
// but it comes out of a std::map (`FUN_001fdec0` is a red-black-tree lookup returning `node+0x24`),
// so the in-memory layout is the LOADER's, not the file's, and guessing which field is the advance
// would be exactly the kind of unvalidated assumption this project keeps paying for. So it does not
// guess: it walks every 4-byte-aligned offset in the record and asks whether the ten values sitting
// at that offset spell the stock vector or the PL vector. Ten values matching a specific vector by
// accident is not a thing that happens, and the offset that matches IS the advance field -- located
// by measurement rather than assumed.
//
// If NEITHER vector matches, that is a fact worth having: the record layout moved, or the game
// updated its font. It logs the bytes it saw and stays on Standard, which is every unmodified
// install in all twelve languages -- so an unrecognised font can only ever leave behaviour where it
// already was, never make it worse.
constexpr int      kFpSlots = 10;
constexpr uint32_t kFpSlot [kFpSlots] = { 60, 61, 62, 84, 85, 86, 98, 117, 118, 179 };
constexpr uint32_t kFpStock[kFpSlots] = { 21, 21, 21, 22, 22, 22, 24,  19,  36,  36 };
constexpr uint32_t kFpPolish[kFpSlots] = { 20, 24, 24, 17, 18, 18, 20,  11,  11,  11 };

constexpr uint32_t RVA_FONT_MGR    = 0x1EE11F8;  // DAT_01f811f8 -- the font manager (FUN_001b5fa0)
constexpr uint32_t RVA_GLYPH_REC   = 0x5F8C0;    // FUN_0017f8c0(slot) -> loaded glyph record
constexpr uint32_t kRecScan        = 0x24;       // bytes of the record we are willing to look at

typedef void* (*Pfn_GlyphRecord)(uint32_t);

void* GlyphRecord(uint32_t slot) {
    auto fn = reinterpret_cast<Pfn_GlyphRecord>(Hooks::ResolveRva(RVA_GLYPH_REC));
    if (!fn) return nullptr;
    __try { return fn(slot); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

std::atomic<bool> g_variantDetected{false};

void DetectVariantOnce() {
    if (g_variantDetected.exchange(true)) return;         // one attempt per process, whatever it finds

    // The manager is created during boot; until it exists there is nothing to read and no honest
    // answer to give, so re-arm and let the next caller try again.
    void* mgr = nullptr;
    if (!MemRead::SafeReadPtr(Hooks::ResolveRva(RVA_FONT_MGR), &mgr) || !mgr) {
        g_variantDetected.store(false, std::memory_order_relaxed);
        return;
    }

    uint32_t rec[kFpSlots][kRecScan / 4] = {};
    for (int i = 0; i < kFpSlots; ++i) {
        void* r = GlyphRecord(kFpSlot[i]);
        if (!r) { g_variantDetected.store(false, std::memory_order_relaxed); return; }
        for (uint32_t off = 0; off < kRecScan; off += 4) {
            if (!MemRead::SafeReadU32(r, off, &rec[i][off / 4])) {
                g_variantDetected.store(false, std::memory_order_relaxed);
                return;
            }
        }
    }

    for (uint32_t off = 0; off < kRecScan; off += 4) {
        bool stock = true, polish = true;
        for (int i = 0; i < kFpSlots; ++i) {
            if (rec[i][off / 4] != kFpStock[i])  stock  = false;
            if (rec[i][off / 4] != kFpPolish[i]) polish = false;
        }
        if (stock == polish) continue;                    // both or neither -- not the advance field
        const Variant v = polish ? Variant::PolishPatch : Variant::Standard;
        g_variant.store(v, std::memory_order_relaxed);
        BuildGlyphTable(v);
        char b[160];
        snprintf(b, sizeof(b),
                 "font atlas DETECTED: %s (advance field at record+0x%02X, 10/10 slots matched)",
                 polish ? "Polish fan patch" : "standard", off);
        Log::Write("TEXT", b);
        return;
    }

    // No vector matched. Print what was actually there -- this is the one line that turns "detection
    // failed" into a fix, and without it the next session would be re-deriving the fingerprint.
    char b[320];
    int n = snprintf(b, sizeof(b), "font atlas UNRECOGNISED (staying standard). slot values:");
    for (int i = 0; i < kFpSlots && n > 0 && n < static_cast<int>(sizeof(b)); ++i) {
        n += snprintf(b + n, sizeof(b) - n, " [%u]", kFpSlot[i]);
        for (uint32_t off = 0; off < kRecScan && n < static_cast<int>(sizeof(b)); off += 4)
            n += snprintf(b + n, sizeof(b) - n, "%s%u", off ? "," : "=", rec[i][off / 4]);
    }
    Log::Write("TEXT", b);
}

// Where the last decode on this thread gave up. See DecodeBail in the header for why this matters.
thread_local DecodeBail g_bail;

// ---- inline element sprites ---------------------------------------------------------------------
// `0F 3F 81 <XX>`, XX = 0x8A..0x91, is the eight-element icon insert. Selector 0x3F already takes
// 2 parameter bytes in EscapeParamCount, so the framing was never wrong -- the escape was simply
// consumed and nothing emitted, which is why "Half Damage: " was followed by silence.
//
// The mapping is the game's own, established three independent ways (all offline, conf 0.99):
//   1. menu_expansion.bin string ids 0x4B27+n -- the very strings FUN_00293ce0:41 appends per set
//      element bit -- each hold exactly `0f 3c c1 fe | 0f 3f 81 <8A+n> | 0f 3c 81 80`.
//   2. help_action.bin names every sprite in its own prose: "Deal <0F 3F 81 8A> fire damage to one
//      foe.", once per element, so the byte and the word sit side by side in shipped data.
//   3. word.bin chunk 4 idx 23..30 and attribute_data.bin's eight u16s (0x2017..0x201E) agree on
//      the same order: Fire, Lightning, Ice, Earth, Water, Wind, Holy, Dark.
// A scan of all 27 US st2e master-data files found 0x3F used ONLY for these eight, plus one stray
// `0F 3F 46 00` in battle_pack.bin which fails the 0x81 test below and is left alone.
constexpr uint8_t kSpriteSel = 0x3F, kSpriteP1 = 0x81, kSpriteLo = 0x8A, kSpriteHi = 0x91;
// The macro printer (see the 0x2E note in the decode loop). `kMacroMax` rejects a cached value
// that cannot be this line's -- macros the game substitutes are small counts, never six digits.
constexpr uint8_t kMacroSel = 0x2E;
constexpr int32_t kMacroMax = 99999;

// Marker parked in the Unicode private-use area while decoding, resolved to a word afterwards.
// Deferring the lookup keeps the decode loop free of std::wstring building and, more importantly,
// lets the adjacency rule below see the text on BOTH sides of the sprite.
constexpr wchar_t kSpriteMark = 0xE000;
inline bool IsSpriteMark(wchar_t ch) { return ch >= kSpriteMark && ch < kSpriteMark + 8; }

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

// Codec-buffer cap. Was 1024 with callers passing 512, which cut the 4-page hunt-tutorial message
// off mid-sentence ("...Then you hunt it, "). A multi-page conversation arrives as ONE string, so
// the cap has to cover the whole thing, not one screen.
constexpr size_t kMaxCodecBytes = 4096;

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

inline bool IsAsciiAlpha(wchar_t ch) {
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
}

inline wchar_t LowerAscii(wchar_t ch) {
    return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
}

// Does the run of letters starting at `at` (scanning `dir`) spell exactly `word`, case-insensitively
// and with nothing else attached? `at` must already be past any spaces.
bool WordAt(const std::wstring& s, size_t at, int dir, const std::wstring& word) {
    if (word.empty()) return false;
    const size_t len = word.size();
    if (dir > 0) {
        if (at + len > s.size()) return false;
        for (size_t k = 0; k < len; ++k)
            if (LowerAscii(s[at + k]) != LowerAscii(word[k])) return false;
        // Reject a longer word that merely starts with it ("firearm" is not "fire").
        return (at + len >= s.size()) || !IsAsciiAlpha(s[at + len]);
    }
    // Walking left, `at` is the run's LAST character, so compare the word backwards too.
    if (at + 1 < len) return false;
    for (size_t k = 0; k < len; ++k)
        if (LowerAscii(s[at - k]) != LowerAscii(word[len - 1 - k])) return false;
    const size_t before = at + 1 - len;           // first char of the matched run
    return (before == 0) || !IsAsciiAlpha(s[before - 1]);
}

// Turn sprite markers into words.
//
// ADJACENCY SUPPRESSION -- and why this is NOT the banned kind of dedup.
//
// The NO-DEDUPLICATION rule bans CROSS-EVENT suppression: a "same as last time, stay quiet" filter
// that makes a re-entered surface silent. This is not that. It is a within-string RENDERING
// decision inside the codec decoder. The game writes both an icon and the word for one token --
// help_action.bin literally reads "Deal <fire sprite> fire damage to one foe." -- so emitting both
// yields "fire fire damage", a decoding artifact rather than information. In the item panel the
// same sprite stands alone after "Half Damage: " and there is no word to collide with, which is
// why a caller-based discriminator is impossible: both strings come out of the same formatter.
//
// No event is suppressed. The string is spoken in full every time, re-entry always re-speaks,
// nothing is cached across calls, there is no time window and no last-spoken state. The decision is
// a pure function of the single string being decoded, so the failure the rule exists to prevent --
// silence on re-entry -- is structurally impossible here.
//
// The DEFAULT IS TO EMIT; suppression requires an exact adjacent-word match. In a locale that
// declines or displaces the noun the match simply fails and the element is said twice: a stumble,
// never silence. If a future locale bug reports a doubled element word, this is the reason.
void ResolveSprites(std::wstring& s) {
    // Early-out must test the whole MARK RANGE, not the literal base character. This read
    // `s.find(kSpriteMark)` for one build, which matches only 0xE000 -- element bit 0 -- so Fire
    // resolved and the other seven were dropped without a trace. Caught from the DESC dump: two
    // byte-identical weapon descriptions differing only in the sprite id gave "Element: Fire" and
    // "Element: " (S125). A range check reads the same but is not the same.
    bool any = false;
    for (const wchar_t ch : s) { if (IsSpriteMark(ch)) { any = true; break; } }
    if (!any) return;

    std::wstring out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t ch = s[i];
        if (!IsSpriteMark(ch)) { out.push_back(ch); continue; }

        std::wstring name = g_spriteResolver ? g_spriteResolver(static_cast<int>(ch - kSpriteMark))
                                             : std::wstring();
        if (!name.empty()) {
            // Right: skip spaces, then compare the letter run.
            size_t r = i + 1;
            while (r < s.size() && s[r] == L' ') ++r;
            bool dupe = (r < s.size()) && WordAt(s, r, +1, name);
            if (!dupe && i > 0) {
                // Left: same, walking backwards from the last non-space before the marker.
                size_t l = i;
                while (l > 0 && s[l - 1] == L' ') --l;
                dupe = (l > 0) && WordAt(s, l - 1, -1, name);
            }
            if (dupe) name.clear();
        }

        if (name.empty()) {
            // Dropped (unresolved, or the word is already there). Collapse the space the sprite
            // was sitting between so the line does not read with a hole in it.
            if (!out.empty() && out.back() == L' ' && i + 1 < s.size() && s[i + 1] == L' ') ++i;
            continue;
        }

        // A run of sprites is a LIST -- "Half Damage: <fire><ice>" is two elements, and the game
        // separates its own visible list with ", " (listhelp_common.bin entry 10).
        if (!out.empty() && (IsAsciiAlpha(out.back()) || (out.back() >= L'0' && out.back() <= L'9')))
            out += L", ";
        out += name;
    }
    s.swap(out);
}

// Shared decode loop, used by BOTH Decode and DecodePages so the codec is understood in exactly
// one place. Always splits at the 0x03 page break.
//
// 0x03 IS CONFIRMED FROM SHIPPED DATA, not inferred. tools/ebp_find_pagebreak.py decoded 17,268
// dialogue messages out of 617 extracted .ebp scripts, tracked the source offset of every emitted
// character, and histogrammed the invisible bytes sitting where a sentence visibly runs into the
// next page ("...the bounty." immediately followed by "You gotta"):
//     0x03           3164 of 3309 occurrences are page boundaries   (95.6%)
//     0x0f 20          68 of 3422                                    (2.0%)
//     0x0f 27          36 of 2427                                    (1.5%)
// which matches FUN_002ac5f0, where 0x03 is the only control case that returns 0 (ending the draw
// pass) after storing the resume position in *param_2.
//
// Always splits; a message without one yields a single page, and
// Decode simply concatenates -- which reproduces the previous behaviour exactly, since 0x03 used to
// fall through the `default: return 1` arm and emit nothing.
bool DecodeToPages(const uint8_t* p, size_t maxBytes, std::vector<std::wstring>& pages) {
    // Detect the font atlas before the first character is mapped. It lives HERE, at the one funnel
    // every decode in the mod passes through, rather than in an Init: the font manager does not
    // exist yet when the mod initialises, and the first string the mod ever decodes must already be
    // using the right table. One relaxed load per decoded STRING (not per character), and once it
    // succeeds this is a single predictable branch forever after.
    if (!g_variantDetected.load(std::memory_order_relaxed)) DetectVariantOnce();

    pages.clear();
    g_bail = DecodeBail{};
    if (!p) return false;
    if (maxBytes > kMaxCodecBytes) maxBytes = kMaxCodecBytes;

    uint8_t buf[kMaxCodecBytes];
    size_t n = 0;
    if (!SafeCopy(p, maxBytes, buf, &n)) return false;

    pages.emplace_back();
    std::wstring* cur = &pages.back();
    cur->reserve(n);

    size_t i = 0;
    while (i < n) {
        const uint8_t c = buf[i];

        if (c == 0x0F) {                                  // inline format escape
            if (i + 1 >= n) { g_bail = { i, c, "truncated escape" }; break; }
            const int params = EscapeParamCount(buf, n, i + 1);
            if (params < 0) {                             // unknown selector: stop, never guess
                g_bail = { i + 1, buf[i + 1], "unknown escape selector" };
                break;
            }
            // Escapes that YIELD TEXT emit here; the advance below is unchanged for all of them.
            if (buf[i + 1] == kSpriteSel && i + 3 < n && buf[i + 2] == kSpriteP1 &&
                buf[i + 3] >= kSpriteLo && buf[i + 3] <= kSpriteHi) {
                cur->push_back(static_cast<wchar_t>(kSpriteMark + (buf[i + 3] - kSpriteLo)));
            }
            // 0x2E IS THE MACRO PRINTER -- the number the game substitutes into its own dialogue,
            // as in "<n> Bhujerbans heed your words". Confirmed by a Ghidra xref pass: the escape
            // dispatcher FUN_002AC5F0's case 0x2E reads the macro table out of its render context
            // at `param_1[0xE]`, and FUN_002E16B0 (the message-show path) is what puts it there.
            //
            // This decoder sees codec BYTES and has no render context, so it cannot follow that
            // pointer. `MessageMacro` hooks the WRITER instead and keeps the last value; the script
            // sets a macro immediately before showing the line that uses it, so the most recent
            // write is the one this line wants. See message_macro.h.
            //
            // PURELY ADDITIVE: this selector already consumed its two parameters and emitted
            // nothing, so every line WITHOUT a 0x2E decodes byte for byte as before. Only the lines
            // that were silently losing a number change, which is the entire point.
            else if (buf[i + 1] == kMacroSel) {
                int32_t v = 0;
                if (MessageMacro::Latest(&v) && v >= 0 && v <= kMacroMax) {
                    const std::wstring digits = std::to_wstring(v);
                    cur->append(digits);
                }
                // Out of range means the cached value is not this line's -- say nothing rather than
                // put a wrong number in the player's ear. The gap reads as it did before.
            }
            i += 2 + static_cast<size_t>(params);
            continue;
        }

        if (c < 0x10) {                                   // control byte
            if (IsSpaceControl(c)) cur->push_back(L' ');
            else if (c == 0x02)    cur->push_back(L'\n');
            else if (c == 0x03) {                         // PAGE BREAK -- start a new page
                pages.emplace_back();
                cur = &pages.back();                      // re-seat: emplace_back may reallocate
            }
            const int len = ControlLength(c);
            if (len < 0) { g_bail = { i, c, "unknown control length" }; break; }
            if (len == 0) break;                          // genuine terminator, not a failure
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

        // ONE TABLE, generated from the game's own glyph table -- see game_glyphs.h. This
        // replaced range arithmetic for A-Z / a-z / 0-9 plus a hand-written switch of the
        // seventeen punctuation marks that had been won empirically, one session at a time. The
        // generator reproduces all eighty of those mappings before it will emit, so nothing that
        // was established by reading shipped text has been given up; what changed is that the
        // bytes NOBODY had got to yet now decode instead of vanishing.
        //
        // What was being dropped: the whole accented Latin block 0x54-0x84
        // (A-grave through eszett -- 0x72 is n-tilde), and a surprising amount of plain ASCII
        // (@ # $ ^ * _ ; \ " [ ] > { } |). "Senor" was not becoming "Senor"; it was becoming
        // "Seor", exactly the way "Cuchulainn" read as "Cchulainn" before 0x81 was added by hand.
        //
        // A ZERO ENTRY STILL MEANS DROP. The table is the atlas, so a slot the atlas does not
        // fill has no character to speak -- silence beats a guess, unchanged.
        if (!g_glyphReady.load(std::memory_order_acquire)) BuildGlyphTable(g_variant.load());
        wchar_t glyph = g_glyph[c];

        // The two places the mod deliberately speaks something other than what the atlas draws.
        // Both predate this table, both were chosen for the screen reader rather than for
        // fidelity, and both are play-confirmed -- so they are applied here rather than baked
        // into the generated file, where they would look like parser bugs.
        if (c == 0x8F) glyph = L'-';          // atlas draws U+2014 em-dash
        else if (c == 0xC4) glyph = L'\x2265';  // atlas draws U+2267; U+2265 is the one readers voice

        if (glyph) cur->push_back(glyph);
        ++i;
    }
    // One place, so Decode and DecodePages cannot diverge. Cheap: returns immediately unless the
    // page actually carried a sprite marker.
    for (auto& pg : pages) ResolveSprites(pg);
    return true;
}


// Trim leading/trailing whitespace. A page routinely starts with the newline that followed the
// previous page's break, and a lone blank line read aloud is just a stumble.
std::wstring Trimmed(const std::wstring& t) {
    static const wchar_t kWs[] = { L' ', L'\t', L'\r', L'\n', L'\0' };
    const wchar_t* ws = kWs;
    const size_t b = t.find_first_not_of(ws);
    if (b == std::wstring::npos) return std::wstring();
    const size_t e = t.find_last_not_of(ws);
    return t.substr(b, e - b + 1);
}

} // namespace

std::wstring Decode(const uint8_t* p, size_t maxBytes) {
    std::vector<std::wstring> pages;
    if (!DecodeToPages(p, maxBytes, pages)) return std::wstring();
    // Concatenate: 0x03 previously fell through the `default: return 1` arm and emitted nothing, so
    // every existing caller sees byte-identical output to before.
    std::wstring out;
    for (const auto& pg : pages) out += pg;
    return out;
}

void DecodePages(const uint8_t* p, size_t maxBytes, std::vector<std::wstring>& out) {
    out.clear();
    std::vector<std::wstring> pages;
    if (!DecodeToPages(p, maxBytes, pages)) return;
    for (auto& pg : pages) {
        std::wstring t = Trimmed(pg);
        if (!t.empty()) out.push_back(std::move(t));
    }
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

void SetElementSpriteResolver(SpriteNameFn fn) {
    g_spriteResolver = fn;
}

// (S130's SetVariant went with the mod-menu row in S147. Nothing outside this file chooses the
// variant any more -- DetectVariantOnce reads it off the loaded atlas. Re-adding a setter would put
// back the way a wrong answer could be persisted and then trusted forever.)

Variant GetVariant() { return g_variant.load(std::memory_order_relaxed); }

DecodeBail LastDecodeBail() { return g_bail; }

bool IsMostlyPrintable(const std::wstring& s) {
    if (s.empty()) return false;
    // WHY THIS COUNTS ACCENTED LETTERS. It used to require >=60% of the string to be ASCII 0x20-0x7E
    // and >=1 character in A-Z/a-z. That held only because the decoder DROPPED every accented byte:
    // whatever survived was ASCII by construction, so the test could not fail on real text.
    //
    // The generated glyph table (game_glyphs.h) ended that. A French item name or a Polish line now
    // decodes with its accents intact -- and under the old test a short, heavily-accented string
    // could fall under 60% ASCII, or contain no A-Z at all, and be judged garbage. Roughly thirty
    // call sites gate on this function, so that is not a wrong reading, it is SILENCE: a fix that
    // turned partial speech into none.
    //
    // The job here is still to reject binary rubbish read out of a stale pointer, so the shape is
    // unchanged -- it is the alphabet that widened. Letters are counted with iswalpha (any script),
    // and the printable share now admits the Latin-1 and Latin Extended-A ranges the atlas actually
    // contains, which is what "printable" was always trying to mean.
    size_t printable = 0, alpha = 0;
    for (wchar_t ch : s) {
        const bool asciiPrintable = (ch >= 0x20 && ch < 0x7f);
        const bool latinAccented  = (ch >= 0x00A0 && ch <= 0x024F);  // Latin-1 Supplement + Ext-A/B
        if (asciiPrintable || latinAccented || ch == L'\n') ++printable;
        if (iswalpha(static_cast<wint_t>(ch))) ++alpha;
    }
    return alpha >= 1 && printable >= (s.size() * 3 + 4) / 5;  // >= 60%
}

} // namespace GameText

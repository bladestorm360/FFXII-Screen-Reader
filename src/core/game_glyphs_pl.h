#pragma once

// Polish fan-translation glyph overrides (PL_ff12_v1.3, "spolszczenie-1.3", 2020-04-24).
//
// NOT GENERATED, and it CANNOT be. The patch repaints the font texture and adjusts ten advance
// widths, but **never updates the character metadata in `font00.dat`** — every slot still claims
// the stock letter it used to draw. So the file that is authoritative for a stock install
// (game_glyphs.h) is actively wrong here, and the real mapping had to be recovered from the
// patch's own translated TEXT.
//
// METHOD. Decode the patch's shipped `ps2data` text with the stock table and read the Polish. Where
// a slot has been repurposed the stock letter appears in a position Polish orthography forbids, and
// the correct letter is the one that makes the word. Each entry below carries the witnesses that
// fixed it; they are complete words from the game's own data, not inference from frequency.
//
// ⚠ STRUCK (Session 147): ~~"The patch installs by REPACKING THE VBF IN PLACE, so there are no loose
// files to detect and no marker to stat. Selection is therefore a player setting, not autodetection
// — see ModMenu's Text glyphs row."~~
//
// The repack claim is true and the conclusion did not follow. "No marker on disk" is not "no marker",
// because the thing being detected is not a file — it is WHICH ATLAS THE GAME LOADED, which is in
// memory the moment the font manager exists. And this very file already recorded the marker without
// recognising it: the patch "adjusts ten advance widths". Diffed byte for byte, the two `font00.dat`
// files (both 46,876 bytes) differ in EXACTLY 20 bytes — ten records, each with its duplicated
// advance pair at +0x0C/+0x10 changed, and nothing else. That is a deterministic fingerprint.
//
//   slot  60  61  62   84  85  86   98  117 118  179
//   stock 21  21  21   22  22  22   24  19  36   36
//   PL    20  24  24   17  18  18   20  11  11   11
//
// `GameText::DetectVariantOnce` reads them back through the game's own font manager. There is no
// setting and no ModMenu row any more — the mod knows which table it is holding without being told.
//
// The METHOD below is unaffected and still stands: the character metadata really does still name the
// stock letters, so the MAPPING had to come from the patch's own translated text. Only the "you must
// ask the player which one" conclusion is struck.

#include <cstdint>

namespace GameText {

// Codec byte -> character, for the slots the Polish patch repurposed. Anything not listed here
// keeps its stock meaning from kGlyph.
struct GlyphOverride { uint8_t byte; wchar_t ch; };

constexpr GlyphOverride kGlyphPolish[] = {
    // ---- lowercase: every one of these is confirmed by a complete, readable Polish word --------
    { 0x6C, L'\x0105' },  // stock a-grave  -> a-ogonek   "dosięgnąć celu", "Ciążenie"
    { 0x71, L'\x0107' },  // stock c-cedilla-> c-acute    "odzyskuje przytomność", "Żółć"
    { 0x72, L'\x0119' },  // stock e-grave  -> e-ogonek   "zamienia się w kamień", "częściowo"
    { 0x73, L'\x015B' },  // stock e-acute  -> s-acute    "przypadłości", "Odświeżenie"
    { 0x74, L'\x017C' },  // stock e-circ   -> z-dot      "nie może", "Kradzież", "Nieużywany"
    { 0x75, L'\x017A' },  // stock e-uml    -> z-acute    "Wsiądź", "Zejdź", "Niedźwiedzicy"
    { 0x7A, L'\x0144' },  // stock n-tilde  -> n-acute    "zamienia się w kamień"
    { 0x94, L'\x0142' },  // stock inv-?    -> l-stroke   "minęły różne przypadłości", "Osłona"

    // ---- uppercase: five confirmed by complete words -------------------------------------------
    { 0x5A, L'\x0118' },  // stock E-grave  -> E-ogonek   "BROŃ JEDNORĘCZNA", "BROŃ OBURĘCZNA"
    { 0x5B, L'\x015A' },  // stock E-acute  -> S-acute    "Pani Życia i Śmierci", "Śnieżyca"
    { 0x5C, L'\x017B' },  // stock E-circ   -> Z-dot      "Piekielny Żar", "PŻ" (Punkty Życia)
    { 0x5D, L'\x0179' },  // stock E-uml    -> Z-acute    "Bezimienne Źródło"
    { 0x62, L'\x0143' },  // stock N-tilde  -> N-acute    "BROŃ", "BROŃ DALEKOSIĘŻNA"

    // L-STROKE IS NOT WHERE THE PATTERN SAYS IT SHOULD BE. Lowercase l-stroke sits at 0x94, a
    // PUNCTUATION slot, so the -0x18 block rule below cannot place its capital; the obvious guess
    // (0x93, the neighbouring inverted-exclamation slot) is wrong. It is at 0x81 — the very byte the
    // stock table maps to u-acute, which this mod had hand-derived years ago for "Cuchulainn".
    // 173 word-initial occurrences, and the witnesses are unambiguous.
    { 0x81, L'\x0141' },  // stock u-acute  -> L-stroke   "Łatwo", "Łowca", "Łupieżca"; and 42 item
                          //   and enemy names: "Arkadyjski Łucznik", "Cesarska Łuska", "Deszcz Łez"

    // ---- uppercase A-ogonek and C-acute: DERIVED, not witnessed ---------------------------------
    // Every confirmed pair whose members BOTH sit inside the accented-letter block is separated by
    // exactly 0x18, with no counterexample:
    //     e-ogonek 0x72 / E-ogonek 0x5A     s-acute 0x73 / S-acute 0x5B
    //     z-dot    0x74 / Z-dot    0x5C     z-acute 0x75 / Z-acute 0x5D
    //     n-acute  0x7A / N-acute  0x62
    // a-ogonek (0x6C) and c-acute (0x71) are both in-block, so their capitals are 0x54 and 0x59.
    // The two out-of-block letters (l-stroke) are the documented exception, and they are exactly
    // the pair that does NOT follow it — which is why the rule is stated over in-block pairs only.
    //
    // No direct witness exists in the patch's data, and that is expected rather than alarming:
    // capital A-ogonek is essentially unattested in Polish and capital C-acute is word-initial only
    // in rare proper nouns. Recorded as rule-derived so a future session does not mistake it for a
    // measurement. If either is wrong, the cost is one wrong letter in a word almost no player will
    // meet.
    { 0x54, L'\x0104' },  // stock A-grave   -> A-ogonek  (rule-derived)
    { 0x59, L'\x0106' },  // stock C-cedilla -> C-acute   (rule-derived)
};

// Unchanged and worth stating, because it looks like an omission: o-acute needs no entry. The stock
// atlas already carries it at 0x7C / 0x64, and the patch left both alone — "Podróżnik", "Żółć" and
// "Ósma" all decode correctly with the stock table.

} // namespace GameText

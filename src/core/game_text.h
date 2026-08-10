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

// Which font atlas this install is running, because the atlas IS the character map.
//
// `Standard` is the stock `us` atlas, shared by all seven Western locales. `PolishPatch` is the
// PL_ff12_v1.3 fan translation, which repaints ~16 accented slots to Polish letters WITHOUT
// updating the metadata that says what they are — so nothing on disk can be read to detect it and
// nothing in the file can be trusted to describe it. See game_glyphs_pl.h for how the real mapping
// was recovered and what each entry rests on.
//
// ⚠ STRUCK (Session 147): ~~"It is a player setting rather than autodetection because the patch
// repacks the game archive in place: it leaves no loose file, no marker and no version string to
// test."~~ True of the DISK, and irrelevant -- what the setting described was which atlas the GAME
// LOADED, and that is in memory. The patch changes ten advance widths in `font00.dat` and nothing
// else, which is a deterministic fingerprint; `DetectVariantOnce` in the .cpp reads them back
// through the game's own font manager and locates the advance field by matching rather than by
// assuming a struct layout. There is no setting and no mod-menu row any more.
enum class Variant : uint8_t { Standard = 0, PolishPatch = 1 };

// Which atlas the running game is using. DETECTED, never set: nothing outside game_text.cpp chooses
// this. A relaxed atomic, written once on the first decode after the font manager exists and read
// on the game thread inside the decode loop.
Variant GetVariant();

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

// ---- inline SPRITE escapes -> words -------------------------------------------------------------
// Some escapes draw a picture where a word belongs. The eight ELEMENT icons are the ones that
// matter: an accessory's detail panel reads "Half Damage: " and then draws an icon, so without
// this the whole affinity block is inaudible and accessories cannot be told apart.
//
// core/ must not depend on battle/, so the decoder does not resolve the name itself -- it asks a
// resolver the owner registers at startup. Same idiom as TextCapture's painted-row callback.
// `spriteIndex` is the ELEMENT BIT 0..7 (Fire .. Dark). Return an empty string for "unknown", and
// the decoder drops the sprite rather than inventing anything.
//
// With no resolver registered the decoder behaves exactly as it did before this existed.
using SpriteNameFn = std::wstring (*)(int spriteIndex);
void SetElementSpriteResolver(SpriteNameFn fn);

// ---- decode diagnostics -------------------------------------------------------------------------
// The decoder STOPS DEAD on a token whose length it does not know -- it must, because guessing an
// advance desynchronises the rest of the string. The cost is that everything after that byte is
// silently discarded, and in a composed item description the affinity rows (Element / Immune /
// Absorb / Half Damage / Weak / Equip) sit at the END. So one unknown byte reads as "the accessory
// says nothing about its statuses", with no error anywhere.
//
// This records where the last decode on THIS THREAD gave up, so a caller can log it beside the text
// it got. `reason` is null when the string ran to its natural terminator.
struct DecodeBail {
    size_t      offset = 0;      // byte index into the codec source
    uint8_t     value  = 0;      // the byte that stopped it
    const char* reason = nullptr;
};
DecodeBail LastDecodeBail();

} // namespace GameText

#pragma once

#include "core/game_text.h"

#include <cstdint>
#include <string>

// Shared layout of a party-member ABILITY SUMMARY entry — the record used by BOTH summary pages:
// the Magicks page (focus routine FUN_002c3b90) and the Technicks / Quickenings / Remedy Lore /
// Espers page (FUN_002c53b0).
//
// TWO readers consume these records, which is why the layout lives here rather than in either:
//   * ability_summary_reader — the license board's `F` overlay, where the pages HAVE a cursor and
//     only the highlighted entry is spoken;
//   * status_reader — the Status screen, where the very same pages are STATIC displays with no
//     browsable cursor, so every slot is enumerated into a virtual buffer instead.
//
// Entry arrays are INLINE in the page object (not a pointer indirection). All values confirmed live
// (Session 71 probe_status_data) and previously shipped in ability_summary_reader.
namespace AbilityEntry {

constexpr uint32_t STRIDE       = 0x20;
constexpr uint32_t OFF_NAME     = 0x00;    // name codec — the game's own "?" when unlearned
constexpr uint32_t OFF_DESC     = 0x10;    // description codec (null when the name is "?")
constexpr uint32_t OFF_FLAGS    = 0x18;    // bit 0x20000 = learned/bright, clear = greyed
constexpr uint32_t FLAG_LEARNED = 0x20000;

// ---- page object ------------------------------------------------------------------------------
constexpr uint32_t OFF_CHILDREN   = 0x60;  // -> layout-children array
constexpr uint32_t OFF_CHILD_TEXT = 0x18;  // child+0x18 = its text codec

// ---- abilities page (obj[0] == RVA 0x1A4460; parked at menuCtx+0x128) --------------------------
constexpr uint32_t ABIL_ENTRIES = 0x0E0;
constexpr int      ABIL_COUNT   = 54;
constexpr uint32_t ABIL_INDEX   = 0x7A0;   // highlighted entry (u16) — cursor pages only
constexpr uint32_t ABIL_SECTION = 0x7A4;   // current section 0..3 (u16) — cursor pages only
constexpr int      ABIL_SECTIONS = 4;
constexpr uint32_t SECTION_CHILD[ABIL_SECTIONS] = { 0x28, 0x40, 0x58, 0x70 };

// NOTE: there is deliberately no slot->section table here. The game keeps the cursor's current
// section in `obj + ABIL_SECTION`, so readers follow that rather than partitioning the array
// themselves. (A measured partition {0,24,27,41} existed briefly for an enumeration approach that
// was removed — do not reintroduce it; the game's own index is authoritative and survives content
// changes.)

// ---- magicks page (obj[0] == RVA 0x1A3560; parked at menuCtx+0x120) ----------------------------
constexpr uint32_t MAGK_ENTRIES = 0x0C8;
constexpr int      MAGK_COUNT   = 81;
constexpr uint32_t MAGK_INDEX   = 0xAE8;

// What an unlearned slot is spoken as. The game draws "?" there; a literal "?" is commonly dropped
// by screen readers at default punctuation verbosity, which would re-silence the row.
constexpr wchar_t kEmptySlot[] = L"empty";

// Decode a codec string (GameText guards internally). `skip` strips the shared-pool 00 00 prefix;
// harmless when absent, since a valid string never starts with a 0x00 terminator.
inline std::wstring DecodeCodec(const uint8_t* codec, bool skip) {
    if (!codec) return std::wstring();
    const uint8_t* p = skip ? GameText::SkipVariantPrefix(codec) : codec;
    std::wstring s = GameText::Decode(p, 320);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// Decode an entry's NAME. The game writes its own "?" placeholder (FUN_002f9860(0x4C7)) into an
// unlearned slot, and that string contains NO letters — IsMostlyPrintable requires at least one, so
// the ordinary gate rejects it and the row goes silent. Before any magick is learned that is the
// whole Magicks page. Detect the placeholder explicitly and report the slot as empty; keep the gate
// for everything else so a stale pointer still yields nothing.
inline std::wstring DecodeName(const uint8_t* codec, bool* outEmptySlot) {
    *outEmptySlot = false;
    if (!codec) return std::wstring();
    std::wstring s = GameText::Decode(GameText::SkipVariantPrefix(codec), 320);
    if (s.empty()) return std::wstring();
    if (s.find(L'?') != std::wstring::npos &&
        s.find_first_not_of(L" ?") == std::wstring::npos) {   // only '?' (and spaces)
        *outEmptySlot = true;
        return kEmptySlot;
    }
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

} // namespace AbilityEntry

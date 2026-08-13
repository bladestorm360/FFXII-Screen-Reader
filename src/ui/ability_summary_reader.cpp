#include "ui/ability_summary_reader.h"
#include "ui/ability_entry.h"
#include "ui/gambit_picker_reader.h"
#include "ui/text_capture.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "speech/speech.h"
#include "speech/phrasebook.h"
#include "core/logger.h"

#include <Windows.h>
#include <cstdint>
#include <string>

namespace {

using MemRead::PtrAt;
using MemRead::SafeReadU16;
using MemRead::SafeReadU32;

// ---- the two page controllers (abs = RVA + 0x120000) --------------------------------------------
// Each is that page's focus/refresh choke point, taking the controller as its only argument.
constexpr uint32_t RVA_ABIL_FOCUS = 0x1A53B0;   // FUN_002c53b0 — Technicks/Quickenings/Remedy/Espers
constexpr uint32_t RVA_MAGK_FOCUS = 0x1A3B90;   // FUN_002c3b90 — Magicks

// The record layout, the page offsets and the decode rules are SHARED with status_reader (the Status
// screen renders these same two pages as static, cursorless displays) — see ability_entry.h.
constexpr uint32_t OFF_ABIL_ENTRIES = AbilityEntry::ABIL_ENTRIES;
constexpr uint32_t OFF_ABIL_INDEX   = AbilityEntry::ABIL_INDEX;
constexpr uint32_t OFF_ABIL_SECTION = AbilityEntry::ABIL_SECTION;
constexpr uint32_t OFF_MAGK_ENTRIES = AbilityEntry::MAGK_ENTRIES;
constexpr uint32_t OFF_MAGK_INDEX   = AbilityEntry::MAGK_INDEX;

constexpr uint32_t OFF_SUM_CHILDREN = AbilityEntry::OFF_CHILDREN;
constexpr uint32_t OFF_CHILD_TEXT   = AbilityEntry::OFF_CHILD_TEXT;

constexpr uint32_t ENTRY_STRIDE     = AbilityEntry::STRIDE;
constexpr uint32_t OFF_ENT_NAME     = AbilityEntry::OFF_NAME;
constexpr uint32_t OFF_ENT_DESC     = AbilityEntry::OFF_DESC;
constexpr uint32_t OFF_ENT_FLAGS    = AbilityEntry::OFF_FLAGS;
constexpr uint32_t ENT_FLAG_LEARNED = AbilityEntry::FLAG_LEARNED;

constexpr int      SUM_SECTIONS = AbilityEntry::ABIL_SECTIONS;   // Technicks/Quickenings/Remedy/Espers

typedef uint64_t (*Pfn_SumFocus)(void*);
Pfn_SumFocus s_origAbilFocus = nullptr;
Pfn_SumFocus s_origMagkFocus = nullptr;

// Section-change detector. The heading is announced whenever the player enters a different SECTION —
// either by switching pages (each page is a distinct controller object, so `F` is an owner change) or
// by moving the cursor across a boundary into Technicks / Quickenings / Remedy Lore / Espers.
//
// ~~"re-announcing the heading on every crossing was noise, so announce it on page change only"~~ is
// STRUCK (Session 71, tester): without it, crossing from Technicks into Remedy Lore just said "Blind"
// and read as the mod reporting the wrong thing entirely. A sighted player sees the heading above the
// column; the section is the only thing that makes a bare status name make sense. It fires once per
// crossing, not per row, and DETECTS a transition rather than suppressing a repeat — not speech dedup.
// Game thread.
void*    g_sumOwner   = nullptr;
uint16_t g_sumSection = 0xFFFF;

using AbilityEntry::DecodeCodec;
using AbilityEntry::DecodeName;

// Speak the highlighted entry of a summary page. Both pages share the entry format; only the
// entry-array and index offsets differ. Game thread, from the page's own focus routine.
void AnnounceEntry(void* obj, uint32_t entriesOff, uint32_t indexOff, bool hasSections) {
    if (!obj) return;

    // STAND DOWN FOR THE GAMBIT PICKER -- arbitration, not a speech filter.
    //
    // These page controllers do not belong exclusively to the license board's `F` overlay: the
    // gambit action picker drives them too, and it drives them TWICE for one event. A live log
    // caught "Cure, unavailable" spoken twice in the same millisecond from one owner, in the middle
    // of a gambit edit. The picker has its own reader now, reading the picker's own row array, so
    // the surface has one speaker again -- and the ability page keeps every case that is really
    // its own. Deleting one of two paths that cover different cases is the mistake this project has
    // already paid for; standing one down while the other drives is the sanctioned shape.
    if (GambitPickerReader::IsLive()) return;
    uint16_t idx = 0;
    if (!SafeReadU16(obj, indexOff, &idx)) return;
    void* entry = reinterpret_cast<char*>(obj) + entriesOff + static_cast<size_t>(idx) * ENTRY_STRIDE;

    // An unlearned slot comes back as "empty"; a null name is a hidden row.
    bool emptySlot = false;
    std::wstring name = DecodeName(
        reinterpret_cast<const uint8_t*>(PtrAt(entry, OFF_ENT_NAME)), &emptySlot);
    if (name.empty()) return;

    // Heading whenever the SECTION changes — on a page switch, or on crossing a boundary within the
    // page. The game keeps the current section in obj+0x7A4, so this follows the game's own notion of
    // which column the cursor is in rather than any assumption about slot ranges.
    std::wstring line;
    const bool pageChanged = (obj != g_sumOwner);
    g_sumOwner = obj;
    if (hasSections) {
        uint16_t sec = 0;
        if (SafeReadU16(obj, OFF_ABIL_SECTION, &sec) && sec < SUM_SECTIONS) {
            if (pageChanged || sec != g_sumSection) {
                g_sumSection = sec;
                void* children = PtrAt(obj, OFF_SUM_CHILDREN);
                void* child = children ? PtrAt(children, AbilityEntry::SECTION_CHILD[sec]) : nullptr;
                std::wstring head = DecodeCodec(
                    reinterpret_cast<const uint8_t*>(PtrAt(child, OFF_CHILD_TEXT)), /*skip=*/true);
                if (!head.empty()) line = head + L", ";
            }
        }
    } else if (pageChanged) {
        g_sumSection = 0xFFFF;   // Magicks has no sections; re-arm so returning to the other page speaks
    }
    line += name;

    // Greyed rows carry real information — Remedy Lore lists real status names greyed until they
    // become curable — so mark them. An empty slot is already fully described by "empty".
    uint32_t flags = 0;
    SafeReadU32(entry, OFF_ENT_FLAGS, &flags);
    if (!(flags & ENT_FLAG_LEARNED) && !emptySlot) line += std::wstring(L", ") + Phrase::Get(Phrase::Id::Unavailable);

    // Publish even when empty: nothing else bumps the help generation on this screen, so leaving a
    // previous entry's description in place would make `o` read a stale one.
    TextCapture::ProvideHelpText(
        DecodeCodec(reinterpret_cast<const uint8_t*>(PtrAt(entry, OFF_ENT_DESC)), /*skip=*/true));

    Log::WriteW("LICENSE", "summary:", obj, line);
    Speech::Output(line, /*interrupt=*/true);
}

// Run the original first so the page has settled its index and selection, then read.
uint64_t HookedAbilFocus(void* obj) {
    const uint64_t ret = s_origAbilFocus ? s_origAbilFocus(obj) : 0;
    AnnounceEntry(obj, OFF_ABIL_ENTRIES, OFF_ABIL_INDEX, /*hasSections=*/true);
    return ret;
}

uint64_t HookedMagkFocus(void* obj) {
    const uint64_t ret = s_origMagkFocus ? s_origMagkFocus(obj) : 0;
    AnnounceEntry(obj, OFF_MAGK_ENTRIES, OFF_MAGK_INDEX, /*hasSections=*/false);
    return ret;
}

} // namespace

namespace AbilitySummaryReader {

bool Init() {
    bool ok = Hooks::InstallTyped(RVA_ABIL_FOCUS, &HookedAbilFocus, &s_origAbilFocus);
    ok     &= Hooks::InstallTyped(RVA_MAGK_FOCUS, &HookedMagkFocus, &s_origMagkFocus);
    Log::Write("LICENSE", ok
        ? "AbilitySummaryReader: `F` summary pages (abilities + magicks) hooked"
        : "AbilitySummaryReader: a focus hook FAILED to install — see Hooks log.");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_MAGK_FOCUS);
    Hooks::Uninstall(RVA_ABIL_FOCUS);
    g_sumOwner = nullptr;
    g_sumSection = 0xFFFF;
}

} // namespace AbilitySummaryReader

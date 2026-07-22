#include "ui/ability_summary_reader.h"
#include "ui/text_capture.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "speech/speech.h"
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
constexpr uint32_t RVA_ABIL_FOCUS = 0x1A53B0;   // FUN_002c53b0 — Technicks / Mist / Remedy / Espers
constexpr uint32_t RVA_MAGK_FOCUS = 0x1A3B90;   // FUN_002c3b90 — Magicks

// Entry arrays are INLINE in the controller (not a pointer indirection).
constexpr uint32_t OFF_ABIL_ENTRIES = 0x0E0;    // abilities page: 54 entries
constexpr uint32_t OFF_ABIL_INDEX   = 0x7A0;    // ...highlighted entry index (u16)
constexpr uint32_t OFF_ABIL_SECTION = 0x7A4;    // ...current section 0..3 (u16)
constexpr uint32_t OFF_MAGK_ENTRIES = 0x0C8;    // magicks page: 81 entries
constexpr uint32_t OFF_MAGK_INDEX   = 0xAE8;    // ...highlighted entry index (u16)

constexpr uint32_t OFF_SUM_CHILDREN = 0x60;     // obj+0x60 -> layout-children array
constexpr uint32_t OFF_CHILD_TEXT   = 0x18;     // child+0x18 = its text codec

constexpr uint32_t ENTRY_STRIDE     = 0x20;
constexpr uint32_t OFF_ENT_NAME     = 0x00;     // name codec — the game's own "?" when unlearned
constexpr uint32_t OFF_ENT_DESC     = 0x10;     // description codec (null when the name is "?")
constexpr uint32_t OFF_ENT_FLAGS    = 0x18;     // bit 0x20000 = learned/bright, clear = greyed
constexpr uint32_t ENT_FLAG_LEARNED = 0x20000;

constexpr int      SUM_SECTIONS = 4;            // Technicks / Mist / Remedy Lore / Espers
constexpr uint32_t SECTION_CHILD_OFF[SUM_SECTIONS] = { 0x28, 0x40, 0x58, 0x70 };

typedef uint64_t (*Pfn_SumFocus)(void*);
Pfn_SumFocus s_origAbilFocus = nullptr;
Pfn_SumFocus s_origMagkFocus = nullptr;

// Page-change detector: the heading is announced when the player switches PAGES (each page is a
// distinct controller object, so `F` is exactly an owner change), not when the cursor crosses a
// section boundary — re-announcing "Technicks" / "Remedy Lore" on every crossing was noise. This
// DETECTS a transition rather than suppressing a repeat, so it is not speech dedup. Game thread.
void* g_sumOwner = nullptr;

// What an unlearned slot is spoken as. The game draws "?" there; a literal "?" is commonly
// dropped by screen readers at default punctuation verbosity, which would re-silence the row.
constexpr wchar_t kEmptySlot[] = L"empty";

// Decode a codec string (GameText guards internally). `skip` strips the shared-pool 00 00 prefix;
// harmless when absent, since a valid string never starts with a 0x00 terminator.
std::wstring DecodeCodec(const uint8_t* codec, bool skip) {
    if (!codec) return std::wstring();
    const uint8_t* p = skip ? GameText::SkipVariantPrefix(codec) : codec;
    std::wstring s = GameText::Decode(p, 320);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// Decode an entry's NAME. The game writes its own "?" placeholder (FUN_002f9860(0x4C7)) into an
// unlearned slot, and that string contains NO letters — IsMostlyPrintable requires at least one,
// so the ordinary gate rejects it and the row goes silent. Before any magick is learned that is
// the whole Magicks page. Detect the placeholder explicitly and report the slot as empty; keep the
// gate for everything else so a stale pointer still yields nothing.
std::wstring DecodeEntryName(const uint8_t* codec, bool* outEmptySlot) {
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

// Speak the highlighted entry of a summary page. Both pages share the entry format; only the
// entry-array and index offsets differ. Game thread, from the page's own focus routine.
void AnnounceEntry(void* obj, uint32_t entriesOff, uint32_t indexOff, bool hasSections) {
    if (!obj) return;
    uint16_t idx = 0;
    if (!SafeReadU16(obj, indexOff, &idx)) return;
    void* entry = reinterpret_cast<char*>(obj) + entriesOff + static_cast<size_t>(idx) * ENTRY_STRIDE;

    // An unlearned slot comes back as "empty"; a null name is a hidden row.
    bool emptySlot = false;
    std::wstring name = DecodeEntryName(
        reinterpret_cast<const uint8_t*>(PtrAt(entry, OFF_ENT_NAME)), &emptySlot);
    if (name.empty()) return;

    // Heading only when the player switched PAGES — the section index still picks WHICH heading.
    std::wstring line;
    const bool pageChanged = (obj != g_sumOwner);
    g_sumOwner = obj;
    if (hasSections && pageChanged) {
        uint16_t sec = 0;
        if (SafeReadU16(obj, OFF_ABIL_SECTION, &sec) && sec < SUM_SECTIONS) {
            void* children = PtrAt(obj, OFF_SUM_CHILDREN);
            void* child = children ? PtrAt(children, SECTION_CHILD_OFF[sec]) : nullptr;
            std::wstring head = DecodeCodec(
                reinterpret_cast<const uint8_t*>(PtrAt(child, OFF_CHILD_TEXT)), /*skip=*/true);
            if (!head.empty()) line = head + L", ";
        }
    }
    line += name;

    // Greyed rows carry real information — Remedy Lore lists real status names greyed until they
    // become curable — so mark them. An empty slot is already fully described by "empty".
    uint32_t flags = 0;
    SafeReadU32(entry, OFF_ENT_FLAGS, &flags);
    if (!(flags & ENT_FLAG_LEARNED) && !emptySlot) line += L", unavailable";

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
}

} // namespace AbilitySummaryReader

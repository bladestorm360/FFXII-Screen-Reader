#include "ui/gambit_picker_reader.h"

#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "speech/speech.h"
#include "speech/phrasebook.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace GambitPickerReader {
namespace {

using MemRead::Obj0;
using MemRead::PtrAt;
using MemRead::SafeReadU8;
using MemRead::SafeReadU16;

// ---- picker layout (abs = RVA + 0x120000) ------------------------------------------------------
constexpr uint32_t RVA_PICKER  = 0x44B4D0;  // FUN_0056b4d0 -- the condition/action chooser
constexpr uint32_t P_ROWS      = 0x0E0;     // picker+0x0E0 + i*0x20 = row i
constexpr uint32_t ROW_STRIDE  = 0x20;
constexpr int      ROW_MAX     = 17;        // the array is memset 0x220 = 17 * 0x20
constexpr uint32_t R_NAME      = 0x00;      // codec*
constexpr uint32_t R_ID        = 0x08;      // u16, 0xFFFF = slot unused
constexpr uint32_t R_COST      = 0x0C;      // u16 MP cost / owned count -- LOGGED, not spoken
constexpr uint32_t R_AVAIL     = 0x10;      // u8  0 = selectable; non-zero = not acquired. The
                                            // decompile suggested 0x0F for the not-acquired class;
                                            // the live log measured **0x10** ("Cure, unavailable" on
                                            // a character without the licence). Tested != 0, so both
                                            // readings behave the same -- but the MEASURED value is
                                            // 0x10 and that is what any future gate uses.
constexpr uint32_t P_MODE      = 0x588;     // u8  1 = condition list, 2 = action list
constexpr uint32_t P_CAT       = 0x595;     // s8  current category (the tab left/right moves)
constexpr uint32_t P_CATCNT    = 0x596;     // u8  category count
constexpr uint32_t P_ROWCUR    = 0x599;     // s8  the picker's own copy of the row cursor
constexpr uint16_t ID_UNSET    = 0xFFFF;

// Where the player is. `g_live` gates ability_summary_reader out (see the header); `g_onScreen` is
// only for the diagnostic below, which must not fire for every list in the game.
bool  g_live     = false;
bool  g_onScreen = false;

// Names on this surface come from the same master-data pool the inventory rows do, so use the same
// two-step: decode straight, then retry past the shared-pool 00 00 variant prefix. Empty on a stale
// or placeholder pointer, which is what makes the caller decline rather than speak nonsense.
std::wstring RowName(void* row) {
    const uint8_t* codec = static_cast<const uint8_t*>(PtrAt(row, R_NAME));
    if (!codec) return std::wstring();
    std::wstring s = GameText::Decode(codec, 128);
    if (GameText::IsMostlyPrintable(s)) return s;
    s = GameText::Decode(GameText::SkipVariantPrefix(codec), 128);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// ONE emit function for this surface: wording, logging and interrupt policy live here and nowhere
// else. `cat` and `cursor` ride along because they are what a category switch has to be diagnosed
// from -- `cursor` is the picker's own copy of the row index, so a disagreement with the dispatch's
// `row` prints itself instead of becoming a session of guesswork.
void Speak(void* owner, const std::wstring& line, int row, int cursor, int cat, int catCount,
           int mode, uint16_t id, uint8_t avail, uint16_t cost) {
    char tag[160];
    snprintf(tag, sizeof(tag),
             "row=%d cursor=%d cat=%d/%d mode=%d id=0x%04X avail=0x%02X cost=%u:",
             row, cursor, cat, catCount, mode, id, avail, cost);
    Log::WriteW("PICKER", tag, owner, line);
    Speech::Output(line, /*interrupt=*/true);
}

// THE CATEGORY HAS NO NAME, AND THE MOD DELIBERATELY SAYS NOTHING FOR IT. **ANSWERED 2026-08-13 —
// DO NOT RE-DERIVE THIS.** A diagnostic shipped in the first build printed every candidate route
// against a live category walk, and the log refuted all of them:
//
//   * `DefName(0x15, family)` resolves only the four BATTLE COMMANDS — Attack / Magicks / Technicks
//     / Items. All five magick tabs share family 1, so it cannot tell them apart.
//   * `DefName(0x15, categoryIndex)` is nonsense on this surface: it is the battle-command table
//     indexed by a tab number ("NOT USED concentration", "Summon", "Foecraft").
//   * `DefName(0x18, family)` is the MAGICK-SCHOOL table (0-3 = White / Black / Time / Green
//     Magicks) but the family byte is not a school id, so it names the wrong school every time.
//     NOTE this corrects `ingame_menu_reader.cpp`'s CAT_CHOOSER_* comment, which has 0x15 and 0x18
//     the other way round.
//   * `picker+0x580`, which the category-step functions zero, is the 180-frame "you cannot pick
//     this" ERROR banner (`FUN_0056ac50`, ids 0xC75-0xC7A / 0xCF0), not a label.
//
// The 11 action tabs are a GAMBIT-SPECIFIC grouping (1 Attack, 5 Magicks, 3 Items, 2 Technicks) with
// no string table behind them; the strip draws icons. Anything spoken here would be a fabricated
// label. **Tester's call, asked and answered: announce nothing.** The corrected first row of the new
// category already distinguishes every tab, which is what the switch says now.
//
// The remaining state is on the row line below (`cat=%d/%d mode=%d`), which costs nothing — the
// diagnostic's four `DefName` game calls per switch are gone with the question they answered.

// If the class constant above is wrong, this is the line that says so -- and it says it only while
// the gambit screen is up, so it cannot print for every list in the game. Capped: it exists to
// name ONE constant, not to trace a surface.
void NoteForeignOwner(void* owner) {
    if (!g_onScreen || !owner) return;
    static void* s_last  = nullptr;
    static int   s_lines = 0;
    if (owner == s_last || s_lines >= 4) return;
    s_last = owner;
    ++s_lines;
    const uintptr_t base = reinterpret_cast<uintptr_t>(Hooks::ResolveRva(0));
    const uintptr_t cls  = reinterpret_cast<uintptr_t>(Obj0(owner));
    char m[192];
    snprintf(m, sizeof(m),
             "focus on owner=%p class RVA=0x%llX while the gambit screen is up -- NOT the picker "
             "class (expected 0x%X); the generic path still covers it",
             owner, static_cast<unsigned long long>(cls >= base ? cls - base : cls), RVA_PICKER);
    Log::Write("PICKER", m);
}

} // namespace

bool IsPicker(void* owner) {
    return owner && Obj0(owner) == Hooks::ResolveRva(RVA_PICKER);
}

bool IsLive() { return g_live; }

void NotePanelFocus() {
    g_onScreen = true;
    g_live     = false;      // the panel has the cursor again, so the picker is closed
}

bool OnFocus(void* owner, int index) {
    if (!IsPicker(owner)) { NoteForeignOwner(owner); return false; }
    STALL_SCOPE("GambitPickerReader::OnFocus");

    // DECLINE, NEVER GUESS. Every early exit below leaves the focus unclaimed, so the generic
    // painted-row path speaks exactly as it does today -- a shape we do not recognise costs the
    // player nothing.
    if (index < 0 || index >= ROW_MAX) {
        char m[96];
        snprintf(m, sizeof(m), "row %d outside 0..%d -- DECLINED", index, ROW_MAX - 1);
        Log::Write("PICKER", m);
        return false;
    }
    void* row = static_cast<char*>(owner) + P_ROWS + static_cast<uint32_t>(index) * ROW_STRIDE;

    uint16_t id = ID_UNSET;
    if (!SafeReadU16(row, R_ID, &id) || id == ID_UNSET) return false;   // unused slot

    std::wstring line = RowName(row);
    if (line.empty()) {
        char m[96];
        snprintf(m, sizeof(m), "row %d id=0x%04X name did not decode -- DECLINED", index, id);
        Log::Write("PICKER", m);
        return false;
    }

    uint8_t avail = 0, catRaw = 0, catCount = 0, mode = 0, cursor = 0;
    SafeReadU8(row,   R_AVAIL,  &avail);
    SafeReadU8(owner, P_CAT,    &catRaw);
    SafeReadU8(owner, P_CATCNT, &catCount);
    SafeReadU8(owner, P_MODE,   &mode);
    SafeReadU8(owner, P_ROWCUR, &cursor);
    uint16_t cost = 0;
    SafeReadU16(row, R_COST, &cost);
    const int cat = static_cast<int8_t>(catRaw);

    // The game substitutes its own "???" into the name of an entry the character has not acquired,
    // so without this the row reads as a bare "???" and says nothing about why. `Unavailable` is
    // already what the ability page says on this very surface -- this keeps that, it does not invent
    // a word for it.
    if (avail) line += std::wstring(L", ") + Phrase::Get(Phrase::Id::Unavailable);

    g_live = true;
    Speak(owner, line, index, static_cast<int8_t>(cursor), cat, catCount, mode, id, avail, cost);
    return true;
}

} // namespace GambitPickerReader

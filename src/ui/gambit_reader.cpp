#include "ui/gambit_reader.h"

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

namespace GambitReader {
namespace {

using MemRead::Obj0;
using MemRead::PtrAt;
using MemRead::SafeReadU8;
using MemRead::SafeReadU16;

// ---- panel layout (abs = RVA + 0x120000) -------------------------------------------------------
// All of these are probe-confirmed; see gambit_reader.h for the pass criteria.
constexpr uint32_t RVA_PANEL   = 0x4491E0;  // FUN_005691e0 -- the gambit screen, pause cmd 0x4B9
constexpr uint32_t P_RECS      = 0x160;     // panel+0x160 + i*0x20 = display record i
constexpr uint32_t REC_STRIDE  = 0x20;
constexpr int      REC_MAX     = 13;        // the array is memset 0x1A0 = 13 * 0x20
constexpr uint32_t P_MASK      = 0x124;     // u16 per-row enable mask, bit i-1 for display row i
constexpr uint32_t P_COUNT     = 0x126;     // u8  row count (max 12)
constexpr uint32_t P_COL       = 0x33E;     // u8  column cursor: 0 whole row, 1 condition, 2 action
constexpr uint32_t R_COND_NAME = 0x00;      // codec* -- rec 0 holds the CHARACTER name here
constexpr uint32_t R_ACT_NAME  = 0x08;      // codec* -- null on rec 0 (the header has no action)
constexpr uint32_t R_ON        = 0x14;      // u8  enabled; on rec 0 this is the MASTER toggle
constexpr uint32_t R_CLASS     = 0x15;      // u8  2 = empty row

constexpr uint8_t  CLASS_EMPTY = 2;
constexpr int      COL_ROW = 0, COL_COND = 1, COL_ACT = 2;

// Decode one of the record's two name pointers.
//
// NO SkipVariantPrefix: the panel stores these already variant-selected, exactly as the actor binder
// does for NameForActor. The probe's raw hex confirms it -- rec[1] reads "Ally: status = KO" straight
// from the first byte with no 00 00 lead.
std::wstring RecName(void* panel, int rec, uint32_t field) {
    const uint8_t* codec = static_cast<const uint8_t*>(
        PtrAt(panel, P_RECS + static_cast<uint32_t>(rec) * REC_STRIDE + field));
    if (!codec) return std::wstring();
    std::wstring s = GameText::Decode(codec, 128);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

bool RecByte(void* panel, int rec, uint32_t field, uint8_t* out) {
    return SafeReadU8(panel, P_RECS + static_cast<uint32_t>(rec) * REC_STRIDE + field, out);
}

std::wstring OnOffWord(bool on) {
    return Phrase::Get(on ? Phrase::Id::On : Phrase::Id::Off);
}

// ONE emit function for this surface, per the one-choke-point rule: the wording, the logging and the
// interrupt policy exist here and nowhere else.
void Speak(void* owner, const std::wstring& line, int rec, int col) {
    if (line.empty()) return;
    char tag[64];
    snprintf(tag, sizeof(tag), "gambit rec=%d col=%d:", rec, col);
    Log::WriteW("INGAME", tag, owner, line);
    Speech::Output(line, /*interrupt=*/true);
}

// STATE FILTER, and it is the per-frame exception rather than a speech dedup.
//
// The panel RESENDS 0x8000 for an unchanged state: the probe caught SEVEN identical
// `msg=0x8000 val=1 col=1` messages on menu entry, because the per-frame cat-0xA handler reconciles
// panel+0x33E against its saved copy at panel+0x33F and re-sends through FUN_002d1ac0. Speaking every
// one of those would stutter the screen unusably.
//
// It also absorbs the measured row-0 hazard: crossing onto the header row sends TWO 0x8000 for one
// keypress and the FIRST carries the STALE column (probe #31 val=0 col=1, then #33 val=0 col=0). The
// forced column below makes both messages resolve to the same (rec, col), so the second collapses
// here instead of announcing the wrong column.
//
// This is a TRANSITION detector keyed on the panel pointer too, so leaving the screen and coming back
// re-announces -- a fresh panel never matches the remembered one.
void* s_lastPanel = nullptr;
int   s_lastRec   = -1;
int   s_lastCol   = -1;

bool StateChanged(void* panel, int rec, int col) {
    if (panel == s_lastPanel && rec == s_lastRec && col == s_lastCol) return false;
    s_lastPanel = panel; s_lastRec = rec; s_lastCol = col;
    return true;
}

} // namespace

bool IsGambitPanel(void* owner) {
    return owner && Obj0(owner) == Hooks::ResolveRva(RVA_PANEL);
}

bool OnFocus(void* panel, int recIndex) {
    if (!IsGambitPanel(panel)) return false;
    STALL_SCOPE("GambitReader::OnFocus");

    uint8_t rowCount = 0, colRaw = 0;
    if (!SafeReadU8(panel, P_COUNT, &rowCount) || !SafeReadU8(panel, P_COL, &colRaw)) {
        Log::Write("INGAME", "gambit: panel row-count/column unreadable -- SILENT");
        return true;                       // ours either way; silence beats wrong speech
    }
    if (recIndex < 0 || recIndex >= REC_MAX || recIndex > static_cast<int>(rowCount)) {
        char m[96];
        snprintf(m, sizeof(m), "gambit: rec %d outside 0..%u -- SILENT", recIndex, rowCount);
        Log::Write("INGAME", m);
        return true;
    }

    // THE HEADER ROW HAS NO COLUMNS, so force column 0 there. rec[0]'s action-name pointer is null
    // (probe: `actHex = null`), i.e. there is no second column to be on -- and this is what makes the
    // stale-column first message of a row-0 crossing collapse into the corrected one.
    const int col = (recIndex == 0) ? COL_ROW : static_cast<int>(colRaw);
    if (!StateChanged(panel, recIndex, col)) return true;

    uint8_t on = 0, cls = 0;
    RecByte(panel, recIndex, R_ON, &on);
    RecByte(panel, recIndex, R_CLASS, &cls);

    // --- the character header + gambit master toggle ---
    if (recIndex == 0) {
        std::wstring line = RecName(panel, 0, R_COND_NAME);
        if (line.empty()) {
            Log::Write("INGAME", "gambit: header name did not decode -- SILENT");
            return true;
        }
        Speak(panel, line + L", " + OnOffWord(on != 0), recIndex, col);
        return true;
    }

    // --- an unset row ---
    // Spoken, not silent, and this is NOT the "never speak filler" case. That rule is about having
    // nothing to report; here the report IS that the slot is empty, and on a 12-row list silence
    // would leave the player unable to tell the cursor moved. `EmptySlot` already exists for exactly
    // this and ability_summary_reader sets the precedent.
    if (cls == CLASS_EMPTY) {
        Speak(panel, Phrase::Get(Phrase::Id::EmptySlot), recIndex, col);
        return true;
    }

    const std::wstring cond = RecName(panel, recIndex, R_COND_NAME);
    const std::wstring act  = RecName(panel, recIndex, R_ACT_NAME);

    // BOTH WITNESSES TO THE ROW'S ON/OFF STATE ON ONE LINE. `rec+0x14` and bit i-1 of panel+0x124 are
    // two recordings of one fact and the probe showed them agreeing (mask 0x13, rows 1/2/5 set), so a
    // divergence reads itself out of the log rather than becoming a session of guesswork about which
    // one the reader should have trusted.
    uint16_t mask = 0;
    if (SafeReadU16(panel, P_MASK, &mask)) {
        const bool maskSaysOn = (mask & (1u << (recIndex - 1))) != 0;
        if (maskSaysOn != (on != 0)) {
            char m[144];
            snprintf(m, sizeof(m),
                     "gambit: WITNESSES DISAGREE on rec %d -- rec+0x14=%u mask=0x%04X says %d",
                     recIndex, on, mask, maskSaysOn ? 1 : 0);
            Log::Write("INGAME", m);
        }
    }

    // Column cursor decides how much of the row to say: on the whole row, everything; inside a
    // column, that column alone, because the player moved there to hear it.
    std::wstring line;
    if (col == COL_COND)      line = cond;
    else if (col == COL_ACT)  line = act;
    else {
        line = cond;
        if (!act.empty())  line += (line.empty() ? L"" : L", ") + act;
        if (!line.empty()) line += L", " + OnOffWord(on != 0);
    }

    if (line.empty()) {
        char m[128];
        snprintf(m, sizeof(m),
                 "gambit: rec %d col %d produced no text (cond=%d act=%d chars) -- SILENT",
                 recIndex, col, static_cast<int>(cond.size()), static_cast<int>(act.size()));
        Log::Write("INGAME", m);
        return true;
    }
    Speak(panel, line, recIndex, col);
    return true;
}

} // namespace GambitReader

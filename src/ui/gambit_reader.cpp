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
// DAT_02ca9700 -- THE VISIBLE panel of the three-set carousel. Three panels exist and ALL THREE take
// the entry 0x8000: the live log caught one keypress producing three identical utterances at the same
// millisecond from owners 2BFD8D80 / 2BFE8A80 / 2BFE98C0. They were inaudible only because each speaks
// with interrupt, so the first two were cut off -- which also meant the voice belonged to whichever
// panel dispatched LAST, not to the set on screen.
constexpr uint32_t RVA_CUR_PANEL = 0x2B89700;
constexpr uint32_t P_RECS      = 0x160;     // panel+0x160 + i*0x20 = display record i
constexpr uint32_t REC_STRIDE  = 0x20;
constexpr int      REC_MAX     = 13;        // the array is memset 0x1A0 = 13 * 0x20
constexpr uint32_t P_MASK      = 0x124;     // u16 per-row enable mask, bit i-1 for display row i
constexpr uint32_t P_COUNT     = 0x126;     // u8  row count (max 12)
constexpr uint32_t P_COL       = 0x33E;     // u8  column cursor: 0 whole row, 1 condition, 2 action
constexpr uint32_t R_COND_NAME = 0x00;      // codec* -- rec 0 holds the CHARACTER name here
constexpr uint32_t R_ACT_NAME  = 0x08;      // codec* -- null on rec 0 (the header has no action)
constexpr uint32_t R_COND_ID   = 0x10;      // u16 condition id, 0xFFFF when that half is unset
constexpr uint32_t R_ACT_ID    = 0x12;      // u16 action id,    0xFFFF when that half is unset
constexpr uint32_t R_ON        = 0x14;      // u8  enabled; on rec 0 this is the MASTER toggle
constexpr uint32_t R_CLASS     = 0x15;      // u8  class -- LOGGED, NEVER GATED ON. See below.

constexpr uint16_t ID_UNSET    = 0xFFFF;

// **THE CLASS BYTE MEANS "INCOMPLETE", NOT "EMPTY", AND GATING ON IT WAS THE REPORTED DEFECT.**
//
// A gambit row can hold a CONDITION with no ACTION: the player picks the condition, the game commits
// it, and the action cell stays blank until they pick one. The record builder FUN_00567b60 (RVA
// 0x447B60) writes rec+0x10 and the real condition name into rec+0x00 FIRST, unconditionally, and
// only afterwards downgrades rec+0x15 to 2 when EITHER id is 0xFFFF. The single-field commit
// FUN_0056a1d0 (0x44A1D0) does the same on picker confirm, and FUN_00569f90 (0x449F90) writes the
// half-set row back to the save block -- which is why a condition-only row survives closing the menu.
// The row painter FUN_00568bb0 (0x448BB0) assigns rec+0x00 into the condition sprite for every row
// regardless of class, so a sighted player READS THAT CONDITION ON SCREEN; class 2 only dims it.
//
// So the two ids are the only test that answers the question, and they answer it separately:
//   rec+0x10 != 0xFFFF -> a condition is set        rec+0x12 != 0xFFFF -> an action is set
// A truly empty row is both unset -- what the row-clear path writes.
//
// The S94 probe (probe_gambit_menu_output.log) had ALREADY caught the class byte taking 0, 1 and 2
// on one screen. It read as "2 = empty" only because that run never edited a row, so every class-2
// row it saw happened to be fully empty. The byte is kept on the log line below as evidence, and its
// real meaning is recorded in GameArchitecture.md: 2 = incomplete, 0/1 = the condition's target
// class (that last part is 0.80 and NOT acted on anywhere).

// THE THREE COLUMNS, from the game's own help ids rather than from a guess. FUN_005691e0 `case 0xc`
// picks the description by this very cursor -- 0xCF1 for 0, 0xCEE for 1, 0xCEF for 2 (and 0xCF2 when
// there is no row) -- and those ids resolve in `help_menu.bin`, section 3, to:
//     0xCF1  "Toggle slot ON/OFF."                                            -> column 0
//     0xCEE  "Change the conditions under which an action is performed."      -> column 1
//     0xCEF  "Change which action is performed."                              -> column 2
//     0xCF2  "Toggle gambits ON/OFF."                                         -> record 0's master
//
// **Column 0 is the per-slot ON/OFF checkbox, NOT "the whole row"** -- which is what the first version
// of this reader assumed, so arrowing onto it read the entire row out instead of the one state the
// player had highlighted. That was the reported defect: *"the left and right nav keys don't seem to be
// announcing what is highlighted correctly."* There is no "whole row" cursor position at all.
constexpr int COL_ONOFF = 0, COL_COND = 1, COL_ACT = 2;

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

bool RecU16(void* panel, int rec, uint32_t field, uint16_t* out) {
    return SafeReadU16(panel, P_RECS + static_cast<uint32_t>(rec) * REC_STRIDE + field, out);
}

// ONE field of a row, as the player should hear it.
//
// An unset half is not a decode failure -- the builder stores the game's own blank placeholder
// (FUN_002f9860(0x3EB), a single non-letter glyph) in that half's name pointer, which RecName's
// printability gate correctly rejects. So the ID decides whether the field is absent, and the word
// for an absent one is the phrasebook's, not the codec's.
std::wstring FieldText(void* panel, int rec, uint32_t nameField, uint16_t id) {
    if (id == ID_UNSET) return Phrase::Get(Phrase::Id::EmptySlot);
    return RecName(panel, rec, nameField);
}

std::wstring OnOffWord(bool on) {
    return Phrase::Get(on ? Phrase::Id::On : Phrase::Id::Off);
}

// ONE emit function for this surface, per the one-choke-point rule: the wording, the logging and the
// interrupt policy exist here and nowhere else.
// `cls` rides on the tag rather than in a separate line: it is the byte this reader used to gate on,
// so every future log carries the state it was read in alongside what was actually said.
void Speak(void* owner, const std::wstring& line, int rec, int col, uint8_t cls) {
    if (line.empty()) return;
    char tag[64];
    snprintf(tag, sizeof(tag), "gambit rec=%d col=%d cls=%u:", rec, col, cls);
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

enum class Move { None, Row, Column };

// A ROW move and a COLUMN move are different questions and get different answers, which is the other
// half of the fix: moving DOWN a row means the player wants the whole row, moving LEFT/RIGHT means
// they want the one field they just landed on. Arriving on a new panel counts as a row move, so
// entering the screen (or flipping to another gambit set) always announces a full row.
Move Moved(void* panel, int rec, int col) {
    Move m;
    if (panel != s_lastPanel || rec != s_lastRec) m = Move::Row;
    else if (col != s_lastCol)                    m = Move::Column;
    else                                          m = Move::None;
    s_lastPanel = panel; s_lastRec = rec; s_lastCol = col;
    return m;
}

// The panel the player is actually looking at. Non-null and different => a background set of the
// carousel; stay silent for it. A null read means the global is not up yet, in which case believing
// the dispatch is better than going mute.
bool IsVisiblePanel(void* panel) {
    void* cur = PtrAt(Hooks::ResolveRva(RVA_CUR_PANEL), 0);
    return !cur || cur == panel;
}

} // namespace

bool IsGambitPanel(void* owner) {
    return owner && Obj0(owner) == Hooks::ResolveRva(RVA_PANEL);
}

bool OnFocus(void* panel, int recIndex) {
    if (!IsGambitPanel(panel)) return false;
    STALL_SCOPE("GambitReader::OnFocus");

    // All three carousel panels take the entry 0x8000; only the visible one has anything to say.
    if (!IsVisiblePanel(panel)) return true;

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
    const int col = (recIndex == 0) ? COL_ONOFF : static_cast<int>(colRaw);
    const Move moved = Moved(panel, recIndex, col);
    if (moved == Move::None) return true;

    uint8_t on = 0, cls = 0;
    RecByte(panel, recIndex, R_ON, &on);
    RecByte(panel, recIndex, R_CLASS, &cls);

    // --- the character header + gambit master toggle ---
    // BEFORE the unset test below, because record 0's ids are 0/0, not 0xFFFF -- it is not a gambit
    // row and its two id slots do not mean what they mean everywhere else.
    if (recIndex == 0) {
        std::wstring line = RecName(panel, 0, R_COND_NAME);
        if (line.empty()) {
            Log::Write("INGAME", "gambit: header name did not decode -- SILENT");
            return true;
        }
        Speak(panel, line + L", " + OnOffWord(on != 0), recIndex, col, cls);
        return true;
    }

    uint16_t condId = ID_UNSET, actId = ID_UNSET;
    if (!RecU16(panel, recIndex, R_COND_ID, &condId) || !RecU16(panel, recIndex, R_ACT_ID, &actId)) {
        char m[96];
        snprintf(m, sizeof(m), "gambit: rec %d ids unreadable -- SILENT", recIndex);
        Log::Write("INGAME", m);
        return true;                       // silence beats guessing at a row's contents
    }

    // --- an unset row: BOTH halves missing ---
    // Spoken, not silent, and this is NOT the "never speak filler" case. That rule is about having
    // nothing to report; here the report IS that the slot is empty, and on a 12-row list silence
    // would leave the player unable to tell the cursor moved. `EmptySlot` already exists for exactly
    // this and ability_summary_reader sets the precedent.
    if (condId == ID_UNSET && actId == ID_UNSET) {
        Speak(panel, Phrase::Get(Phrase::Id::EmptySlot), recIndex, col, cls);
        return true;
    }

    const std::wstring cond = FieldText(panel, recIndex, R_COND_NAME, condId);
    const std::wstring act  = FieldText(panel, recIndex, R_ACT_NAME,  actId);

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

    // WHICH KEY MOVED decides how much to say -- not the column, which is what the first version got
    // wrong. Arrive on a new ROW and you want all of it; move LEFT/RIGHT and you want only the field
    // you landed on, because you already heard the rest a moment ago and the row has not changed.
    //
    // A HALF-SET ROW ANSWERS ON EVERY COLUMN. `cond` and `act` are each either the game's own text or
    // the word "empty", so the row reads "Foe: party leader's target, empty, off" and the action
    // column answers "empty" instead of dropping into the no-text SILENT path below.
    std::wstring line;
    if (moved == Move::Row) {
        line = cond;
        if (!act.empty())  line += (line.empty() ? L"" : L", ") + act;
        if (!line.empty()) line += L", " + OnOffWord(on != 0);
    } else if (col == COL_COND) {
        line = cond;
    } else if (col == COL_ACT) {
        line = act;
    } else {
        // The ON/OFF checkbox. Its STATE is the whole content of this column -- the game's own
        // description bar already says what the column is ("Toggle slot ON/OFF."), and inventing a
        // spoken label for it would be a fabricated one. Same call the tester made for the party
        // toggle: the cursor has not left the row, so repeating the row would bury the one bit they
        // moved to hear.
        line = OnOffWord(on != 0);
    }

    if (line.empty()) {
        // Reachable now only when a SET id's name failed to decode -- an unset half already carries
        // the "empty" word. The ids are on the line so the next log says which half went wrong.
        char m[160];
        snprintf(m, sizeof(m),
                 "gambit: rec %d col %d produced no text (cond=%d act=%d chars, "
                 "condId=0x%04X actId=0x%04X cls=%u) -- SILENT",
                 recIndex, col, static_cast<int>(cond.size()), static_cast<int>(act.size()),
                 condId, actId, cls);
        Log::Write("INGAME", m);
        return true;
    }
    Speak(panel, line, recIndex, col, cls);
    return true;
}

} // namespace GambitReader

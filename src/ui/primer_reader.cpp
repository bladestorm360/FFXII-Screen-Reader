#include "ui/primer_reader.h"

#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "speech/phrasebook.h"
#include "speech/speech.h"
#include "ui/text_capture.h"
#include "ui/virtual_buffer.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace {

using MemRead::Obj0;
using MemRead::PtrAt;
using MemRead::SafeReadU8;
using MemRead::SafeReadU16;

// ---- hooks + classes (all confirmed live, probe_save_and_primer) --------------------------------
constexpr uint32_t RVA_REC_BUILD = 0x456300;  // FUN_00576300(record) -- fills the 0x50-byte entry record
constexpr uint32_t RVA_SET_PAGE  = 0x453BE0;  // FUN_00573be0(viewer, pageIdx) -- lays out one page
constexpr uint32_t RVA_MODE_SET  = 0x44F2A0;  // FUN_0056f2a0(container, mode) -- picks the sub-screen
constexpr uint32_t RVA_VIEWER    = 0x4542D0;  // FUN_005742d0 -- the page viewer's class

// ---- HUNTS (sub-screen 6). Every offset below confirmed live by probe_primer_screens -----------
constexpr uint32_t RVA_HUNT_LIST   = 0x459420;  // FUN_00579420 -- the Hunts list window class
constexpr uint32_t RVA_HUNT_ROW    = 0x457C30;  // FUN_00577c30(kind, cells, rowIndex, ...) row build
constexpr uint32_t RVA_HUNT_REC    = 0x25E5B0;  // FUN_0037e5b0(id, out) -- fills the row struct
constexpr uint32_t RVA_HUNT_DETAIL = 0x458450;  // FUN_00578450(win, msg) -- the Quest Progress pane
// The mark-complete bitfield. FUN_0046b1a0's ENTIRE body is this test, so it is a memory read here
// and NOT a game call: bit (id & 7) of *(u8*)(BLOCK + 0x1020 + (id >> 3)). The probe printed the
// block at 0x2164480 absolute -- FUN_002ef640's base + 0x200, which is what FUN_002ef2b0 returns.
constexpr uint32_t RVA_HUNT_FLAGS  = 0x2044480;
constexpr uint32_t OFF_HUNT_FLAGS  = 0x1020;

constexpr uint32_t OFF_HR_NAME  = 0x00;   // char* mark name          ("Red & Rotten in the Desert")
constexpr uint32_t OFF_HR_ID    = 0x08;   // u16   the bitfield id    (row 1 -> 128)
constexpr uint32_t OFF_HR_GIVER = 0x10;   // char* petitioner + place ("Tomaj (Rabanastre)")
constexpr uint32_t OFF_QP_INNER = 0xD0;   // huntsDetail+0xD0 -> the block whose +0x28 is the body
constexpr uint32_t OFF_QP_TEXT  = 0x28;

constexpr int MAX_HUNT_ROWS = 128;

// ---- SKY PIRATE'S DEN (sub-screen 7) ------------------------------------------------------------
// The Den is a PICTURE of sprites, not a list: it emits no 0x8000, paints through no list painter,
// and moving between characters is internal to its own window. So the cursor is read from the window
// and the tooltip is caught at construction.
constexpr uint32_t RVA_ENTRY_LIST = 0x451A50; // FUN_00571a50 -- Bestiary / Tips entry list
constexpr uint32_t RVA_DEN       = 0x452E10;  // FUN_00572e10 -- the Den window
constexpr uint32_t RVA_DEN_TIP   = 0x452600;  // FUN_00572600 -- the achievement tooltip it creates
constexpr uint32_t OFF_DEN_CURSOR= 0xCB;      // den+0xCB    u8 achievement index; 0x1E = nothing picked
constexpr uint32_t OFF_TIP_INDEX = 0xC8;      // tooltip+0xC8 u8 the same index, latched at construct
constexpr uint8_t  DEN_CURSOR_NONE = 0x1E;
// den+0xD0 + cursor*0x80 is the per-achievement record the Den hands to the tooltip
// (`FUN_00572e10:77`), and 0x1100 bytes are memset there at construct -- 34 records of 0x80.
constexpr uint32_t OFF_DEN_RECORDS   = 0xD0;
constexpr uint32_t DEN_RECORD_STRIDE = 0x80;
// FUN_00572600 resolves BOTH of these itself, so these bases are the game's own arithmetic, not a
// fitted mapping: `FUN_002f9860(idx + 0x1b68)` and `FUN_002f9860(idx + 0x5dde)`. Corroborated live --
// index 0 produced ids 7016 ("Balthier") and 24030 ("Awarded for Attacking over 300 times...").
constexpr int DEN_NAME_ID_BASE = 0x1B68;
constexpr int DEN_TEXT_ID_BASE = 0x5DDE;

// Read off the VIEWER, straight out of FUN_00573be0's own body -- no latching, no guessing.
constexpr uint32_t OFF_VIEW_REC   = 0xD0;     // viewer+0xD0   -> the 0x50 entry record
constexpr uint32_t OFF_VIEW_FLAGS = 0xC8;     // viewer+0xC8   bit 0 -> the page index is OFFSET BY ONE
constexpr uint32_t OFF_VIEW_MODE  = 0x8060;   // viewer+0x8060 -- the sub-screen id (5 = Bestiary)

constexpr uint32_t OFF_REC_PAGES = 0x03;      // u8 page count
constexpr uint32_t OFF_REC_ARRAY = 0x08;      // + i*8 -> char* page i
constexpr int      MAX_PAGES     = 16;        // real entries run 1-3; the cap is a garbage guard
constexpr size_t   PAGE_MAX      = 2048;      // longest page seen is ~600 bytes

typedef void (*Pfn_RecBuild)(void* record);
typedef void (*Pfn_SetPage)(void* viewer, int pageIdx);
typedef void (*Pfn_ModeSet)(void* container, int mode);
typedef void (*Pfn_HuntRow)(int kind, void* cells, int rowIndex, void* tbl, void* strs);
typedef void (*Pfn_HuntRec)(int id, void* out);
typedef uint64_t (*Pfn_HuntDetail)(void* win, void* msg);
Pfn_RecBuild s_origRecBuild = nullptr;
Pfn_SetPage  s_origSetPage  = nullptr;
Pfn_ModeSet  s_origModeSet  = nullptr;
Pfn_HuntRow    s_origHuntRow    = nullptr;
Pfn_HuntRec    s_origHuntRec    = nullptr;
Pfn_HuntDetail s_origHuntDetail = nullptr;

// Row -> (petitioner, complete), filled as the list paints. The mark NAME is deliberately NOT kept:
// the generic painted-cell path already speaks it on the same focus, and a second speaker on one row
// is the race choice_reader.cpp records. Only what a sighted player reads that the mod could not --
// the petitioner cell, and the COMPLETE badge, which is sprite art with no text behind it.
struct HuntRow { std::wstring name; std::wstring giver; bool complete = false; bool valid = false; };
HuntRow g_huntRows[MAX_HUNT_ROWS];
int     g_huntRowBuilding = -1;   // set by the row-build hook, read by the record hook inside it

std::mutex        g_mutex;
void*             g_record = nullptr;   // the entry currently open
void*             g_viewer = nullptr;   // the page viewer that last laid a page out
VB::VirtualBuffer g_buffer;
bool              g_haveBuffer = false;

// One page's codec string, or null.
const uint8_t* PageText(void* rec, int i) {
    if (!rec || i < 0 || i >= MAX_PAGES) return nullptr;
    return static_cast<const uint8_t*>(PtrAt(rec, OFF_REC_ARRAY + static_cast<uint32_t>(i) * 8));
}

int PageCount(void* rec) {
    uint8_t n = 0;
    if (!rec || !SafeReadU8(rec, OFF_REC_PAGES, &n)) return 0;
    return (n > MAX_PAGES) ? 0 : n;                     // a wild count means this is not our record
}

// Split a decoded page into announceable lines. The game wraps its own prose with hard newlines at
// column width, so a raw line-per-entry buffer would read " mean-" / "spirited beastie" -- the
// hyphenation is IN the text. Joined into sentences instead, which is what a listener actually wants
// to step through.
void SplitLines(const std::wstring& body, std::vector<std::wstring>& out) {
    std::wstring cur;
    for (size_t i = 0; i < body.size(); ++i) {
        const wchar_t c = body[i];
        if (c == L'\n') {
            // A hard wrap mid-word: the game hyphenates, so glue the halves back together.
            if (!cur.empty() && cur.back() == L'-') { cur.pop_back(); continue; }
            if (!cur.empty()) cur += L' ';
            continue;
        }
        cur += c;
        // End of a sentence -> one entry. Keeps each utterance short enough to re-hear cheaply.
        if ((c == L'.' || c == L'!' || c == L'?') && cur.size() > 8) {
            out.push_back(cur);
            cur.clear();
        }
    }
    while (!cur.empty() && (cur.back() == L' ')) cur.pop_back();
    if (!cur.empty()) out.push_back(cur);
}

// Decode page `i` into its HEADER and BODY. Pages are "HEADER 0x03 BODY" and DecodePages splits on
// exactly that byte, so this is the game's own division, not a guess at punctuation.
bool ReadPage(void* rec, int i, std::wstring* header, std::wstring* body) {
    const uint8_t* p = PageText(rec, i);
    if (!p) return false;
    std::vector<std::wstring> parts;
    GameText::DecodePages(p, PAGE_MAX, parts);
    if (parts.empty()) return false;
    if (parts.size() == 1) { header->clear(); *body = parts[0]; }
    else {
        *header = parts[0];
        body->clear();
        for (size_t k = 1; k < parts.size(); ++k) {
            if (!body->empty()) *body += L' ';
            *body += parts[k];
        }
    }
    // A page whose body is unreadable is not spoken -- never fabricate, and never read raw codec.
    return GameText::IsMostlyPrintable(*header) || GameText::IsMostlyPrintable(*body);
}

// Strip the hard wraps out of a header for speech ("Plant\nCactus" -> "Plant, Cactus": the bestiary
// puts genus and species on two lines and they are two facts, not one word).
std::wstring FlattenHeader(const std::wstring& h) {
    std::wstring s;
    for (wchar_t c : h) {
        if (c == L'\n') { if (!s.empty() && s.back() != L' ') s += L", "; }
        else s += c;
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L',')) s.pop_back();
    return s;
}

// Append one record page to the walk buffer as "header" + one entry per sentence.
void AppendPage(void* rec, int recPage, std::vector<VB::VirtualBuffer::Entry>& entries) {
    std::wstring header, body;
    if (!ReadPage(rec, recPage, &header, &body)) return;
    const std::wstring head = FlattenHeader(header);
    if (!head.empty()) entries.push_back([head]() { return head; });
    std::vector<std::wstring> lines;
    SplitLines(body, lines);
    for (const auto& l : lines) entries.push_back([l]() { return l; });
}

void AnnouncePage(void* viewer, int pageIdx) {
    // THE RECORD AND THE PAGE OFFSET BOTH COME OFF THE VIEWER, because FUN_00573be0 reads them there:
    //     bVar9 = param_2 + 1; if ((*(uint *)(param_1 + 200) & 1) == 0) bVar9 = param_2;
    //     uVar7 = *(undefined8 *)(*(longlong *)(param_1 + 0xd0) + 8 + bVar9 * 8);
    // THAT `+ 1` IS WHY THE BESTIARY READ WRONG. The record page is the displayed page PLUS ONE when
    // viewer+0xC8 bit 0 is set, and the Bestiary sets it -- record page 0 there is the
    // CLASSIFICATION / GENUS box, which is not a page you can turn to. Reading page N for display N
    // therefore spoke the classification while the player was on Observations, and Observations while
    // they were on the Adventurer's Handbook: always one behind, on exactly the screens that have the
    // extra page. Traveller's Tips has the bit CLEAR, which is why it read correctly and the Bestiary
    // did not -- the same code, two behaviours, and the mod only ever implemented one.
    void* rec = PtrAt(viewer, OFF_VIEW_REC);
    if (!rec) { std::lock_guard<std::mutex> lk(g_mutex); rec = g_record; }
    if (!rec) return;

    uint32_t flags = 0;
    MemRead::SafeReadU32(viewer, OFF_VIEW_FLAGS, &flags);
    const int firstShown = (flags & 1u) ? 1 : 0;      // record page of the FIRST turnable page
    const int recPage    = pageIdx + firstShown;

    const int total = PageCount(rec);
    if (total <= 0 || recPage < 0 || recPage >= total) return;

    std::wstring header, body;
    if (!ReadPage(rec, recPage, &header, &body)) return;

    const std::wstring head = FlattenHeader(header);
    std::wstring line = head;                       // `line` is the WHOLE page, for `o`
    if (!body.empty()) { if (!line.empty()) line += L". "; line += body; }
    if (line.empty()) return;

    // THE BUFFER HOLDS THE WHOLE ENTRY, not just the page on screen. A sighted player can see the
    // classification box and both pages; walking only the current page was the "partial buffer".
    // Page 0 first when it is the classification block, then every turnable page in order.
    std::vector<VB::VirtualBuffer::Entry> entries;
    if (firstShown == 1) AppendPage(rec, 0, entries);        // CLASSIFICATION / GENUS
    for (int p = firstShown; p < total; ++p) AppendPage(rec, p, entries);
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_viewer = viewer;
        g_buffer = VB::VirtualBuffer(std::move(entries));
        g_haveBuffer = true;
    }

    // The classification box is on screen on EVERY page of a bestiary entry, so it belongs in what
    // `o` re-reads -- but NOT in the announcement, see below.
    if (firstShown == 1) {
        std::wstring cHead, cBody;
        if (ReadPage(rec, 0, &cHead, &cBody)) {
            const std::wstring cls = FlattenHeader(cHead.empty() ? cBody : cHead);
            if (!cls.empty()) { line += L". "; line += cls; }
        }
    }

    // `o` re-reads the whole page: the documented extension point for surfaces the game does not
    // feed to the description-bar setter.
    TextCapture::ProvideHelpText(line);

    // SPEAK THE HEADING ONLY; THE BUFFER CARRIES THE ENTRY.
    //
    // This used to recite the heading, the whole body AND the classification box in one breath, which
    // landed as "two separate, disconnected bits of the screen" -- because that is exactly what it
    // was: prose from the right-hand panel followed by a genus/species label from a box in the
    // corner, with nothing to signal the jump. Announcing the HEADING tells the player which page
    // they turned to, and everything else is one arrow key away in the order the screen has it.
    std::wstring spoken = head;
    if (spoken.empty()) {                            // a page with no heading -> its first sentence
        std::lock_guard<std::mutex> lk(g_mutex);
        spoken = g_buffer.Current();
    }
    if (spoken.empty()) return;
    Log::WriteW("PRIMER", "page:", viewer, spoken);
    Speech::Output(spoken, /*interrupt=*/true);
}

// The mark-complete flag, as a pure read.
bool HuntComplete(int huntId) {
    if (huntId < 0 || huntId > 0xFFFF) return false;
    void* blk = Hooks::ResolveRva(RVA_HUNT_FLAGS);
    if (!blk) return false;
    uint8_t byte = 0;
    if (!SafeReadU8(blk, OFF_HUNT_FLAGS + static_cast<uint32_t>(huntId >> 3), &byte)) return false;
    return (byte & (1u << (huntId & 7))) != 0;
}

std::wstring DecodeAt(void* rec, uint32_t off) {
    const uint8_t* p = static_cast<const uint8_t*>(PtrAt(rec, off));
    if (!p) return std::wstring();
    std::wstring s = GameText::Decode(p, 256);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// FUN_00577c30(kind, cells, rowIndex, ...) builds ONE hunt row and calls FUN_0037e5b0 inside itself.
// Pairing the two is what turns "here is a row struct" into "here is the struct for ROW N": the
// struct call carries no row index, and the focus message carries no struct.
void HookedHuntRow(int kind, void* cells, int rowIndex, void* tbl, void* strs) {
    g_huntRowBuilding = rowIndex;
    if (s_origHuntRow) s_origHuntRow(kind, cells, rowIndex, tbl, strs);
    g_huntRowBuilding = -1;
}

void HookedHuntRec(int id, void* outRec) {
    if (s_origHuntRec) s_origHuntRec(id, outRec);
    const int row = g_huntRowBuilding;
    if (row < 0 || row >= MAX_HUNT_ROWS || !outRec) return;
    uint16_t huntId = 0;
    SafeReadU16(outRec, OFF_HR_ID, &huntId);
    std::lock_guard<std::mutex> lk(g_mutex);
    g_huntRows[row].name     = DecodeAt(outRec, OFF_HR_NAME);
    g_huntRows[row].giver    = DecodeAt(outRec, OFF_HR_GIVER);
    g_huntRows[row].complete = HuntComplete(static_cast<int>(huntId));
    g_huntRows[row].valid    = true;
}

// The Quest Progress pane, raised by confirming a mark. FUN_00578450 stores the block it was handed
// at win+0xD0, and the body hangs off that at +0x28 -- read from the WINDOW rather than the construct
// message, so this does not depend on the caller's argument shape.
uint64_t HookedHuntDetail(void* win, void* msg) {
    const uint64_t ret = s_origHuntDetail ? s_origHuntDetail(win, msg) : 0;
    uint32_t kind = 0;
    if (!msg || !MemRead::SafeReadU32(msg, 0, &kind) || kind != 1) return ret;   // construct only

    void* inner = PtrAt(win, OFF_QP_INNER);
    if (!inner) return ret;
    const uint8_t* body = static_cast<const uint8_t*>(PtrAt(inner, OFF_QP_TEXT));
    if (!body) return ret;
    std::wstring text = GameText::Decode(body, PAGE_MAX);
    if (!GameText::IsMostlyPrintable(text)) return ret;

    std::vector<std::wstring> lines;
    SplitLines(text, lines);
    if (lines.empty()) return ret;

    std::vector<VB::VirtualBuffer::Entry> entries;
    for (const auto& l : lines) entries.push_back([l]() { return l; });
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_viewer = win;                 // liveness key for the walk buffer: this window
        g_buffer = VB::VirtualBuffer(std::move(entries));
        g_haveBuffer = true;
    }

    // THE WHOLE SCREEN GOES IN THE BUFFER; ONLY THE FIRST LINE IS SPOKEN.
    //
    // This first shipped reciting the entire quest log on confirm, which is a wall of text for a
    // screen the player opened to BROWSE. The buffer is the point of the feature: arrows walk it a
    // line at a time and `o` re-reads the lot. Speaking the opening line is the "you are here" cue
    // and nothing more -- the same shape StatusReader uses when its page becomes active.
    std::wstring first;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        first = g_buffer.JumpTop();
    }
    std::wstring whole;
    for (const auto& l : lines) { if (!whole.empty()) whole += L' '; whole += l; }
    TextCapture::ProvideHelpText(whole);
    Log::WriteW("PRIMER", "quest progress:", win, first);
    if (!first.empty()) Speech::Output(first, /*interrupt=*/true);
    return ret;
}

typedef uint64_t (*Pfn_Den)(void* win, void* msg);
Pfn_Den s_origDen    = nullptr;
Pfn_Den s_origDenTip = nullptr;
uint8_t g_denCursor  = DEN_CURSOR_NONE;

// The Den window. It sends no focus message of any kind, so the cursor is read off the window after
// the game's own handler has run and announced only when the VALUE CHANGES.
//
// PERMITTED change-check (CLAUDE.md's per-frame exception). GUARDS: FUN_00572e10, the Den's window
// proc, which the engine calls for its update/draw messages as well as for input -- without the check
// the same character would be announced continuously. This is a TRANSITION DETECTOR on the game's own
// cursor byte, the same shape dialogue_reader uses for the page cursor, not a speech dedup: it is
// reset to "nothing picked" whenever the Den is left, so re-entering re-announces.
uint64_t HookedDen(void* win, void* msg) {
    const uint64_t ret = s_origDen ? s_origDen(win, msg) : 0;
    uint8_t cur = DEN_CURSOR_NONE;
    if (!win || !SafeReadU8(win, OFF_DEN_CURSOR, &cur)) return ret;
    if (cur == g_denCursor) return ret;
    g_denCursor = cur;
    if (cur == DEN_CURSOR_NONE) return ret;      // moved off everything -> nothing to report

    // THE CURSOR IS A POSITION ON THE PICTURE, NOT AN ACHIEVEMENT ID. Using it as one read the second
    // icon -- a Chocobo -- as "Fran". The proof was in the mod's own log: the TOOLTIP was correct
    // ("Basch. Awarded for felling over 500 foes") while navigation was wrong, and the tooltip does
    // not use the cursor. It uses the per-achievement RECORD the Den hands it, at
    // den+0xD0 + cursor*0x80 (FUN_00572e10:77), so the id is read from there.
    //
    // The id is logged beside the cursor precisely because this is the one link the decompile could
    // not settle -- Ghidra's local ordering in FUN_00572e10's argument block is ambiguous, so if a
    // name is ever wrong again the log says which of the two numbers to blame instead of guessing.
    void* rec = static_cast<char*>(win) + OFF_DEN_RECORDS +
                static_cast<size_t>(cur) * DEN_RECORD_STRIDE;
    uint8_t id = 0;
    if (!SafeReadU8(rec, 0, &id)) return ret;

    const std::wstring name = TextCapture::ResolveStringById(DEN_NAME_ID_BASE + id);
    if (name.empty()) return ret;                 // unresolvable -> silent, never a number
    char m[96];
    snprintf(m, sizeof(m), "den: cursor=%u -> achievement id %u", cur, id);
    Log::Write("PRIMER", m);
    Log::WriteW("PRIMER", "den:", win, name);
    Speech::Output(name, /*interrupt=*/true);
    return ret;
}

// The achievement tooltip, raised by confirming a character. Its own constructor resolves the name
// and the body, so this reads the index it latched and asks for the same two ids.
uint64_t HookedDenTip(void* win, void* msg) {
    const uint64_t ret = s_origDenTip ? s_origDenTip(win, msg) : 0;
    uint32_t kind = 0;
    if (!msg || !MemRead::SafeReadU32(msg, 0, &kind) || kind != 1) return ret;   // construct only
    uint8_t idx = 0;
    if (!win || !SafeReadU8(win, OFF_TIP_INDEX, &idx)) return ret;
    {
        // The tooltip latched this index itself and it is KNOWN GOOD (it produced the right award in
        // play). Logged so it can be compared against the id navigation derived from the record -- if
        // those two ever disagree, the record-byte reading above is what is wrong.
        char m[80];
        snprintf(m, sizeof(m), "den award: tooltip index %u", idx);
        Log::Write("PRIMER", m);
    }

    const std::wstring name = TextCapture::ResolveStringById(DEN_NAME_ID_BASE + idx);
    const std::wstring text = TextCapture::ResolveStringById(DEN_TEXT_ID_BASE + idx);
    if (name.empty() && text.empty()) return ret;

    std::wstring line = name;
    if (!text.empty()) { if (!line.empty()) line += L". "; line += text; }

    std::vector<std::wstring> lines;
    SplitLines(text, lines);
    std::vector<VB::VirtualBuffer::Entry> entries;
    if (!name.empty()) entries.push_back([name]() { return name; });
    for (const auto& l : lines) entries.push_back([l]() { return l; });
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_viewer = win;
        g_buffer = VB::VirtualBuffer(std::move(entries));
        g_haveBuffer = true;
    }
    TextCapture::ProvideHelpText(line);
    Log::WriteW("PRIMER", "den award:", win, line);
    Speech::Output(line, /*interrupt=*/true);
    return ret;
}

void HookedRecBuild(void* record) {
    if (s_origRecBuild) s_origRecBuild(record);
    // The record is filled by the time the original returns; latch it as the open entry.
    std::lock_guard<std::mutex> lk(g_mutex);
    g_record = record;
}

void HookedSetPage(void* viewer, int pageIdx) {
    if (s_origSetPage) s_origSetPage(viewer, pageIdx);
    STALL_SCOPE("PrimerReader::SetPage");
    AnnouncePage(viewer, pageIdx);
}

// Moving between sub-screens (or leaving) invalidates the open entry. THIS IS THE RE-ARM, and it is
// here on purpose: three separate guards in this codebase have gone silent because their state
// outlived the object it described and nothing ever cleared it (see S126). The buffer must not keep
// answering arrow keys for an entry that is no longer on screen.
void HookedModeSet(void* container, int mode) {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_record = nullptr;
        g_viewer = nullptr;
        g_buffer = VB::VirtualBuffer();
        g_haveBuffer = false;
    }
    g_denCursor = DEN_CURSOR_NONE;   // leaving the Den must re-arm it, or re-entry is silent
    if (s_origModeSet) s_origModeSet(container, mode);
}

} // namespace

namespace PrimerReader {

bool OnMenuNavKey(int vk) {
    // LEFT/RIGHT belong to the game (page turn); HookedSetPage announces the result. Declining here
    // is what keeps one keypress to one speaker.
    if (vk == VK_LEFT || vk == VK_RIGHT) return false;

    std::wstring line;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_haveBuffer || !g_viewer || g_buffer.IsEmpty()) return false;
        // LIVENESS, not an assumption. The window we latched must still BE one of the surfaces that
        // owns a buffer; a freed or recycled one fails the class check (or faults, which Obj0
        // swallows) and the buffer stands down rather than answering for a screen that has gone.
        //
        // THREE CLASSES, NOT ONE. This tested only the page viewer, which is the Bestiary/Tips class --
        // so the Quest Progress buffer and the Sky Pirate's Den buffer were built correctly and then
        // refused every arrow key, because the window that owned them could never pass a test that
        // only accepted a different class. The buffer looked "not working" when it was actually full
        // and unreachable. A liveness check has to know every surface it is guarding.
        void* cls = Obj0(g_viewer);
        if (cls != Hooks::ResolveRva(RVA_VIEWER) &&
            cls != Hooks::ResolveRva(RVA_HUNT_DETAIL) &&
            cls != Hooks::ResolveRva(RVA_DEN_TIP)) {
            g_haveBuffer = false;
            return false;
        }
        switch (vk) {
            case VK_UP:   line = g_buffer.Previous();   break;
            case VK_DOWN: line = g_buffer.Next();       break;
            case VK_HOME: line = g_buffer.JumpTop();    break;
            case VK_END:  line = g_buffer.JumpBottom(); break;
            default: return false;
        }
    }
    if (line.empty()) return false;
    Log::WriteW("PRIMER", "nav:", nullptr, line);
    Speech::Output(line, /*interrupt=*/true);
    return true;
}

bool OwnsSurface(void* w) {
    if (!w) return false;
    void* cls = Obj0(w);
    return cls == Hooks::ResolveRva(RVA_ENTRY_LIST) || cls == Hooks::ResolveRva(RVA_VIEWER) ||
           cls == Hooks::ResolveRva(RVA_HUNT_LIST)  || cls == Hooks::ResolveRva(RVA_HUNT_DETAIL) ||
           cls == Hooks::ResolveRva(RVA_DEN)        || cls == Hooks::ResolveRva(RVA_DEN_TIP);
}

bool OwnsHuntList(void* w) {
    return w && Obj0(w) == Hooks::ResolveRva(RVA_HUNT_LIST);
}

bool OnHuntFocus(void* owner, int index) {
    if (!OwnsHuntList(owner) || index < 0 || index >= MAX_HUNT_ROWS) return false;
    std::wstring name, giver; bool complete = false, valid = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        valid    = g_huntRows[index].valid;
        name     = g_huntRows[index].name;
        giver    = g_huntRows[index].giver;
        complete = g_huntRows[index].complete;
    }
    if (!valid || name.empty()) return false;

    // ONE LINE, ONE SPEAKER: mark, status, petitioner -- the row as a sighted player reads it.
    //
    // This first shipped with only the status and petitioner, handed to `o` on the reasoning that the
    // painted-cell path already spoke the name so adding to it would double up. That avoided a race
    // the tester never had and created a worse problem: the two halves of one row were split across
    // two different keypresses, and the half you get without asking is the half you can already
    // guess. Claiming the row outright and building the whole sentence here is what "mark name,
    // status, petitioner" actually needs -- and the name comes from the same struct as the rest, so
    // there is no second source to drift from.
    std::wstring line = name;
    if (complete) { line += L", "; line += Phrase::Get(Phrase::Id::HuntComplete); }
    // No word for an incomplete mark: the game draws no badge for one, so there is nothing to report.
    if (!giver.empty()) { line += L", "; line += giver; }

    Log::WriteW("PRIMER", "hunt:", owner, line);
    Speech::Output(line, /*interrupt=*/true);
    return true;       // claimed -- the generic path must NOT also speak the bare name
}

bool Init() {
    bool ok  = Hooks::InstallTyped(RVA_REC_BUILD, &HookedRecBuild, &s_origRecBuild);
    ok      &= Hooks::InstallTyped(RVA_SET_PAGE,  &HookedSetPage,  &s_origSetPage);
    ok      &= Hooks::InstallTyped(RVA_MODE_SET,  &HookedModeSet,  &s_origModeSet);
    ok      &= Hooks::InstallTyped(RVA_HUNT_ROW,    &HookedHuntRow,    &s_origHuntRow);
    ok      &= Hooks::InstallTyped(RVA_HUNT_REC,    &HookedHuntRec,    &s_origHuntRec);
    ok      &= Hooks::InstallTyped(RVA_HUNT_DETAIL, &HookedHuntDetail, &s_origHuntDetail);
    ok      &= Hooks::InstallTyped(RVA_DEN,     &HookedDen,    &s_origDen);
    ok      &= Hooks::InstallTyped(RVA_DEN_TIP, &HookedDenTip, &s_origDenTip);
    Log::Write("PRIMER", ok
        ? "PrimerReader initialized (entry record FUN_00576300, page FUN_00573be0, sub-screen FUN_0056f2a0)"
        : "PrimerReader: a hook FAILED to install -- see Hooks log; the Clan Primer body stays silent");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_DEN_TIP);
    Hooks::Uninstall(RVA_DEN);
    Hooks::Uninstall(RVA_HUNT_DETAIL);
    Hooks::Uninstall(RVA_HUNT_REC);
    Hooks::Uninstall(RVA_HUNT_ROW);
    Hooks::Uninstall(RVA_MODE_SET);
    Hooks::Uninstall(RVA_SET_PAGE);
    Hooks::Uninstall(RVA_REC_BUILD);
    std::lock_guard<std::mutex> lk(g_mutex);
    g_record = nullptr;
    g_viewer = nullptr;
    g_buffer = VB::VirtualBuffer();
    g_haveBuffer = false;
}

} // namespace PrimerReader

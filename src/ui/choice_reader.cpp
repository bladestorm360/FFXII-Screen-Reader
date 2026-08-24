#include "ui/choice_reader.h"

#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "ui/text_capture.h"
#include "speech/speech.h"

#include <Windows.h>
#include <cstdio>
#include <mutex>
#include <vector>

namespace ChoiceReader {
namespace {

using MemRead::Obj0;

// ---- RVAs / offsets (abs = RVA + 0x120000) ----------------------------------------------------
constexpr uint32_t RVA_CHOICE_WND = 0x186190;  // FUN_002a6190 -- field dialogue / choice window
constexpr uint32_t RVA_CHOICE_TICK = 0x189980; // FUN_002a9980(widget) -- mid-dialogue choice tick
constexpr uint32_t OFF_W_TEXT   = 0x28;        // widget+0x28 = message text base
constexpr uint32_t OFF_W_OFFSET = 0x8A;        // widget+0x8A = u16 byte offset (current page)
constexpr uint32_t OFF_W_CURSOR = 0x58;        // widget+0x58 = i16 cursor index
constexpr uint32_t OFF_W_COUNT  = 0xA2;        // widget+0xA2 = u8 option count (mode 2)

// ---- the widget's OTHER selection mode: an editable number (see choice_reader.h) ---------------
constexpr uint32_t OFF_W_STATE  = 0xB0;        // widget+0xB0 = state word; low byte = mode
constexpr uint32_t OFF_W_VALUE  = 0x54;        // widget+0x54 = i32 selected value (mode 4)
constexpr uint32_t OFF_W_DIGITS = 0xA1;        // widget+0xA1 = u8 digit count (mode 4)
constexpr uint8_t  MODE_NUMERIC = 4;           // the `0F 2D` numeric field (2 = the 0x0E list)
constexpr uint32_t FLAG_CANDIDATES = 0x10000000u;  // state word bit 28: pick-from-list, not a range

// WHERE THE MENU'S OWN DATA LIVES. FUN_002b35a0 (RVA 0x1935A0) configures the field out of the
// window's inline argument table and nowhere else, so these four slots ARE the menu:
//
//     slot 29  how many candidates. >= 1 selects pick-from-a-list; < 1 selects a numeric range.
//     slot 28  the candidate index the field opens on (clamped to count-1).
//     slot 30  \ the two bounds of a range. The function orders them and records WHICH SLOT is
//     slot 31  / the low one in bits 0-5 of widget+0x58 and the high one in bits 6-11.
//
// AND THE CANDIDATES THEMSELVES ARE SLOTS 0 .. count-1 -- the function reads the selected value
// as `table[index]` and then walks `table[0..count-1]` to size the digit width. That is the
// destination list, and it is why reading widget+0x54 alone can echo the number on screen but can
// never say what the lift offers.
constexpr uint32_t ARG_VALUE      = 8;    // entry+8: the int for a type-0 (numeric) slot
constexpr uint32_t SLOT_INDEX     = 28;
constexpr uint32_t SLOT_COUNT     = 29;
constexpr int      MAX_CANDIDATES = 28;   // slots 0..27; 28-31 are the configuration above

typedef uint64_t (*Pfn_ChoiceTick)(void*);
Pfn_ChoiceTick s_origChoiceTick = nullptr;

constexpr uint32_t OFF_LIST_BLOCK = 0x0D0;     // window+0x0D0 = the embedded list widget
constexpr uint32_t OFF_HIDE_MASK  = 0x084;     // listBlock+0x84 = per-slot hidden bitmask (u32)

constexpr uint8_t  CTRL_OPTIONS   = 0x0E;      // the option-block marker
constexpr uint8_t  CTRL_PAGE      = 0x03;      // page break (GameText::DecodePages splits on it)
constexpr uint8_t  CTRL_COL_A     = 0x0A;      // column separator inside a row
constexpr uint8_t  CTRL_COL_B     = 0x05;      // column separator inside a row
constexpr size_t   TEXT_SCAN_MAX  = 4096;      // same cap message_reader uses for a whole message
constexpr int      MAX_OPTIONS    = 32;        // FUN_002b2ce0 bounds its walk at 0x20 slots

// The message currently on screen. Written by NotePage and read by OnFocus, both on the game
// thread, but under the lock so a mid-transition focus cannot see a half-copied buffer.
std::mutex           g_textMutex;
std::vector<uint8_t> g_text;
const uint8_t*       g_base = nullptr;     // the widget's text base this copy was taken from
bool                 g_queueNext = false;   // the first focus after a new message queues behind it
size_t               g_pageOff = 0;        // BYTE OFFSET of the page on screen (the widget's own cursor)
// Which DETECTOR owns the surface currently on screen. Two exist because neither covers both
// cases: the 0x8000 dispatch drives the notice board's navigation, and the per-frame tick is the
// only thing that sees an in-dialogue choice (it sends no message at all). They share a window --
// the Yes/No widget IS the board window's embedded list block -- so this cannot be decided by
// class. It is decided by OBSERVATION: if a 0x8000 arrives for this message/page, the dispatch
// covers it and the tick stands down. Reset on every new message and page, so the board's page 0
// and the petitioner Yes/No three pages later each get the right speaker.
bool                 g_dispatchCovers = false;

constexpr uint32_t ARG_STRIDE = 0x10;       // FUN_002ac5f0 case 0x2e: args + index*0x10

// What the per-frame tick last SPOKE, and the key its change-check compares against. Declared here
// rather than as function-local statics because they need a RESET EVENT (ForgetLastCursor) and a
// SEED (NotePage), both outside the tick. Game thread only -- the tick, NotePage and the reset all
// run on it.
//   g_lastCursor  the option-list row (mode 2)
//   g_lastValue   the numeric field's value (mode 4). -1 is unreachable for a real field -- a floor,
//                 a quantity and a price are all non-negative -- so it doubles as "nothing seeded".
void*   g_lastWidget = nullptr;
int16_t g_lastCursor = -1;
int32_t g_lastValue  = -1;


// A row's trailing `0F 2E <argIndex> 90` names the argument that carries its Status. Read the index
// out of the row itself rather than assuming it equals the row number -- the escape is the game's
// own statement of which argument belongs to this row, and it costs nothing to believe it.
bool RowArgIndex(const uint8_t* p, size_t len, uint32_t* outIdx) {
    for (size_t i = 0; i + 3 < len; ++i) {
        if (p[i] == 0x0F && p[i + 1] == 0x2E) { *outIdx = p[i + 2] & 0x7Fu; return true; }
    }
    return false;
}

// Resolve one substitution argument to text -- this is the Status column.
//
// THE TABLE IS INLINE AT window+0x1B8, and finding it took longer than it should have because
// Ghidra renders the call site with only four arguments:
//     thunk_FUN_002b32d0(dest, *(window+0x1A8), *(renderState+0x20), 0)
// FUN_002b32d0 actually takes NINE; params 5-9 go on the stack and show up as staged locals. Its
// body settles which one matters -- `*(param_1 + 0x70) = param_7`, and ctx+0x70 IS the ctx[0xE]
// that FUN_002ac5f0 `case 0x2e` indexes. Ordering those staged locals by address makes param_7
// `param_1 + 0x1b8`, taken BY ADDRESS. So window+0x1A8 was never the argument block (it is
// param_2), and +0x1B8 is not a pointer to the table -- it IS the table. Dereferencing it yielded
// 0x1, which is simply entry 0's type field.
//
// Entry layout, from `case 0x2e` (FUN_002ac5f0):
//     +0  int type   -- 1 or 2: a codec STRING; 0: a numeric substitution (the bill number)
//     +8  const uint8_t* codec, null-terminated (the branch strlen()s it before drawing)
constexpr uint32_t OFF_ARG_TABLE  = 0x1B8;   // INLINE table, not a pointer
constexpr int32_t  ARG_TYPE_STR_A = 1;
constexpr int32_t  ARG_TYPE_STR_B = 2;

std::wstring ResolveArg(void* window, uint32_t idx) {
    char* table = static_cast<char*>(window) + OFF_ARG_TABLE;
    const uint32_t entry = idx * ARG_STRIDE;

    int type = 0;
    if (!MemRead::SafeReadInt(table + entry, &type)) return std::wstring();
    if (type != ARG_TYPE_STR_A && type != ARG_TYPE_STR_B) {
        return std::wstring();   // numeric slot (the bill number), not a status word
    }
    const uint8_t* codec = static_cast<const uint8_t*>(MemRead::PtrAt(table, entry + 8));
    if (!codec) return std::wstring();
    std::wstring t = GameText::Decode(codec, 96);
    return GameText::IsMostlyPrintable(t) ? t : std::wstring();
}

// Replicates FUN_002b2ce0(listBlock, visibleIdx, total): map a VISIBLE row to its ABSOLUTE slot by
// skipping slots whose bit is set in the hidden mask. The game stores the result at window+0x124,
// but our dispatch hook runs BEFORE the original handler, so that field is still stale here --
// compute it rather than read a value the game has not written yet.
int AbsoluteIndex(void* window, int visibleIdx, int total) {
    if (total <= 0 || total > MAX_OPTIONS || visibleIdx < 0) return visibleIdx;
    uint32_t mask = 0;
    if (!MemRead::SafeReadU32(window, OFF_LIST_BLOCK + OFF_HIDE_MASK, &mask)) return visibleIdx;
    int visible = 0;
    for (int slot = 0; slot < total; ++slot) {
        if ((mask >> (slot & 0x1F)) & 1) continue;      // hidden slot -- not counted
        if (visible == visibleIdx) return slot;
        ++visible;
    }
    return visibleIdx;
}

// Decode one row, splitting on its COLUMN separators. 0x0A and 0x05 both decode to NOTHING (0x05 is
// deliberately excluded from GameText's space controls), so handing a whole entry to Decode runs the
// columns together -- "ThexteraI". This yields "Thextera, I" instead. The trailing 0x0F icon escape
// contributes no characters and simply drops out.
std::wstring DecodeRow(const uint8_t* p, size_t len) {
    std::wstring out;
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        const bool sep = (i == len) || (p[i] == CTRL_COL_A) || (p[i] == CTRL_COL_B);
        if (!sep) continue;
        if (i > start) {
            std::vector<uint8_t> col(p + start, p + i);
            col.push_back(0);                       // entries are NOT null-terminated
            std::wstring c = GameText::Decode(col.data(), col.size());
            if (!c.empty() && GameText::IsMostlyPrintable(c)) {
                if (!out.empty()) out += L", ";
                out += c;
            }
        }
        start = i + 1;
    }
    return out;
}

// Locate the 0x0E option block and return the codec bytes of option `slot`, or an empty span.
// Mirrors FUN_003ffdf0's header walk exactly -- see choice_reader.h for the layout.
//
// EVERY FALSE RETURN NAMES ITSELF. All seven used to report as "no 0x0E block", which is how a
// surface the reader had never met (the Draklor lift's numeric field) read as a parse failure for a
// whole session. `why` is a literal, so LogFail's pointer comparison still tells them apart.
bool OptionCodec(const std::vector<uint8_t>& buf, int slot, size_t pageOff,
                 const uint8_t** outPtr, size_t* outLen, const char** why) {
    *outPtr = nullptr; *outLen = 0; *why = "unknown";
    if (slot < 0) { *why = "negative slot"; return false; }

    // Start at the page the game says is on screen. Without this the search always returns the FIRST
    // block in the message -- the notice board's mark list -- even when the player is three pages
    // further on, looking at a Yes/No. `pageOff` is the widget's own byte cursor (widget+0x8A), so
    // this is the same position FUN_002a8c50 walks from and cannot drift out of step with the box.
    size_t m = pageOff;
    if (m >= buf.size()) { *why = "page offset past the end of the cached message"; return false; }

    // The question text and any column headers precede the marker, so it is never the first byte.
    // Stop at the NEXT page break: a page without a block has no options, and borrowing the next
    // page's would be worse than silence.
    while (m < buf.size() && buf[m] != CTRL_OPTIONS) {
        if (buf[m] == 0x00 || buf[m] == CTRL_PAGE) {
            *why = "no 0x0E block on this page (terminator or page break reached first)";
            return false;
        }
        ++m;
    }
    if (m + 5 >= buf.size()) { *why = "0x0E block header runs past the cached message"; return false; }

    const int count = buf[m + 1] & 0x7F;
    if (count <= 0 || count > MAX_OPTIONS) { *why = "0x0E count byte out of range"; return false; }
    if (slot >= count) { *why = "slot past the block's own count"; return false; }
    const uint8_t flags = buf[m + 4];

    // FUN_003ffdf0: p = m+4; if (flags & 1) p += ceil(count/7); then p += 1.
    size_t p = m + 4;
    if (flags & 1) p += static_cast<size_t>((count + 6) / 7);
    p += 1;

    // Length-prefixed entries: [len & 0x7F][len bytes]. A 0x00 length ends the REAL list, which is
    // shorter than `count` -- that byte is a capacity (32 on a board with ~10 bills).
    for (int i = 0; i <= slot; ++i) {
        if (p >= buf.size()) { *why = "entry walk ran past the cached message"; return false; }
        const size_t len = buf[p] & 0x7F;
        if (len == 0) { *why = "slot past the list terminator (count is a capacity)"; return false; }
        if (p + 1 + len > buf.size()) { *why = "entry length runs past the cached message"; return false; }
        if (i == slot) { *outPtr = &buf[p + 1]; *outLen = len; return true; }
        p += 1 + len;
    }
    *why = "entry walk ended before reaching the slot";
    return false;
}

// POD-only: copies the codec string into `dst` and returns how many bytes it got. Stops at the 0x00
// that terminates the whole string (FUN_003ffdf0's own walk ends there, and a codec byte is never
// 0x00), and KEEPS whatever it read if the tail faults -- `n` survives the __except, so a string
// near a page boundary still yields its options instead of nothing. Separate function because
// __try cannot live in one that needs object unwinding, and the caller owns a std::vector.
size_t CopyCodec(const uint8_t* p, uint8_t* dst, size_t cap) {
    size_t n = 0;
    __try {
        while (n < cap) {
            const uint8_t b = p[n];
            dst[n++] = b;
            if (b == 0x00) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* keep the prefix already copied */ }
    return n;
}

// THE SINGLE SPEECH CHOKE POINT for this reader. Both detectors end here, so the queue-vs-interrupt
// policy, the logging and the line format exist in exactly one place. An earlier build spoke from
// two sites with two different policies; they raced, and the plainer line won.
void EmitOption(void* logOwner, const char* kind, int idx, int count, const std::wstring& text) {
    bool queue = false;
    {
        std::lock_guard<std::mutex> lk(g_textMutex);
        queue = g_queueNext;
        g_queueNext = false;
    }
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "%s[%d/%d]: ", kind, idx, count);
    Log::WriteW("READER", hdr, logOwner, text);
    // QUEUE on the first option after a new message or page turn, so the telop's question
    // ("Which bill would you like to read?" / "Will you go and speak to the petitioner?") is not
    // cut off. Every later cursor move interrupts, so arrowing stays responsive.
    if (queue) Speech::SpeakQueued(text);
    else       Speech::Output(text, /*interrupt=*/true);
}

// Build the spoken line for one option: its columns, plus the substitution argument when the owning
// window carries an argument table. `window` may be null for a surface that has none.
//
// A ROW CAN BE NOTHING BUT ITS SUBSTITUTION, and that is what kept the gate-crystal teleport list
// silent. Its destination rows are literally `0F 2E <idx> 90` and nothing else -- four bytes, measured
// (probe_teleport_rows, 2026-07-30: entry len `0x84 & 0x7F` = 4, body `0f 2e 80 90`). DecodeRow yields
// nothing for an escape that contributes no characters, so the old `if (text.empty()) return text` bailed
// out one line BEFORE the argument that holds the place name was ever looked at. Row 25 spoke only
// because it is a literal "Cancel".
//
// So: resolve first, then decide. Empty text + a resolved argument means the argument IS the row. Text
// plus an argument keeps appending, which is the notice board's Status column and must not regress.
std::wstring BuildOptionLine(void* window, const uint8_t* codec, size_t len) {
    std::wstring text = DecodeRow(codec, len);
    if (!window) return text;
    uint32_t argIdx = 0;
    if (!RowArgIndex(codec, len, &argIdx)) return text;
    const std::wstring arg = ResolveArg(window, argIdx);
    if (arg.empty()) return text;
    if (text.empty()) return arg;              // the row IS the substitution (teleport destinations)
    return text + L", " + arg;                 // row text PLUS its argument (notice-board Status)
}

// How many option slots this list really has. The count lives on the widget the window embeds at
// +0xD0 -- the same OFF_W_COUNT field the per-frame tick reads at HookedChoiceTick, so this is not a
// new offset. OnFocus used to pass the CONSTANT MAX_OPTIONS as the total, which is why its log lines
// read `choice[25/32]` on a 26-row list and why the hide-mask walk scanned six slots past the
// terminator. Measured 26 on the gate-crystal list. MAX_OPTIONS stays the clamp and the fallback.
int OptionSlotCount(void* window) {
    uint8_t n = 0;
    if (!MemRead::SafeReadU8(window, OFF_LIST_BLOCK + OFF_W_COUNT, &n)) return MAX_OPTIONS;
    if (n == 0 || n > MAX_OPTIONS) return MAX_OPTIONS;
    return static_cast<int>(n);
}

// One line per distinct failure reason per window. A silent-if-wrong reader with NO diagnostic
// makes its own bug invisible -- which is exactly what happened on the first attempt here.
void LogFail(void* window, const char* why, int a, int b) {
    static const char* s_lastWhy = nullptr;
    static void*       s_lastWnd = nullptr;
    if (why == s_lastWhy && window == s_lastWnd) return;   // transition detector, not a speech dedup
    s_lastWhy = why; s_lastWnd = window;
    char m[192];
    snprintf(m, sizeof(m), "choice SILENT (%s) wnd=%p a=%d b=%d", why, window, a, b);
    Log::Write("READER", m);
}

// The numeric field's CONFIGURATION, read the way FUN_002b35a0 wrote it. Game thread only.
struct NumericField {
    bool    candidates = false;   // widget+0xB0 bit 28: a list of values, not a free range
    int     count      = 0;
    int32_t value[MAX_CANDIDATES] = {};
    int32_t lo = 0, hi = 0;       // range flavour
    int     index = -1;           // widget+0x58 bits 12-17
    bool    valid = false;
};
NumericField g_field;

// One argument-table entry's integer. The table is INLINE at window+0x1B8, stride 0x10, the value at
// +8 -- the same layout ResolveArg already walks for the string slots.
bool ArgValue(void* window, uint32_t slot, int32_t* out) {
    return MemRead::SafeReadU32(window, OFF_ARG_TABLE + slot * ARG_STRIDE + ARG_VALUE,
                                reinterpret_cast<uint32_t*>(out));
}

// Read the whole field, not the number it happens to be showing. Mirrors FUN_002b35a0's own reads.
bool ReadNumericField(void* widget, NumericField* f) {
    *f = NumericField{};
    void* window = static_cast<char*>(widget) - OFF_LIST_BLOCK;
    if (!IsChoiceWindow(window)) return false;   // never read fields off an unvalidated object

    uint32_t state = 0, packed = 0;
    if (!MemRead::SafeReadU32(widget, OFF_W_STATE, &state)) return false;
    if (!MemRead::SafeReadU32(widget, OFF_W_CURSOR, &packed)) return false;
    f->candidates = (state & FLAG_CANDIDATES) != 0;

    if (f->candidates) {
        int32_t n = 0;
        if (!ArgValue(window, SLOT_COUNT, &n) || n < 1) return false;
        if (n > MAX_CANDIDATES) n = MAX_CANDIDATES;
        for (int i = 0; i < n; ++i) {
            if (!ArgValue(window, static_cast<uint32_t>(i), &f->value[i])) return false;
        }
        f->count = n;
        f->index = static_cast<int>((packed >> 12) & 0x3F);
        if (f->index >= n) f->index = n - 1;    // the same clamp FUN_002b35a0 applies
    } else {
        // The BOUND SLOTS ARE NAMED BY THE CURSOR WORD, not fixed at 30 and 31 -- the function swaps
        // which is which when slot 30 holds the larger number. Read the slot it points at.
        if (!ArgValue(window, packed & 0x3Fu, &f->lo)) return false;
        if (!ArgValue(window, (packed >> 6) & 0x3Fu, &f->hi)) return false;
    }
    f->valid = true;
    return true;
}

// The reader's own line, once per prompt -- what this menu HOLDS, not a hunt for where it lives.
// (This replaced a discovery instrument that dumped argument slots 28-31 and 64 bytes of page hex.
// Those four slots are the CONFIGURATION; the destinations are slots 0..count-1, which it never
// logged -- so it could not have answered the question it was shipped to answer. The decompile
// states the layout outright and should have been read first.)
void LogNumericField(void* widget, const NumericField& f, int32_t live, uint8_t digits) {
    char m[512];
    int k = snprintf(m, sizeof(m), "numeric field: wnd=%p %s", widget,
                     f.candidates ? "candidates" : "range");
    if (!f.valid) {
        snprintf(m + k, sizeof(m) - k, " -- UNREADABLE (window failed its class check, or the"
                 " configuration slots faulted); falling back to the live value alone");
    } else if (f.candidates) {
        for (int i = 0; i < f.count && k > 0 && static_cast<size_t>(k) < sizeof(m); ++i) {
            k += snprintf(m + k, sizeof(m) - k, "%s%d%s", i ? ", " : " ",
                          static_cast<int>(f.value[i]), i == f.index ? "*" : "");
        }
        k += snprintf(m + k, sizeof(m) - k, " (index %d of %d)", f.index, f.count);
    } else {
        k += snprintf(m + k, sizeof(m) - k, " %d..%d", static_cast<int>(f.lo),
                      static_cast<int>(f.hi));
    }
    if (static_cast<size_t>(k) < sizeof(m)) {
        // THE CONTROL: in candidate mode the game's own +0x54 must equal the slot the index names.
        // If it ever does not, the model is wrong and this says so instead of quietly speaking on.
        const bool agree = !f.valid || !f.candidates ||
                           (f.index >= 0 && f.index < f.count && f.value[f.index] == live);
        snprintf(m + k, sizeof(m) - k, " | live=%d digits=%u%s", static_cast<int>(live), digits,
                 agree ? "" : "  <-- +0x54 DISAGREES WITH THE SLOT THE INDEX NAMES");
    }
    Log::Write("READER", m);
}

} // namespace

void NotePage(void* widget, const uint8_t* base, size_t byteOffset, bool numericField,
              int32_t value) {
    {
        std::lock_guard<std::mutex> lk(g_textMutex);
        if (base != g_base) {                       // a different message: re-snapshot it
            g_base = base;
            g_text.assign(TEXT_SCAN_MAX, 0);
            const size_t n = base ? CopyCodec(base, g_text.data(), TEXT_SCAN_MAX) : 0;
            g_text.resize(n);
        }
        g_pageOff = byteOffset;
        g_dispatchCovers = false;   // a new page may be driven by the other detector
        // The first option focus lands right after this and must not cut the page off -- the notice
        // board's question and the petitioner Yes/No both arrive as page text, spoken a moment
        // earlier.
        g_queueNext = true;
    }

    // SEED THE NUMERIC TICK, so the number the page line is about to speak is not spoken twice. The
    // caller is DialogueReader on the game thread, which is the thread the tick runs on, so this
    // lands before any tick that could use it -- no lock, no one-shot flag, no filter. Every LATER
    // value differs from the seed and is spoken, including a move back to the starting floor.
    if (numericField) {
        g_lastWidget = widget;
        g_lastValue  = value;
        g_lastCursor = -1;          // the row cursor means nothing in this mode

        // READ THE MENU, not just the number it is showing. This is the prompt-open event, so it
        // is where the destination list is established -- once, from the game's own argument
        // table, exactly as FUN_002b35a0 built it.
        uint8_t digits = 0;
        MemRead::SafeReadU8(widget, OFF_W_DIGITS, &digits);
        ReadNumericField(widget, &g_field);
        LogNumericField(widget, g_field, value, digits);
    }
}

bool OnFocus(void* window, int visibleIndex) {
    if (!window) return false;
    STALL_SCOPE("ChoiceReader::OnFocus");

    std::vector<uint8_t> buf;
    size_t pageOff = 0;
    {
        std::lock_guard<std::mutex> lk(g_textMutex);
        if (g_text.empty()) { LogFail(window, "no message text cached", 0, visibleIndex); return false; }
        buf     = g_text;
        pageOff = g_pageOff;
    }

    const int total = OptionSlotCount(window);
    const int slot = AbsoluteIndex(window, visibleIndex, total);
    const uint8_t* codec = nullptr;
    size_t len = 0;
    const char* why = nullptr;
    if (!OptionCodec(buf, slot, pageOff, &codec, &len, &why)) {
        LogFail(window, why, slot, static_cast<int>(pageOff));
        return false;
    }
    const std::wstring text = BuildOptionLine(window, codec, len);
    if (text.empty()) { LogFail(window, "row decoded empty", slot, visibleIndex); return false; }

    // A 0x8000 reached us: this surface is dispatch-driven, so the tick must not double-speak it.
    //
    // This line sits AFTER the empty-text return above, so on the teleport list it had never executed --
    // every destination row bailed first and the two-detector arbitration never once ran on that
    // surface. Nothing here needed changing; resolving the substitution is what reaches it.
    { std::lock_guard<std::mutex> lk(g_textMutex); g_dispatchCovers = true; }
    EmitOption(window, "choice", visibleIndex, total, text);
    return true;
}

bool IsChoiceWindow(void* owner) {
    return owner && Obj0(owner) == Hooks::ResolveRva(RVA_CHOICE_WND);
}

namespace {

// THE NUMERIC BRANCH of the tick. `widget+0x54` is the value the player is choosing, and
// FUN_002a9980 writes it in BOTH flavours of the field -- a free range and a pick-from-candidates
// list -- so this needs no index of its own and never touches the packed spinner state. A TRANSITION
// DETECTOR on the game's own value, guarding the per-frame FUN_002a9980: the same sanctioned
// exception the option branch below uses, and for the same reason.
//
// The baseline is SEEDED by NotePage with the value the page line already spoke, so opening the
// prompt says the whole sentence once and each move says the new number. Nothing is suppressed --
// a move back to the STARTING floor differs from the value last spoken, so it speaks.
void TickNumericField(void* widget) {
    int32_t value = -1;
    if (!MemRead::SafeReadU32(widget, OFF_W_VALUE, reinterpret_cast<uint32_t*>(&value))) return;
    if (value < 0) return;   // read before the stepper configured the field; not a number to say

    if (widget == g_lastWidget && value == g_lastValue) return;   // nothing moved
    g_lastWidget = widget; g_lastValue = value;
    // Drop the OPTION-LIST key as well. The two branches share `g_lastWidget`, and a widget that
    // has been a numeric field must not carry a stale row key back into mode 2 -- re-entering a
    // surface has to announce. -1 is not a reachable row, so this can only ever ADD speech.
    g_lastCursor = -1;

    STALL_SCOPE("ChoiceReader::HookedChoiceTick");

    uint8_t digits = 0;
    MemRead::SafeReadU8(widget, OFF_W_DIGITS, &digits);
    // THE DIGITS ARE THE GAME'S -- nothing is composed here. Speaking the bare number is what every
    // other value row in the mod does (config_reader's volume steps); the words around it, "Select
    // destination:" and the trailing F, were spoken with the page and are not repeated per step.
    // The log header reads `numeric-field[<value>/<digits>]`.
    EmitOption(widget, "numeric-field", value, static_cast<int>(digits), std::to_wstring(value));
}

} // namespace

// FUN_002a9980(widget): the mid-dialogue choice state machine. PER-FRAME, so it is guarded by a
// change-check on the cursor -- the sanctioned form of that exception (CLAUDE.md names
// battle_target_reader's g_lastHandle guarding FUN_002bfd20 as the model). This is a TRANSITION
// detector on `widget+0x58`, not a speech dedup.
//
// WHAT THIS COMMENT USED TO CLAIM, AND WHY IT WAS WRONG (S126): "re-entering the prompt
// re-announces, because the widget is rebuilt and the remembered cursor no longer matches." Both
// halves are assumptions about pointer identity, and menu_reader.cpp:102 has recorded since S51 that
// THE ENGINE RECYCLES THESE ADDRESSES. A prompt torn down and re-opened can land on the same widget
// address with its cursor back at the same starting index -- key equal, `return`, silent. Same shape
// as the shop container and the dialogue page key, both fixed in the same session.
//
// So the guard now has a real re-arm: ForgetLastCursor, called on the game's own end-of-message
// latch and when a list screen opens over the box. Nothing was made quieter; a stale key was stopped
// from outliving the prompt it belonged to.
uint64_t HookedChoiceTick(void* widget) {
    const uint64_t ret = s_origChoiceTick ? s_origChoiceTick(widget) : 0;
    if (!widget) return ret;

    // Stand down if the 0x8000 dispatch is covering this message/page -- it is the authoritative
    // detector where it fires, and it knows the real navigation index.
    { std::lock_guard<std::mutex> lk(g_textMutex); if (g_dispatchCovers) return ret; }

    // ASK THE WIDGET WHICH SELECTION IT IS RUNNING before reading a single field. FUN_002a9980 is
    // the state machine for both modes and the fields it uses are the SAME BYTES with different
    // meanings, so reading them without this is how the Draklor lift's digit width (2) was taken for
    // a two-option list and its packed spinner state for a row cursor. See choice_reader.h.
    uint8_t mode = 0;
    if (!MemRead::SafeReadU8(widget, OFF_W_STATE, &mode)) return ret;
    if (mode == MODE_NUMERIC) { TickNumericField(widget); return ret; }

    int16_t cursor = 0; uint8_t count = 0;
    if (!MemRead::SafeReadU16(widget, OFF_W_CURSOR, reinterpret_cast<uint16_t*>(&cursor))) return ret;
    if (!MemRead::SafeReadU8(widget, OFF_W_COUNT, &count) || count == 0) return ret;

    if (widget == g_lastWidget && cursor == g_lastCursor) return ret;   // nothing moved
    g_lastWidget = widget; g_lastCursor = cursor;

    STALL_SCOPE("ChoiceReader::HookedChoiceTick");

    // Start at the widget's OWN page offset rather than counting 0x03 breaks -- FUN_002a8c50 walks
    // from exactly here, so it cannot drift out of step with what is on screen.
    const uint8_t* base = static_cast<const uint8_t*>(MemRead::PtrAt(widget, OFF_W_TEXT));
    uint16_t off = 0;
    if (!base || !MemRead::SafeReadU16(widget, OFF_W_OFFSET, &off)) return ret;

    std::vector<uint8_t> buf(TEXT_SCAN_MAX, 0);
    const size_t n = CopyCodec(base + off, buf.data(), TEXT_SCAN_MAX);
    if (n == 0) return ret;
    buf.resize(n);

    const uint8_t* codec = nullptr; size_t len = 0;
    const char* why = nullptr;
    if (!OptionCodec(buf, cursor, /*pageOff=*/0, &codec, &len, &why)) {  // buf starts at the page
        LogFail(widget, why, cursor, count);
        return ret;
    }
    void* window = static_cast<char*>(widget) - OFF_LIST_BLOCK;
    if (!IsChoiceWindow(window)) window = nullptr;   // a widget not embedded in one has no arg table
    const std::wstring text = BuildOptionLine(window, codec, len);
    if (text.empty()) return ret;
    EmitOption(widget, "dialogue-choice", cursor, count, text);
    return ret;
}

void ForgetLastCursor() {
    g_lastWidget = nullptr;
    g_lastCursor = -1;
    g_lastValue  = -1;   // and the numeric field's, for the same reason: the widget address recycles
}

bool Init() {
    const bool ok = Hooks::InstallTyped(RVA_CHOICE_TICK, &HookedChoiceTick, &s_origChoiceTick);
    Log::Write("READER", ok
        ? "ChoiceReader: mid-dialogue choice tick hooked (FUN_002a9980)"
        : "ChoiceReader: FUN_002a9980 hook FAILED -- in-dialogue choices stay silent");
    return ok;
}

void Shutdown() { Hooks::Uninstall(RVA_CHOICE_TICK); ForgetLastCursor(); }

} // namespace ChoiceReader

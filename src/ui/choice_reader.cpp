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
constexpr uint32_t OFF_W_COUNT  = 0xA2;        // widget+0xA2 = u8 option count

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
bool OptionCodec(const std::vector<uint8_t>& buf, int slot, size_t pageOff,
                 const uint8_t** outPtr, size_t* outLen) {
    *outPtr = nullptr; *outLen = 0;
    if (slot < 0) return false;

    // Start at the page the game says is on screen. Without this the search always returns the FIRST
    // block in the message -- the notice board's mark list -- even when the player is three pages
    // further on, looking at a Yes/No. `pageOff` is the widget's own byte cursor (widget+0x8A), so
    // this is the same position FUN_002a8c50 walks from and cannot drift out of step with the box.
    size_t m = pageOff;
    if (m >= buf.size()) return false;

    // The question text and any column headers precede the marker, so it is never the first byte.
    // Stop at the NEXT page break: a page without a block has no options, and borrowing the next
    // page's would be worse than silence.
    while (m < buf.size() && buf[m] != CTRL_OPTIONS) {
        if (buf[m] == 0x00 || buf[m] == CTRL_PAGE) return false;
        ++m;
    }
    if (m + 5 >= buf.size()) return false;

    const int count = buf[m + 1] & 0x7F;
    if (count <= 0 || count > MAX_OPTIONS || slot >= count) return false;
    const uint8_t flags = buf[m + 4];

    // FUN_003ffdf0: p = m+4; if (flags & 1) p += ceil(count/7); then p += 1.
    size_t p = m + 4;
    if (flags & 1) p += static_cast<size_t>((count + 6) / 7);
    p += 1;

    // Length-prefixed entries: [len & 0x7F][len bytes]. A 0x00 length ends the REAL list, which is
    // shorter than `count` -- that byte is a capacity (32 on a board with ~10 bills).
    for (int i = 0; i <= slot; ++i) {
        if (p >= buf.size()) return false;
        const size_t len = buf[p] & 0x7F;
        if (len == 0) return false;                    // past the last real entry
        if (p + 1 + len > buf.size()) return false;
        if (i == slot) { *outPtr = &buf[p + 1]; *outLen = len; return true; }
        p += 1 + len;
    }
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

// Build the spoken line for one option: its columns, plus the Status substitution when the owning
// window carries an argument table. `window` may be null for a surface that has none.
std::wstring BuildOptionLine(void* window, const uint8_t* codec, size_t len) {
    std::wstring text = DecodeRow(codec, len);
    if (text.empty() || !window) return text;
    uint32_t argIdx = 0;
    if (RowArgIndex(codec, len, &argIdx)) {
        const std::wstring status = ResolveArg(window, argIdx);
        if (!status.empty()) text += L", " + status;
    }
    return text;
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

} // namespace

void NotePage(const uint8_t* base, size_t byteOffset) {
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
    // board's question and the petitioner Yes/No both arrive as page text, spoken a moment earlier.
    g_queueNext = true;
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

    const int slot = AbsoluteIndex(window, visibleIndex, MAX_OPTIONS);
    const uint8_t* codec = nullptr;
    size_t len = 0;
    if (!OptionCodec(buf, slot, pageOff, &codec, &len)) {
        LogFail(window, "no 0x0E block on this page / slot past the list terminator",
                slot, static_cast<int>(pageOff));
        return false;
    }
    const std::wstring text = BuildOptionLine(window, codec, len);
    if (text.empty()) { LogFail(window, "row decoded empty", slot, visibleIndex); return false; }

    // A 0x8000 reached us: this surface is dispatch-driven, so the tick must not double-speak it.
    { std::lock_guard<std::mutex> lk(g_textMutex); g_dispatchCovers = true; }
    EmitOption(window, "choice", visibleIndex, static_cast<int>(MAX_OPTIONS), text);
    return true;
}

bool IsChoiceWindow(void* owner) {
    return owner && Obj0(owner) == Hooks::ResolveRva(RVA_CHOICE_WND);
}

// FUN_002a9980(widget): the mid-dialogue choice state machine. PER-FRAME, so it is guarded by a
// change-check on the cursor -- the sanctioned form of that exception (CLAUDE.md names
// battle_target_reader's g_lastHandle guarding FUN_002bfd20 as the model). This is a TRANSITION
// detector on `widget+0x58`, not a speech dedup: re-entering the prompt re-announces, because the
// widget is rebuilt and the remembered cursor no longer matches.
uint64_t HookedChoiceTick(void* widget) {
    const uint64_t ret = s_origChoiceTick ? s_origChoiceTick(widget) : 0;
    if (!widget) return ret;

    // Stand down if the 0x8000 dispatch is covering this message/page -- it is the authoritative
    // detector where it fires, and it knows the real navigation index.
    { std::lock_guard<std::mutex> lk(g_textMutex); if (g_dispatchCovers) return ret; }

    int16_t cursor = 0; uint8_t count = 0;
    if (!MemRead::SafeReadU16(widget, OFF_W_CURSOR, reinterpret_cast<uint16_t*>(&cursor))) return ret;
    if (!MemRead::SafeReadU8(widget, OFF_W_COUNT, &count) || count == 0) return ret;

    static void*   s_lastWidget = nullptr;
    static int16_t s_lastCursor = -1;
    if (widget == s_lastWidget && cursor == s_lastCursor) return ret;   // nothing moved
    s_lastWidget = widget; s_lastCursor = cursor;

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
    if (!OptionCodec(buf, cursor, /*pageOff=*/0, &codec, &len)) {   // buf already starts at the page
        LogFail(widget, "choice widget: no 0x0E block at the widget's own offset", cursor, count);
        return ret;
    }
    void* window = static_cast<char*>(widget) - OFF_LIST_BLOCK;
    if (!IsChoiceWindow(window)) window = nullptr;   // a widget not embedded in one has no arg table
    const std::wstring text = BuildOptionLine(window, codec, len);
    if (text.empty()) return ret;
    EmitOption(widget, "dialogue-choice", cursor, count, text);
    return ret;
}

bool Init() {
    const bool ok = Hooks::InstallTyped(RVA_CHOICE_TICK, &HookedChoiceTick, &s_origChoiceTick);
    Log::Write("READER", ok
        ? "ChoiceReader: mid-dialogue choice tick hooked (FUN_002a9980)"
        : "ChoiceReader: FUN_002a9980 hook FAILED -- in-dialogue choices stay silent");
    return ok;
}

void Shutdown() { Hooks::Uninstall(RVA_CHOICE_TICK); }

} // namespace ChoiceReader

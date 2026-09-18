#include "ui/choice_reader.h"

#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "ui/choice_block.h"
#include "speech/speech.h"

#include <Windows.h>
#include <cstdio>
#include <vector>

namespace ChoiceReader {
namespace {

using MemRead::Obj0;
using ChoiceBlock::CTRL_COL_A;
using ChoiceBlock::CTRL_COL_B;
using ChoiceBlock::MAX_OPTIONS;

// ---- RVAs / offsets (abs = RVA + 0x120000) ----------------------------------------------------
constexpr uint32_t RVA_CHOICE_WND = 0x186190;  // FUN_002a6190 -- field dialogue / choice window
constexpr uint32_t RVA_CHOICE_TICK = 0x189980; // FUN_002a9980(widget) -- mid-dialogue choice tick
constexpr uint32_t OFF_W_TEXT   = 0x28;        // widget+0x28 = message text base
constexpr uint32_t OFF_W_OFFSET = 0x8A;        // widget+0x8A = u16 byte offset (current page)
constexpr uint32_t OFF_W_CURSOR = 0x58;        // widget+0x58 = mode 4's packed spinner state
constexpr uint32_t OFF_W_COUNT  = 0xA2;        // widget+0xA2 = u8 option count (mode 2)

// ---- the widget's selection modes (see choice_reader.h) ----------------------------------------
constexpr uint32_t OFF_W_STATE  = 0xB0;        // widget+0xB0 = state word; low byte = mode
constexpr uint32_t OFF_W_VALUE  = 0x54;        // widget+0x54 = i32 -- THE SELECTION, modes 2 and 4
constexpr uint32_t OFF_W_DIGITS = 0xA1;        // widget+0xA1 = u8 digit count (mode 4)
constexpr uint8_t  MODE_OPTIONS = 2;           // the 0x0E option list
constexpr uint8_t  MODE_NUMERIC = 4;           // the `0F 2D` numeric field
constexpr uint32_t FLAG_CANDIDATES = 0x10000000u;  // state word bit 28: pick-from-list, not a range

// STATE WORD BIT 22 IS THE FLAVOUR OF THE OPTION LIST, and it is why this reader used to need two
// detectors. FUN_002a5590 lays the 0x0E block out in one of two ways:
//
//   clear -- INLINE. FUN_002a9980 runs the cursor itself and sends no message at all.
//   set   -- a CHILD LIST WINDOW at window+0xC0. FUN_002a5590 builds it and then sets this bit;
//            FUN_002a9980's mode-2 path tests the bit and returns immediately, so widget+0x58 never
//            moves. The child owns the highlight and announces it as a FUN_00247510 msg 0x8000.
//
// The bit lives at window+0x180, which IS widget+0xB0 (0xD0 + 0xB0). Both flavours nevertheless end
// at the SAME field through the SAME helper -- see the note on HookedChoiceTick -- which is what
// lets one detector read both. Read here only to name the flavour in the log.
constexpr uint32_t FLAG_CHILD_LIST = 0x00400000u;

// WHERE THE NUMERIC MENU'S OWN DATA LIVES. FUN_002b35a0 (RVA 0x1935A0) configures the field out of
// the window's inline argument table and nowhere else, so these four slots ARE the menu:
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
constexpr uint32_t SLOT_COUNT     = 29;
constexpr int      MAX_CANDIDATES = 28;   // slots 0..27; 28-31 are the configuration above

typedef uint64_t (*Pfn_ChoiceTick)(void*);
Pfn_ChoiceTick s_origChoiceTick = nullptr;

constexpr uint32_t OFF_LIST_BLOCK = 0x0D0;     // window+0x0D0 = the embedded text/list widget
constexpr size_t   TEXT_SCAN_MAX  = 4096;      // same cap message_reader uses for a whole message

// The first option after a new message or page turn QUEUES behind the page text; every later move
// interrupts. Game thread only -- NotePage, the tick and the reset all run on it, which is why this
// needs no lock. (The cached-message snapshot that did need one is gone; the parse reads the page
// live off the widget instead.)
bool g_queueNext = false;

constexpr uint32_t ARG_STRIDE = 0x10;       // FUN_002ac5f0 case 0x2e: args + index*0x10

// WHAT THE TICK LAST SPOKE -- one key for both modes, because both now read the same field.
// Declared here rather than as function-local statics because they need a RESET EVENT
// (ForgetLastCursor) and a SEED (NotePage), both outside the tick.
//   g_lastMode   the selection mode the key was taken in. IN THE KEY because +0x54 means different
//                things per mode: a widget address that recycles from a numeric field into an option
//                list must not match on the number it used to hold.
//   g_lastValue  widget+0x54 -- the option SLOT in mode 2, the VALUE in mode 4. -1 is unreachable
//                for either, so it doubles as "nothing seeded".
void*   g_lastWidget = nullptr;
uint8_t g_lastMode   = 0xFF;
int32_t g_lastValue  = -1;

// One shared literal, so LogFail's pointer-identity dedup keeps this reason distinct from the
// parse's own set rather than merging with whichever string the linker happened to pool it with.
constexpr const char* kWhyRowEmpty = "row decoded empty";

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
// Ghidra renders the call site with only four arguments while FUN_002b32d0 actually takes NINE:
// params 5-9 go on the stack and show up as staged locals. Ordering those staged locals by address
// makes param_7 `param_1 + 0x1b8`, taken BY ADDRESS. So window+0x1A8 was never the argument block
// (it is param_2), and +0x1B8 is not a pointer to the table -- it IS the table. Dereferencing it
// yielded 0x1, which is simply entry 0's type field.
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

// Decode one row, splitting on its COLUMN separators. 0x0A and 0x05 both decode to NOTHING (0x05 is
// deliberately excluded from GameText's space controls), so handing a whole entry to Decode runs the
// columns together -- "ThexteraI". This yields "Thextera, I" instead. The trailing 0x0F icon escape
// contributes no characters and simply drops out.
// A ROW THAT IS ONLY A NUMBER IS STILL A ROW, and `IsMostlyPrintable` cannot say so: it ends in
// `alpha >= 1 && printable >= 60%`, and a quantity like "10" has no letter in it. That letter
// requirement is load-bearing where the predicate is used to reject binary rubbish read out of a
// stale pointer (about thirty call sites), so it is not touched -- this widens the gate HERE only.
//
// **THIS IS WHAT SILENCED THE PHAROS SUBTERRA ORB PEDESTAL** (S190, log 2026-09-18 06:49:45.812:
// `choice SILENT (row decoded empty) a=0 b=4`). Its three quantity rows decode to bare numbers and
// were judged garbage; row 3 spoke only because "Cancel" is a word. Any numeric option list has the
// same defect, so the fix belongs at the gate rather than at the pedestal.
//
// Digits only, and at least one of them: binary rubbish does not decode to an unbroken run of
// digits and spaces, so nothing this rejected before is admitted now.
bool IsNumericRow(const std::wstring& s) {
    bool digit = false;
    for (wchar_t ch : s) {
        if (ch >= L'0' && ch <= L'9') { digit = true; continue; }
        if (ch == L' ' || ch == L',' || ch == L'.') continue;
        return false;
    }
    return digit;
}

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
            if (!c.empty() && (GameText::IsMostlyPrintable(c) || IsNumericRow(c))) {
                if (!out.empty()) out += L", ";
                out += c;
            }
        }
        start = i + 1;
    }
    return out;
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

// THE SINGLE SPEECH POINT for this reader. One detector feeds it now, but the queue-vs-interrupt
// policy, the logging and the line format still live in exactly one place.
void EmitOption(void* logOwner, const char* kind, int idx, int count, const std::wstring& text,
                const char* extra = nullptr) {
    const bool queue = g_queueNext;
    g_queueNext = false;

    char hdr[96];
    snprintf(hdr, sizeof(hdr), "%s[%d/%d]%s: ", kind, idx, count, extra ? extra : "");
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

// THE NUMERIC BRANCH. `widget+0x54` is the value the player is choosing, and FUN_002a9980 writes it
// in BOTH flavours of the field -- a free range and a pick-from-candidates list -- so this needs no
// index of its own and never touches the packed spinner state.
//
// The baseline is SEEDED by NotePage with the value the page line already spoke, so opening the
// prompt says the whole sentence once and each move says the new number. Nothing is suppressed --
// a move back to the STARTING floor differs from the value last spoken, so it speaks.
void SpeakNumericValue(void* widget, int32_t value) {
    uint8_t digits = 0;
    MemRead::SafeReadU8(widget, OFF_W_DIGITS, &digits);
    // THE DIGITS ARE THE GAME'S -- nothing is composed here. Speaking the bare number is what every
    // other value row in the mod does (config_reader's volume steps); the words around it, "Select
    // destination:" and the trailing F, were spoken with the page and are not repeated per step.
    // The log header reads `numeric-field[<value>/<digits>]`.
    EmitOption(widget, "numeric-field", value, static_cast<int>(digits), std::to_wstring(value));
}

} // namespace

void NotePage(void* widget, const uint8_t* base, size_t byteOffset, bool numericField,
              int32_t value) {
    // `base` / `byteOffset` are no longer cached: the parse reads the page LIVE off the widget, so
    // it cannot be left pointing at a page the box has already turned. See choice_reader.h.
    (void)base;
    (void)byteOffset;

    // The first option focus lands right after this and must not cut the page off -- the notice
    // board's question and the petitioner Yes/No both arrive as page text, spoken a moment earlier.
    g_queueNext = true;

    // SEED THE NUMERIC TICK, so the number the page line is about to speak is not spoken twice. The
    // caller is DialogueReader on the game thread, which is the thread the tick runs on, so this
    // lands before any tick that could use it -- no lock, no one-shot flag, no filter. Every LATER
    // value differs from the seed and is spoken, including a move back to the starting floor.
    if (numericField) {
        g_lastWidget = widget;
        g_lastMode   = MODE_NUMERIC;
        g_lastValue  = value;

        // READ THE MENU, not just the number it is showing. This is the prompt-open event, so it
        // is where the destination list is established -- once, from the game's own argument
        // table, exactly as FUN_002b35a0 built it.
        uint8_t digits = 0;
        MemRead::SafeReadU8(widget, OFF_W_DIGITS, &digits);
        ReadNumericField(widget, &g_field);
        LogNumericField(widget, g_field, value, digits);
    }
}

bool IsChoiceWindow(void* owner) {
    return owner && Obj0(owner) == Hooks::ResolveRva(RVA_CHOICE_WND);
}

// FUN_002a9980(widget): the mid-dialogue selection state machine, and now this reader's ONLY
// detector. PER-FRAME, so it is guarded by a change-check on the game's own selection field -- the
// sanctioned form of that exception (CLAUDE.md names battle_target_reader's g_lastHandle guarding
// FUN_002bfd20 as the model). This is a TRANSITION detector on `widget+0x54`, not a speech dedup.
//
// WHY +0x54 AND NOT THE ROW CURSOR +0x58. There are two flavours of option list (see
// FLAG_CHILD_LIST above) and +0x58 only moves in one of them: when FUN_002a5590 builds a child list
// window it sets bit 22, and this very function tests that bit and returns before touching the
// cursor. A reader keyed on +0x58 therefore sees such a prompt open and then nothing, for ever --
// which is exactly the Archades "Commit this tale to memory." prompt, where the opening option
// spoke and no highlight ever did.
//
// BOTH flavours resolve the highlight through FUN_002b2ce0 (the hidden-slot walk) and store the
// result in the SAME PLACE: this function writes `+0x54` at the end of every inline tick, and the
// window handler FUN_002a6190 writes `window+0x124` on each 0x8000 -- and window+0x124 IS
// widget+0x54 (0xD0 + 0x54). One field, already resolved by the game, covers both. That is what
// retired the second detector: the 0x8000 path, the cached message it scanned, the stand-down flag
// that arbitrated the two, and our own re-implementation of FUN_002b2ce0 are all gone with it.
//
// THE MODE GATE IS LOAD-BEARING. +0x54 means different things per mode -- in mode 0/5 it is the
// park reason, and `3` (a page break) would read as "option 3" every time a box turned a page.
// Only modes 2 and 4 are selections.
//
// The guard's re-arm is ForgetLastCursor, called on the game's own end-of-message latch and when a
// list screen opens over the box. A prompt torn down and re-opened can land on the same widget
// address with its selection back at the same slot (menu_reader.cpp:102 -- THE ENGINE RECYCLES
// THESE ADDRESSES), so without that re-arm the key would match and the re-opened prompt stay silent.
uint64_t HookedChoiceTick(void* widget) {
    // AT THE TOP, so the [PERF] block counts CALLS. It used to sit below the change-check, which
    // made that counter measure emissions instead -- and left the corpus unable to answer how often
    // this function runs while a child-list prompt is up, the one question the redesign turns on.
    STALL_SCOPE("ChoiceReader::HookedChoiceTick");

    const uint64_t ret = s_origChoiceTick ? s_origChoiceTick(widget) : 0;
    if (!widget) return ret;

    uint32_t state = 0;
    if (!MemRead::SafeReadU32(widget, OFF_W_STATE, &state)) return ret;
    const uint8_t mode = static_cast<uint8_t>(state & 0xFF);
    if (mode != MODE_OPTIONS && mode != MODE_NUMERIC) return ret;

    int32_t value = -1;
    if (!MemRead::SafeReadU32(widget, OFF_W_VALUE, reinterpret_cast<uint32_t*>(&value))) return ret;
    if (widget == g_lastWidget && mode == g_lastMode && value == g_lastValue) return ret;

    if (mode == MODE_NUMERIC) {
        if (value < 0) return ret;   // read before the stepper configured the field; not a number,
                                     // and not a key worth taking either
        g_lastWidget = widget; g_lastMode = mode; g_lastValue = value;
        SpeakNumericValue(widget, value);
        return ret;
    }

    uint8_t count = 0;
    if (!MemRead::SafeReadU8(widget, OFF_W_COUNT, &count) || count == 0) return ret;
    if (value < 0 || value >= MAX_OPTIONS) return ret;   // not a slot any block could hold
    g_lastWidget = widget; g_lastMode = mode; g_lastValue = value;

    // Start at the widget's OWN page offset rather than counting 0x03 breaks -- FUN_002a8c50 walks
    // from exactly here, so it cannot drift out of step with what is on screen. Reading it live is
    // also what makes this detector independent of whether DialogueReader had anything to SAY about
    // the page: an options-only page decodes to no text at all, and the cache this used to consult
    // was never told such a page had turned.
    const uint8_t* base = static_cast<const uint8_t*>(MemRead::PtrAt(widget, OFF_W_TEXT));
    uint16_t off = 0;
    if (!base || !MemRead::SafeReadU16(widget, OFF_W_OFFSET, &off)) return ret;

    std::vector<uint8_t> buf(TEXT_SCAN_MAX, 0);
    const size_t n = CopyCodec(base + off, buf.data(), TEXT_SCAN_MAX);
    if (n == 0) return ret;
    buf.resize(n);

    const uint8_t* codec = nullptr; size_t len = 0;
    const char* why = nullptr;
    if (!ChoiceBlock::OptionCodec(buf, value, /*pageOff=*/0, &codec, &len, &why)) {
        LogFail(widget, why, value, count);   // buf starts at the page
        return ret;
    }
    void* window = static_cast<char*>(widget) - OFF_LIST_BLOCK;
    if (!IsChoiceWindow(window)) window = nullptr;   // a widget not embedded in one has no arg table
    const std::wstring text = BuildOptionLine(window, codec, len);
    if (text.empty()) { LogFail(widget, kWhyRowEmpty, value, count); return ret; }

    // `child=` names which flavour laid this list out. One grep then shows that both reach the same
    // speaker -- the claim this design rests on, stated by the log rather than by a comment.
    char extra[16];
    snprintf(extra, sizeof(extra), " child=%d", (state & FLAG_CHILD_LIST) ? 1 : 0);
    EmitOption(widget, "dialogue-choice", value, count, text, extra);
    return ret;
}

void ForgetLastCursor() {
    g_lastWidget = nullptr;
    g_lastMode   = 0xFF;
    g_lastValue  = -1;   // the widget address recycles, so the key must not outlive its prompt
}

bool Init() {
    const bool ok = Hooks::InstallTyped(RVA_CHOICE_TICK, &HookedChoiceTick, &s_origChoiceTick);
    Log::Write("READER", ok
        ? "ChoiceReader: selection tick hooked (FUN_002a9980); highlight read from widget+0x54"
        : "ChoiceReader: FUN_002a9980 hook FAILED -- in-dialogue choices stay silent");
    return ok;
}

void Shutdown() { Hooks::Uninstall(RVA_CHOICE_TICK); ForgetLastCursor(); }

} // namespace ChoiceReader

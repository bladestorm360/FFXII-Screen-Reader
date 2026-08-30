#include "ui/config_reader.h"
#include "ui/menu_state.h"
#include "ui/text_capture.h"
#include "core/game_text.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "speech/phrasebook.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Config value read (new-game / options screens). Row array at ctrl+0xE8 (stride 0x18, see
// MenuState::ConfigRowWidget). Two enum layouts, per FUN_0023ed80's type switch:
//   FUN_0023e770 (types 1/2/8): selected index at row+0xd0; option cells inline at row+0xd8 + i*8;
//     the selected cell's codec label = *(*(*(cell+0x60)+8)+0x18).
//   FUN_0023d6b0 (type 3): option children in the ptr-array at row+0x60 (base row+0xd0,
//     count row+0xd2); selected = the child with (child+8)&1; label at *(child+0x18).
// The SEH-guarded readers below copy POD bytes only; decoding happens outside the guard, because
// GameText::Decode builds a std::wstring and C++ objects may not live in a __try scope.

namespace {

using MemRead::SafeReadU32;
using ValueRow = MenuState::ValueRow;

constexpr uint32_t OFF_ROW_CHILDS  = 0x60;    // value row -> option-child pointer array
constexpr uint32_t OFF_ROW_CBASE   = 0xD0;    // value row -> base child index (u8, = 8)
constexpr uint32_t OFF_ROW_CCOUNT  = 0xD2;    // value row -> option count (u8, = 6)
constexpr uint32_t OFF_CHILD_FLAGS = 0x08;    // option child -> base flags; bit0 = selected
constexpr uint32_t OFF_CHILD_LABEL = 0x18;    // option child -> *(child+0x18) = codec label bytes
constexpr uint32_t OFF_E770_SELIDX = 0xD0;    // E770 row -> selected index, stored directly
constexpr uint32_t OFF_E770_CELLS  = 0xD8;    // E770 row -> inline option cells (stride 8)
constexpr uint32_t OFF_GFX_FMTBUF  = 0xCC;    // B6F0 row -> inline formatted display text
// Controls key-binding row (FUN_0023c5c0): caches DIK key CODES only, never the display name.
constexpr uint32_t OFF_BINDROW_CODE0  = 0xD0; // *(u32)(row+0xd0 + col*4) = live key code (col 0 = keyboard)
constexpr uint32_t OFF_BINDROW_REBIND = 0xEC; // *(u32)(row+0xec) != 0 => mid-rebind ("press a key")
constexpr int      BIND_ID_REBIND     = 0x46dd;  // the game's own "press a key" prompt string id

// Copy the codec label bytes of the option at display index `idx`, per row class.
bool ReadOptionBytes(void* row, ValueRow kind, int idx, uint8_t* out, size_t cap, size_t* outLen) {
    if (!row || idx < 0 || cap == 0) return false;
    __try {
        char* r = reinterpret_cast<char*>(row);
        const uint8_t* codec = nullptr;
        if (kind == ValueRow::EnumE770) {
            void* cell = *reinterpret_cast<void* const*>(
                r + OFF_E770_CELLS + static_cast<size_t>(idx) * sizeof(void*));
            void* arr  = cell ? *reinterpret_cast<void* const*>(reinterpret_cast<char*>(cell) + 0x60) : nullptr;
            void* leaf = arr  ? *reinterpret_cast<void* const*>(reinterpret_cast<char*>(arr) + 8)     : nullptr;
            if (leaf) codec = *reinterpret_cast<const uint8_t* const*>(reinterpret_cast<char*>(leaf) + 0x18);
        } else if (kind == ValueRow::EnumD6B0 || kind == ValueRow::EnumDB40) {
            void* arr = *reinterpret_cast<void* const*>(r + OFF_ROW_CHILDS);
            int base  = *reinterpret_cast<uint8_t*>(r + OFF_ROW_CBASE);
            void* child = arr ? *reinterpret_cast<void* const*>(
                reinterpret_cast<char*>(arr) + static_cast<size_t>(base + idx) * sizeof(void*)) : nullptr;
            if (child) codec = *reinterpret_cast<const uint8_t* const*>(
                reinterpret_cast<char*>(child) + OFF_CHILD_LABEL);
        }
        if (!codec) return false;
        size_t k = 0;
        for (; k + 1 < cap; ++k) { uint8_t b = codec[k]; out[k] = b; if (!b) break; }
        out[k] = 0;
        *outLen = k;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Current selected display index of a value row (per class), or -1.
int SelectedIndex(void* row, ValueRow kind) {
    if (!row) return -1;
    __try {
        char* r = reinterpret_cast<char*>(row);
        if (kind == ValueRow::EnumE770) {
            return *reinterpret_cast<uint8_t*>(r + OFF_E770_SELIDX);   // stored directly
        }
        if (kind == ValueRow::EnumD6B0 || kind == ValueRow::EnumDB40) {
            void* arr = *reinterpret_cast<void* const*>(r + OFF_ROW_CHILDS);
            if (!arr) return -1;
            int base  = *reinterpret_cast<uint8_t*>(r + OFF_ROW_CBASE);
            int count = *reinterpret_cast<uint8_t*>(r + OFF_ROW_CCOUNT);
            for (int i = 0; i < count && i < 32; ++i) {
                void* child = *reinterpret_cast<void* const*>(
                    reinterpret_cast<char*>(arr) + static_cast<size_t>(base + i) * sizeof(void*));
                if (child && (*reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(child) + OFF_CHILD_FLAGS) & 1u))
                    return i;
            }
        }
        return -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Decode the option label of a value row at display index `idx`. Empty if unreadable.
std::wstring OptionLabel(void* row, ValueRow kind, int idx) {
    if (kind == ValueRow::None || idx < 0) return std::wstring();
    uint8_t buf[256];
    size_t len = 0;
    if (!ReadOptionBytes(row, kind, idx, buf, sizeof(buf), &len) || len == 0) return std::wstring();
    std::wstring s = GameText::Decode(buf, len);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// IS THIS ROW REALLY AN ENUM OF LABELS? A row that classifies as one of the three enum builders but
// whose options are drawn as GAUGE BLOCKS rather than words has no per-option string anywhere, and
// walking to "the selected cell's label" lands on bytes that are not text. Config's **Battle Speed**
// does exactly this: it spoke `Battle Speed: æÏèêïêäæÍUÍÒ` while every other row on the same screen
// ("Battle Mode: Wait", "Target Lines: On", "Cursor Position: Default") read correctly.
//
// THE MEASUREMENT THAT NAMES IT, straight out of the log: the value-change path
// (RowValueAtNewValue -> OptionLabel(row, kind, nv)) produced the IDENTICAL string for two different
// `nv` values, one after the other. An enum's label is per-option by definition, so a row that
// answers the same bytes for two distinct option indices is not an enum, whatever its builder is.
// That is a structural fact about the data, not a guess about the glyphs -- and it cannot misfire on
// a real enum, because two options of one setting are never the same word.
//
// Undecidable (only one option reads) => treat it as an enum and speak, which is the pre-existing
// behaviour. This is a REFUSAL, not a repair: the correct read for the row is still unknown, so the
// mod says nothing rather than mojibake, and logs what it would need to do the job properly.
bool HasPerOptionLabels(void* row, ValueRow kind) {
    uint8_t a[256], b[256];
    size_t la = 0, lb = 0;
    if (!ReadOptionBytes(row, kind, 0, a, sizeof(a), &la) || la == 0) return true;   // undecidable
    if (!ReadOptionBytes(row, kind, 1, b, sizeof(b), &lb) || lb == 0) return true;   // undecidable
    if (la != lb) return true;
    return memcmp(a, b, la) != 0;
}

// Its own function because a std::wstring may not live in a __try scope -- the same split every
// other guarded read in this file uses (see the note at the top).
bool ReadOptionCount(void* row, int* out) {
    if (!row || !out) return false;
    __try {
        *out = *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(row) + OFF_ROW_CCOUNT);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// GAUGE ROWS READ AS A NUMBER (S174). A row with no per-option labels still knows perfectly well
// WHICH option is selected -- `SelectedIndex` finds it by the child's flag bit, and the refusal log
// has been printing it as `sel=` all along. What was missing was never the value; it was a way to
// say it. So the blocks get counted instead of decoded: "6 of 6".
//
// SCOPED TO D6B0/DB40 ON PURPOSE. Those two carry an option COUNT at row+0xD2, which is what makes
// "of 6" true rather than invented. E770 has no count field, so it keeps refusing -- a bare index
// with no range would be a number the player cannot act on ("speed 6" of how many?).
//
// A count that reads as 0, or as something absurd, degrades to the bare position rather than
// printing a range we do not believe. Same instinct as FormatSlider's max==0 guard.
std::wstring GaugeReadout(void* row, ValueRow kind, int sel) {
    if (sel < 0) return std::wstring();
    if (kind != ValueRow::EnumD6B0 && kind != ValueRow::EnumDB40) return std::wstring();
    int count = 0;
    if (!ReadOptionCount(row, &count)) count = 0;

    // `sel` is a 0-based display index; the gauge the player sees is 1-based.
    std::wstring out = std::to_wstring(sel + 1);
    if (count > 0 && count <= 32 && sel < count) {
        out += Phrase::Get(Phrase::Id::OfJoiner);
        out += std::to_wstring(count);
    }
    return out;
}

// One line per distinct row, so a cursor parked on the offender cannot flood the file. What a future
// session needs is all here: the class pointer (the builder address stored at row+0 by FUN_00244f50
// -- FUN_0023ed80's type switch maps type -> builder, and type 10 -> FUN_0023e400 is a SEVENTH
// builder ClassifyValueRow does not know), the kind it matched, the selected index, and the bytes
// that were being decoded as a label.
//
// It stayed after S174 gave gauges a readout, because "took the gauge path" is exactly what a future
// log needs to show -- a row that lands here and is NOT a gauge is the next defect.
void LogGaugeRow(void* row, ValueRow kind, int sel, bool spoke) {
    static void* s_seen[8] = {};
    for (void* p : s_seen) if (p == row) return;
    for (void*& p : s_seen) if (!p) { p = row; break; }

    void* cls = nullptr;
    uint8_t raw[16] = {};
    size_t len = 0;
    __try { cls = *reinterpret_cast<void* const*>(row); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ReadOptionBytes(row, kind, sel < 0 ? 0 : sel, raw, sizeof(raw), &len);

    char hex[64] = {};
    for (size_t i = 0; i < len && i < 16; ++i) snprintf(hex + i * 3, 4, "%02X ", raw[i]);
    char m[256];
    snprintf(m, sizeof(m),
             "config row has no per-option labels (gauge, not an enum) -- %s: row=%p class=%p "
             "kind=%d sel=%d bytes=[%s]",
             spoke ? "read as a NUMBER" : "REFUSED, no option count",
             row, cls, static_cast<int>(kind), sel, hex);
    Log::Write("READER", m);
}

// FUN_0023ebe0 slider: the gauge child (*(*(row+0x60))) holds the current value at +0x18 and the
// range at +0x1c. We speak a number: raw when the range is <= 100 (already a 0-100-ish scale), a
// rounded percentage when the range is larger.
bool ReadSlider(void* row, uint32_t* valOut, uint32_t* maxOut) {
    if (!row) return false;
    __try {
        void* arr = *reinterpret_cast<void* const*>(reinterpret_cast<char*>(row) + OFF_ROW_CHILDS);
        if (!arr) return false;
        void* gauge = *reinterpret_cast<void* const*>(arr);   // child[0]
        if (!gauge) return false;
        *valOut = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(gauge) + 0x18);
        *maxOut = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(gauge) + 0x1c);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::wstring FormatSlider(uint32_t val, uint32_t max) {
    if (max == 0) return std::wstring();
    int n = (max > 100) ? static_cast<int>((static_cast<uint64_t>(val) * 100 + max / 2) / max)
                        : static_cast<int>(val);
    return std::to_wstring(n);
}

// Graphics value rows (FUN_0023b330 / FUN_0023b6f0) format their display value into an inline codec
// buffer on the row via FUN_0023b530. Copy + decode.
bool ReadInlineBytes(void* row, uint32_t off, uint8_t* out, size_t cap, size_t* outLen) {
    if (!row || cap == 0) return false;
    __try {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(reinterpret_cast<char*>(row) + off);
        size_t k = 0;
        for (; k + 1 < cap; ++k) { uint8_t b = p[k]; out[k] = b; if (!b) break; }
        out[k] = 0;
        *outLen = k;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::wstring InlineCodecValue(void* row, uint32_t off) {
    uint8_t buf[256];
    size_t len = 0;
    if (!ReadInlineBytes(row, off, buf, sizeof(buf), &len) || len == 0) return std::wstring();
    std::wstring s = GameText::Decode(buf, len);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// Replicate FUN_001e0b00(code): a DIK key code -> localized key-name string id. Pure arithmetic;
// the base depends on the active keyboard layout, matching the game's own GetKeyboardLayout switch
// (US default, FR 0x40c, DE 0x407). Keyboard keys are code >= 0x1c.
int KeyCodeToStringId(int code) {
    int base = 0x46e1;                                            // US / default
    uintptr_t lang = reinterpret_cast<uintptr_t>(GetKeyboardLayout(0)) & 0xfff;
    if (lang == 0x40c)      base = 0x4749;                        // FR
    else if (lang == 0x407) base = 0x47b1;                        // DE
    if (code >= 0x1c) return code - 0x1c + base;
    if (code >= 1)    return code + 0x46dd;
    return 0x46dc;
}

// Controls key-binding row: the row caches only DIK CODES (col 0 = primary keyboard), never the
// name -- the game resolves the name transiently at draw via FUN_001e0b00(code) -> id ->
// FUN_002f9860. We replicate the arithmetic and read the NAME from TextCapture's id cache, which
// the game's own draw of this row just populated (no game call). Keyboard column only; controller
// bindings are a later pass.
std::wstring ControlsBindingValue(void* row) {
    uint32_t rebind = 0;
    if (SafeReadU32(row, OFF_BINDROW_REBIND, &rebind) && rebind != 0)
        return TextCapture::StringById(BIND_ID_REBIND);           // "press a key" — the game's own prompt
    uint32_t code = 0;
    if (!SafeReadU32(row, OFF_BINDROW_CODE0, &code) || code == 0) return std::wstring();   // unbound
    return TextCapture::StringById(KeyCodeToStringId(static_cast<int>(code)));
}

} // namespace

namespace ConfigReader {

std::wstring RowValue(void* row) {
    const ValueRow kind = MenuState::ClassifyValueRow(row);
    switch (kind) {
        case ValueRow::None:
            return std::wstring();
        case ValueRow::SliderEBE0:      // audio volume
        case ValueRow::SliderB330: {    // Graphics slider
            uint32_t val = 0, max = 0;
            return ReadSlider(row, &val, &max) ? FormatSlider(val, max) : std::wstring();
        }
        case ValueRow::GfxEnumB6F0:
            return InlineCodecValue(row, OFF_GFX_FMTBUF);
        case ValueRow::KeyBindC5C0:
            return ControlsBindingValue(row);
        default: {                      // E770 / D6B0 / DB40 enums
            const int sel = SelectedIndex(row, kind);
            if (!HasPerOptionLabels(row, kind)) {
                std::wstring gauge = GaugeReadout(row, kind, sel);
                LogGaugeRow(row, kind, sel, !gauge.empty());
                return gauge;                                   // empty => still refused, still silent
            }
            return OptionLabel(row, kind, sel);
        }
    }
}

std::wstring RowValueAtNewValue(void* row, int nv) {
    const ValueRow kind = MenuState::ClassifyValueRow(row);
    if (kind == ValueRow::None || nv < 0) return std::wstring();
    if (kind == ValueRow::SliderEBE0) {
        uint32_t val = 0, max = 0;
        if (!ReadSlider(row, &val, &max)) return std::wstring();
        return FormatSlider(static_cast<uint32_t>(nv), max);   // nv is the new gauge value
    }
    // Same fork as RowValue. This path is where the defect was MEASURED (two different `nv`,
    // identical bytes), so it must not keep speaking what the other path declines to -- and now that
    // gauges have a readout it must not stay silent where the other path speaks either.
    //
    // `nv` IS ASSUMED TO BE THE DISPLAY INDEX, exactly as the enum line below already assumes for
    // this same row class. That assumption is play-proven for the enum rows ("Battle Mode: Wait");
    // for a gauge it is not, so if a change speaks a number the highlight then contradicts, this is
    // the line to drop -- the value would simply ride the next paint instead.
    if (!HasPerOptionLabels(row, kind)) return GaugeReadout(row, kind, nv);
    return OptionLabel(row, kind, nv);                          // nv is the new option index
}

} // namespace ConfigReader

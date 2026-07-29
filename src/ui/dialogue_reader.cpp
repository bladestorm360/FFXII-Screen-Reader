#include "ui/dialogue_reader.h"

#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "speech/speech.h"
#include "ui/choice_reader.h"
#include "ui/message_reader.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// Dialogue pagination, driven by the game's own page cursor. See dialogue_reader.h for the dispatch
// table that settles why FUN_002a8c50 is the one hook that reaches every paginated message.
namespace DialogueReader {
namespace {

// ---- offline-derived RVAs / offsets (abs = RVA + 0x120000) -------------------
constexpr uint32_t RVA_TEXT_WALK  = 0x188C50;   // FUN_002a8c50(widget, stopByte) -- the text walk
constexpr uint32_t RVA_MSGWIN_REG = 0x203F200;  // DAT_0215f200 -- 8 message-window slots

constexpr uint32_t OFF_W_TEXT  = 0x28;   // widget+0x28  message text base
constexpr uint32_t OFF_W_PAGE  = 0x8A;   // widget+0x8A  u16 byte offset of the page on screen
constexpr uint32_t OFF_W_TYPE  = 0xA3;   // widget+0xA3  dispatch type (0 choice-capable, 1 plain)
constexpr uint32_t OFF_W_STATE = 0xB0;   // widget+0xB0  state word, low byte = mode
constexpr uint32_t OFF_W_WAIT  = 0x54;   // widget+0x54  park reason: 3 = page break, 0x23 = `0F 23`
constexpr uint32_t OFF_W_ENDED = 0xC0;   // widget+0xC0  1 = message ended; the call CONSUMES it

constexpr uint32_t OFF_TEXT_BLOCK = 0xD0;   // messageWindow+0xD0 = this text widget
constexpr int      MSGWIN_SLOTS   = 8;
constexpr uint32_t MSGWIN_STRIDE  = 0x68;

constexpr size_t TEXT_SCAN_MAX = 4096;      // the cap GameText uses for a whole message

// ---- hook trampoline ---------------------------------------------------------
typedef void (*Pfn_TextWalk)(void* widget, uint8_t stopByte);
Pfn_TextWalk s_origTextWalk = nullptr;

bool g_initialized = false;

// The page last handed to speech, PER MESSAGE-WINDOW SLOT.
//
// It has to be per slot, not one global triple: the game lays out several text widgets in a frame,
// and a single "last seen" key would flip between them every frame and re-emit the dialogue page
// each time round. The game models these as 8 slots, so we do too.
struct PageKey {
    const uint8_t* base = nullptr;
    uint32_t       off  = 0xFFFFFFFFu;   // sentinel: nothing emitted for this slot yet
};
std::mutex g_mutex;
PageKey    g_lastPage[MSGWIN_SLOTS];

void ForgetSlot(int slot) {
    if (slot < 0 || slot >= MSGWIN_SLOTS) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    g_lastPage[slot] = PageKey();
}

void ForgetAll() {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (int i = 0; i < MSGWIN_SLOTS; ++i) g_lastPage[i] = PageKey();
}

// Which message-window slot owns this text widget, or -1 if none does.
//
// `FUN_002e16b0` -- the content setter the mod has read since Session 19 -- stores the window it
// builds in `DAT_0215f200`: 8 slots, stride 0x68, window pointer at +0x00. The text widget is
// `window+0xD0`, the same relation ChoiceReader walks in reverse to reach the window from the
// widget. Membership in that registry IS the definition of "a message the game paginates", which is
// what keeps this hook off every other text block the engine lays out through the same dispatch
// slot -- the field menu shares the `FUN_002a6190` window class, so class identity alone would not
// do it. Eight guarded pointer reads, and it runs BEFORE the change-check so a foreign widget can
// never disturb a live message's page key.
int LiveMessageSlot(void* widget) {
    void* reg = Hooks::ResolveRva(RVA_MSGWIN_REG);
    if (!reg || !widget) return -1;
    for (int i = 0; i < MSGWIN_SLOTS; ++i) {
        void* window = MemRead::PtrAt(reg, static_cast<uint32_t>(i) * MSGWIN_STRIDE);
        if (window && static_cast<char*>(window) + OFF_TEXT_BLOCK == widget) return i;
    }
    return -1;
}

// A silent-if-wrong reader with NO diagnostic makes its own bug invisible (choice_reader.cpp learned
// that the hard way): if dialogue ever goes quiet, this says the page was seen and which widget it
// was dropped on. Hard-capped as well as per-widget, because this runs on a per-frame path and two
// foreign widgets alternating would otherwise log every frame.
void LogReject(void* widget) {
    static void* s_lastRejected = nullptr;
    static int   s_rejects      = 0;
    if (widget == s_lastRejected || s_rejects >= 16) return;
    s_lastRejected = widget;
    ++s_rejects;
    char m[176];
    snprintf(m, sizeof(m),
             "text widget outside the message-window registry, not a dialogue page (not spoken): "
             "wnd=%p", widget);
    Log::Write("DIALOGUE", m);
}

// THE SINGLE SPEECH POINT for dialogue. Page 1 and page N arrive here identically -- page 1 is just
// the cursor's first value on a new message -- so one wording, one interrupt policy and one log line
// cover every page the game shows. There is no second speaker to race with.
void EmitPage(void* widget, const uint8_t* base, uint16_t off) {
    // DecodePages splits on the codec's 0x03 page break, so element 0 is exactly the page sitting at
    // the cursor and everything after it belongs to screens the player has not reached.
    std::vector<std::wstring> pages;
    GameText::DecodePages(base + off, TEXT_SCAN_MAX, pages);
    if (pages.empty() || !GameText::IsMostlyPrintable(pages.front())) return;
    const std::wstring& page = pages.front();

    // Hand ChoiceReader the message and the cursor BEFORE speaking: an option block on this page
    // (the notice board's bill list, a mid-dialogue Yes/No) has to queue behind the page text rather
    // than cut it off, and NotePage is what arms that.
    ChoiceReader::NotePage(base, off);

    uint8_t type = 0xFF, mode = 0xFF;
    int32_t wait = -1;
    MemRead::SafeReadU8(widget, OFF_W_TYPE, &type);
    MemRead::SafeReadU8(widget, OFF_W_STATE, &mode);       // low byte of the state word
    MemRead::SafeReadU32(widget, OFF_W_WAIT, reinterpret_cast<uint32_t*>(&wait));

    char hdr[144];
    snprintf(hdr, sizeof(hdr), "page[wnd=%p base=%p off=%u type=%u mode=%u wait=%d]: ",
             widget, static_cast<const void*>(base), static_cast<unsigned>(off),
             static_cast<unsigned>(type), static_cast<unsigned>(mode), static_cast<int>(wait));
    Log::WriteW("DIALOGUE", hdr, page);

    MessageReader::NoteSpoken(page);   // the shared `t` re-read store -- one store, not a second one
    Speech::Output(page, /*interrupt=*/true);
}

// FUN_002a8c50(widget, stopByte): the message widget's text walk, and the WRITER of the page cursor
// `widget+0x8A` (`:199-200` advances it past a 0x03 page break). PER-FRAME while a box is on screen,
// so the emit is guarded by a change-check on (widget, base, cursor) -- the sanctioned form of that
// exception, naming the per-frame function it guards, exactly as choice_reader.cpp's tick names
// FUN_002a9980.
//
// This is a TRANSITION DETECTOR on the game's own cursor, not a speech dedup: the key is dropped the
// moment the message ends, so re-entering the same conversation announces it again.
void HookedTextWalk(void* widget, uint8_t stopByte) {
    // Read the end-of-message latch BEFORE the original. `+0xC0` is set to 1 by the codec-0x00 branch
    // and the NEXT call consumes it -- it skips the walk and clears the field back to 0 (`:535-536`)
    // -- so after the original has run it always reads 0.
    uint32_t ended = 0;
    if (widget) MemRead::SafeReadU32(widget, OFF_W_ENDED, &ended);

    if (s_origTextWalk) s_origTextWalk(widget, stopByte);
    if (!widget) return;

    static bool s_firstFire = true;
    if (s_firstFire) {
        s_firstFire = false;
        Log::Write("DIALOGUE", "diag: text walk FUN_002a8c50 fired");
    }

    // Identify the slot FIRST. A text block that is not a live message must not touch any page key.
    const int slot = LiveMessageSlot(widget);
    if (slot < 0) { LogReject(widget); return; }

    if (ended == 1) {          // the box finished -- re-arm so a repeat of it speaks again
        ForgetSlot(slot);
        return;
    }

    const uint8_t* base = static_cast<const uint8_t*>(MemRead::PtrAt(widget, OFF_W_TEXT));
    uint16_t off = 0;
    if (!base || !MemRead::SafeReadU16(widget, OFF_W_PAGE, &off)) return;

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        PageKey& last = g_lastPage[slot];
        if (base == last.base && off == last.off) return;   // the cursor did not move: same page
        last.base = base;
        last.off  = off;
    }

    STALL_SCOPE("DialogueReader::TextWalk");
    EmitPage(widget, base, off);
}

} // namespace

bool Init() {
    if (g_initialized) {
        Log::Write("DIALOGUE", "DialogueReader::Init called twice — ignoring");
        return true;
    }
    const bool ok = Hooks::InstallTyped(RVA_TEXT_WALK, &HookedTextWalk, &s_origTextWalk);
    g_initialized = true;
    Log::Write("DIALOGUE", ok
        ? "DialogueReader initialized (page cursor widget+0x8A via FUN_002a8c50 — every input device)"
        : "DialogueReader: FUN_002a8c50 hook FAILED — dialogue pages will not speak");
    return ok;
}

void Shutdown() {
    if (!g_initialized) return;
    Hooks::Uninstall(RVA_TEXT_WALK);
    g_initialized = false;
    ForgetAll();
    Log::Write("DIALOGUE", "DialogueReader shut down");
}

} // namespace DialogueReader

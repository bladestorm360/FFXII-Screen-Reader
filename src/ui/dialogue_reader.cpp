#include "ui/dialogue_reader.h"

#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "core/frame_probe.h"
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
constexpr uint32_t RVA_MSG_SET    = 0x1C16B0;   // FUN_002e16b0(ctx, slot, textPtr, _) content setter
constexpr uint32_t RVA_MSGWIN_REG = 0x203F200;  // DAT_0215f200 -- 8 message-window slots

constexpr uint32_t OFF_W_TEXT  = 0x28;   // widget+0x28  message text base
constexpr uint32_t OFF_W_PAGE  = 0x8A;   // widget+0x8A  u16 byte offset of the page on screen
constexpr uint32_t OFF_W_TYPE  = 0xA3;   // widget+0xA3  dispatch type (0 choice-capable, 1 plain)
constexpr uint32_t OFF_W_STATE = 0xB0;   // widget+0xB0  state word, low byte = mode
// +0x54 CARRIES A DIFFERENT MEANING PER MODE, and calling it "wait" is what made a live value look
// like noise: the lift prompt logged `wait=66` and 66 was the selected floor, not a park reason.
// mode 0/5 -> park reason (3 = page break, 0x23 = the `0F 23` wait escape); mode 4 -> the numeric
// field's VALUE; mode 2 -> the raw option index. The log line names it by the mode it is in.
constexpr uint32_t OFF_W_VALUE  = 0x54;
constexpr uint32_t OFF_W_DIGITS = 0xA1;  // widget+0xA1  mode 4: digit count of the numeric field
constexpr uint8_t  MODE_NUMERIC = 4;     // widget+0xB0 low byte: an editable number, set by `0F 2D`
constexpr uint32_t OFF_W_ENDED = 0xC0;   // widget+0xC0  1 = message ended; the call CONSUMES it

constexpr uint32_t OFF_TEXT_BLOCK = 0xD0;   // messageWindow+0xD0 = this text widget
constexpr int      MSGWIN_SLOTS   = 8;
constexpr uint32_t MSGWIN_STRIDE  = 0x68;

constexpr size_t TEXT_SCAN_MAX = 4096;      // the cap GameText uses for a whole message

// ---- hook trampolines --------------------------------------------------------
typedef void (*Pfn_TextWalk)(void* widget, uint8_t stopByte);
// FOUR PARAMETERS, and the arity is not negotiable (CLAUDE.md: a detour's arity must match the
// callee's -- under-declaring is what crashed the shop in S129). All four are read in the body of
// FUN_002e16b0, and the archive already carries the signature from when the mod hooked this same
// function in Session 19 (Docs\sessions_001_050.md, Docs\GameArchitecture.md).
typedef int  (*Pfn_MsgSet)(void* ctx, int slot, void* textPtr, void* unused);
Pfn_TextWalk s_origTextWalk = nullptr;
Pfn_MsgSet   s_origMsgSet   = nullptr;

bool g_initialized = false;

// How many inert repeats of the end latch to see before the log says so. One line, once per box.
constexpr uint32_t kIdleReportAt = 64;

// The page last handed to speech, PER MESSAGE-WINDOW SLOT.
//
// It has to be per slot, not one global triple: the game lays out several text widgets in a frame,
// and a single "last seen" key would flip between them every frame and re-emit the dialogue page
// each time round. The game models these as 8 slots, so we do too.
//
// `widget` is part of the key, not decoration. A slot is a RECYCLED index into the game's registry,
// so slot 3 tomorrow is a different box from slot 3 today; without this the key outlives the box it
// was guarding and suppresses the next one that lands in the same slot.
struct PageKey {
    const void*    widget = nullptr;     // the text widget this key was taken from
    const uint8_t* base = nullptr;
    uint32_t       off  = 0xFFFFFFFFu;   // sentinel: nothing emitted for this slot yet
    // The game's end-of-message field is a LEVEL that oscillates, not an event -- see HookedTextWalk.
    // `ended` records that it has already been handled for THIS key, so every later observation of it
    // is inert; `idled` counts those so the log can say how many re-speaks the old code produced.
    bool           ended = false;
    uint32_t       idled = 0;
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
// slot -- the party menu shares the `FUN_002a6190` window class, so class identity alone would not
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
    // Cap raised from 16 in Session 126: a single play session had already burned 13, and once it is
    // spent this diagnostic is dead for the rest of the process -- so the one rejection you actually
    // want to see, hours in, is the one it cannot report. The per-widget check above is what does the
    // real per-frame throttling; the cap is only a backstop against two widgets alternating.
    if (widget == s_lastRejected || s_rejects >= 64) return;
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
    // READ THE WIDGET STATE FIRST. A page can host an EDITABLE NUMBER instead of an option list --
    // the Draklor lift's "Select destination: __F" -- and the `0F 2D` escape that draws it carries
    // no digits: the live value is on the widget at +0x54 and the decoder cannot reach it. Opening
    // the scope around the decode is what puts the floor number into the sentence the player hears.
    uint8_t type = 0xFF, mode = 0xFF, digits = 0;
    int32_t value = -1;
    MemRead::SafeReadU8(widget, OFF_W_TYPE, &type);
    MemRead::SafeReadU8(widget, OFF_W_STATE, &mode);       // low byte of the state word
    MemRead::SafeReadU32(widget, OFF_W_VALUE, reinterpret_cast<uint32_t*>(&value));
    MemRead::SafeReadU8(widget, OFF_W_DIGITS, &digits);

    // DecodePages splits on the codec's 0x03 page break, so element 0 is exactly the page sitting at
    // the cursor and everything after it belongs to screens the player has not reached.
    std::vector<std::wstring> pages;
    if (mode == MODE_NUMERIC) {
        // Scoped to this one decode; in every other mode 0x2D decodes exactly as it always has.
        GameText::NumericFieldScope field(value, digits);
        GameText::DecodePages(base + off, TEXT_SCAN_MAX, pages);
    } else {
        GameText::DecodePages(base + off, TEXT_SCAN_MAX, pages);
    }
    // TELL ChoiceReader THE BOX TURNED A PAGE -- BEFORE the printability bail below, not after.
    // It arms the queue so the first option falls in behind this page rather than cutting it off,
    // and seeds the numeric field with the value this line is about to speak so the tick says it
    // once and then only on a MOVE.
    //
    // ABOVE THE BAIL because a page can carry an option block and NO TEXT OF ITS OWN: Decode stops
    // dead at the 0x0E marker (ControlLength returns -1, "length unknown -- stop, never guess"), so
    // such a page decodes to nothing and the bail fires. Below the bail this call was skipped on
    // exactly the pages a choice prompt opens on -- which is how the Archades "Commit this tale to
    // memory." prompt came to queue its first option behind a flag left set by the PREVIOUS page.
    ChoiceReader::NotePage(widget, base, off, mode == MODE_NUMERIC, value);

    if (pages.empty() || !GameText::IsMostlyPrintable(pages.front())) {
        // ONE LINE PER PAGE, not per frame -- the caller only reaches EmitPage when the cursor
        // moved. It says out loud that the box turned onto a page this reader has nothing to say
        // about, which is what a bare option block looks like from here; pair it with the
        // `dialogue-choice[...] child=1` line that should follow within a frame or two.
        char q[160];
        snprintf(q, sizeof(q), "page[wnd=%p off=%u mode=%u] carries no speakable text -- noted for"
                 " the choice reader, not spoken", widget, static_cast<unsigned>(off),
                 static_cast<unsigned>(mode));
        Log::Write("DIALOGUE", q);
        return;
    }
    const std::wstring& page = pages.front();

    char hdr[160];
    snprintf(hdr, sizeof(hdr), "page[wnd=%p base=%p off=%u type=%u mode=%u %s=%d]: ",
             widget, static_cast<const void*>(base), static_cast<unsigned>(off),
             static_cast<unsigned>(type), static_cast<unsigned>(mode),
             mode == MODE_NUMERIC ? "value" : "park", static_cast<int>(value));
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
// This is a TRANSITION DETECTOR on the game's own cursor, not a speech dedup. It is scoped to the box
// currently on screen, and there are THREE ways it is dropped, because no one of them is enough and a
// gap reads as a dedup to the player:
//   1. the game installs a new message in the slot -> HookedMsgSet (FUN_002e16b0), the WRITER of the
//      registry entry;
//   2. a different widget appears in the same registry slot -> the key is stale, dropped in the
//      change-check itself;
//   3. a list screen opens over the box -> DialogueReader::ForgetLivePages, from InventoryReader's
//      FUN_005655f0 hook.
//
// `+0xC0` USED TO BE (1), AND IT WAS THE WRONG SIGNAL TWICE OVER. It is read PRE-call, so it is visible
// only on the call AFTER the message ended -- and when a shop tears the box down that extra walk never
// happens, so the latch is never observed and the key never drops (Session 126: exiting a shop and
// re-entering did not re-speak the clerk -- same recycled slot, same `base`, `off` 0 both times, equal
// key, silence). Worse, when the box DOES stay on screen the field oscillates every frame, so wiping on
// it made a finished box re-speak forever -- see the end-latch arm below. It is now a once-per-message
// notification to the other two readers and nothing else; the re-arm belongs to (1).
void HookedTextWalk(void* widget, uint8_t stopByte) {
    // Read the end-of-message latch BEFORE the original. `+0xC0` is set to 1 by the codec-0x00 branch
    // and the NEXT call consumes it -- it skips the walk and clears the field back to 0 (`:535-536`)
    // -- so after the original has run it always reads 0.
    uint32_t ended = 0;
    if (widget) MemRead::SafeReadU32(widget, OFF_W_ENDED, &ended);

    if (s_origTextWalk) s_origTextWalk(widget, stopByte);
    // One relaxed increment, BEFORE the widget filter, because the question is how often the GAME
    // calls this -- not how often we accept the call. A tester sees this surface misbehave only at
    // raised game speed; this counter is what shows whether the walk runs inside the sim loop.
    FrameProbe::OnTextWalk();
    if (!widget) return;

    static bool s_firstFire = true;
    if (s_firstFire) {
        s_firstFire = false;
        Log::Write("DIALOGUE", "diag: text walk FUN_002a8c50 fired");
    }

    // Identify the slot FIRST. A text block that is not a live message must not touch any page key.
    const int slot = LiveMessageSlot(widget);
    if (slot < 0) { LogReject(widget); return; }

    // THE END LATCH IS A LEVEL, NOT AN EVENT -- and treating it as one made a finished box repeat
    // its page forever. FUN_002a8c50 sets `+0xC0 = 1` when the walk reaches the codec terminator
    // (:136-146) and the NEXT call takes the skipped path, sets `+0xC0 = 0` and `mode = 1`
    // (:534-536) -- and mode 1 passes the guard at :139, so the call after THAT latches again. While
    // a finished box sits on screen the field therefore reads 1, 0, 1, 0 at frame rate.
    //
    // This arm used to ForgetSlot on every 1 it saw, which wiped the page key; the very next frame
    // read 0, found no key, and re-emitted. Speech every two frames with interrupt=true, so each
    // utterance was cut off after ~33 ms and the player heard one fragment repeating with no way to
    // stop it -- reported from the first-area tutorial box, where the walk runs to the terminator and
    // the box then waits for a dismissal press instead of being torn down. Ordinary dialogue parks at
    // a 0x03 page break BEFORE the terminator, which is why it never showed there.
    //
    // So: act ONCE per key, and KEEP base/off. The emit path's own equality check then holds on every
    // `ended == 0` frame and the loop cannot start. The re-arm the wipe used to provide comes from
    // HookedMsgSet below -- the game's own "this slot got a new message" event.
    if (ended == 1) {
        bool     first = false;
        uint32_t idled = 0;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            PageKey& last = g_lastPage[slot];
            if (last.widget != widget) { last = PageKey(); last.widget = widget; }
            first = !last.ended;
            last.ended = true;
            if (!first) idled = ++last.idled;
        }
        if (first) {
            // The mid-dialogue choice widget IS this box's embedded list block (both readers reach it
            // as window+0xD0), so a finished message also retires any option cursor that was on it.
            // Its own guard has no way to see this -- FUN_002a9980 simply stops being called.
            ChoiceReader::ForgetLastCursor();
            // Same event, third consumer: the `t` re-read line belonged to THIS message. Without this
            // it outlived the box and `t` went on speaking a finished conversation anywhere in the
            // game. Firing it ONCE also fixes a second casualty of the loop -- it used to run every
            // two frames, so `t` was dead for as long as such a box was up.
            MessageReader::ForgetLastLine();
        } else if (idled == kIdleReportAt) {
            // The measurement, not a guess: this many inert repeats is this many re-speaks the old
            // code produced on this box. Its ABSENCE from a log of the reported tutorial box would
            // mean the oscillation is not what happened there.
            char m[192];
            snprintf(m, sizeof(m),
                     "end latch idled %ux on wnd=%p -- a finished box is sitting on screen; before "
                     "this fix each one re-spoke the page", idled, widget);
            Log::Write("DIALOGUE", m);
        }
        return;
    }

    const uint8_t* base = static_cast<const uint8_t*>(MemRead::PtrAt(widget, OFF_W_TEXT));
    uint16_t off = 0;
    if (!base || !MemRead::SafeReadU16(widget, OFF_W_PAGE, &off)) return;

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        PageKey& last = g_lastPage[slot];
        // Drop (2): this slot is holding a key taken from a DIFFERENT box. Recycled slot, new message.
        if (widget != last.widget) last = PageKey();
        if (base == last.base && off == last.off) return;   // the cursor did not move: same page
        last.widget = widget;
        last.base   = base;
        last.off    = off;
        last.ended  = false;      // a genuinely new page: the latch state belonged to the old one
        last.idled  = 0;
    }

    STALL_SCOPE("DialogueReader::TextWalk");
    EmitPage(widget, base, off);
}

// FUN_002e16b0(ctx, slot, textPtr, _): the whole-message content setter, and THE WRITER of the
// registry slot -- it tears the old window out of DAT_0215f200 and installs a freshly built one
// (:159-169). That makes it the one honest "this slot got a new message" signal, which is exactly
// the re-arm the end-latch wipe used to provide: without it, a message re-shown on a RECYCLED widget
// at the same offset would collide with the retained key and go silent, which is the S126 shop-clerk
// failure and the failure mode this project cares about most.
//
// IT SPEAKS NOTHING, AND IT MUST NEVER BE MADE TO. This is NOT a restoration of the Session 52
// content-setter reader that was deleted for making pagination keyboard-only (see message_reader.cpp's
// MOVED OUT note) -- it forgets a page key and returns. It also fires ~18 times in 140 ms at area
// load, all slot 0 (debug.md, the ambient-chatter table); harmless for a re-arm, fatal for a speaker.
int HookedMsgSet(void* ctx, int slot, void* textPtr, void* unused) {
    const int ret = s_origMsgSet ? s_origMsgSet(ctx, slot, textPtr, unused) : 0;
    // The game's own clamp (FUN_002e16b0:40-54): negative -> 0, and 7 is the ceiling.
    const int s = (slot < 0) ? 0 : (slot > MSGWIN_SLOTS - 1 ? MSGWIN_SLOTS - 1 : slot);
    ForgetSlot(s);
    return ret;
}

} // namespace

bool Init() {
    if (g_initialized) {
        Log::Write("DIALOGUE", "DialogueReader::Init called twice — ignoring");
        return true;
    }
    bool ok = Hooks::InstallTyped(RVA_TEXT_WALK, &HookedTextWalk, &s_origTextWalk);
    // The re-arm. If THIS one fails the pages still speak, but a message re-shown on a recycled
    // widget can go silent -- so the failure has to be visible rather than implied by the ok flag.
    if (!Hooks::InstallTyped(RVA_MSG_SET, &HookedMsgSet, &s_origMsgSet)) {
        ok = false;
        Log::Write("DIALOGUE", "FUN_002e16b0 re-arm hook FAILED — a re-shown message may not repeat");
    }
    g_initialized = true;
    Log::Write("DIALOGUE", ok
        ? "DialogueReader initialized (page cursor widget+0x8A via FUN_002a8c50 — every input "
          "device; page keys re-armed by the content setter FUN_002e16b0)"
        : "DialogueReader: a hook FAILED — see the line above and the Hooks log");
    return ok;
}

bool IsBoxLive() {
    void* reg = Hooks::ResolveRva(RVA_MSGWIN_REG);
    if (!reg) return false;
    for (int i = 0; i < MSGWIN_SLOTS; ++i) {
        if (MemRead::PtrAt(reg, static_cast<uint32_t>(i) * MSGWIN_STRIDE)) return true;
    }
    return false;
}

void ForgetLivePages() {
    ForgetAll();
    // Same event, same reason: a list screen opening over a conversation retires the option cursor
    // as surely as it retires the page key. Kept here rather than at the call site so the two can
    // never drift apart -- one event, one meaning.
    ChoiceReader::ForgetLastCursor();
    // And the `t` re-read line, for the same reason again: the box it came from is gone.
    MessageReader::ForgetLastLine();
}

void Shutdown() {
    if (!g_initialized) return;
    Hooks::Uninstall(RVA_MSG_SET);
    Hooks::Uninstall(RVA_TEXT_WALK);
    g_initialized = false;
    ForgetAll();
    Log::Write("DIALOGUE", "DialogueReader shut down");
}

} // namespace DialogueReader

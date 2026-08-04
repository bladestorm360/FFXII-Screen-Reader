#include "ui/message_reader.h"
#include "core/mem_read.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "speech/speech.h"
#include "core/logger.h"
#include "core/stall_probe.h"
#include "input/input_tracker.h"
#include "ui/dialogue_reader.h"
#include "ui/menu_state.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>


// Message-text reader. Two surfaces, both decoded from the game's own codec bytes and neither
// paginated — a page-turn concept would be meaningless for either:
//
//  A. "Obtained <item>" — the treasure/loot popup (FUN_0035e070, the 0x6e8-byte widget created by
//     FUN_0035df40 with its handle in _DAT_022ca430). On its case-1 (build) the FULLY COMPOSED text
//     sits at widget+0xC8: FUN_002b4090 (codec-sprintf) fills it from a template of three
//     `0f 31 80 80 80` substitution slots separated by 0x02, one per line (item / item / gil, max 3;
//     the template is truncated in-place for 1 or 2 entries). Reached from BOTH the treasure caller
//     (FUN_0050faf0) and the field caller (FUN_002a59c0), so it covers chest loot either way.
//     It is a timed toast — case 2 self-destructs when the animation ends — so it never paginates
//     and fires exactly once per popup.
//
//  B. Menu system messages — the FUN_0057c480 surface ("cannot equip", "sold"). Kept, but it is
//     MENU-ONLY: its case 1 does *(longlong*)(DAT_0209ac30 + 0x328) = surface, i.e. it registers
//     into the menu manager. It can never fire for field text, and in a 17-minute play log it never
//     fired at all. It is NOT the item/treasure path — that claim (spec'd at 0.90, below this
//     project's 0.98 bar) was wrong; surface A above is the real one.
//
// MOVED OUT — the telop/dialogue content setter FUN_002e16b0. It is a WHOLE-MESSAGE setter: it hands
// over every page of a conversation in one string, so this module split it on the codec's 0x03 page
// break and advanced through the pages on an observed Space/Enter press. That made multi-page
// dialogue keyboard-only — a controller player heard page 1 and nothing after it — because the mod
// was watching for a key rather than for the box advancing. Dialogue now lives in
// `ui/dialogue_reader`, which reads the game's own page cursor (widget+0x8A, written by
// FUN_002a8c50) and is therefore blind to which device turned the page. Do not reintroduce a
// content-setter page split here.
//
// REMOVED EARLIER — the e5f0 "dialogue" pair (FUN_003c02b0 resolver + FUN_002baf80 page proc). They
// are the WORLD MAP screen, not dialogue: page+0x138 is a MAP id and the resolver's out+8 is a MAP
// NAME (DAT_02b457e0+0xe8 is the map DB — FUN_003be680(id,&w,&h) returns width/height and callers
// zoom-to-fit; assets live under ArtData/menu/localmap/; the sibling proc FUN_002b7b80 tracks the
// player's world position). The spec's "decisive" evidence — that the widget owns the mini_face_c
// speaker portrait — was a misread: that symbol is a TEXTURE-BUNDLE name passed to FUN_0024a5a0
// alongside battle_4_p / s_font_c, and the FUN_002baf80 family never references it. Left wired,
// these would have spoken a map name over real dialogue.
//
// All read points are offline-derived from the decompile; no game functions are called; every
// dereference is SEH-guarded (MemRead). See FFXII-Decompile\notes\message_text_readpoints_spec.md —
// but note its sections A and B(field) are REFUTED (see debug.md).
namespace {

// ---- offline-derived RVAs / globals (abs = RVA + 0x120000) -------------------
constexpr uint32_t RVA_ITEMPOPUP = 0x23E070;  // FUN_0035e070(widget, msg)    "obtained <item>" toast proc
constexpr uint32_t RVA_PANEL     = 0x45C480;  // FUN_0057c480(surface, msg)   MENU system-message surface

constexpr uint32_t OFF_POPUP_TEXT  = 0xC8;   // item popup -> composed codec text (FUN_002b4090 dest)
constexpr uint32_t POPUP_TEXT_CAP  = 0x4A0;  // ...its sprintf capacity
constexpr uint32_t OFF_SURF_TEXT   = 0x1B0;  // surface -> 0x400-byte codec buffer
constexpr uint32_t OFF_SURF_COUNT  = 0x636;  // surface -> choice count (0 = passive info)
constexpr uint32_t OFF_SURF_FLAGS  = 0x630;  // surface -> producer flags; bit3 (0x8) = passive

// (The speaker-nameplate reader — DAT_02b62d78 + the (flags & 0x405) == 5 draw gate — went with the
//  e5f0 pair above: it existed only to prefix those "dialogue" lines, which were map names. When a
//  real dialogue speaker source is located, re-derive it against the message WIDGET that
//  dialogue_reader hooks rather than assuming this global belongs to it.)

// Directive 3: the FUN_0057c480 yes/no confirms are a distinct class from the already-working
// title/new-game confirms (FUN_00241d40); keep the read-point wired + logged but SILENT unless
// deliberately enabled after testing surfaces an in-game pop-up the existing handler misses.
constexpr bool kSpeakSurfaceConfirms = false;

// ---- hook trampolines --------------------------------------------------------
typedef uintptr_t (*Pfn_ItemPopup)(void* widget, void* msg);
typedef uintptr_t (*Pfn_Panel)(void* surface, void* msg);
Pfn_ItemPopup s_origItemPopup = nullptr;
Pfn_Panel     s_origPanel     = nullptr;

// ---- state (game thread, unless noted) ---------------------------------------
// Last spoken line, for the `t` re-read key. Written on the game thread by this module's surfaces
// AND by DialogueReader (through NoteSpoken), read on the input thread.
std::mutex   g_lastMutex;
std::wstring g_lastLine;

// Body of the pending yes/no confirm surface, captured at its birth (see OnPanelSurface) and
// consumed by MenuReader through TakeConfirmPrompt(). Written on the game thread, read on the
// game thread from the focus path -- its own lock, since g_lastMutex guards the `t` re-read line.
std::mutex   g_confirmMutex;
std::wstring g_confirmPrompt;

// Is the obtained-item toast on screen? Set on FUN_0035e070 case 1 (birth), cleared on case 2 (the
// animation ends and the widget destroys itself). The toast is the one `t` surface with no state
// left to query afterwards, so this latch stands in for the predicate the others have -- and it
// dies with the widget rather than outliving it, which is the whole point of the exercise.
// Written on the game thread, read on the input thread.
std::atomic<bool> g_toastLive{false};

bool g_initialized = false;

void SpeakAndStash(const std::wstring& text, const char* logPrefix) {
    MessageReader::NoteSpoken(text);
    Log::WriteW("MSGTEXT", logPrefix, text);
    Speech::Output(text, /*interrupt=*/true);
}

// The "obtained <item>" toast was BUILT (FUN_0035e070 case 1) — its composed text is now at
// widget+0xC8. Runs AFTER the original, so FUN_002b4090 has already written the buffer.
//
// The text is up to three 0x02-separated lines (item / item / gil) with the item names and counts
// already substituted into the template's `0f 31` slots, so this is the final, ready-to-speak
// string — no macro binding left to do. One fire per popup: the widget self-destructs on case 2
// when its animation ends, so there is no pagination and no repeat.
void OnItemPopup(void* widget) {
    if (!widget) return;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(widget) + OFF_POPUP_TEXT;
    std::wstring text = GameText::Decode(p, POPUP_TEXT_CAP);
    if (!GameText::IsMostlyPrintable(text)) return;
    SpeakAndStash(text, "item: ");
}

// A FUN_0057c480 surface was pumped — on birth, read + classify its composed text.
void OnPanelSurface(void* surface, void* msg) {
    if (!surface || !msg) return;
    int msgCase = 0;
    if (!MemRead::SafeReadInt(msg, &msgCase) || msgCase != 1) return;  // case 1 = surface birth

    std::wstring text = GameText::Decode(
        reinterpret_cast<const uint8_t*>(reinterpret_cast<char*>(surface) + OFF_SURF_TEXT), 0x400);
    if (!GameText::IsMostlyPrintable(text)) return;

    uint8_t  count = 0xFF;
    uint32_t flags = 0;
    MemRead::SafeReadU8(surface, OFF_SURF_COUNT, &count);
    MemRead::SafeReadU32(surface, OFF_SURF_FLAGS, &flags);

    // INFO (item/treasure/battle-system) vs confirm/choice. At case-1 the producer hasn't
    // registered its choices yet, so `count` is 0 for every surface — the passive-text flag
    // `surface[0x630] & 0x8` (set during case-1 from the producer) is the live discriminator.
    // `count == 0` is kept for the post-registration invariant; it's inert here (always 0).
    bool isInfo = (count == 0) && ((flags & 0x8) != 0);
    if (isInfo) {
        SpeakAndStash(text, "panel: ");
        return;
    }
    // Confirm / multi-choice. Birth is the ONLY moment the composed prompt (with its substituted
    // parameter) is readable, so stash it here for MenuReader to speak as the pop-up preamble —
    // speaking it ourselves would be cut off by the Yes/No focus that fires immediately after.
    {
        std::lock_guard<std::mutex> lk(g_confirmMutex);
        g_confirmPrompt = text;
    }
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "panel[confirm/choice count=%u flags=0x%x]: ",
             (unsigned)count, (unsigned)flags);
    Log::WriteW("MSGTEXT", hdr, text);
    if (kSpeakSurfaceConfirms) SpeakAndStash(text, "panel(confirm): ");
}

// ---- detours (all: run the original first, then read the now-populated state) ----
uintptr_t HookedItemPopup(void* widget, void* msg) {
    uintptr_t ret = s_origItemPopup ? s_origItemPopup(widget, msg) : 0;
    STALL_SCOPE("MessageReader::ItemPopup");
    static bool s_firstFire = true;
    if (s_firstFire) { s_firstFire = false; Log::Write("MSGTEXT", "diag: item popup proc FUN_0035e070 fired"); }
    int msgCase = 0;
    if (msg && MemRead::SafeReadInt(msg, &msgCase)) {
        if (msgCase == 1) {                       // case 1 = build/compose
            g_toastLive.store(true, std::memory_order_relaxed);
            OnItemPopup(widget);
        } else if (msgCase == 2) {                // case 2 = the animation ended, widget self-destructs
            g_toastLive.store(false, std::memory_order_relaxed);
        }
    }
    return ret;
}

uintptr_t HookedPanel(void* surface, void* msg) {
    uintptr_t ret = s_origPanel ? s_origPanel(surface, msg) : 0;
    STALL_SCOPE("MessageReader::Panel");
    static bool s_firstFire = true;
    if (s_firstFire) { s_firstFire = false; Log::Write("MSGTEXT", "diag: panel proc FUN_0057c480 fired"); }
    OnPanelSurface(surface, msg);
    return ret;
}

// Is one of the surfaces `g_lastLine` can come from actually on screen right now?
//
// THE BUG THIS CLOSES. `t` was never "repeat whatever the screen reader last said" -- only three
// call sites ever wrote `g_lastLine` (the item toast, the menu system-message panel, and each
// dialogue page), so its SCOPE was right from the start. What it had no notion of was a LIFETIME.
// Nothing cleared the string: not the end of a message, not a map change, not Shutdown. So a
// conversation that ended an hour ago was still what `t` spoke, in the field, in menus, mid-battle.
//
// Three sources, three answers:
//   * a paginated message box -- DialogueReader::IsBoxLive, the game's own message-window registry;
//   * a choice / confirm prompt -- MenuState's already-validated class checks;
//   * the obtained-item toast -- it has NO predicate to ask. FUN_0035e070's widget self-destructs
//     on its case-2 animation end, so there is nothing left to interrogate; the toast arms a latch
//     at birth and the destruct clears it. That is the only one of the three that needs state, and
//     it is state with a defined death rather than a string that lives forever.
bool ASurfaceIsLive() {
    if (DialogueReader::IsBoxLive()) return true;
    if (g_toastLive.load(std::memory_order_relaxed)) return true;
    void* owner = MenuState::FocusedOwner();
    return owner && (MenuState::IsChoicePopup(owner) || MenuState::IsConfirmWindow(owner));
}

// Re-read key (`t`) — runs on the input thread. Speaks the last DIALOGUE or PROMPT line, and only
// while that dialogue or prompt is still up.
//
// SILENCE IS THE CORRECT ANSWER when nothing is open. CLAUDE.md: never speak filler, be silent --
// the reason goes to the log, never to speech.
// TWO INDEPENDENT GUARDS, and the log says which one fired. That is deliberate.
//
// The gate is the live-surface test; the backstop is that `g_lastLine` is now cleared when a box
// ends. Either alone would produce the right silence, and if only one of them is doing the work
// that is worth knowing -- particularly for the gate, whose weak point is unmeasured: if the game
// leaves stale window pointers in `DAT_0215f200` the way it leaves one in `DAT_0208ebc0`, the gate
// would answer "live" forever and only the cleared store would be holding the line. A play log with
// `t` pressed in the field after a conversation distinguishes them in one press.
void OnRereadKey() {
    const bool live = ASurfaceIsLive();
    std::wstring line;
    {
        std::lock_guard<std::mutex> lk(g_lastMutex);
        line = g_lastLine;
    }
    if (!live) {
        Log::Write("MSGTEXT", line.empty()
            ? "'t': silent — no live surface, and the store is empty (both guards agree)"
            : "'t': silent — no dialogue, prompt or toast on screen (the GATE held)");
        return;
    }
    if (line.empty()) {
        // The registry says a box is up but nothing has been spoken to the store. Either the box
        // has not reached its first page yet, or the gate is reading a stale registry slot.
        Log::Write("MSGTEXT", "'t': silent — a surface is live but the store is empty (the CLEAR held)");
        return;
    }
    Speech::Output(line, /*interrupt=*/true);
}

} // namespace

namespace MessageReader {

void NoteSpoken(const std::wstring& text) {
    std::lock_guard<std::mutex> lk(g_lastMutex);
    g_lastLine = text;
}

void ForgetLastLine() {
    std::lock_guard<std::mutex> lk(g_lastMutex);
    g_lastLine.clear();
    g_toastLive.store(false, std::memory_order_relaxed);
}

bool Init() {
    if (g_initialized) {
        Log::Write("MSGTEXT", "MessageReader::Init called twice — ignoring");
        return true;
    }
    InputTracker::SetRereadCallback(&OnRereadKey);
    bool ok = Hooks::InstallTyped(RVA_ITEMPOPUP, &HookedItemPopup, &s_origItemPopup);
    ok     &= Hooks::InstallTyped(RVA_PANEL,     &HookedPanel,     &s_origPanel);
    g_initialized = true;
    Log::Write("MSGTEXT", ok
        ? "MessageReader initialized (obtained-item toast via FUN_0035e070+0xC8; menu system "
          "messages; 't' re-reads last line; menu confirms muted). Dialogue: dialogue_reader."
        : "MessageReader: a hook failed to install — see Hooks log.");
    return ok;
}

void Shutdown() {
    if (!g_initialized) return;
    InputTracker::SetRereadCallback(nullptr);
    Hooks::Uninstall(RVA_PANEL);
    Hooks::Uninstall(RVA_ITEMPOPUP);
    g_initialized = false;
    {
        std::lock_guard<std::mutex> lk(g_confirmMutex);
        g_confirmPrompt.clear();
    }
    ForgetLastLine();
    Log::Write("MSGTEXT", "MessageReader shut down");
}

std::wstring TakeConfirmPrompt() {
    std::lock_guard<std::mutex> lk(g_confirmMutex);
    std::wstring s;
    s.swap(g_confirmPrompt);          // consume: one prompt speaks once
    return s;
}

} // namespace MessageReader

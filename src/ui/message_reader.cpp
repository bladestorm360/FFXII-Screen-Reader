#include "ui/message_reader.h"
#include "core/mem_read.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "speech/speech.h"
#include "core/logger.h"
#include "input/input_tracker.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

// Message-text reader. Two surfaces, both decoded from the game's own codec bytes:
//
//  A. NPC dialogue + in-engine cutscene captions — the e5f0 message window.
//     - FUN_003c02b0(msgId, out) resolves a line; out+8 = codec body ptr, out+2 = attr.
//       We OBSERVE it (like the menu id-resolver hook) and cache the body by id.
//     - FUN_002baf80(page, msg) is the per-page proc; page+0x138 = current line id. On the
//       id edge (a new line displayed) we speak the cached body, prefixed with the speaker
//       name read from the nameplate widget DAT_02b62d78.
//
//  B. Item acquired / treasure / battle system lines — the FUN_0057c480 surface. On its
//     case-1 (birth) the composed codec text sits at surface+0x1B0. surface+0x636 (choice
//     count) + surface+0x630 (producer flags) classify it: count 0 & passive-flag = an
//     informational line we speak; anything with choices is a yes/no confirm / multi-choice
//     dialog — a DIFFERENT mechanism than the already-handled title/new-game confirms, so we
//     classify + log it but stay silent (kSpeakSurfaceConfirms) to avoid double-speak.
//
// All read points are offline-derived from the decompile (>=0.98); see
// FFXII-Decompile\notes\message_text_readpoints_spec.md. No game functions are called; every
// dereference is SEH-guarded (MemRead).
namespace {

// ---- offline-derived RVAs / globals (abs = RVA + 0x120000) -------------------
constexpr uint32_t RVA_RESOLVE   = 0x2A02B0;  // FUN_003c02b0(msgId, out)     dialogue body resolver
constexpr uint32_t RVA_DLG_PAGE  = 0x19AF80;  // FUN_002baf80(page, msg)      dialogue page proc
constexpr uint32_t RVA_PANEL     = 0x45C480;  // FUN_0057c480(surface, msg)   system-message surface
constexpr uint32_t RVA_TELOP     = 0x1C16B0;  // FUN_002e16b0(ctx,slot,text,_) telop/tutorial overlay content setter
constexpr uint32_t RVA_NAMEPLATE = 0x2962D78; // DAT_02b62d78 (ptr global)    speaker nameplate widget

constexpr uint32_t OFF_PAGE_MSGID  = 0x138;  // page    -> current line msg id (s16)
constexpr uint32_t OFF_OUT_BODY    = 0x8;    // resolver out -> codec body ptr
constexpr uint32_t OFF_NAME_CHILDS = 0x60;   // nameplate -> child-array ptr
constexpr uint32_t OFF_NAME_TEXT   = 0x18;   // child[0]  -> codec name ptr
constexpr uint32_t OFF_NAME_FLAGS  = 0x40;   // nameplate -> visible/enable flags
constexpr uint32_t OFF_SURF_TEXT   = 0x1B0;  // surface -> 0x400-byte codec buffer
constexpr uint32_t OFF_SURF_COUNT  = 0x636;  // surface -> choice count (0 = passive info)
constexpr uint32_t OFF_SURF_FLAGS  = 0x630;  // surface -> producer flags; bit3 (0x8) = passive

// Nameplate draw gate (FUN_00245e60): (flags & 0x405) == 5 means visible/enabled. Used so a
// stale name from a previous speaker is never spoken on a line that has no on-screen caption.
constexpr uint32_t NAME_VIS_MASK = 0x405;
constexpr uint32_t NAME_VIS_VAL  = 0x005;

// Directive 3: the FUN_0057c480 yes/no confirms are a distinct class from the already-working
// title/new-game confirms (FUN_00241d40); keep the read-point wired + logged but SILENT unless
// deliberately enabled after testing surfaces an in-game pop-up the existing handler misses.
constexpr bool kSpeakSurfaceConfirms = false;

// ---- hook trampolines --------------------------------------------------------
typedef uintptr_t (*Pfn_Resolve)(uintptr_t msgId, void* out);
typedef uintptr_t (*Pfn_Page)(void* page, void* msg);
typedef uintptr_t (*Pfn_Panel)(void* surface, void* msg);
typedef int       (*Pfn_Telop)(void* ctx, int slot, void* text, void* p4);
Pfn_Resolve s_origResolve = nullptr;
Pfn_Page    s_origPage    = nullptr;
Pfn_Panel   s_origPanel   = nullptr;
Pfn_Telop   s_origTelop   = nullptr;

// ---- state (game thread, unless noted) ---------------------------------------
// Body cached by the resolver hook, consumed by the page hook. Both run synchronously on the
// game thread within the same page build, so no lock is needed.
uint16_t       g_pendingMsgId = 0xFFFF;
const uint8_t* g_pendingBody  = nullptr;   // raw codec ptr; decoded lazily on display
bool           g_havePending  = false;

// Per-page last-SPOKEN line id — advanced only when we actually speak, so a page pump that
// sets the id before the body is resolved doesn't consume the edge (two page buffers => tiny map).
std::unordered_map<void*, int> g_lastSpokenByPage;

// Last spoken line, for the `r` re-read key. Written on the game thread, read on the input thread.
std::mutex   g_lastMutex;
std::wstring g_lastLine;

bool g_initialized = false;

void LogLine(const char* prefix, const std::wstring& text) {
    char utf8[600] = {};
    if (!text.empty())
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    char line[720];
    snprintf(line, sizeof(line), "%s\"%s\"", prefix, utf8);
    Log::Write("MSGTEXT", line);
}

void SpeakAndStash(const std::wstring& text, const char* logPrefix) {
    {
        std::lock_guard<std::mutex> lk(g_lastMutex);
        g_lastLine = text;
    }
    LogLine(logPrefix, text);
    Speech::Output(text, /*interrupt=*/true);
}

// Speaker/caption for the current dialogue line, read memory-only from the nameplate widget.
// Empty when there is no on-screen caption (gated on the widget's visibility, so a stale name
// is never prepended). Never a hard-coded string — it's the game's own codec bytes.
std::wstring ReadSpeaker() {
    void* nameplate = MemRead::PtrAt(Hooks::ResolveRva(RVA_NAMEPLATE), 0);
    if (!nameplate) return std::wstring();
    uint32_t flags = 0;
    if (!MemRead::SafeReadU32(nameplate, OFF_NAME_FLAGS, &flags) ||
        (flags & NAME_VIS_MASK) != NAME_VIS_VAL)
        return std::wstring();
    void* childArr = MemRead::PtrAt(nameplate, OFF_NAME_CHILDS);
    void* child0   = childArr ? MemRead::PtrAt(childArr, 0) : nullptr;
    void* namePtr  = child0 ? MemRead::PtrAt(child0, OFF_NAME_TEXT) : nullptr;
    if (!namePtr) return std::wstring();
    std::wstring s = GameText::Decode(reinterpret_cast<const uint8_t*>(namePtr), 64);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// A dialogue page was pumped — speak its line if a new one is now displayed.
void OnDialoguePage(void* page) {
    if (!page) return;
    int16_t msgId = 0;
    if (!MemRead::SafeReadS16(page, OFF_PAGE_MSGID, &msgId)) return;
    if (msgId < 0) return;  // -1 = no line

    // Diagnostic (file-only): trace every distinct line id the page proc processes, so a
    // play-test log shows whether dialogue fires at all vs. our capture missing it.
    static int s_diagLastMsgId = -2;
    if (msgId != s_diagLastMsgId) {
        s_diagLastMsgId = msgId;
        char d[112];
        snprintf(d, sizeof(d), "diag: dlg page line msgId=%d havePending=%d pendingId=%u",
                 (int)msgId, g_havePending ? 1 : 0, (unsigned)g_pendingMsgId);
        Log::Write("MSGTEXT", d);
    }

    // Already spoke this line on this page? (Edge = state tracking of a NEW line, not a
    // debounce.) The edge advances only when we actually speak — see below — so a pump that
    // sets the id before layout resolves the body doesn't swallow the line.
    auto it = g_lastSpokenByPage.find(page);
    if (it != g_lastSpokenByPage.end() && it->second == msgId) return;

    // Speak only once the body for THIS id has been resolved (the resolver hook caches it
    // during the page's layout pass). If it isn't ready yet, DON'T record — a later pump will.
    if (!g_havePending || g_pendingMsgId != static_cast<uint16_t>(msgId) || !g_pendingBody) return;
    std::wstring body = GameText::Decode(g_pendingBody, 512);
    if (!GameText::IsMostlyPrintable(body)) return;

    if (g_lastSpokenByPage.size() > 32) g_lastSpokenByPage.clear();  // guard vs root reallocation
    g_lastSpokenByPage[page] = msgId;

    std::wstring speaker = ReadSpeaker();
    std::wstring line = speaker.empty() ? body : (speaker + L": " + body);
    SpeakAndStash(line, "dialogue: ");
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
    // Confirm / multi-choice: preserved read-point, classified + logged, but silent.
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "panel[muted confirm/choice count=%u flags=0x%x]: ",
             (unsigned)count, (unsigned)flags);
    LogLine(hdr, text);
    if (kSpeakSurfaceConfirms) SpeakAndStash(text, "panel(confirm): ");
}

// A telop / on-screen tutorial overlay slot had its content set (FUN_002e16b0 param_3 = codec
// text; null = clear). The string is HEADER<0x02>BODY, e.g. "TUTORIAL\nTry using ... to adjust
// the viewing angle" (0x02 -> newline). Distinct from dialogue/panel/help surfaces. NOTE:
// button-icon inserts (0x0f escapes) currently decode to nothing, so key/button glyphs are
// dropped for now — a follow-up will map them to names.
void OnTelop(void* text, int slot) {
    if (!text) return;  // null = clear
    std::wstring s = GameText::Decode(reinterpret_cast<const uint8_t*>(text), 512);
    if (!GameText::IsMostlyPrintable(s)) return;
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "telop[slot=%d]: ", slot);
    SpeakAndStash(s, hdr);
}

// ---- detours (all: run the original first, then read the now-populated state) ----
uintptr_t HookedResolve(uintptr_t msgId, void* out) {
    uintptr_t ret = s_origResolve ? s_origResolve(msgId, out) : 0;
    static bool s_firstFire = true;
    if (s_firstFire) { s_firstFire = false; Log::Write("MSGTEXT", "diag: resolver FUN_003c02b0 fired (msg-DB active)"); }
    if (out) {
        void* bodyPtr = nullptr;
        if (MemRead::SafeReadPtr(reinterpret_cast<char*>(out) + OFF_OUT_BODY, &bodyPtr)) {
            g_pendingMsgId = static_cast<uint16_t>(msgId & 0xFFFF);
            g_pendingBody  = reinterpret_cast<const uint8_t*>(bodyPtr);
            g_havePending  = true;
        }
    }
    return ret;
}

uintptr_t HookedPage(void* page, void* msg) {
    uintptr_t ret = s_origPage ? s_origPage(page, msg) : 0;
    static bool s_firstFire = true;
    if (s_firstFire) { s_firstFire = false; Log::Write("MSGTEXT", "diag: dialogue page proc FUN_002baf80 fired (e5f0 active)"); }
    OnDialoguePage(page);
    return ret;
}

uintptr_t HookedPanel(void* surface, void* msg) {
    uintptr_t ret = s_origPanel ? s_origPanel(surface, msg) : 0;
    static bool s_firstFire = true;
    if (s_firstFire) { s_firstFire = false; Log::Write("MSGTEXT", "diag: panel proc FUN_0057c480 fired"); }
    OnPanelSurface(surface, msg);
    return ret;
}

int HookedTelop(void* ctx, int slot, void* text, void* p4) {
    int ret = s_origTelop ? s_origTelop(ctx, slot, text, p4) : 0;
    static bool s_firstFire = true;
    if (s_firstFire) { s_firstFire = false; Log::Write("MSGTEXT", "diag: telop setter FUN_002e16b0 fired"); }
    OnTelop(text, slot);
    return ret;
}

// Re-read key (`t`) — runs on the input thread. Repeats the last spoken line.
void OnRereadKey() {
    std::wstring line;
    {
        std::lock_guard<std::mutex> lk(g_lastMutex);
        line = g_lastLine;
    }
    if (!line.empty()) Speech::Output(line, /*interrupt=*/true);
}

} // namespace

namespace MessageReader {

bool Init() {
    if (g_initialized) {
        Log::Write("MSGTEXT", "MessageReader::Init called twice — ignoring");
        return true;
    }
    InputTracker::SetRereadCallback(&OnRereadKey);
    bool ok = Hooks::InstallTyped(RVA_RESOLVE,  &HookedResolve, &s_origResolve);
    ok     &= Hooks::InstallTyped(RVA_DLG_PAGE, &HookedPage,    &s_origPage);
    ok     &= Hooks::InstallTyped(RVA_PANEL,    &HookedPanel,   &s_origPanel);
    ok     &= Hooks::InstallTyped(RVA_TELOP,    &HookedTelop,   &s_origTelop);
    g_initialized = true;
    Log::Write("MSGTEXT", ok
        ? "MessageReader initialized (dialogue body+speaker via e5f0; item/treasure/battle-"
          "system panels; telop/tutorial overlay; 't' re-reads last line; surface confirms muted)."
        : "MessageReader: a hook failed to install — see Hooks log.");
    return ok;
}

void Shutdown() {
    if (!g_initialized) return;
    InputTracker::SetRereadCallback(nullptr);
    Hooks::Uninstall(RVA_TELOP);
    Hooks::Uninstall(RVA_PANEL);
    Hooks::Uninstall(RVA_DLG_PAGE);
    Hooks::Uninstall(RVA_RESOLVE);
    g_initialized = false;
    Log::Write("MSGTEXT", "MessageReader shut down");
}

} // namespace MessageReader

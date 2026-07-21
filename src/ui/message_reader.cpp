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


// Message-text reader. Surfaces, all decoded from the game's own codec bytes:
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
//  B. Telop / on-screen tutorial overlay — FUN_002e16b0. WORKS. Note it is a whole-message content
//     setter: it hands us speaker + EVERY page in one string, which is why multi-page screens read
//     all at once. Paginating it needs the consumer-side page state (not yet located).
//
//  C. Menu system messages — the FUN_0057c480 surface ("cannot equip", "sold"). Kept, but it is
//     MENU-ONLY: its case 1 does *(longlong*)(DAT_0209ac30 + 0x328) = surface, i.e. it registers
//     into the menu manager. It can never fire for field text, and in a 17-minute play log it never
//     fired at all. It is NOT the item/treasure path — that claim (spec'd at 0.90, below this
//     project's 0.98 bar) was wrong; surface A above is the real one.
//
// REMOVED — the e5f0 "dialogue" pair (FUN_003c02b0 resolver + FUN_002baf80 page proc). They are the
// WORLD MAP screen, not dialogue: page+0x138 is a MAP id and the resolver's out+8 is a MAP NAME
// (DAT_02b457e0+0xe8 is the map DB — FUN_003be680(id,&w,&h) returns width/height and callers
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
constexpr uint32_t RVA_TELOP     = 0x1C16B0;  // FUN_002e16b0(ctx,slot,text,_) telop/tutorial overlay content setter

constexpr uint32_t OFF_POPUP_TEXT  = 0xC8;   // item popup -> composed codec text (FUN_002b4090 dest)
constexpr uint32_t POPUP_TEXT_CAP  = 0x4A0;  // ...its sprintf capacity
constexpr uint32_t OFF_SURF_TEXT   = 0x1B0;  // surface -> 0x400-byte codec buffer
constexpr uint32_t OFF_SURF_COUNT  = 0x636;  // surface -> choice count (0 = passive info)
constexpr uint32_t OFF_SURF_FLAGS  = 0x630;  // surface -> producer flags; bit3 (0x8) = passive

// (The speaker-nameplate reader — DAT_02b62d78 + the (flags & 0x405) == 5 draw gate — went with the
//  e5f0 pair above: it existed only to prefix those "dialogue" lines, which were map names. When a
//  real dialogue window is located, re-derive the speaker source against THAT widget rather than
//  assuming this global belongs to it.)

// Directive 3: the FUN_0057c480 yes/no confirms are a distinct class from the already-working
// title/new-game confirms (FUN_00241d40); keep the read-point wired + logged but SILENT unless
// deliberately enabled after testing surfaces an in-game pop-up the existing handler misses.
constexpr bool kSpeakSurfaceConfirms = false;

// ---- hook trampolines --------------------------------------------------------
typedef uintptr_t (*Pfn_ItemPopup)(void* widget, void* msg);
typedef uintptr_t (*Pfn_Panel)(void* surface, void* msg);
typedef int       (*Pfn_Telop)(void* ctx, int slot, void* text, void* p4);
Pfn_ItemPopup s_origItemPopup = nullptr;
Pfn_Panel     s_origPanel     = nullptr;
Pfn_Telop     s_origTelop     = nullptr;

// ---- state (game thread, unless noted) ---------------------------------------
// Last spoken line, for the `t` re-read key. Written on the game thread, read on the input thread.
std::mutex   g_lastMutex;
std::wstring g_lastLine;

bool g_initialized = false;

void SpeakAndStash(const std::wstring& text, const char* logPrefix) {
    {
        std::lock_guard<std::mutex> lk(g_lastMutex);
        g_lastLine = text;
    }
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
    // Confirm / multi-choice: preserved read-point, classified + logged, but silent.
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "panel[muted confirm/choice count=%u flags=0x%x]: ",
             (unsigned)count, (unsigned)flags);
    Log::WriteW("MSGTEXT", hdr, text);
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
uintptr_t HookedItemPopup(void* widget, void* msg) {
    uintptr_t ret = s_origItemPopup ? s_origItemPopup(widget, msg) : 0;
    static bool s_firstFire = true;
    if (s_firstFire) { s_firstFire = false; Log::Write("MSGTEXT", "diag: item popup proc FUN_0035e070 fired"); }
    int msgCase = 0;
    if (msg && MemRead::SafeReadInt(msg, &msgCase) && msgCase == 1)   // case 1 = build/compose
        OnItemPopup(widget);
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
    bool ok = Hooks::InstallTyped(RVA_ITEMPOPUP, &HookedItemPopup, &s_origItemPopup);
    ok     &= Hooks::InstallTyped(RVA_PANEL,     &HookedPanel,     &s_origPanel);
    ok     &= Hooks::InstallTyped(RVA_TELOP,     &HookedTelop,     &s_origTelop);
    g_initialized = true;
    Log::Write("MSGTEXT", ok
        ? "MessageReader initialized (obtained-item toast via FUN_0035e070+0xC8; telop/tutorial "
          "overlay; menu system messages; 't' re-reads last line; menu confirms muted)."
        : "MessageReader: a hook failed to install — see Hooks log.");
    return ok;
}

void Shutdown() {
    if (!g_initialized) return;
    InputTracker::SetRereadCallback(nullptr);
    Hooks::Uninstall(RVA_TELOP);
    Hooks::Uninstall(RVA_PANEL);
    Hooks::Uninstall(RVA_ITEMPOPUP);
    g_initialized = false;
    Log::Write("MSGTEXT", "MessageReader shut down");
}

} // namespace MessageReader

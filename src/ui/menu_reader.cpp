#include "ui/menu_reader.h"
#include "ui/text_capture.h"
#include "ui/menu_state.h"
#include "ui/config_reader.h"
#include "ui/ingame_menu_reader.h"
#include "ui/char_select_reader.h"
#include "ui/license_reader.h"
#include "ui/choice_reader.h"
#include "ui/gambit_reader.h"
#include "ui/ability_summary_reader.h"
#include "ui/shop_reader.h"
#include "ui/equip_compare.h"
#include "ui/equip_target_reader.h"
#include "ui/inventory_reader.h"
#include "ui/save_reader.h"
#include "ui/primer_reader.h"
#include "ui/gil_reader.h"
#include "ui/status_reader.h"
#include "ui/popup_reader.h"
#include "ui/battle_target_reader.h"
#include "core/game_text.h"
#include "core/message_macro.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "speech/speech.h"
#include "core/logger.h"
#include "core/stall_probe.h"
#include "input/input_tracker.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <string>

namespace {

// Universal focus signal: FUN_00247510(owner, msg, val). msg 0x8000 = "cursor
// moved to item <val>". It carries distinct cases we tell apart by the owner
// object's handler pointer (obj[0]):
//   * a config screen (obj[0]==FUN_0023fbe0) -> speak the focused row's name and,
//     for a value row (FUN_0023d6b0), append its current value (the highlighted
//     option child's label);
//   * a confirm/quit pop-up (obj[0]==FUN_00241d40) -> speak the body prompt
//     (once) and the focused Yes/No button (id 1000/1001, code-fixed by index);
//   * any other list menu -> speak the focused row's name.
// A config value change (left/right) sends NO focus message; it writes the store
// via FUN_00240750, which we hook to announce the new value.
// NO option-name strings live here — everything spoken is the game's own text.
constexpr uint32_t RVA_DISPATCH     = 0x127510;   // FUN_00247510(owner, msg, val)
// Active-pane isolation: DAT_0208ebc0 holds the window the input pump routes the D-pad to (the
// focused pane); the `owner` of a 0x8000 IS that pane's controller, so owner == *DAT_0208ebc0
// means "this pane holds the cursor". FUN_00244830(old,new,flag) sets it (DAT_0208ebc0 = new) and
// fires just AFTER a pane's entry 0x8000, so we hook it to replay the just-entered focus item.
constexpr uint32_t RVA_FOCUS_SET    = 0x124830;   // FUN_00244830(old, new, flag)

// New-game / config screen (probe- + log-confirmed 2026-07-06). Controller =
// FUN_0023fbe0; the row array is at ctrl+0xE8 (stride 0x18). Value-setting rows come
// in two layouts (per FUN_0023ed80's type switch): types 1/2/8 (On/Off etc.) =
// FUN_0023e770, type 3 = FUN_0023d6b0. A left/right change writes FUN_00240750 with
// no focus message, so that write is the on-change announce trigger.
constexpr uint32_t RVA_STORE_WRITE = 0x120750;  // FUN_00240750(configId, &newValue) — config-store change
constexpr uint32_t RVA_GFX_WRITE   = 0x5DB90;   // FUN_0017db90(configId, curVal, dir) -> newVal — Graphics change
constexpr uint32_t OFF_GFX_ROW_CFGID = 0xC0;    // Graphics value row -> config id (int)

constexpr uint32_t OFF_ROW_CFGID   = 0xC0;    // value row -> config id (u8) — matched against the store write

constexpr uint64_t MSG_FOCUS  = 0x8000;
constexpr uint64_t MSG_YES    = 0x8100;      // no-list 2-choice pop-up results (owner = parent)
constexpr uint64_t MSG_NO     = 0x8101;
constexpr uint64_t MSG_CANCEL = 0x8102;

typedef uintptr_t (*Pfn_Dispatch)(void*, uintptr_t, uintptr_t);
Pfn_Dispatch s_origDispatch = nullptr;

typedef void (*Pfn_StoreWrite)(uintptr_t, void*);   // FUN_00240750(configId, &newDisplayIdx)
Pfn_StoreWrite s_origStoreWrite = nullptr;

typedef uint32_t (*Pfn_GfxWrite)(uint32_t, uint32_t, uint32_t);  // FUN_0017db90(configId, curVal, dir)
Pfn_GfxWrite s_origGfxWrite = nullptr;

typedef void (*Pfn_FocusSet)(void*, void*, int);   // FUN_00244830(old, new, flag)
Pfn_FocusSet s_origFocusSet = nullptr;

std::mutex g_mutex;
// The CURRENTLY-focused row. NOT a dedup key — the config value-change hooks
// (HookedStoreWrite / HookedGfxWrite) get no focus message of their own, so they read these to
// learn which row the value belongs to.
void* g_focusOwner = nullptr;
int   g_focusIndex = -1;
void* g_pendingOwner = nullptr;   // focus whose text wasn't painted yet (menu-entry replay)
int   g_pendingIndex = -1;
// THE REPLAY'S RETRY BUDGET, and why it cannot live in g_pendingOwner.
//
// OnMenuPainted CLEARS the pending slot before it re-invokes OnFocus, so by the time the replay
// runs there is nothing left to compare against -- the pending pair cannot also serve as "have I
// already retried this one". These two survive that clear, which is what makes a bounded retry
// possible at all. Reset when a different surface appears, or when text finally arrives.
void* g_retryOwner = nullptr;
int   g_retryCount = 0;
// One paint is often not enough: the paint that fires the callback need not be the paint that fills
// THIS owner's item map. Small, because the honest cases settle in one or two.
constexpr int kMaxPaintRetries = 8;
void* g_valueChangeOwner = nullptr;   // Graphics value change awaiting a settled paint to announce
int   g_valueChangeIndex = -1;
// Last 0x8000 focus, stashed so the FUN_00244830 focus-change hook can replay the entry item
// once DAT_0208ebc0 flips to the newly-entered pane (the entry 0x8000 fires just before that).
void* g_stashOwner = nullptr;
int   g_stashIndex = -1;
uint32_t g_stashRowOff = 0;
// (There is deliberately no deferred/armed menu-entry announce here any more. Holding the row back
// for a "menu is ready" signal was tried four ways and every one was refuted by measurement -- see
// the note on HookedFocusSet below for what the game's own window messages actually show.)

// Armed when a pop-up pane GAINS the cursor (FUN_00244830), consumed when its body is spoken.
// This exists because the previous trigger was `ownerChanged`, which is an IDENTITY test: the game
// RECYCLES pop-up window addresses, so the second "return to the title screen?" -- and the quit
// prompt after it -- reused the same address, `ownerChanged` was false, and the prompt never spoke
// again. Re-entering a surface must always announce (CLAUDE.md, NO DEDUPLICATION OF SPEECH); this
// is an entry latch keyed on the game's own focus-change event, not a suppression filter.
bool g_popupEntryArmed = false;

void* g_diagOwner = nullptr;       // active-pane diagnostic dedup (owner, focus) pair
void* g_diagFocus = nullptr;
bool  g_initialized = false;

// ---- SEH-guarded raw reads (game objects can be destructed asynchronously) ---
// The guard logic lives once in core/mem_read.h (shared with the message reader).
using MemRead::SafeReadPtr;
using MemRead::PtrAt;
using MemRead::Obj0;
using MemRead::SafeReadU8;
using MemRead::SafeReadU32;
using MemRead::SafeReadInt;

// Surface identity, the focused pane and config row lookup: ui/menu_state.h.
// Config row VALUES (enum label / slider number / key binding): ui/config_reader.h.
// This file keeps the hooks and the speech decisions; it no longer owns either.
using MenuState::ConfigRowWidget;
using MenuState::IsActiveConfig;
using MenuState::IsConfigController;
using MenuState::IsConfigValueRow;
using MenuState::IsConfirmWindow;
using MenuState::IsFocusedPane;
using MenuState::IsTitleMenu;

// Prompt body + button labels live in ui/popup_reader.h (both prompt classes).

// On-demand describe key ('i'): speak the focused item's help/description that the
// game placed in the description bar (captured in TextCapture). Silent if the
// current item has none (silence beats a wrong or invented string). Runs on the
// input thread.
void DescribeHotkey() {
    std::wstring desc = TextCapture::CurrentHelpText();
    if (desc.empty()) return;
    Log::WriteW("READER", "  describe: ", desc);
    Speech::Output(desc, /*interrupt=*/true);
}

void OnFocus(void* owner, int index, bool fromPaint) {
    STALL_SCOPE("MenuReader::OnFocus");
    if (index < 0) return;
    if (IsTitleMenu(owner)) return;               // TitleReader handles the title command menu

    // 1-frame settle: defer config-row speech to the next paint so a scrolled-in row
    // has settled text + value (fixes the occasional missed/stale read on fast scroll).
    // Non-config surfaces (pop-ups, etc.) still speak immediately.
    if (!fromPaint && IsConfigController(owner)) {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_pendingOwner = owner;
        g_pendingIndex = index;
        return;
    }

    // Both prompt classes. Treating the generic Yes/No prompt (FUN_002cdf20) as a pop-up is what
    // stops its buttons falling through to the content path, where a RECYCLED owner address
    // returned stale item text captured from the previous surface (the license-board preview).
    const bool isPopup = IsConfirmWindow(owner) || MenuState::IsChoicePopup(owner);

    // Active-pane gate: a plain in-game content pane (not a pop-up, not a config controller)
    // speaks only when it currently holds the cursor — this is what stops the inventory "mixed"
    // reading (items/magicks/equipment from several panes at once). Pop-ups and config own focus
    // and are exempt. Entering a pane is handled by the FUN_00244830 replay, which re-invokes this
    // (fromPaint) once DAT_0208ebc0 has flipped to the entered pane.
    if (!isPopup && !IsConfigController(owner) && !IsFocusedPane(owner)) return;

    // ONE-PAINT SETTLE, CLAN PRIMER ONLY.
    //
    // Its lists are long enough to SCROLL, and wrapping from the top to the bottom moves every
    // visible row at once. The focus message arrives first, so the captured cells still hold the
    // rows that were on screen a moment ago and the announcement is stale -- text that is real, and
    // belongs to a different row. It settles within a frame, which is why it only ever sounded like a
    // glitch rather than a wrong answer.
    //
    // Rather than a timer or a frame counter, this reuses the machinery already here: stash the focus
    // and let TextCapture's paint callback replay it once the painter has refilled the item map. The
    // replay arrives with fromPaint=true and does the real work below, so this is a DEFERRAL, not a
    // second code path.
    //
    // SCOPED TO THE PRIMER on purpose. Every other menu in the game reads correctly today and a
    // deferral is not free: if a focus somehow never triggers a repaint, the announcement waits for a
    // paint that does not come. A scrolling list always repaints, which is exactly why this is gated
    // on the surfaces that scroll and not applied globally.
    if (!fromPaint && PrimerReader::OwnsSurface(owner)) {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_pendingOwner = owner;
        g_pendingIndex = index;
        return;
    }

    // CLAIMING READERS GO HERE, PAST THE PANE GATE ABOVE -- one place, both entry paths.
    //
    // There are TWO ways into this function: the 0x8000 dispatch, and a REPLAY (fromPaint) from
    // OnMenuPainted / HookedFocusSet. Claiming a row in the dispatch chain instead of here got both
    // halves wrong at once:
    //   * the save list announced TWICE -- once in full from the chain, once bare from the replay
    //     ("Bhujerba: Miners' End, 26 hours ..." then "Bhujerba: Miners' End"), because the chain ran
    //     on a pane that did not yet hold the cursor and the replay then did the job again;
    //   * and every Clan Primer sub-screen was SILENT ON ENTRY, because the entry 0x8000 arrives
    //     before the list has painted -- the hunts row cache was still empty, the reader declined, and
    //     the replay that would have caught it once the cache filled had been told to stand down.
    //
    // Putting the claim below the pane gate fixes both by construction: on an unfocused pane this
    // function returns above and NOBODY speaks, so the replay is the single announcer; on a focused
    // one the dispatch is. Exactly one speaker either way, and a reader that declines still falls
    // through to the generic path (an empty save slot must still get the game's own "empty file"
    // string).
    if (PrimerReader::OnHuntFocus(owner, index)) return;
    if (SaveReader::TryFocus(owner, index)) return;

    // Build what we'll speak: pop-up button label (code-fixed by index), or the
    // focused row's "name" / "name: value".
    std::wstring text;
    if (isPopup) {
        text = PopupReader::ButtonText(index);            // the game's own Yes/No strings
    } else {
        text = TextCapture::FocusedItemText(owner, index);            // row name
        // Append the setting's value — only when this is the ACTIVE config menu, so we
        // never dereference a closed/freed menu's row widgets.
        if (!text.empty() && IsActiveConfig(owner)) {
            void* row = ConfigRowWidget(owner, index);
            std::wstring val = ConfigReader::RowValue(row);
            if (!val.empty()) { text += L": "; text += val; }
        }
    }

    // Record the focus and speak it. NO dedup: this used to drop a focus matching the cached
    // (owner, index, text), which made leaving a pane and returning to the same row SILENT — the
    // pane gate above returns early without updating the cache, so the stale entry survived the
    // excursion and swallowed the re-entry. `ownerChanged` is kept only to gate the pop-up body
    // preamble below, never to suppress the row itself.
    bool ownerChanged;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        ownerChanged = (owner != g_focusOwner);
        g_focusOwner = owner;
        g_focusIndex = index;
    }

    char hdr[160];
    snprintf(hdr, sizeof(hdr), "focus owner=%p index=%d%s%s",
             owner, index, ownerChanged ? " (new surface)" : "", isPopup ? " [popup]" : "");
    Log::Write("READER", hdr);

    // Pop-up body prompt: announce once on entry, before the button.
    bool preambleSpoken = false;
    bool popupEntry = false;
    if (isPopup) {
        std::lock_guard<std::mutex> lk(g_mutex);
        popupEntry = g_popupEntryArmed;
        g_popupEntryArmed = false;
    }
    if (isPopup && (ownerChanged || popupEntry)) {
        // One emit point for pop-up bodies, shared with PopupReader's no-list construction hook so
        // the two paths cannot drift apart on wording or interrupt policy.
        preambleSpoken = PopupReader::SpeakBody(owner);
    }

    if (text.empty()) {
        // Menu entry can fire the first focus before the painter fills the item
        // map (or before the button id is cached). Stash it; TextCapture's
        // paint callback replays this focus once the text is available.
        //
        // THE REPLAY USED TO GET EXACTLY ONE SHOT, AND THAT LOST ANNOUNCEMENTS OUTRIGHT (S126).
        // This stash was gated on `ownerChanged`, but `g_focusOwner` is assigned above, BEFORE this
        // branch -- so on the replay `ownerChanged` is already false, nothing was re-stashed, and
        // OnMenuPainted had cleared the pending slot on its way in. If that paint had not yet filled
        // this owner's item map, the row was gone for good and the player had to move the cursor to
        // get anything. Caught in a live log on the save-slot list: two consecutive
        // "(text not ready - awaiting paint)" for owner ...2C2A14C0 index 7, the second WITHOUT
        // "(new surface)", then silence. Retry until the text lands or the budget runs out.
        bool budgetLeft, firstGiveUp = false;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (owner != g_retryOwner) { g_retryOwner = owner; g_retryCount = 0; }
            budgetLeft = (g_retryCount < kMaxPaintRetries);
            if (budgetLeft) {
                ++g_retryCount;
                g_pendingOwner = owner;
                g_pendingIndex = index;
            } else if (g_retryCount == kMaxPaintRetries) {
                ++g_retryCount;              // step past the cap so the notice below logs ONCE
                firstGiveUp = true;
            }
        }
        if (!budgetLeft) {
            // Say so ONCE per surface rather than falling silent the way the old code did. A surface
            // whose text never paints is a real defect somewhere else, and this is the line that
            // names it instead of leaving a blank where an announcement should be.
            if (firstGiveUp)
                Log::Write("READER", "  TEXT NEVER PAINTED: gave up after the replay budget -- this surface is MUTE");
            return;
        }
        // NO ring dump here. This branch is the NORMAL entry sequence -- the focus routinely beats
        // the painter, which is the entire reason for the replay above -- and TextCapture's dump
        // holds TextCapture::g_mutex across 257 Log::Write calls. That is the same mutex the menu's
        // first paint needs on EVERY row (CellWrapper) and EVERY string (Capture), so the dump
        // serialized the menu's own paint behind our logging and the field menu took ~0.5s to open
        // while the battle menu -- which never reaches OnFocus -- stayed instant.
        //
        // Gating it to once-per-surface did not help: initial open is exactly when ownerChanged is
        // true. The dump is a development aid for "text NEVER arrived"; the one-liner below records
        // the event, and TextCapture::DumpRingToLog remains callable for deliberate diagnosis.
        Log::Write("READER", "  (text not ready — awaiting paint)");
        return;
    }

    // The text arrived, so release the retry budget: a LATER slow entry to this same surface must
    // start from a full budget rather than inherit a spent one.
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (owner == g_retryOwner) { g_retryOwner = nullptr; g_retryCount = 0; }
    }

    Log::WriteW("READER", "  item: ", text);
    Speech::Output(text, /*interrupt=*/!preambleSpoken);
}

// Fired right after the painter fills an owner's item map — replay a menu-entry
// focus whose text wasn't ready yet.
void OnMenuPainted(void* owner) {
    STALL_SCOPE("MenuReader::OnMenuPainted");
    // Focus-pending replay (deferred focus speech, incl. the 1-frame settle).
    void* pend; int idx;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        pend = g_pendingOwner; idx = g_pendingIndex;
    }
    if (owner == pend && idx >= 0) {
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_pendingOwner = nullptr; g_pendingIndex = -1;
        }
        OnFocus(owner, idx, /*fromPaint=*/true);
    }

    // Graphics value-change replay: announce just the new value once the row's display
    // text has settled on this draw (FUN_0017db90 marked the change).
    void* vcOwner; int vcIdx;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        vcOwner = g_valueChangeOwner; vcIdx = g_valueChangeIndex;
    }
    if (owner == vcOwner && vcIdx >= 0) {
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_valueChangeOwner = nullptr; g_valueChangeIndex = -1;
        }
        if (IsActiveConfig(owner)) {
            std::wstring val = ConfigReader::RowValue(ConfigRowWidget(owner, vcIdx));
            if (!val.empty()) {
                Log::WriteW("READER", "  value: ", val);
                Speech::Output(val, /*interrupt=*/true);
            }
        }
    }
}

uintptr_t HookedDispatch(void* owner, uintptr_t msg, uintptr_t val) {
    if (msg == MSG_FOCUS) {
        STALL_SCOPE("MenuReader::HookedDispatch");
        // Bump the tooltip focus generation BEFORE the game handles the focus, so
        // the description it sets (FUN_00291d80) during s_origDispatch is attributed
        // to this focus for the `o` key.
        TextCapture::NotifyFocusChanged();

        // License board / job-select ring have their own reader. The board's focus `val` is a
        // POINTER to the focused cell (not a row index), so hand it the raw 64-bit value before the
        // row-index paths below. NotifyFocusChanged (above) already bumped the help generation, so
        // the reader's ProvideHelpText for the `o` key binds to THIS focus. Still calls the original
        // dispatch (observe-only), identical to the fall-through return at the end.
        if (LicenseReader::OnDispatchFocus(owner, val))
            return s_origDispatch ? s_origDispatch(owner, msg, val) : 0;

        // Field dialogue / choice window (FUN_002a6190): the Hunt notice board and mid-dialogue
        // option prompts. It does NOT paint through FUN_002d28e0, so the generic content path has
        // no rows for it -- the options live in a 0x0E block inside the window's own codec string.
        // Not pane-gated: like the battle command menu this is its own surface, and the probe run
        // showed focusedPane == owner for it anyway.
        // Only claims the focus when it actually SPOKE. Returning unconditionally here was a
        // regression: it cut the generic painted-row path (TextCapture) out of the loop for this
        // window even when ChoiceReader had nothing to say, so a surface the painter might already
        // cover went silent because of a reader that failed.
        // The notice board's navigation. ChoiceReader also runs a per-frame tick for in-dialogue
        // choices, which send no message at all; whichever detector fires for the current
        // message/page claims it and the other stands down, and both speak through one choke point.
        // Claimed either way, so the generic painted-row path never also speaks this window.
        if (ChoiceReader::IsChoiceWindow(owner)) {
            ChoiceReader::OnFocus(owner, static_cast<int>(static_cast<intptr_t>(val)));
            return s_origDispatch ? s_origDispatch(owner, msg, val) : 0;
        }

        // Gambit setup screen (FUN_005691e0). `val` is the display-record index, not a row offset in
        // any ROW_CHAIN class, so the generic content path below has nothing for it.
        if (GambitReader::OnFocus(owner, static_cast<int>(static_cast<intptr_t>(val))))
            return s_origDispatch ? s_origDispatch(owner, msg, val) : 0;

        const int index = static_cast<int>(static_cast<intptr_t>(val));
        const uint32_t rowOff = IngameMenuReader::RowChainOff(owner);

        // Stash this focus so the FUN_00244830 focus-change hook can replay the entry item once
        // DAT_0208ebc0 flips to the entered pane (the entry 0x8000 fires just before that flip, so
        // it would otherwise be gated out). Also emit a deduped active-pane diagnostic.
        //
        // ARM IT ONLY WHEN THE PANE IS NOT YET FOCUSED -- that is exactly the case the replay
        // exists for. It used to be armed on EVERY 0x8000 and never cleared, so a later
        // FUN_00244830 (e.g. focus returning after a pop-up closed) replayed a focus the dispatch
        // path had ALREADY spoken: the row was announced twice, which meant two blocking
        // Tolk_Output(interrupt) calls on the game thread in one frame and a visible hitch on
        // opening the field menu. The battle command menu never had it because FUN_00244830 does
        // not replay that path -- which is why it always felt instant by comparison.
        void* focusWin = MenuState::FocusedOwner();
        const bool willBeGated = (owner != focusWin);
        bool diag;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (willBeGated) {
                g_stashOwner = owner; g_stashIndex = index; g_stashRowOff = rowOff;
            } else {
                g_stashOwner = nullptr; g_stashIndex = -1; g_stashRowOff = 0;
            }
            diag = (owner != g_diagOwner || focusWin != g_diagFocus);
            if (diag) { g_diagOwner = owner; g_diagFocus = focusWin; }
        }
        if (diag) {
            char d[128];
            snprintf(d, sizeof(d), "pane owner=%p focus=%p focused=%d rowOff=0x%X",
                     owner, focusWin, owner == focusWin ? 1 : 0, rowOff);
            Log::Write("READER", d);
        }

        if (IngameMenuReader::IsBattleCommandOwner(owner)) {
            // Battle command menu (Attack / Magicks & Technicks / Items / ...). A SEPARATE system —
            // NOT gated by the field-menu IsFocusedPane pane isolation. `index` = highlighted command.
            IngameMenuReader::OnBattleCommandFocus(owner, index);
        } else if (rowOff) {
            // Row-chain in-game menu (field pause menu + submenus): `val` is the focused row index.
            // Speak only if this pane holds the cursor.
            if (IsFocusedPane(owner))
                IngameMenuReader::OnRowChainFocus(owner, rowOff, index);
        } else if (!IsFocusedPane(owner) || !InventoryReader::TryFocus(owner, index)) {
            // Pause-menu item lists (Items / Loot / Key Items / Magicks / weapon+armor views) carry
            // an owned COUNT the painted-cell path cannot see, so InventoryReader claims those rows
            // and speaks "name count". It claims by struct shape and returns false for anything
            // else -- including an unreadable row -- so everything else still falls through here.
            // Same active-pane gate OnFocus applies internally, checked up front so a background
            // pane's list is never announced.
            OnFocus(owner, index, /*fromPaint=*/false);   // gates the content path internally
        }
    } else if (msg == MSG_YES || msg == MSG_NO || msg == MSG_CANCEL) {
        // No-list 2-choice pop-up result path (owner = parent). Logged for now;
        // the tested quit pop-up is the list variant handled via 0x8000 above.
        char hdr[96];
        snprintf(hdr, sizeof(hdr), "popup result msg=0x%llx owner=%p",
                 (unsigned long long)msg, owner);
        Log::Write("READER", hdr);
    }
    return s_origDispatch ? s_origDispatch(owner, msg, val) : 0;
}

// FUN_00244830(old, new, flag): sets DAT_0208ebc0 = new (the pane gaining the cursor). It fires
// just AFTER the entered pane's first 0x8000 (which was gated out because the focus pointer hadn't
// flipped yet), so we replay that stashed focus now that IsFocusedPane(new) is true — this is what
// makes the first item on entering a submenu speak.
void HookedFocusSet(void* oldWin, void* newWin, int flag) {
    if (s_origFocusSet) s_origFocusSet(oldWin, newWin, flag);
    // A pop-up just took the cursor: arm its body announce. This is the game's own entry event, so
    // it fires even when the window ADDRESS is recycled from the previous pop-up -- which is the
    // case `ownerChanged` could not see (see g_popupEntryArmed).
    if (IsConfirmWindow(newWin) || MenuState::IsChoicePopup(newWin)) {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_popupEntryArmed = true;
    }
    // Brackets each menu-open window: everything since the previous pane entry.
    // 3ms was always exceeded (TextCapture::Capture alone is ~3.7ms per window), so this wrote ~13
    // log lines on the game thread at EVERY pane change -- on the exact path being measured. Only
    // report a window that actually burned real time; the gap anchors dump unconditionally anyway.
    StallProbe::DumpAndReset("menu entry", /*minTotalMs=*/25.0);
    // Start the announce -> first-paint bracket. This hook is where we speak the entered pane, and
    // it is the exact instant the field-menu freeze begins.
    StallProbe::MarkMenuEntry("pane-entry");
    StallProbe::MarkMenuEntryDoneGuard _mmDone;
    STALL_SCOPE("MenuReader::HookedFocusSet");
    void* o; int idx; uint32_t rowOff;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        o = g_stashOwner; idx = g_stashIndex; rowOff = g_stashRowOff;
        // CONSUME it: the stash is a one-shot handoff for a single gated-out entry focus, not a
        // standing record of "the last focus". Leaving it armed let every later FUN_00244830 on the
        // same pane re-announce the same row.
        if (o && o == newWin && idx >= 0) {
            g_stashOwner = nullptr; g_stashIndex = -1; g_stashRowOff = 0;
        }
    }
    // ANNOUNCE THE ENTERED PANE. This replay is what makes the FIRST row of an entered pane speak at
    // all: the pane's own initial 0x8000 is gated out in HookedDispatch (the game has not yet assigned
    // DAT_0208ebc0), so if this does not speak it, nothing does.
    //
    // WHEN to speak splits by pane, exactly mirroring the battle command menu. FUN_00244830 fires at
    // the START of menu construction, so for the field menu (FUN_00280de0) speaking here lands
    // "Status" in the player's ear before the menu is visible -- the reported "speaks then lags". For
    // that ONE class we stash the focus and let the menu's own SHOW message (cat 0x13, in
    // IngameMenuReader) release it, so speech coincides with the menu appearing -- just as the battle
    // menu waits for its own row draw (FUN_00276be0). Every OTHER pane (submenus, config, pop-ups)
    // opens with the shell already up, has no such lag, and still speaks immediately here.
    //
    // (History: four "wait until the menu is ready" signals were refuted by measurement before landing
    // on 0x13 -- first-string-drawn fired next frame, the row's own text at 31ms, 0x11f ACTIVATE was
    // never sent, and a timeout fallback spoke at the wrong time. See IngameMenuReader's field-pane
    // hook for why 0x13 is the game's own visible-open event.)
    if (o && o == newWin && idx >= 0) {
        if (IngameMenuReader::IsFieldPaneOwner(o)) {
            IngameMenuReader::ArmPaneEntry(o, rowOff, idx);   // released on the SHOW message (cat 0x13)
        } else if (rowOff) {
            IngameMenuReader::OnRowChainFocus(o, rowOff, idx);
        } else if (!InventoryReader::TryFocus(o, idx)) {
            // Same split as the dispatch path: an item list speaks its row WITH the count here too,
            // otherwise entering one of those panes would announce the row without it.
            OnFocus(o, idx, /*fromPaint=*/true);
        }
    } else if (newWin) {
        // ---- UNCLAIMED PANE CENSUS (Session 112, LOG-ONLY) -----------------------------------------
        // A pane took the cursor and nothing above spoke for it. That is exactly what the tester's
        // full-screen CONTROLS panel does: the log shows `menu-open pane-entry` firing over and over
        // on map 568 with no text and no speech, and the screenshot confirms it is the game's own
        // on-screen keyboard (footer: `F9 Hide On-Screen Keyboard` / `Space Close`).
        //
        // To ANNOUNCE it the reader has to recognise it, and recognition here is always the owner's
        // obj[0] CLASS pointer against a known RVA -- which this project has never measured for this
        // surface. This line measures it: one entry per DISTINCT class, so a per-frame re-open
        // cannot flood the log, and a hard cap so an unexpected variety cannot either.
        //
        // It speaks NOTHING. Announcing every unclaimed pane would talk over surfaces that are
        // deliberately silent, and a wrong guess here is a regression in a working reader -- so the
        // RVA gets measured first and the announce ships gated on it.
        static void*        s_seenCls[12] = {};
        static int          s_seenN = 0;
        void* cls = MemRead::Obj0(newWin);
        if (cls && s_seenN < 12) {
            bool known = false;
            for (int i = 0; i < s_seenN; ++i) if (s_seenCls[i] == cls) { known = true; break; }
            if (!known) {
                s_seenCls[s_seenN++] = cls;
                const uintptr_t base = reinterpret_cast<uintptr_t>(Hooks::ResolveRva(0));
                const uintptr_t c    = reinterpret_cast<uintptr_t>(cls);
                char m[192];
                snprintf(m, sizeof(m),
                         "unclaimed pane: obj0 RVA=0x%llX win=%p -- no reader spoke for it "
                         "(candidate: the on-screen CONTROLS panel)",
                         static_cast<unsigned long long>(c >= base ? c - base : c), newWin);
                Log::Write("READER", m);
            }
        }
    }
}

// FUN_00240750(configId, &newDisplayIdx): the config store is written when a value
// row changes (left/right). No focus message fires for an in-place value change, so
// this write is the on-change announce trigger. Speak only when the CURRENTLY-
// focused row is the value row whose config id matches — so a "Restore Defaults"
// batch write (many configs; focused row is a button) never speaks, and no per-row
// dedup is needed (FUN_0023d6b0 calls this only on a genuine change).
void HookedStoreWrite(uintptr_t configId, void* pIdx) {
    STALL_SCOPE("MenuReader::HookedStoreWrite");
    void* owner; int idx;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        owner = g_focusOwner;
        idx   = g_focusIndex;
    }
    if (IsActiveConfig(owner) && idx >= 0) {
        void* row = ConfigRowWidget(owner, idx);
        uint8_t rowCfg = 0xff;
        if (IsConfigValueRow(row) && SafeReadU8(row, OFF_ROW_CFGID, &rowCfg) &&
            rowCfg == static_cast<uint8_t>(configId & 0xff)) {
            int newVal = -1;
            SafeReadInt(pIdx, &newVal);                  // new option index OR new slider value
            std::wstring val = ConfigReader::RowValueAtNewValue(row, newVal);
            if (val.empty()) val = ConfigReader::RowValue(row);  // fallback to current state
            if (!val.empty()) {
                Log::WriteW("READER", "  value: ", val);
                Speech::Output(val, /*interrupt=*/true);
            }
        }
    }
    if (s_origStoreWrite) s_origStoreWrite(configId, pIdx);
}

// FUN_0017db90(configId, curVal, dir): the Graphics subsystem's value setter, called on
// left/right in FUN_0023b330/b6f0. The row's display text only refreshes on the next
// draw, so we mark the change (gated to the focused Graphics row's config id) and
// announce it from OnMenuPainted once it has settled.
uint32_t HookedGfxWrite(uint32_t configId, uint32_t curVal, uint32_t dir) {
    uint32_t newVal = s_origGfxWrite ? s_origGfxWrite(configId, curVal, dir) : 0;
    STALL_SCOPE("MenuReader::HookedGfxWrite");
    void* owner; int idx;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        owner = g_focusOwner;
        idx   = g_focusIndex;
    }
    if (idx >= 0 && IsActiveConfig(owner) && MenuState::IsGraphicsConfig(owner)) {
        void* row = ConfigRowWidget(owner, idx);
        int rowCfg = -1;
        if (IsConfigValueRow(row) &&
            SafeReadInt(reinterpret_cast<char*>(row) + OFF_GFX_ROW_CFGID, &rowCfg) &&
            rowCfg == static_cast<int>(configId)) {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_valueChangeOwner = owner;
            g_valueChangeIndex = idx;
        }
    }
    return newVal;
}

} // namespace

namespace MenuReader {

bool Init() {
    if (g_initialized) {
        Log::Write("READER", "MenuReader::Init called twice — ignoring");
        return true;
    }
    TextCapture::SetMenuPaintedCallback(&OnMenuPainted);
    InputTracker::SetDescribeCallback(&DescribeHotkey);
    bool ok = Hooks::InstallTyped(RVA_DISPATCH,    &HookedDispatch,   &s_origDispatch);
    ok     &= Hooks::InstallTyped(RVA_STORE_WRITE, &HookedStoreWrite, &s_origStoreWrite);
    ok     &= Hooks::InstallTyped(RVA_GFX_WRITE,   &HookedGfxWrite,   &s_origGfxWrite);
    ok     &= Hooks::InstallTyped(RVA_FOCUS_SET,   &HookedFocusSet,   &s_origFocusSet);  // active-pane entry replay
    ok     &= IngameMenuReader::Init();   // battle command + target-reticle name hooks
    ok     &= CharSelectReader::Init();   // field-menu character chooser: Party membership + Status vitals
    ok     &= ChoiceReader::Init();       // mid-dialogue choice widget (polls input, sends no message)
    ok     &= BattleTargetReader::Init(); // battle target-selection readout (FUN_00329220 + ctx+0xde0)
    ok     &= LicenseReader::Init();      // license board / job select / char-select + U -> LP
    ok     &= AbilitySummaryReader::Init(); // the `F` ability/magick summary pages
    // BEFORE ShopReader: its FUN_002cc4f0 hook must be live so a snapshot already exists by the
    // time the shop's highlight handler runs (the game refreshes the panel inside FUN_0056e5d0).
    ok     &= EquipCompare::Init();       // per-character stat deltas behind the 4-9 keys
    ok     &= EquipTargetReader::Init();  // the post-purchase "equip it to whom?" screen
    ok     &= ShopReader::Init();         // shop Buy/Sell/Bazaar item name+price+inventory on highlight
    ok     &= InventoryReader::Init();    // pause item lists: row quantity + active category name
    MessageMacro::Init();                 // dialogue macros: the number in "<n> Bhujerbans heed ..."

    ok     &= GilReader::Init();          // `g` -> party gil total (field / shop / menus)
    ok     &= StatusReader::Init();       // Status screen: 3-page virtual buffer on the arrow keys
    PopupReader::Init();                  // NO-LIST confirm prompts (Game Over): no 0x8000 to hook
    g_initialized = true;
    Log::Write("READER", ok
        ? "MenuReader initialized (0x8000 -> row name+value; config value-on-change via "
          "FUN_00240750; 'i' -> item description; pop-up Yes/No; title skipped)."
        : "MenuReader: a hook failed to install — see Hooks log.");
    return ok;
}

void Shutdown() {
    if (!g_initialized) return;
    TextCapture::SetMenuPaintedCallback(nullptr);
    InputTracker::SetDescribeCallback(nullptr);
    ChoiceReader::Shutdown();
    IngameMenuReader::Shutdown();
    CharSelectReader::Shutdown();
    BattleTargetReader::Shutdown();
    LicenseReader::Shutdown();
    AbilitySummaryReader::Shutdown();
    StatusReader::Shutdown();
    InventoryReader::Shutdown();
    ShopReader::Shutdown();
    EquipTargetReader::Shutdown();
    EquipCompare::Shutdown();
    GilReader::Shutdown();
    PopupReader::Shutdown();
    Hooks::Uninstall(RVA_FOCUS_SET);
    Hooks::Uninstall(RVA_GFX_WRITE);
    Hooks::Uninstall(RVA_STORE_WRITE);
    Hooks::Uninstall(RVA_DISPATCH);
    g_initialized = false;
    StallProbe::DumpAndReset("session total");
    Log::Write("READER", "MenuReader shut down");
}

} // namespace MenuReader

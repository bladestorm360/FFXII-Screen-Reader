#include "navigation/entity_list.h"
#include "navigation/entity_list_internal.h"
#include "navigation/entity_labels.h"
#include "navigation/entity_scan.h"
#include "navigation/map_names.h"
#include "navigation/map_query.h"
#include "navigation/nav_common.h"
#include "navigation/nav_rva.h"        // TRAP_VISIBLE -- the cycle hides Traps on the game's own latch
#include "navigation/player_state.h"
#include "ui/text_prompt.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "speech/speech.h"
#include "speech/phrasebook.h"
#include "core/logger.h"

#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// The HOTKEY COMMANDS over the entity list: the [ / ] cursor, the - / = category filter, F5's
// availability filter, `/` describe, and F6 labelling. Split out of entity_list.cpp when that file
// passed the project's 500-line ceiling.
//
// Maintaining a set and driving a cursor across it are different jobs; they shared one file only
// because both need the module state, which now lives in entity_list_internal.h. Every function here
// takes g_mutex itself -- entity_list.cpp's own callers never reach into this file.
namespace EntityList {

using namespace Internal;
using EntityScan::Entity;
using EntityScan::CategoryWord;

static void CycleLocked(int dir, const FVec3& playerPos) {
    RefreshPositionsLocked(playerPos);
    std::vector<size_t> view = FilteredSortedLocked();
    if (view.empty()) { ClearFocusLocked(); SpeakNoTargets(); return; }

    // Find current focus within the view by stable identity (exact, else identity re-lock).
    int cur = FindFocusInViewLocked(view);
    const int nv = static_cast<int>(view.size());
    int next = (cur < 0) ? 0 : ((cur + dir) % nv + nv) % nv;
    const Entity& e = g_entities[view[next]];
    SetFocusLocked(e);
    SpeakEntityLocked(e, playerPos);
}

void CmdNext() {
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();   // fresh — pick up objects that appeared since the last command
    FVec3 p;
    if (!ReadPlayer(p)) { Speech::Output(Phrase::Get(Phrase::Id::PositionUnavailable)); return; }
    CycleLocked(+1, p);
}

void CmdPrev() {
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();
    FVec3 p;
    if (!ReadPlayer(p)) { Speech::Output(Phrase::Get(Phrase::Id::PositionUnavailable)); return; }
    CycleLocked(-1, p);
}

void CmdDescribeCurrent() {
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();
    FVec3 p;
    if (!ReadPlayer(p)) { Speech::Output(Phrase::Get(Phrase::Id::PositionUnavailable)); return; }
    RefreshPositionsLocked(p);
    std::vector<size_t> view = FilteredSortedLocked();
    if (view.empty()) { SpeakNoTargets(); return; }
    // Speak the current focus (by stable identity), or the nearest if the focus is gone / unset.
    int fi = FindFocusInViewLocked(view);
    size_t sel = (fi >= 0) ? view[fi] : view[0];
    SetFocusLocked(g_entities[sel]);
    SpeakEntityLocked(g_entities[sel], p);

    // Obstacle-aware hint toward the selection (<=5 rays; safe on the input thread).
    // A full A* grid is a later enhancement (thousands of rays => needs game-thread
    // execution to avoid racing the physics step).
    if (MapQuery::HasWorld()) {
        float hintFacing = 0.0f;
        PlayerState::ReadCameraForwardStable(hintFacing);   // always yields a reference
        const FVec3 tgt = g_entities[sel].pos;
        const float bodyPad = 0.9f;                  // test at body height, not at the feet
        const FVec3 from{ p.x, p.y + bodyPad, p.z };
        if (MapQuery::SegmentClear(from, FVec3{ tgt.x, p.y + bodyPad, tgt.z })) {
            Speech::SpeakQueued(Phrase::Get(Phrase::Id::PathClear));
        } else {
            // Heading convention matches nav_common::BearingDeg: north = -Z, so a
            // heading `a` maps to world offset (sin a, -cos a) in (x, z).
            const float base = std::atan2(tgt.x - p.x, -(tgt.z - p.z));
            float dist = NavCommon::Distance2D(p, tgt);
            const float probe = dist < 5.0f ? dist : 5.0f;   // look ~5 m per heading
            const float offs[4] = { 0.785398f, -0.785398f, 1.570796f, -1.570796f };  // +/-45, +/-90
            bool found = false;
            for (float o : offs) {
                const float a = base + o;
                const FVec3 pt{ p.x + std::sin(a) * probe, p.y + bodyPad, p.z - std::cos(a) * probe };
                if (MapQuery::SegmentClear(from, pt)) {
                    std::wstring s = Phrase::Get(Phrase::Id::BlockedBearPrefix);   // same frame as the bearing just spoken
                    s += NavCommon::CardinalOfHeadingRelative(a, hintFacing);
                    Speech::SpeakQueued(s);
                    found = true;
                    break;
                }
            }
            if (!found) Speech::SpeakQueued(Phrase::Get(Phrase::Id::BlockedWord));
        }
    }
}

// Is the game showing floor traps right now? The latch FUN_002f82f0 keeps from the Libra predicate
// -- see NavRva::TRAP_VISIBLE. One guarded read, no game call, and the same value EntityScan::ScanTraps
// gates on, so the category cycle and the list can never disagree about whether traps exist.
static bool TrapsVisible() {
    uint32_t v = 0;
    return MemRead::SafeReadU32(Hooks::ResolveRva(NavRva::TRAP_VISIBLE), 0, &v) && v != 0;
}

static void ChangeCategoryLocked(int dir) {
    int c = static_cast<int>(g_currentCategory);
    int n = static_cast<int>(Category::Count);
    c = ((c + dir) % n + n) % n;
    // TRAPS ARE THE ONE CATEGORY THAT CAN VANISH FROM THE CYCLE. The game hides floor traps until a
    // party member has Libra up, and the tester asked for the category to be hidden on the same
    // condition -- so with the latch clear, stepping onto it carries straight on in the same
    // direction. Bounded by `n` so a hypothetical all-skipping state cannot spin.
    //
    // SKIPS TRAP AND NOTHING ELSE. The obvious generalisation -- "skip any empty category" -- would
    // silence `"Shop, 0"` and every other zero this cycle deliberately announces, which is a working
    // surface the tester navigates by. Widening onto it to save a branch is exactly L-48.
    if (static_cast<Category>(c) == Category::Trap && !TrapsVisible()) {
        const int step = (dir >= 0) ? 1 : -1;
        for (int guard = 0; guard < n && static_cast<Category>(c) == Category::Trap; ++guard)
            c = ((c + step) % n + n) % n;
    }
    g_currentCategory = static_cast<Category>(c);
    // Rescan live actors BEFORE counting — the other commands (Next/Prev/Describe)
    // rescan, but this one used to count over the previous scan's stale set, so a
    // category whose actors weren't in that scan spoke a stale "0" even though the
    // object exists and appears once the user cycles. Rescanning makes the count live.
    RescanLocked();
    // Count matches + speak category name. Counts through the shared predicate, so the number
    // spoken is exactly what [ and ] will step through under the current availability filter.
    size_t matches = 0;
    for (auto& e : g_entities)
        if (PassesFiltersLocked(e)) ++matches;
    wchar_t buf[96];
    _snwprintf_s(buf, _TRUNCATE, L"%s, %zu", CategoryWord(g_currentCategory), matches);
    Speech::Output(buf);
    ClearFocusLocked();   // re-anchor to nearest on next cycle
}

void CmdNextCategory() { std::lock_guard<std::mutex> lk(g_mutex); ChangeCategoryLocked(+1); }
void CmdPrevCategory() { std::lock_guard<std::mutex> lk(g_mutex); ChangeCategoryLocked(-1); }

// F5 — flip the availability filter. Speaks the mode plus the resulting count, the same
// "<what>, <n>" shape the category cycle uses, so the two feel like one control surface.
void CmdToggleAvailability() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_availability = (g_availability == Availability::All) ? Availability::Gated : Availability::All;
    RescanLocked();   // live count, same reason the category cycle rescans first
    size_t matches = 0;
    for (auto& e : g_entities)
        if (PassesFiltersLocked(e)) ++matches;
    wchar_t buf[96];
    _snwprintf_s(buf, _TRUNCATE, L"%s, %zu",
                 (g_availability == Availability::Gated) ? Phrase::Get(Phrase::Id::StoryGated)
                                                        : Phrase::Get(Phrase::Id::CatAll), matches);
    Speech::Output(buf);
    ClearFocusLocked();   // re-anchor to nearest in the new view
}

// F6 -- name the focused entity in the player's own words.
//
// THE CLIPBOARD IS GONE (Session 185). It was never the feature; it was the workaround. The mod is
// read-only on the keyboard -- it inspects the `const` DirectInput buffer the game polls and never
// swallows a key -- so there was no way to run a text field INSIDE the game, and "type it somewhere
// else, copy it, come back and press F6" was what that bought. A separate top-level window does not
// bend that rule at all: typing into a Win32 EDIT control is window-message input and never touches
// the DirectInput buffer. See `ui/text_prompt.h`.
//
// TWO QUESTIONS, AND WHICH ONE YOU GET IS DECIDED BY THE OBJECT, NOT BY A MODE:
//   * no custom name yet -> an edit field. OK with text names it; OK with nothing, or Cancel, does
//     nothing at all. An empty field is no longer how a name is cleared -- that meaning moved to the
//     confirmation below, where it can be ASKED as a question instead of guessed from a blank.
//   * already named      -> a Yes/No box asking whether to clear it. Yes puts the object back to
//     whatever the mod calls it from the game's own data; No leaves it alone. To RENAME, clear it and
//     press F6 again -- one question per press, and neither can be answered by accident.
//
// THE DIALOG DOES NOT RUN HERE. `TextPrompt` puts it on a thread of its own and calls back when it
// closes, so this function returns immediately and neither the game thread nor the hotkey thread is
// ever sitting inside a modal loop.

namespace {

// What the callback needs to find its way back to the entity after the dialog closes. Captured BY
// VALUE under the lock, because the player may have left the map by the time they press OK -- a
// pointer into the entity vector would be pointing at a rescan that has already happened.
struct PendingLabel {
    int          mapId = -1;
    int16_t      nameIdx = -1;
    std::wstring key;        // the entity's baseLabel: the label store's identity, never `label`
    FVec3        pos{};
    uint8_t      container = 0;
    uint16_t     slot = 0;
};
PendingLabel g_pending;      // one prompt at a time -- TextPrompt::Busy() is the interlock

// Write the label the player chose (or an empty string to clear it) and say what happened. Runs on
// the prompt's thread; takes the list mutex like every other command in this file.
void ApplyPendingLabel(const std::wstring& text) {
    std::lock_guard<std::mutex> lk(g_mutex);
    // THE MAP MUST STILL BE THE ONE THEY WERE STANDING ON. A dialog has no time limit and the store
    // is keyed by map, so filing a name under a map the player has left would attach their words to
    // whatever object happened to share the id over there.
    if (MapNames::CurrentMapId() != g_pending.mapId) {
        Log::Write("NAV-DIAG", "label: the map changed while the prompt was open -- discarded");
        return;
    }
    EntityLabels::SetLabel(g_pending.mapId, g_pending.nameIdx, g_pending.key, g_pending.pos,
                           g_pending.container, g_pending.slot, text);
    RescanLocked();   // re-label the live list so the confirmation and the cursor agree immediately

    if (text.empty()) Speech::Output(Phrase::Get(Phrase::Id::LabelCleared));
    else              Speech::Output(Phrase::Get(Phrase::Id::LabelledPrefix) + text);
}

void OnNameEntered(TextPrompt::Result r, const std::wstring& text, void*) {
    if (r != TextPrompt::Result::Ok) { Log::Write("NAV-DIAG", "label: cancelled"); return; }
    std::wstring t = text;
    // Trim: a name pasted out of a text editor usually brings whitespace with it.
    while (!t.empty() && (t.back() == L'\r' || t.back() == L'\n' || t.back() == L' ' || t.back() == L'\t'))
        t.pop_back();
    size_t lead = 0;
    while (lead < t.size() && (t[lead] == L' ' || t[lead] == L'\t')) ++lead;
    t.erase(0, lead);
    if (t.size() > 64) t.resize(64);   // it gets SPOKEN every time the entity is announced
    // OK on an empty field is NOT a clear any more -- clearing is its own question now, and reading a
    // blank as one would delete a name on a keypress the player meant as "never mind".
    if (t.empty()) { Log::Write("NAV-DIAG", "label: empty field -- nothing changed"); return; }
    ApplyPendingLabel(t);
}

void OnClearConfirmed(TextPrompt::Result r, const std::wstring&, void*) {
    if (r != TextPrompt::Result::Ok) {
        Speech::Output(Phrase::Get(Phrase::Id::ModCancelled));
        return;
    }
    ApplyPendingLabel(std::wstring());
}

} // namespace

void CmdLabelFocus() {
    // One at a time. Without this a second F6 while the box is up would re-capture the focus behind
    // the player's back and answer the first dialog with the second entity.
    if (TextPrompt::Busy()) return;

    std::wstring existing, spoken;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        RescanLocked();
        FVec3 p;
        if (!ReadPlayer(p)) { Speech::Output(Phrase::Get(Phrase::Id::PositionUnavailable)); return; }
        RefreshPositionsLocked(p);

        std::vector<size_t> view = FilteredSortedLocked();
        if (view.empty()) { SpeakNoTargets(); return; }
        int fi = FindFocusInViewLocked(view);
        const Entity& e = g_entities[(fi >= 0) ? view[fi] : view[0]];

        // Exits are named by the map script itself and keyed by controller, not by a handle-table
        // slot, so there is nothing stable to hang a label on. Say nothing rather than pretend.
        if (!e.sceneObj) {
            Log::Write("NAV-DIAG", "label: focused entity is a map transition, not a scene object -- no key to store");
            return;
        }

        // `baseLabel`, never `label`: by the time the player points at this entity its label may
        // already carry a " 2" suffix, and keying on the suffixed words would file the name under an
        // identity no later scan can reproduce.
        g_pending.mapId     = MapNames::CurrentMapId();
        g_pending.nameIdx   = e.nameIdx;
        g_pending.key       = e.baseLabel.empty() ? e.label : e.baseLabel;
        g_pending.pos       = e.pos;
        g_pending.container = e.container;
        g_pending.slot      = e.slot;

        existing = EntityLabels::LabelFor(g_pending.mapId, g_pending.nameIdx,
                                          g_pending.key, g_pending.pos);
        spoken   = e.label;
    }
    // EVERYTHING BELOW IS OUTSIDE THE LOCK. Raising a window while holding the entity-list mutex
    // would hand a UI thread a lock the game-side readers want, for as long as the player types.

    if (!existing.empty()) {
        TextPrompt::AskYesNo(
            L"Clear custom name",
            L"\"" + existing + L"\" is your own name for this object.\n\n"
            L"Clear it and go back to the name the game gives it?",
            &OnClearConfirmed, nullptr);
        return;
    }
    TextPrompt::AskText(L"Name this object",
                        L"Your name for \"" + spoken + L"\":",
                        std::wstring(), &OnNameEntered, nullptr);
}

// S181. The cursor move a guide needs: put the focus on the one entity a caller can identify, and
// hand back the words the list would speak for it. It deliberately does NOT speak and does NOT
// route -- the caller composes its own sentence, and the player's own route key still does the
// routing through `GetCurrentTarget`, which prefers the focus.
//
// The FILTERS ARE NOT CONSULTED. The target of a puzzle step is a fact about the puzzle, not about
// which category the player happens to be cycling, and `GetCurrentTarget` reads the focus before it
// reads any filtered view -- so focusing an Exit while the player is browsing NPCs still routes to
// the exit. Nothing else in the mod sets the focus without the player asking for that object.
bool FocusWhere(EntityTest test, void* ctx, std::wstring* outLabel) {
    if (!test) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();
    FVec3 p;
    if (ReadPlayer(p)) RefreshPositionsLocked(p);

    const Entity* best = nullptr;
    for (const Entity& e : g_entities) {
        if (!test(e.sceneObj, e.seamGroup, ctx)) continue;
        // NEAREST WINS. A puzzle step usually names exactly one entity, but a door with two sides is
        // two objects sharing one routine, and the one you can reach is the near one.
        if (!best || e.dist2D < best->dist2D) best = &e;
    }
    if (!best) return false;
    SetFocusLocked(*best);
    if (outLabel) *outLabel = best->label;
    return true;
}

} // namespace EntityList

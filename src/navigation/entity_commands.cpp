#include "navigation/entity_list.h"
#include "navigation/entity_list_internal.h"
#include "navigation/entity_labels.h"
#include "navigation/entity_scan.h"
#include "navigation/map_names.h"
#include "navigation/map_query.h"
#include "navigation/nav_common.h"
#include "navigation/player_state.h"
#include "speech/speech.h"
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
    if (!ReadPlayer(p)) { Speech::Output(L"Position unavailable"); return; }
    CycleLocked(+1, p);
}

void CmdPrev() {
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();
    FVec3 p;
    if (!ReadPlayer(p)) { Speech::Output(L"Position unavailable"); return; }
    CycleLocked(-1, p);
}

void CmdDescribeCurrent() {
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();
    FVec3 p;
    if (!ReadPlayer(p)) { Speech::Output(L"Position unavailable"); return; }
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
            Speech::SpeakQueued(L"Path clear");
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
                    std::wstring s = L"Blocked, bear ";   // same frame as the bearing just spoken
                    s += NavCommon::CardinalOfHeadingRelative(a, hintFacing);
                    Speech::SpeakQueued(s);
                    found = true;
                    break;
                }
            }
            if (!found) Speech::SpeakQueued(L"Blocked");
        }
    }
}

static void ChangeCategoryLocked(int dir) {
    int c = static_cast<int>(g_currentCategory);
    int n = static_cast<int>(Category::Count);
    c = ((c + dir) % n + n) % n;
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
                 (g_availability == Availability::Gated) ? L"Story-gated" : L"All", matches);
    Speech::Output(buf);
    ClearFocusLocked();   // re-anchor to nearest in the new view
}

// F6 -- name the focused entity with whatever text is on the clipboard.
//
// The mod cannot capture typing: it passes the DirectInput buffer to the game as `const` and never
// swallows a key, so there is no way to run a text field in-game without breaking the read-only input
// rule. The clipboard sidesteps that entirely -- type the name anywhere, copy it, focus the entity,
// press F6. Nothing is injected into the game and no game memory is written; the clipboard is an OS
// resource the mod only reads.
//
// An empty clipboard clears the label, which is how a mistake is undone.
void CmdLabelFromClipboard() {
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();
    FVec3 p;
    if (!ReadPlayer(p)) { Speech::Output(L"Position unavailable"); return; }
    RefreshPositionsLocked(p);

    std::vector<size_t> view = FilteredSortedLocked();
    if (view.empty()) { SpeakNoTargets(); return; }
    int fi = FindFocusInViewLocked(view);
    const Entity& e = g_entities[(fi >= 0) ? view[fi] : view[0]];

    // Exits are named by the map script itself and keyed by controller, not by a handle-table slot, so
    // there is nothing stable to hang a label on. Say nothing rather than pretend it worked.
    if (!e.sceneObj) {
        Log::Write("NAV-DIAG", "label: focused entity is a map transition, not a scene object -- no key to store");
        return;
    }

    std::wstring text;
    if (OpenClipboard(nullptr)) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* src = static_cast<const wchar_t*>(GlobalLock(h))) {
                text = src;
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    // Trim: a copy out of a text editor usually brings a trailing newline with it.
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.pop_back();
    size_t lead = 0;
    while (lead < text.size() && (text[lead] == L' ' || text[lead] == L'\t')) ++lead;
    text.erase(0, lead);
    if (text.size() > 64) text.resize(64);   // it gets SPOKEN every time the entity is announced

    // `baseLabel`, never `label`: by the time the player points at this entity its label may already
    // carry a " 2" suffix, and keying on the suffixed words would file the label under an identity no
    // later scan can reproduce.
    const std::wstring& key = e.baseLabel.empty() ? e.label : e.baseLabel;
    EntityLabels::SetLabel(MapNames::CurrentMapId(), e.nameIdx, key, e.pos,
                           e.container, e.slot, text);
    RescanLocked();   // re-label the live list so the confirmation and the cursor agree immediately

    if (text.empty()) {
        Speech::Output(L"Label cleared");
    } else {
        Speech::Output(L"Labelled " + text);
    }
}

} // namespace EntityList

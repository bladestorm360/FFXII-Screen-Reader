#include "navigation/entity_list.h"
#include "navigation/entity_scan.h"
#include "navigation/entity_diag.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "navigation/nav_common.h"
#include "navigation/player_state.h"
#include "navigation/map_query.h"
#include "navigation/map_names.h"
#include "navigation/map_exits.h"
#include "navigation/map_script.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/game_text.h"
#include "core/logger.h"
#include "speech/speech.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>
#include <cstdio>

using namespace MemRead;

namespace EntityList {

namespace {

// The Entity record, the object scanner and the npcdic classification live in
// navigation/entity_scan.{h,cpp}; the `` ` `` object dump in navigation/entity_diag.cpp.
// This file owns the LIST and the CURSOR: what was found, what the player is focused on,
// and the commands over it.
using EntityScan::Entity;
using EntityScan::CategoryWord;
using EntityScan::kExitMaxDist;


std::mutex             g_mutex;
std::vector<Entity>    g_entities;
Category               g_currentCategory = Category::All;

// The [ / ] focus, tracked across rescans by STABLE IDENTITY (not a bare scene-object pointer,
// which can be reused/aliased by a pooled combatant slot or momentarily drop from a rescan and
// silently re-anchor the cursor to the nearest object). Match tiers: exact (pointer + name-key +
// label) beats an identity re-lock (name-key + label + category, adopting the object's new pointer).
struct CursorId {
    void*        obj     = nullptr;
    int16_t      nameIdx = 0;
    std::wstring label;
    Category     cat     = Category::All;
    bool         valid   = false;
};
CursorId               g_cursor;

// Rebuild the list in place. The scan itself lives in EntityScan; this just binds it to our
// vector, which every caller below already holds the lock for.
int RescanLocked() { return EntityScan::Build(g_entities); }


// Re-read live positions for the current set; drop objects whose transform no longer
// reads (despawned / mid-teardown). Keeps identity (scene-object ptr) stable so focus
// survives. Caller holds g_mutex.
void RefreshPositionsLocked(const FVec3& playerPos) {
    for (auto it = g_entities.begin(); it != g_entities.end();) {
        if (it->fixed) {   // exit/map-jump: fixed world pos, no scene node — keep pos, just re-range
            it->dist2D = NavCommon::Distance2D(playerPos, it->pos);
            ++it;
            continue;
        }
        FVec3 p;
        if (!PlayerState::ReadSceneObjectPos(it->sceneObj, p)) { it = g_entities.erase(it); continue; }
        it->pos = p;
        it->dist2D = NavCommon::Distance2D(playerPos, p);
        ++it;
    }
}

// Indices into g_entities matching the active category filter, nearest-first.
std::vector<size_t> FilteredSortedLocked() {
    std::vector<size_t> v;
    for (size_t i = 0; i < g_entities.size(); ++i)
        if (g_currentCategory == Category::All || g_entities[i].category == g_currentCategory)
            v.push_back(i);
    std::sort(v.begin(), v.end(), [](size_t a, size_t b) {
        return g_entities[a].dist2D < g_entities[b].dist2D;
    });
    return v;
}

bool ReadPlayer(FVec3& pos) {
    return PlayerState::ReadPlayerPos(pos);
}

// --- cursor identity (lock-by-identity across rescans) ---------------------

// 2 = exact (same object): pointer + name-key + label all agree — rejects a pointer that a pooled
// slot reused for a DIFFERENT unit. 1 = identity re-lock: the same logical object under a new
// pointer (name-key + label + category agree). 0 = no match.
int CursorMatch(const Entity& e) {
    if (!g_cursor.valid) return 0;
    if (e.sceneObj == g_cursor.obj && e.nameIdx == g_cursor.nameIdx && e.label == g_cursor.label)
        return 2;
    if (e.nameIdx == g_cursor.nameIdx && e.label == g_cursor.label && e.category == g_cursor.cat)
        return 1;
    return 0;
}

// Index WITHIN `view` of the focused object, preferring an exact match over an identity re-lock;
// -1 if the focus is genuinely gone. So the "fall back to nearest" path is taken only when the
// object truly departed, never on a transient rescan wobble or a pointer alias.
int FindFocusInViewLocked(const std::vector<size_t>& view) {
    int relock = -1;
    for (int i = 0; i < static_cast<int>(view.size()); ++i) {
        int m = CursorMatch(g_entities[view[i]]);
        if (m == 2) return i;
        if (m == 1 && relock < 0) relock = i;
    }
    return relock;
}

void SetFocusLocked(const Entity& e) {
    g_cursor.obj = e.sceneObj;
    g_cursor.nameIdx = e.nameIdx;
    g_cursor.label = e.label;
    g_cursor.cat = e.category;
    g_cursor.valid = true;
}

void ClearFocusLocked() { g_cursor = CursorId{}; }

void SpeakEntityLocked(const Entity& e, const FVec3& playerPos) {
    if (e.noBearing) { Speech::Output(e.label); return; }       // map-atlas pos: name only, no direction
    float facingRad = 0.0f;
    PlayerState::ReadCameraForward(facingRad);                  // "North" = forward = where UP takes you
    std::wstring phrase = e.label;
    phrase += L". ";
    phrase += NavCommon::DescribeDirectionRelative(playerPos, e.pos, facingRad);   // egocentric
    Speech::Output(phrase);
}

void SpeakNoTargets() { Speech::Output(L"No targets"); }
} // namespace

// --- public API ------------------------------------------------------------

bool Init() {
    Log::Write("NAV", "entity list ready");
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_entities.clear();
    ClearFocusLocked();
}

int Rescan() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return RescanLocked();
}

void OnFieldFrame() {
    // GAME THREAD, once per field frame. The handle-table containers stream in lazily —
    // the object containers register AFTER the field-active bit is set (notably on a
    // save-load, where the player is dropped past the map seam), and the scanner otherwise
    // only rebuilds on the `` ` `` key, so a loaded map's objects never get picked up. Here
    // we auto-rebuild the moment the active-container SET changes. The builder writes each
    // container's entries+count BEFORE flipping its active bit last, so one rescan on the
    // change captures the just-streamed objects. O(1) (a 5-byte mask read) while stable.
    static uint32_t s_lastMask = 0;
    uint32_t mask = EntityScan::ActiveContainerMask();
    if (mask == s_lastMask) return;          // container set unchanged — nothing to do
    s_lastMask = mask;

    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<Entity> prev;
    const bool hadObjects = !g_entities.empty();
    if (hadObjects) prev = g_entities;                 // snapshot a good set
    int n = RescanLocked();
    // Never replace a populated list with a transient empty while a container is still live
    // (honors the "don't let the edge check wipe the scan" caution); a real teardown (mask==0)
    // is allowed to clear it, and the next cycle command prunes any departed objects.
    if (n == 0 && hadObjects && mask != 0)
        g_entities = std::move(prev);

    // Announce the area just entered ("Entering <name>") — ONLY on a real field area, and only when the
    // area actually changed (edge-triggered on the name; re-entering the same-named area after a menu
    // won't re-announce).
    //
    // CONTEXT GATE: IsFieldActive() alone is NOT enough — its 0x10 bit is already set at boot (the log
    // caught this block running pre-title with "no sub-map index"). NavSafeFailMask bits 0/1/2 = field sim
    // live + field module STARTED (only 1 after the first field entry) + valid area id (not mid-transition);
    // all three clear == we are genuinely on a field area. We deliberately do NOT use the full
    // IsFieldNavSafe(), which also demands the Bullet world — the prologue never builds one, so that would
    // suppress the announcement entirely.
    //
    // PERMITTED per-frame change-check (the no-dedup rule's one exception; see CLAUDE.md).
    // GUARDS: FUN_0022a770, the per-field-frame tick this whole function hangs off. The area name
    // is ambient state, not an event — without the check there is no "you entered somewhere" edge
    // to announce, only a value that is true on every frame.
    constexpr uint8_t kFieldContextBits = 0x07;   // 0 field, 1 field-started, 2 areaId
    if (mask != 0 && (PlayerState::NavSafeFailMask() & kFieldContextBits) == 0) {
        static std::wstring s_lastArea;
        std::wstring area = CurrentAreaName();
        if (!area.empty() && area != s_lastArea) {
            s_lastArea = area;
            std::wstring phrase = L"Entering ";
            phrase += area;
            // The spoken text itself is logged by Speech (SPEAK-OUT). Log the map id + each half here,
            // since those are what the speech log can't show — and they are exactly what distinguishes a
            // region-only announcement from a full one when a name looks wrong.
            const int aid = MapNames::CurrentMapId();
            const std::wstring sub = MapNames::ResolveAreaName(aid);
            const std::wstring reg = MapNames::ResolveRegionName(aid);
            char s8[64] = {}, r8[64] = {};
            for (size_t k = 0; k < sub.size() && k < 63; ++k) s8[k] = (sub[k] < 128) ? static_cast<char>(sub[k]) : '?';
            for (size_t k = 0; k < reg.size() && k < 63; ++k) r8[k] = (reg[k] < 128) ? static_cast<char>(reg[k]) : '?';
            char m[192];
            snprintf(m, sizeof(m), "announce: mapId=%d sub=\"%s\" region=\"%s\"", aid, s8, r8);
            Log::Write("NAV", m);
            Speech::Output(phrase);
        }

        // Per-area dump: the field-sign exits we now list (world pos + resolved destination), plus the
        // +0x54 map-jump world points for cross-reference. The +0x70 count is the number this session's
        // ABI fix unblocked — it read 0 on every map while the group index was being passed in junk.
        std::vector<MapExits::ExitRec> fs;
        MapExits::EnumerateFieldSignExits(fs, /*logRaw=*/true);                 // named exits (WORLD pos)
        FVec3 pep; const FVec3* ppp = PlayerState::ReadPlayerPos(pep) ? &pep : nullptr;
        if (ppp) {
            char m[96];
            snprintf(m, sizeof(m), "map-exits: player world=(%.1f,%.1f,%.1f)", pep.x, pep.y, pep.z);
            Log::Write("NAV-DIAG", m);
        }
        char fsm[64];
        snprintf(fsm, sizeof(fsm), "map-exits(+0x70) usable+named: %zu", fs.size());
        Log::Write("NAV-DIAG", fsm);
        std::vector<MapExits::ExitRec> jp;
        MapExits::EnumerateMapJumps(ppp, kExitMaxDist, jp, /*logRaw=*/true);    // +0x54 world points (diag)

        // The destination is a LITERAL in the loaded field script: scan for mapjump(dest,entrance,flags)
        // calls and log each with its resolved name + dispatch context. This is the real exit-dest source;
        // the log pins the source-door<->dest pairing for wiring.
        MapExits::DiagScanScriptMapjumps();
    }
}

std::wstring CurrentAreaName() {
    // "<region>: <sub-area>" for the current map id, e.g. "Nalbina Fortress: Lower Apartments".
    const int id = MapNames::CurrentMapId();
    if (id <= 0) return L"";
    return MapNames::ResolveFullAreaName(id);
}

void CmdRescan() {
    int n = Rescan();
    std::wstring area = CurrentAreaName();   // lock-free; does not touch g_entities
    wchar_t buf[160];
    if (!area.empty())
        _snwprintf_s(buf, _TRUNCATE, L"%s. %d objects", area.c_str(), n);
    else
        _snwprintf_s(buf, _TRUNCATE, L"%d objects", n);
    Speech::Output(buf);
}

// Shared body for Next/Prev: refresh, build the nearest-first view, move focus.
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
        float facingRad = 0.0f;
        PlayerState::ReadCameraForward(facingRad);   // egocentric "bear <cardinal>" hint (forward = UP)
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
                    std::wstring s = L"Blocked, bear ";
                    s += NavCommon::CardinalOfHeadingRelative(a, facingRad);
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
    // Count matches + speak category name.
    size_t matches = 0;
    for (auto& e : g_entities)
        if (g_currentCategory == Category::All || e.category == g_currentCategory) ++matches;
    wchar_t buf[96];
    _snwprintf_s(buf, _TRUNCATE, L"%s, %zu", CategoryWord(g_currentCategory), matches);
    Speech::Output(buf);
    ClearFocusLocked();   // re-anchor to nearest on next cycle
}

void CmdNextCategory() { std::lock_guard<std::mutex> lk(g_mutex); ChangeCategoryLocked(+1); }
void CmdPrevCategory() { std::lock_guard<std::mutex> lk(g_mutex); ChangeCategoryLocked(-1); }

bool GetCurrentTarget(FVec3& outPos, std::wstring& outLabel) {
    if (!PlayerState::IsFieldActive()) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();
    if (g_entities.empty()) return false;
    FVec3 p;
    if (PlayerState::ReadPlayerPos(p)) RefreshPositionsLocked(p);
    // Prefer the focused object (by stable identity: exact, else re-lock); else the nearest in
    // the active filter. Read-only query (drives `\`) — does not mutate the cursor.
    int relock = -1;
    for (size_t i = 0; i < g_entities.size(); ++i) {
        int m = CursorMatch(g_entities[i]);
        if (m == 2) { outPos = g_entities[i].pos; outLabel = g_entities[i].label; return true; }
        if (m == 1 && relock < 0) relock = static_cast<int>(i);
    }
    if (relock >= 0) { outPos = g_entities[relock].pos; outLabel = g_entities[relock].label; return true; }
    std::vector<size_t> view = FilteredSortedLocked();
    if (view.empty()) return false;
    outPos = g_entities[view[0]].pos;
    outLabel = g_entities[view[0]].label;
    return true;
}
// `` ` `` object dump. The walk itself is in entity_diag.cpp; the lock stays here, with the list it
// protects, because the dump reads live game tables and must not race a rescan.
void LogDiagnostic() {
    std::lock_guard<std::mutex> lk(g_mutex);
    EntityDiag::DumpLocked();
}

} // namespace EntityList

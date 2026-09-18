#include "navigation/entity_list.h"
#include "navigation/entity_list_internal.h"
#include "navigation/entity_scan.h"
#include "navigation/entity_labels.h"
#include "navigation/entity_diag.h"
#include "navigation/item_scan.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "navigation/nav_common.h"
#include "navigation/player_state.h"
#include "navigation/map_query.h"
#include "navigation/map_names.h"
#include "navigation/reach_gate.h"
#include "ui/menu_state.h"
#include "ui/mod_menu.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/game_text.h"
#include "core/logger.h"
#include "core/stall_probe.h"
#include "speech/speech.h"
#include "speech/phrasebook.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>
#include <cstdio>
#include <atomic>

using namespace MemRead;

namespace EntityList {

namespace Internal {

// The Entity record, the object scanner and the npcdic classification live in
// navigation/entity_scan.{h,cpp}; the `` ` `` object dump in navigation/entity_diag.cpp.
// This file owns the LIST and the CURSOR: what was found, what the player is focused on,
// and the commands over it.
using EntityScan::Entity;
using EntityScan::CategoryWord;


std::mutex             g_mutex;
std::vector<Entity>    g_entities;
Category               g_currentCategory = Category::All;
Availability           g_availability    = Availability::All;   // never start out hiding anything

CursorId               g_cursor;

// Rebuild the list in place. The scan itself lives in EntityScan; this just binds it to our
// vector, which every caller below already holds the lock for.
// How long an entity that has stopped appearing in the scan is kept listed. The handle table streams
// objects in and out constantly -- one NPC flickering was enough to make the tester's target "disappear
// randomly from the pathfinder" -- and a rebuild happens on EVERY cycle keypress, so a single absent
// frame used to delete somebody mid-approach. A real despawn still leaves after this; streaming noise
// removes nobody.
constexpr uint64_t kEntityGraceMs = 2000;

// The timestamp RescanLocked stamped onto everything the last scan actually produced. It is what
// lets RefreshPositionsLocked below tell a live entity from one that is merely being CARRIED
// through the grace window, without either of them needing a second flag on the record.
uint64_t g_lastScanMs = 0;

// Full rebuild, then carry over anything that has only just stopped being reported.
int RescanLocked() {
    std::vector<Entity> fresh;
    bool detail = false;
    EntityScan::Build(fresh, &detail);
    const uint64_t now = GetTickCount64();

    // Anything present this scan is current; anything missing keeps the timestamp it already had, so
    // the window measures continuous absence rather than time since the object was first seen.
    for (auto& e : fresh) {
        bool carried = false;
        for (const auto& old : g_entities) {
            if (!old.sceneObj || old.sceneObj != e.sceneObj) continue;
            carried = true;
            // AN ENEMY DOES NOT BECOME AN NPC BECAUSE ONE SAMPLE CAME BACK QUIET.
            //
            // Tester report (S94): on a rescan, enemies were re-filed as NPCs. There is only one
            // builder -- EntityScan::Build, reached only from here -- so this is not two code paths
            // disagreeing; it is one path whose answer is not stable across samples. `Enemy` on the
            // field comes solely from the actor-pool faction override (entity_scan.cpp), because the
            // classifier calls every character an NPC and ScanCombatants cannot correct an entry the
            // handle table already listed. So the scan where the pool does not answer produces NPC,
            // and every cycle keypress rebuilds the list, which makes the downgrade immediate.
            //
            // Only a MISSING verdict is overridden, and only in the one direction. A pool that
            // answered "not a foe" is believed -- an enemy that genuinely turns friendly must be
            // able to stop being an Enemy. `factionVerdict` is what separates the two, which the
            // category alone could not.
            if (!e.factionVerdict && e.category == Category::NPC &&
                old.category == Category::Enemy) {
                e.category = Category::Enemy;
                e.factionVerdict = true;   // the verdict is carried, not re-derived
            }
            break;
        }
        (void)carried;
        e.lastSeenMs = now;
    }
    int nCarried = 0, nEvicted = 0, nFiltered = 0;
    for (const auto& old : g_entities) {
        if (!old.sceneObj) continue;                       // fixed exits are rebuilt every scan anyway
        bool stillThere = false;
        for (const auto& e : fresh) if (e.sceneObj == old.sceneObj) { stillThere = true; break; }
        if (stillThere) continue;
        // MISSING BECAUSE WE DELETED IT, not because the engine stopped reporting it.
        //
        // This loop cannot tell those apart on its own, and the window is the wrong instrument for
        // the first case: a filtered object is a LIVE engine object that the scan will keep finding
        // and keep dropping, so carrying it for two seconds means every rescan re-admits it and logs
        // the drop again -- a filter that reports success and changes nothing. It silently undid both
        // the shadow drop and the unplaced-character pass. An explicit "we removed it" signal is the
        // right shape for a deliberate removal; a timeout is only right for an absence.
        //
        // (Until S148 this comment also said the entry could NEVER age out, because
        // RefreshPositionsLocked re-stamped `lastSeenMs` off a still-readable transform. That was
        // true, and it was a bug in RefreshPositionsLocked rather than a property of filtering --
        // fixed there. The signal below is still the right mechanism and stays.)
        if (EntityScan::WasFilteredThisScan(old.sceneObj)) { ++nFiltered; continue; }
        if (old.lastSeenMs == 0 || now - old.lastSeenMs > kEntityGraceMs) { ++nEvicted; continue; }
        ++nCarried;
        fresh.push_back(old);                              // keep it, with its last known position
    }

    // THE FALSIFIER FOR THE GRACE WINDOW. Nothing used to report what this loop did, which is why a
    // window that could never expire (see RefreshPositionsLocked) sat unnoticed through twenty
    // archived dev logs -- every one of them showed the drop tallies from the scan and nothing at
    // all about what the merge then put back. `evicted` going non-zero after a kill is the proof
    // that a dead enemy leaves; `carried` staying high across successive rescans is the proof that
    // it does not. Silent when all three are zero, which is the normal case, so it costs nothing.
    if (nCarried || nEvicted || nFiltered) {
        char m[160];
        snprintf(m, sizeof(m), "grace: carried=%d (within %llums) evicted=%d (aged out) filtered=%d (explicit)",
                 nCarried, static_cast<unsigned long long>(kEntityGraceMs), nEvicted, nFiltered);
        Log::Write("NAV", m);
    }

    // LABELLING RUNS HERE, NOT INSIDE Build -- after the merge above, so the list that gets numbered
    // is the list the player actually hears. It used to run at the end of Build, which the persistent
    // store made harmless (a number, once assigned, was permanent). Now that numbers are worked out
    // within a scan it would be a live bug: a group member that streams out for a frame leaves the
    // survivors to compact to 1..N-1 while the carried entity still holds its old suffix, so two
    // entries would answer to one number for up to the whole grace window.
    //
    // Both passes are idempotent, which is what makes it safe to run them over carried entities that
    // have already been through them once.
    EntityScan::ApplyPlayerLabels(fresh);
    EntityScan::NumberDuplicateLabels(fresh, detail);
    // S179/S182: the route key's own recorded answer on every entry, carried ones included, while the
    // world it was given in still holds. Removes nothing; the filter below acts on it only while the row
    // is on.
    ReachGate::Annotate(fresh);

    g_lastScanMs = now;
    g_entities.swap(fresh);
    return static_cast<int>(g_entities.size());
}


// Re-read live positions and ranges for the current set. Keeps identity (scene-object ptr) stable
// so focus survives. Never removes anything -- eviction is RescanLocked's job. Caller holds g_mutex.
//
// ONLY THE SCAN MAY STAMP `lastSeenMs`, so an entity that RescanLocked is merely CARRYING through
// the grace window is skipped entirely here: no transform read, no stamp. Both halves matter.
//
//  * The stamp. A carried entity still holds the scene-object pointer it had when the scan last saw
//    it, and that memory is not recycled the moment the engine drops the object, so the transform
//    read goes on succeeding. Stamping on that basis made `lastSeenMs` measure "when was a pointer
//    last readable" instead of "when did the scan last see this" -- and since every nav hotkey
//    refreshes, the 2 s window could then never expire. A killed enemy stayed listed for the life of
//    the map, which is exactly the reported defect.
//  * The read. Dereferencing that same stale pointer for up to two seconds is guarded and cannot
//    fault, but it can hand back coordinates belonging to whatever now occupies the memory. The last
//    position the scan actually saw is the better answer, and it is already in the record.
//
// S148. The S147 write-up blamed the actor-pool walk for having no HP test and proposed adding one;
// the user's play evidence is that the entity DOES disappear from the game, so there was never
// anything to gate -- the tracker simply was not noticing. Do not reintroduce an HP gate here: the
// same walk files allies as NPCs, and a KO'd party member is revivable and still worth listing.
//
// The comment this replaced claimed the function dropped objects whose transform stopped reading.
// The body was changed to keep them; only the comment survived.
void RefreshPositionsLocked(const FVec3& playerPos) {
    for (auto& e : g_entities) {
        // exit/map-jump: fixed world pos, no scene node -- keep pos, just re-range.
        // Carried-through-the-window: same treatment, and for the reasons in the header above.
        if (e.fixed || e.lastSeenMs != g_lastScanMs) {
            e.dist2D = NavCommon::Distance2D(playerPos, e.pos);
            continue;
        }
        FVec3 p;
        if (!PlayerState::ReadSceneObjectPos(e.sceneObj, p)) {
            // A single failed transform read is streaming noise, not a despawn. Keep the last known
            // position and let the grace window in RescanLocked decide when the object is really gone.
            e.dist2D = NavCommon::Distance2D(playerPos, e.pos);
            continue;
        }
        e.pos = p;
        e.dist2D = NavCommon::Distance2D(playerPos, p);
    }
}

// True when an entity passes BOTH active filters. The single choke point every command goes
// through (Next/Prev/Describe/GetCurrentTarget/the category count), so the two filters compose
// without any command needing to know about them.
bool PassesFiltersLocked(const Entity& e) {
    if (g_currentCategory != Category::All && e.category != g_currentCategory) return false;
    if (g_availability == Availability::Gated && e.available) return false;
    // The `Unreachable filter` row (S179, rebuilt S182; default OFF). Off: this line is never true, so
    // everything is listed. On: hides what the route key answered "No path" for, while the world it was
    // asked in still holds (reach_gate.h); anything never routed to is never hidden.
    if (ModMenu::UnreachableFilterOn() && ReachGate::Hides(e.reach)) return false;
    return true;
}

// Indices into g_entities matching the active filters, nearest-first.
std::vector<size_t> FilteredSortedLocked() {
    std::vector<size_t> v;
    for (size_t i = 0; i < g_entities.size(); ++i)
        if (PassesFiltersLocked(g_entities[i]))
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
// THE FOCUS CLAMP. Identity is the OBJECT -- the scene-object pointer, which entity_scan.h documents
// as stable for the whole time a map is loaded -- and NOTHING else.
//
// It used to also require `e.label == g_cursor.label`, in BOTH tiers. That looked harmless until the
// duplicate-suffix numbering was found to shift: the handle table streams objects in and out (measured
// NPC=14 <-> 15 across 118 rescans on one map), every keypress rebuilds the list, and a renumber made
// the focused NPC unrecognisable. FindFocusInViewLocked then returned -1 and CycleLocked restarted at
// view[0] -- the NEAREST. That is the reported "tracking Rabanastran 7, kept dropping back to 5".
//
// CORRECTED (Session 81): that fix reached tier 2 only. Tier 1 still compared the SUFFIXED label, so
// the same failure survived through the re-lock door -- and stateless numbering, which recompacts a
// group the moment a member streams out, makes a renumber more likely rather than less. Tier 1 now
// compares `baseLabel`, the un-suffixed words, which no renumber can change.
//
// A nearer entity can now change nothing the cursor looks at, so it can never steal the focus.
int CursorMatch(const Entity& e) {
    if (!g_cursor.valid) return 0;
    // Fixed exits have no scene node; their nameIdx encodes the controller index and is their identity.
    if (!e.sceneObj || !g_cursor.obj) {
        if (!e.sceneObj && !g_cursor.obj && e.nameIdx == g_cursor.nameIdx &&
            e.category == g_cursor.cat) return 2;
    } else if (e.sceneObj == g_cursor.obj) {
        return 2;
    }
    // Re-lock only when the object itself is gone and something equivalent took its place.
    const std::wstring& base = e.baseLabel.empty() ? e.label : e.baseLabel;
    if (e.nameIdx == g_cursor.nameIdx && base == g_cursor.baseLabel && e.category == g_cursor.cat)
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
    // Un-suffixed, so a renumber cannot make the focused entity unrecognisable. `baseLabel` is empty
    // only for an entity that reached here without passing ApplyPlayerLabels; fall back to the spoken
    // label so the re-lock still has something to compare.
    g_cursor.baseLabel = e.baseLabel.empty() ? e.label : e.baseLabel;
    g_cursor.cat = e.category;
    g_cursor.valid = true;
}

void ClearFocusLocked() { g_cursor = CursorId{}; }

void SpeakEntityLocked(const Entity& e, const FVec3& playerPos) {
    if (e.noBearing) { Speech::Output(e.label); return; }       // map-atlas pos: name only, no direction
    // RELATIVE crow-flies bearing in compass words ("Northwest, 8 steps" == 8 steps forward-left).
    // Same frame and vocabulary as the `\` route legs, so the two always agree. The reference comes
    // from the STABLE camera read, which always yields one -- a distance with no direction is
    // useless to walk on, so there is deliberately no direction-less path.
    float facingRad = 0.0f;
    const char* refSrc = "none";
    PlayerState::ReadCameraForwardStable(facingRad, &refSrc);   // always yields a reference
    // Same reference the route legs log, for the same reason: a bearing that seems to jump is the game's
    // camera moving, and `ref=` is what proves it. Input thread, so guard the previous value atomically.
    {
        static std::atomic<float> s_prevRef{0.0f};
        static std::atomic<bool>  s_havePrev{false};
        const float deg = facingRad * 57.2957795f;
        float dref = 0.0f;
        if (s_havePrev.load(std::memory_order_relaxed)) {
            dref = deg - s_prevRef.load(std::memory_order_relaxed);
            while (dref > 180.0f)  dref -= 360.0f;
            while (dref < -180.0f) dref += 360.0f;
        }
        s_prevRef.store(deg, std::memory_order_relaxed);
        s_havePrev.store(true, std::memory_order_relaxed);
        char m[128];
        snprintf(m, sizeof(m), "announce ref=%.1fdeg src=%s dref=%.1fdeg", deg, refSrc, dref);
        Log::Write("NAV", m);
    }
    std::wstring phrase = e.label;
    phrase += L". ";
    phrase += NavCommon::DescribeDirectionRelative(playerPos, e.pos, facingRad);
    Speech::Output(phrase);
}

void SpeakNoTargets() { Speech::Output(Phrase::Get(Phrase::Id::NoTargets)); }
} // namespace Internal

using namespace Internal;

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
    // REMOVED: an `if (MenuState::IsAnyMenuOpen()) return;` gate. It was a speculative fix for the
    // party-menu stall, it did NOT fix it (the probe measured this whole function at ~5 ms), and it
    // was actively wrong: IsAnyMenuOpen is "any window holds the input focus", which in BATTLE is
    // true almost continuously because the battle command window holds focus -- so it silently
    // killed the field object scan for the entire fight. A speculative fix that breaks a working
    // feature is worse than the problem it guessed at.
    STALL_SCOPE("EntityList::OnFieldFrame");

    // Ground loot lives in its OWN pool, not the handle table, so the container mask can never see a
    // drop appear or get picked up. Its three hooks set a flag instead; consuming it here is what
    // makes a fresh drop show up while the player is already sitting in the Items category, with no
    // rescan keypress. Read it FIRST, and unconditionally, so the mask's early-out cannot swallow
    // the edge.
    const bool lootChanged = ItemScan::TakeDirty();

    static uint32_t s_lastMask = 0;
    uint32_t mask = EntityScan::ActiveContainerMask();
    if (mask == s_lastMask && !lootChanged) return;   // nothing changed — nothing to do
    s_lastMask = mask;

    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<Entity> prev;
    const bool hadObjects = !g_entities.empty();
    if (hadObjects) prev = g_entities;                 // snapshot a good set
    const int mapNow = MapNames::CurrentMapId();
    int n = RescanLocked();
    // Never replace a populated list with a transient empty while a container is still live
    // (honors the "don't let the edge check wipe the scan" caution); a real teardown (mask==0)
    // is allowed to clear it, and the next cycle command prunes any departed objects.
    //
    // ...but never ACROSS A MAP CHANGE. The snapshot is stamped with the map it was taken on, so this
    // path can no longer resurrect the previous area's objects and exits. (No user-visible staleness was
    // ever actually measured -- when the tester reported "exits from the previous map", North End had
    // listed exactly its own seven doors, and the confusion came from several exits sharing one
    // placeholder name. The path is real even if that report was not, and closing it costs one int.)
    static int s_snapshotMap = -1;
    if (n == 0 && hadObjects && mask != 0 && s_snapshotMap == mapNow)
        g_entities = std::move(prev);
    s_snapshotMap = mapNow;

    // Announce the area just entered ("Entering <name>") — ONLY on a real field area, and only when the
    // area actually changed (edge-triggered on the name; re-entering the same-named area after a menu
    // won't re-announce).
    //
    // CONTEXT GATE: IsFieldActive() alone is NOT enough — its 0x10 bit is already set at boot (the log
    // caught this block running pre-title with "no sub-map index"). We deliberately do NOT use the
    // full IsFieldNavSafe(), which also demands the Bullet world — the prologue never builds one, so
    // that would suppress the announcement entirely.
    //
    // THE BITS, AND WHY EACH ONE IS HERE — this mask has now been wrong in BOTH directions, so the
    // reasoning is spelled out rather than left to a bit count:
    //
    //   bit 0 CondFieldActive   field sim live
    //   bit 1 CondFieldStarted  field module STARTED (only 1 after the first field entry)
    //   bit 5 CondLeaderPtr     the party leader actor exists (zeroed at teardown start)
    //   bit 7 CondLeaderObj     ...and resolves through the handle table
    //
    // BIT 2 (areaId) IS DELIBERATELY ABSENT AND MUST NOT COME BACK. Session 93 established that bits
    // 2/3 are not a readiness signal — `0x0C` is the engine's TERMINAL "this area has no such
    // resource" state written by FUN_003ea820, which nothing ever retries — and removed them from
    // IsFieldNavSafe(); this gate was missed. Measured across 21 logs with no exceptions: every map
    // reporting failMask=0x00 announced (11/11), every map reporting failMask=0x0C did not (3/3 —
    // Bhujerba 805, 806, 806). A whole city was silent. Note what that was NOT: Rabanastre announces
    // section-to-section perfectly, so the s_lastArea check below was never the cause.
    //
    // BITS 5/7 ARE WHAT BIT 2 WAS ACTUALLY DOING, DONE HONESTLY. Dropping bit 2 alone reinstated the
    // pre-title bug the first paragraph warns about, and the log caught it within seconds of boot:
    //     [+5468ms] announce: mapId=12 sub="" region="Pharos at Ridorana"
    // — eight seconds before "Load Game", on the boot map, with an empty sub-area and a region name
    // that is simply a bogus lookup. Bit 2 had been suppressing that as a side effect of areaId being
    // 0xFFFFFFFF at boot, which is why nobody noticed it was also suppressing Bhujerba.
    //
    // "Is a party standing on a field map" is the thing this gate actually wants, and the leader
    // actor IS that question — it does not exist at boot and it does exist everywhere the player can
    // walk. Unlike the area manifest it is a liveness property, not a per-area resource lookup, so it
    // has no terminal-false state to fall into. Bhujerba (failMask=0x0C) has bits 5 and 7 CLEAR, so
    // the city that started all this still announces.
    //
    // PERMITTED per-frame change-check (the no-dedup rule's one exception; see CLAUDE.md).
    // GUARDS: FUN_0022a770, the per-field-frame tick this whole function hangs off. The area name
    // is ambient state, not an event — without the check there is no "you entered somewhere" edge
    // to announce, only a value that is true on every frame.
    constexpr uint8_t kFieldContextBits = 0x01 | 0x02 | 0x20 | 0x80;   // field, started, leader ptr+obj
    if (mask != 0 && (PlayerState::NavSafeFailMask() & kFieldContextBits) == 0) {
        static std::wstring s_lastArea;
        std::wstring area = CurrentAreaName();
        if (!area.empty() && area != s_lastArea) {
            s_lastArea = area;
            std::wstring phrase = Phrase::Get(Phrase::Id::EnteringPrefix);
            phrase += area;
            // The spoken text itself is logged by Speech (SPEAK-OUT). Log the map id + each half here,
            // since those are what the speech log can't show — and they are exactly what distinguishes a
            // region-only announcement from a full one when a name looks wrong.
            // A new area: re-read the label store, so editing the text file by hand takes effect
            // without restarting the game.
            EntityLabels::Reload();
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

            // (The per-area map-exit dump that lived here wrote ~1,250 NAV-DIAG lines SYNCHRONOUSLY
            // on the game thread at EVERY area change -- a real stall on every transition, and one the
            // exit FEATURE never needs: it was pure diagnostic (results discarded; the exit reader
            // enumerates on demand with logRaw=false). It moved to the ` diagnostic key --
            // NavCommands::DiagnosticDump -- where it runs OPT-IN, in whatever area the player is
            // standing in. The one-line area-change context ("announce: mapId=...") above stays.)
        }
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
        _snwprintf_s(buf, _TRUNCATE, Phrase::Get(Phrase::Id::FmtAreaObjects), area.c_str(), n);
    else
        _snwprintf_s(buf, _TRUNCATE, Phrase::Get(Phrase::Id::FmtObjects), n);
    Speech::Output(buf);
}

// Shared body for Next/Prev: refresh, build the nearest-first view, move focus.
bool GetCurrentTarget(FVec3& outPos, std::wstring& outLabel, bool* outIsTransition,
                      void** outSceneObj, int* outSeamGroup) {
    if (outIsTransition) *outIsTransition = false;
    if (outSceneObj) *outSceneObj = nullptr;
    if (outSeamGroup) *outSeamGroup = 0;
    if (!PlayerState::IsFieldActive()) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();
    if (g_entities.empty()) return false;
    FVec3 p;
    if (PlayerState::ReadPlayerPos(p)) RefreshPositionsLocked(p);
    // Only the entity knows whether the target is a transition surface, and the planner needs that at
    // the moment the player arrives -- see EntityScan::Entity::isTransition.
    auto take = [&](const EntityScan::Entity& e) {
        outPos   = e.pos;
        outLabel = e.label;
        if (outIsTransition) *outIsTransition = e.isTransition;
        if (outSceneObj) *outSceneObj = e.sceneObj;
        if (outSeamGroup) *outSeamGroup = e.seamGroup;
    };
    // Prefer the focused object (by stable identity: exact, else re-lock); else the nearest in
    // the active filter. Read-only query (drives `\`) — does not mutate the cursor.
    int relock = -1;
    for (size_t i = 0; i < g_entities.size(); ++i) {
        int m = CursorMatch(g_entities[i]);
        if (m == 2) { take(g_entities[i]); return true; }
        if (m == 1 && relock < 0) relock = static_cast<int>(i);
    }
    if (relock >= 0) { take(g_entities[relock]); return true; }
    std::vector<size_t> view = FilteredSortedLocked();
    if (view.empty()) return false;
    take(g_entities[view[0]]);
    return true;
}
int CollectPositionsByNameIdx(int16_t nameIdx, std::vector<FVec3>& out) {
    out.clear();
    if (!PlayerState::IsFieldActive()) return 0;
    std::lock_guard<std::mutex> lk(g_mutex);
    for (const EntityScan::Entity& e : g_entities) {
        if (e.nameIdx != nameIdx || e.fixed) continue;
        // Fresh transform, same streaming-noise rule as RefreshPositionsLocked: one failed read
        // keeps the last known position rather than dropping a live actor mid-request.
        FVec3 p = e.pos;
        FVec3 live{};
        if (e.sceneObj && PlayerState::ReadSceneObjectPos(e.sceneObj, live)) p = live;
        out.push_back(p);
    }
    return static_cast<int>(out.size());
}

int CollectSceneObjectsByNameIdx(int16_t nameIdx, void** out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = 0;
    std::lock_guard<std::mutex> lk(g_mutex);
    for (const EntityScan::Entity& e : g_entities) {
        if (e.nameIdx != nameIdx || !e.sceneObj) continue;
        out[n++] = e.sceneObj;
        if (n >= cap) break;
    }
    return n;
}

namespace {
// The gather itself. Caller must NOT hold g_mutex; this takes it.
void CollectNpcsOnce(const FVec3& from, int maxOut, std::vector<NearbyNPC>& out) {
    out.clear();
    std::lock_guard<std::mutex> lk(g_mutex);
    RefreshPositionsLocked(from);
    for (const EntityScan::Entity& e : g_entities) {
        if (e.category != Category::NPC || e.label.empty()) continue;
        NearbyNPC n;
        n.label   = e.label;
        n.pos     = e.pos;
        n.nameIdx = e.nameIdx;
        n.dist2D  = e.dist2D;
        out.push_back(std::move(n));
    }
    std::sort(out.begin(), out.end(),
              [](const NearbyNPC& a, const NearbyNPC& b) { return a.dist2D < b.dist2D; });
    if (static_cast<int>(out.size()) > maxOut) out.resize(static_cast<size_t>(maxOut));
}
} // namespace

int CollectNearestNPCs(const FVec3& from, int maxOut, std::vector<NearbyNPC>& out) {
    out.clear();
    if (maxOut <= 0 || !PlayerState::IsFieldActive()) return 0;

    CollectNpcsOnce(from, maxOut, out);
    if (!out.empty()) return static_cast<int>(out.size());

    // NO NPCs YET IS USUALLY A STALE LIST, NOT AN EMPTY STREET. The handle-table containers stream
    // in at different times, and the NPC one arrives LATE: the 2026-08-05 play log has a rescan at
    // 22.6 s finding 7 objects with NPC=0 (exits and doors only) and the next at 41.8 s finding 21
    // with NPC=14. Every crowd-key press in between landed in that window and was answered, quite
    // truthfully and quite uselessly, with "no targets" while the player stood next to somebody.
    //
    // OnFieldFrame's auto-rescan is edge-triggered on the ACTIVE CONTAINER MASK, so NPCs appearing
    // inside a container that was already active move no edge and trigger no rescan. Asking for a
    // fresh scan when — and only when — the answer would otherwise be "nobody here" costs nothing
    // in the normal case and closes that window. The retry runs OUTSIDE the lock because Rescan
    // takes g_mutex itself.
    Rescan();
    CollectNpcsOnce(from, maxOut, out);
    return static_cast<int>(out.size());
}

int CollectNearby(const FVec3& from, float maxDist, int maxOut, std::vector<NearbyEntity>& out) {
    out.clear();
    if (maxOut <= 0 || maxDist <= 0.0f || !PlayerState::IsFieldActive()) return 0;

    std::lock_guard<std::mutex> lk(g_mutex);
    RefreshPositionsLocked(from);
    for (const EntityScan::Entity& e : g_entities) {
        // A map-atlas offset is not a world position, so there is no direction to pan it to.
        if (e.noBearing || e.dist2D > maxDist) continue;
        NearbyEntity n;
        n.cat    = e.category;
        n.pos    = e.pos;
        n.dist2D = e.dist2D;
        n.id     = e.sceneObj;
        out.push_back(n);
    }
    std::sort(out.begin(), out.end(),
              [](const NearbyEntity& a, const NearbyEntity& b) { return a.dist2D < b.dist2D; });
    if (static_cast<int>(out.size()) > maxOut) out.resize(static_cast<size_t>(maxOut));
    return static_cast<int>(out.size());
}

std::wstring LabelForSceneObject(void* sceneObj) {
    if (!sceneObj) return std::wstring();
    std::lock_guard<std::mutex> lk(g_mutex);
    for (const EntityScan::Entity& e : g_entities)
        if (e.sceneObj == sceneObj) return e.label;
    return std::wstring();
}

// `` ` `` object dump. The walk itself is in entity_diag.cpp; the lock stays here, with the list it
// protects, because the dump reads live game tables and must not race a rescan.
void LogDiagnostic() {
    std::lock_guard<std::mutex> lk(g_mutex);
    EntityDiag::DumpLocked();
}

} // namespace EntityList

#include "navigation/entity_list.h"
#include "navigation/nav_rva.h"
#include "navigation/nav_common.h"
#include "navigation/player_state.h"
#include "navigation/map_query.h"
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

// A live interactive field object. Identity is the SCENE OBJECT pointer, which is stable
// while the map is loaded (the handle table holds it from load to teardown).
struct Entity {
    void*        sceneObj = nullptr;   // handle-table scene object (identity)
    uint32_t     flags    = 0;         // *(sceneObj+0x1C): FLAG_TALK (NPC) / FLAG_ACTION
    int16_t      nameIdx  = 0;         // *(sceneObj+0x102): npcdic id (>=0) / custom (<0)
    Category     category = Category::Object;
    std::wstring label;
    FVec3        pos;
    float        dist2D   = 0.0f;      // to player, refreshed per command
};

std::mutex             g_mutex;
std::vector<Entity>    g_entities;
Category               g_currentCategory = Category::All;
void*                  g_currentObj      = nullptr;  // focus (scene object), tracked across refreshes
bool                   g_egocentric      = false;    // cardinal by default

// --- helpers ---------------------------------------------------------------

const wchar_t* CategoryWord(Category c) {
    switch (c) {
        case Category::All:         return L"All";
        case Category::Exit:        return L"Exit";
        case Category::SaveCrystal:  return L"Save Crystal";
        case Category::GateCrystal:  return L"Gate Crystal";
        case Category::Treasure:    return L"Treasure";
        case Category::NPC:         return L"NPC";
        case Category::Object:      return L"Interactables";
        case Category::Enemy:       return L"Enemy";
        default:                    return L"Interactables";
    }
}

// npcdic codec-string pointer for a name index — replicates FUN_003eac10 (the game's
// own npcdic lookup) memory-only. The blob is loaded once at boot; DAT_02b5e0d8 holds
// its base. Even slot = display name (odd = yomi/reading). The offset table stores
// relocated absolute pointers as s32, read here exactly as the game does; 0 /
// out-of-range -> null.
const uint8_t* NpcdicName(int id) {
    void* blob = PtrAt(Hooks::ResolveRva(NavRva::NPCDIC_BASE), 0);
    if (!blob) return nullptr;
    uint32_t count = 0;
    if (!SafeReadU32(blob, NavRva::NPCDIC_COUNT_OFF, &count)) return nullptr;
    uint32_t slot = static_cast<uint32_t>(id) * 2;
    if (slot >= count) return nullptr;
    uint32_t entry = 0;
    if (!SafeReadU32(blob, NavRva::NPCDIC_TABLE_OFF + slot * 4, &entry) || entry == 0)
        return nullptr;
    // Sign-extend the s32 to a full pointer, as the game does (blob mapped low).
    return reinterpret_cast<const uint8_t*>(
        static_cast<intptr_t>(static_cast<int32_t>(entry)));
}

// The game's own (current-locale) display name for a field object, read memory-only
// from its SCENE OBJECT exactly as FUN_00263990 does: a name index at +0x102 selects
// the global npcdic dictionary; a negative index means a per-map custom string at
// +0xf8 (set by the map's fieldsignmes script). Empty on failure -> caller falls back
// to a category word. No game-function call — pure reads, SEH-guarded via MemRead.
std::wstring ResolveObjectName(void* sceneObj) {
    if (!sceneObj) return L"";
    int16_t idx = 0;
    if (!SafeReadS16(sceneObj, NavRva::SCENEOBJ_NAME_IDX, &idx)) return L"";
    const uint8_t* codec =
        (idx < 0) ? reinterpret_cast<const uint8_t*>(PtrAt(sceneObj, NavRva::SCENEOBJ_NAME_STR))
                  : NpcdicName(static_cast<int>(static_cast<uint32_t>(idx) & NavRva::NPCDIC_NAME_MASK));
    void* empty = Hooks::ResolveRva(NavRva::EMPTY_STRING);
    if (!codec || reinterpret_cast<void*>(const_cast<uint8_t*>(codec)) == empty) return L"";
    std::wstring s = GameText::Decode(codec, 256);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// FUN_003778b0() -> current-area name codec*. POD-only SEH wrapper.
typedef const uint8_t* (__fastcall* Pfn_AreaName)();
const uint8_t* CallAreaName() {
    Pfn_AreaName fn = reinterpret_cast<Pfn_AreaName>(Hooks::ResolveRva(NavRva::CURRENT_AREA_NAME));
    if (!fn) return nullptr;
    __try { return fn(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// True when an npcdic name key falls in the field gimmick-object band (433-469:
// treasure, urn, crystals, anchor). Used so a named gimmick is always listed even if
// its interaction flag is momentarily clear.
bool InGimmickBand(int16_t nameIdx) {
    if (nameIdx < 0) return false;
    int id = static_cast<int>(static_cast<uint32_t>(nameIdx) & NavRva::NPCDIC_NAME_MASK);
    return id >= 433 && id <= 469;
}

// Category from the interaction flags + the npcdic id band (both locale-independent).
// The named gimmick sub-types come from the npcdic id (sceneObj+0x102 when >= 0): ids
// 433-469 are the field gimmick-object band (434 Treasure, 468 Urn, 466 Gate Crystal,
// 469 Save Crystal, 435-459/467 area/life crystals). The NPC-vs-object split then rides
// the scene object's interaction flags (FUN_0025b820): FLAG_TALK => a talk target
// (person), otherwise an action target (gate/door/switch) => object. The spoken LABEL
// is always the game's own text; this only drives the category FILTER.
Category ClassifyByNameKey(uint32_t flags, int16_t nameIdx) {
    if (nameIdx >= 0) {
        int id = static_cast<int>(static_cast<uint32_t>(nameIdx) & NavRva::NPCDIC_NAME_MASK);
        if (id == 434 || id == 468)                    return Category::Treasure;
        if (id == 466)                                 return Category::GateCrystal;
        if (id == 469 || id == 467 || (id >= 435 && id <= 459))
                                                       return Category::SaveCrystal;
        if (id >= 433 && id <= 469)                    return Category::Object;   // misc gimmick
    }
    if (flags & NavRva::FLAG_TALK) return Category::NPC;   // talk target => person
    // No talk flag: a named person outside the gimmick band is still a person.
    if (nameIdx >= 0) {
        int id = static_cast<int>(static_cast<uint32_t>(nameIdx) & NavRva::NPCDIC_NAME_MASK);
        if (id < 433 || id >= 470) return Category::NPC;
    }
    return Category::Object;                               // action gimmick (gate/door/switch)
}

// Re-read live positions for the current set; drop objects whose transform no longer
// reads (despawned / mid-teardown). Keeps identity (scene-object ptr) stable so focus
// survives. Caller holds g_mutex.
void RefreshPositionsLocked(const FVec3& playerPos) {
    for (auto it = g_entities.begin(); it != g_entities.end();) {
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

bool ReadPlayer(FVec3& pos, float& yaw) {
    if (!PlayerState::ReadPlayerPos(pos)) return false;
    yaw = 0.0f;
    PlayerState::ReadPlayerYaw(yaw);   // best-effort; only egocentric uses it
    return true;
}

void SpeakEntityLocked(const Entity& e, const FVec3& playerPos, float yaw) {
    std::wstring phrase = e.label;
    phrase += L". ";
    phrase += NavCommon::DescribeDirection(playerPos, e.pos, g_egocentric, yaw);
    Speech::Output(phrase);
}

void SpeakNoTargets() { Speech::Output(L"No targets"); }

bool AlreadyListed(void* sceneObj) {
    for (const auto& e : g_entities) if (e.sceneObj == sceneObj) return true;
    return false;
}

// Append live COMBATANTS (allies + enemies) from the BtlWork pool. Field NPCs/gimmicks come
// from the handle table above; battle combatants (party, guests, enemies) live in this pool
// with their `def` record — they carry NO talk/action flag and NO npcdic key, so the handle-
// table filter drops them, and they must be read here (this is what makes them navigable in a
// battle). Per slot: def = actor+0x698 (null = empty); active = actor+0 & 0x10; SKIP the player-
// controlled unit (def+5 == 0 = the leader). Enemy vs ally = the def-attribute bit 24 the game's
// own target classifiers read (def+5 is player-vs-AI, NOT faction — runtime-disproven). Name =
// codec* at actor+0x18; position via the actor's scene node. Identity = the scene object, so it
// dedupes against the handle-table pass. Caller holds g_mutex.
void ScanCombatantsLocked() {
    void* pool = PtrAt(Hooks::ResolveRva(NavRva::ACTOR_POOL_BASE), 0);
    if (!pool) return;
    uint32_t count = 0;
    if (!SafeReadU32(Hooks::ResolveRva(NavRva::ACTOR_POOL_COUNT), 0, &count) || count == 0)
        return;
    if (count > 64) count = 64;   // sanity clamp (pool is 32 slots)

    for (uint32_t i = 0; i < count; ++i) {
        void* actor = static_cast<char*>(pool) + static_cast<size_t>(i) * NavRva::ACTOR_STRIDE;
        void* def = PtrAt(actor, NavRva::ACTOR_DEF_PTR);
        if (!def) continue;                                              // empty slot
        uint8_t active = 0;
        if (!SafeReadU8(actor, NavRva::ACTOR_ACTIVE_OFF, &active) ||
            (active & NavRva::ACTOR_ACTIVE_BIT) == 0) continue;          // not active / no model
        uint8_t def5 = 0xff;
        if (!SafeReadU8(def, NavRva::DEF_KIND_BYTE, &def5) ||
            def5 == NavRva::PLAYER_DEF_KIND) continue;                   // player-controlled leader -> skip
        void* sceneObj = PtrAt(actor, NavRva::ACTOR_SCENEOBJ);
        if (!sceneObj || AlreadyListed(sceneObj)) continue;

        // Enemy vs ally = the scene-kind nibble (the game's own faction test): kind==3 => ally,
        // kind==5 => dead/removed (drop), else => enemy.
        uint8_t kindByte = 0;
        if (!SafeReadU8(sceneObj, NavRva::SCENEOBJ_KIND_OFF, &kindByte)) continue;
        const uint8_t kind = kindByte & NavRva::KIND_MASK;
        if (kind == NavRva::KIND_DEAD) continue;                         // dead/removed

        FVec3 pos;
        if (!PlayerState::ReadSceneObjectPos(sceneObj, pos)) continue;
        if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) continue;   // unplaced reserve unit

        Entity e;
        e.sceneObj = sceneObj;
        e.flags    = 0;
        e.nameIdx  = -1;
        e.pos      = pos;
        e.category = (kind == NavRva::KIND_ALLY) ? Category::NPC : Category::Enemy;
        const uint8_t* codec = reinterpret_cast<const uint8_t*>(PtrAt(actor, NavRva::ACTOR_NAME_STR));
        if (codec) {
            std::wstring s = GameText::Decode(codec, 256);
            if (GameText::IsMostlyPrintable(s)) e.label = s;
        }
        if (e.label.empty()) e.label = CategoryWord(e.category);
        g_entities.push_back(e);
    }
}

// Rebuild the set from the scene-object HANDLE TABLE — the game's own registry of live
// interactive field objects (DAT_02098e10, 5 containers), populated at map load and
// walked every frame by FUN_0025b820 to decide what the player is near. We list every
// live talk/action object with a readable position: NPCs (FLAG_TALK) and action gimmicks
// like gates/doors/switches/treasure/crystals (FLAG_ACTION) — the latter are present from
// load, not spawned on approach. Positions come from the scene object's transform
// (sceneObj+0xB8), the same chain the leader uses. Caller holds g_mutex.
int RescanLocked() {
    g_entities.clear();
    if (!PlayerState::IsFieldActive()) return 0;

    void* base = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!base) return 0;
    void* leader = PlayerState::ReadLeaderSceneObject();

    for (uint32_t c = 0; c < NavRva::HANDLE_TABLE_CONTAINERS; ++c) {
        void* table = static_cast<char*>(base) + static_cast<size_t>(c) * NavRva::HANDLE_TABLE_STRIDE;
        uint8_t active = 0;
        if (!SafeReadU8(table, NavRva::TBL_ACTIVE_OFF, &active) || (active & 1) == 0) continue;
        void* entries = PtrAt(table, NavRva::TBL_ENTRIES_OFF);
        if (!entries) continue;
        uint32_t count = 0;
        if (!SafeReadU32(entries, NavRva::ENTRIES_COUNT_OFF, &count) || count == 0 || count > 4096)
            continue;

        for (uint32_t i = 0; i < count; ++i) {
            void* obj = PtrAt(entries, NavRva::ENTRIES_SLOT0_OFF + i * 8);
            if (!obj || obj == leader) continue;

            uint32_t flags = 0;
            SafeReadU32(obj, NavRva::SCENEOBJ_FLAGS_OFF, &flags);
            int16_t nameIdx = 0;
            SafeReadS16(obj, NavRva::SCENEOBJ_NAME_IDX, &nameIdx);
            // List interactive objects (NPCs / action gimmicks) plus any named gimmick.
            const bool interactive = (flags & (NavRva::FLAG_TALK | NavRva::FLAG_ACTION)) != 0;
            if (!interactive && !InGimmickBand(nameIdx)) continue;

            FVec3 pos;
            if (!PlayerState::ReadSceneObjectPos(obj, pos)) continue;
            if (AlreadyListed(obj)) continue;

            Entity e;
            e.sceneObj = obj;
            e.flags = flags;
            e.nameIdx = nameIdx;
            e.pos = pos;
            e.label = ResolveObjectName(obj);
            e.category = ClassifyByNameKey(flags, e.nameIdx);
            if (e.label.empty()) e.label = CategoryWord(e.category);
            g_entities.push_back(e);
        }
    }

    // Combatants (allies + enemies) come from the BtlWork pool, not the handle table — the
    // handle-table filter drops them, so in a battle this is what makes them navigable.
    ScanCombatantsLocked();

    char msg[64];
    snprintf(msg, sizeof(msg), "rescan: %zu field objects", g_entities.size());
    Log::Write("NAV", msg);
    return static_cast<int>(g_entities.size());
}

// Bitmask of currently-active handle-table containers (bit c set iff container c's active
// flag is set). Memory-only + SEH-guarded; touches only the handle table, not g_entities.
uint32_t ActiveContainerMask() {
    void* base = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!base) return 0;
    uint32_t mask = 0;
    for (uint32_t c = 0; c < NavRva::HANDLE_TABLE_CONTAINERS; ++c) {
        void* table = static_cast<char*>(base) + static_cast<size_t>(c) * NavRva::HANDLE_TABLE_STRIDE;
        uint8_t active = 0;
        if (SafeReadU8(table, NavRva::TBL_ACTIVE_OFF, &active) && (active & 1))
            mask |= (1u << c);
    }
    return mask;
}

} // namespace

// --- public API ------------------------------------------------------------

bool Init() {
    Log::Write("NAV", "entity list ready");
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_entities.clear();
    g_currentObj = nullptr;
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
    uint32_t mask = ActiveContainerMask();
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
}

std::wstring CurrentAreaName() {
    const uint8_t* p = CallAreaName();
    if (!p || reinterpret_cast<void*>(const_cast<uint8_t*>(p)) == Hooks::ResolveRva(NavRva::EMPTY_STRING))
        return L"";
    std::wstring s = GameText::Decode(p, 128);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
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
static void CycleLocked(int dir, const FVec3& playerPos, float yaw) {
    RefreshPositionsLocked(playerPos);
    std::vector<size_t> view = FilteredSortedLocked();
    if (view.empty()) { g_currentObj = nullptr; SpeakNoTargets(); return; }

    // Find current focus within the view.
    int cur = -1;
    for (int i = 0; i < static_cast<int>(view.size()); ++i)
        if (g_entities[view[i]].sceneObj == g_currentObj) { cur = i; break; }

    int next = (cur < 0) ? 0
                         : ((cur + dir) % static_cast<int>(view.size()) + static_cast<int>(view.size()))
                               % static_cast<int>(view.size());
    const Entity& e = g_entities[view[next]];
    g_currentObj = e.sceneObj;
    SpeakEntityLocked(e, playerPos, yaw);
}

void CmdNext() {
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();   // fresh — pick up objects that appeared since the last command
    FVec3 p; float yaw;
    if (!ReadPlayer(p, yaw)) { Speech::Output(L"Position unavailable"); return; }
    CycleLocked(+1, p, yaw);
}

void CmdPrev() {
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();
    FVec3 p; float yaw;
    if (!ReadPlayer(p, yaw)) { Speech::Output(L"Position unavailable"); return; }
    CycleLocked(-1, p, yaw);
}

void CmdDescribeCurrent() {
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();
    FVec3 p; float yaw;
    if (!ReadPlayer(p, yaw)) { Speech::Output(L"Position unavailable"); return; }
    RefreshPositionsLocked(p);
    std::vector<size_t> view = FilteredSortedLocked();
    if (view.empty()) { SpeakNoTargets(); return; }
    // Speak the current focus, or the nearest if no focus yet.
    size_t sel = view[0];
    for (size_t idx : view) if (g_entities[idx].sceneObj == g_currentObj) { sel = idx; break; }
    g_currentObj = g_entities[sel].sceneObj;
    SpeakEntityLocked(g_entities[sel], p, yaw);

    // Obstacle-aware hint toward the selection (<=5 rays; safe on the input thread).
    // A full A* grid is a later enhancement (thousands of rays => needs game-thread
    // execution to avoid racing the physics step).
    if (MapQuery::HasWorld()) {
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
                    s += NavCommon::CardinalOfHeading(a);
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
    g_currentObj = nullptr;   // re-anchor to nearest on next cycle
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
    // Prefer the focused object; else the nearest in the active filter.
    for (const auto& e : g_entities) {
        if (e.sceneObj == g_currentObj) { outPos = e.pos; outLabel = e.label; return true; }
    }
    std::vector<size_t> view = FilteredSortedLocked();
    if (view.empty()) return false;
    outPos = g_entities[view[0]].pos;
    outLabel = g_entities[view[0]].label;
    return true;
}

void SetEgocentric(bool on) { g_egocentric = on; }
bool IsEgocentric() { return g_egocentric; }

// Dump the raw handle table: every live scene object in every container, with its
// interaction flags, npcdic name key, resolved name, and world position. This is the
// data that proves where a given interactive object (e.g. the tutorial gate) actually
// lives and how it should classify. File-only (never spoken); bounded by the per-
// container count clamp. Interactive (talk/action) objects get a full line; the rest
// are only counted.
void LogDiagnostic() {
    std::lock_guard<std::mutex> lk(g_mutex);
    FVec3 pp; bool haveP = PlayerState::ReadPlayerPos(pp);
    void* leader = PlayerState::ReadLeaderSceneObject();
    void* base = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);

    char hdr[160];
    snprintf(hdr, sizeof(hdr),
             "==== handle-table diag: player=(%.2f,%.2f,%.2f) haveP=%d leader=%p ====",
             pp.x, pp.y, pp.z, haveP ? 1 : 0, leader);
    Log::Write("NAV-DIAG", hdr);

    for (uint32_t c = 0; c < NavRva::HANDLE_TABLE_CONTAINERS && base; ++c) {
        void* table = static_cast<char*>(base) + static_cast<size_t>(c) * NavRva::HANDLE_TABLE_STRIDE;
        uint8_t active = 0;
        SafeReadU8(table, NavRva::TBL_ACTIVE_OFF, &active);
        void* entries = PtrAt(table, NavRva::TBL_ENTRIES_OFF);
        uint32_t count = 0;
        if (entries) SafeReadU32(entries, NavRva::ENTRIES_COUNT_OFF, &count);

        uint32_t shown = 0, plain = 0;
        if (entries && (active & 1) && count <= 4096) {
            for (uint32_t i = 0; i < count; ++i) {
                void* obj = PtrAt(entries, NavRva::ENTRIES_SLOT0_OFF + i * 8);
                if (!obj) { ++plain; continue; }
                uint32_t flags = 0;
                SafeReadU32(obj, NavRva::SCENEOBJ_FLAGS_OFF, &flags);
                int16_t nameIdx = 0;
                SafeReadS16(obj, NavRva::SCENEOBJ_NAME_IDX, &nameIdx);
                std::wstring lbl = ResolveObjectName(obj);
                // Skip the mass of anonymous, flagless props/triggers; dump anything
                // interactive, named, or in the gimmick band (where the gate must fall).
                if (flags == 0 && lbl.empty() && !InGimmickBand(nameIdx)) { ++plain; continue; }
                ++shown;

                uint8_t catByte = 0, readyByte = 0;
                SafeReadU8(obj, NavRva::SCENEOBJ_TYPE_BYTE, &catByte);   // low5=category, high3=class
                SafeReadU8(obj, 0x14, &readyByte);                      // & 0x20 model, & 0x40 ready
                FVec3 pos; bool havePos = PlayerState::ReadSceneObjectPos(obj, pos);
                char nlabel[48] = {};
                for (size_t k = 0; k < lbl.size() && k < 47; ++k)
                    nlabel[k] = (lbl[k] < 128) ? static_cast<char>(lbl[k]) : '?';
                char line[256];
                snprintf(line, sizeof(line),
                         "    [%u:%u] obj=%p cat=%02X r14=%02X flags=%08X%s%s nameIdx=%d \"%s\" pos=(%.2f,%.2f,%.2f) hp=%d",
                         c, i, obj, catByte, readyByte, flags,
                         (flags & NavRva::FLAG_TALK) ? " TALK" : "",
                         (flags & NavRva::FLAG_ACTION) ? " ACT" : "",
                         nameIdx, nlabel, pos.x, pos.y, pos.z, havePos ? 1 : 0);
                Log::Write("NAV-DIAG", line);
            }
        }
        char ch[128];
        snprintf(ch, sizeof(ch),
                 "  container %u: active=%u count=%u shown=%u plain=%u",
                 c, active & 1, count, shown, plain);
        Log::Write("NAV-DIAG", ch);
    }
    // Combatant pool dump (party + enemies) — CONFIRMS the enemy discriminator. def+5 is
    // player-vs-AI (0 = the leader, skipped), NOT faction. The faction test is the scene-kind
    // nibble kind = *(u8)(sceneObj+0x0e)&0xf: kind==3 ally, {1,2,7} enemy, 5 dead. Expect the
    // enemies (Imperial Swordsman, Air Cutter Remora) kind!=3 and the party (Reks/Basch/soldiers)
    // kind==3. charid = def+4 (roster index) is a secondary cross-check.
    void* pool = PtrAt(Hooks::ResolveRva(NavRva::ACTOR_POOL_BASE), 0);
    uint32_t pcount = 0;
    SafeReadU32(Hooks::ResolveRva(NavRva::ACTOR_POOL_COUNT), 0, &pcount);
    if (pcount > 64) pcount = 64;
    char ph[96];
    snprintf(ph, sizeof(ph), "==== combatant pool: base=%p count=%u ====", pool, pcount);
    Log::Write("NAV-DIAG", ph);
    for (uint32_t i = 0; pool && i < pcount; ++i) {
        void* actor = static_cast<char*>(pool) + static_cast<size_t>(i) * NavRva::ACTOR_STRIDE;
        void* def = PtrAt(actor, NavRva::ACTOR_DEF_PTR);
        if (!def) continue;                                  // empty slot
        uint8_t active = 0, def5 = 0xff, charid = 0, kindByte = 0;
        SafeReadU8(actor, NavRva::ACTOR_ACTIVE_OFF, &active);
        SafeReadU8(def, NavRva::DEF_KIND_BYTE, &def5);
        SafeReadU8(def, NavRva::DEF_CHARID, &charid);
        void* sceneObj = PtrAt(actor, NavRva::ACTOR_SCENEOBJ);
        if (sceneObj) SafeReadU8(sceneObj, NavRva::SCENEOBJ_KIND_OFF, &kindByte);
        int kind = kindByte & NavRva::KIND_MASK;
        const char* faction = (kind == NavRva::KIND_DEAD) ? "dead"
                            : (def5 == NavRva::PLAYER_DEF_KIND || kind == NavRva::KIND_ALLY) ? "ally"
                            : "enemy";
        FVec3 pos{}; bool havePos = sceneObj && PlayerState::ReadSceneObjectPos(sceneObj, pos);
        std::wstring lbl;
        const uint8_t* codec = reinterpret_cast<const uint8_t*>(PtrAt(actor, NavRva::ACTOR_NAME_STR));
        if (codec) { std::wstring s = GameText::Decode(codec, 256); if (GameText::IsMostlyPrintable(s)) lbl = s; }
        char nlabel[48] = {};
        for (size_t k = 0; k < lbl.size() && k < 47; ++k) nlabel[k] = (lbl[k] < 128) ? static_cast<char>(lbl[k]) : '?';
        char line[256];
        snprintf(line, sizeof(line),
                 "    pool[%u] actor=%p active=%02X def+5=%d kind=%d charid=%d -> %s \"%s\" pos=(%.2f,%.2f,%.2f) posOk=%d",
                 i, actor, active, static_cast<int>(static_cast<int8_t>(def5)),
                 kind, charid, faction,
                 nlabel, pos.x, pos.y, pos.z, havePos ? 1 : 0);
        Log::Write("NAV-DIAG", line);
    }

    Log::Write("NAV-DIAG", "==== end handle-table diag ====");
}

} // namespace EntityList

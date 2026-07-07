#include "navigation/entity_list.h"
#include "navigation/nav_rva.h"
#include "navigation/nav_common.h"
#include "navigation/player_state.h"
#include "navigation/bullet_query.h"
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

struct Entity {
    void*        actor    = nullptr;   // pool slot ptr (identity across refreshes)
    void*        sceneObj = nullptr;   // *(actor+0x10)
    uint32_t     slot     = 0;         // pool index (for field-sign category lookup)
    uint16_t     defId    = 0;
    uint8_t      kind     = 0;         // 0 = character/NPC, 1 = gimmick
    Category     category = Category::Object;
    std::wstring label;
    FVec3        pos;
    float        dist2D   = 0.0f;      // to player, refreshed per command
};

std::mutex             g_mutex;
std::vector<Entity>    g_entities;
Category               g_currentCategory = Category::All;
void*                  g_currentActor    = nullptr;  // focus, tracked across refreshes
bool                   g_egocentric      = false;    // cardinal by default

// --- helpers ---------------------------------------------------------------

void* PoolBase() { return PtrAt(Hooks::ResolveRva(NavRva::ACTOR_POOL_BASE), 0); }

uint32_t PoolCount() {
    uint32_t n = 0;
    SafeReadU32(Hooks::ResolveRva(NavRva::ACTOR_POOL_COUNT), 0, &n);
    return n;
}

bool ReadActorPos(void* actor, FVec3& out) {
    float x = 0, y = 0, z = 0;
    if (!SafeReadF32(actor, NavRva::ACTOR_POS_X, &x)) return false;
    if (!SafeReadF32(actor, NavRva::ACTOR_POS_Y, &y)) return false;
    if (!SafeReadF32(actor, NavRva::ACTOR_POS_Z, &z)) return false;
    out = FVec3{ x, y, z };
    return true;
}

const wchar_t* CategoryWord(Category c) {
    switch (c) {
        case Category::All:         return L"All";
        case Category::Exit:        return L"Exit";
        case Category::SaveCrystal:  return L"Save Crystal";
        case Category::GateCrystal:  return L"Gate Crystal";
        case Category::Treasure:    return L"Treasure";
        case Category::NPC:         return L"Person";
        case Category::Object:      return L"Object";
        default:                    return L"Object";
    }
}

// FUN_0035d380(type, objid) -> static name-context ptr. POD-only SEH wrapper (the
// resolver walks game tables; a torn read degrades to null). Kept separate from the
// wstring code so the __try scope holds no C++ objects.
typedef void* (__fastcall* Pfn_NameCtx)(int type, int objid);
void* CallNameCtx(int objid) {
    Pfn_NameCtx fn = reinterpret_cast<Pfn_NameCtx>(Hooks::ResolveRva(NavRva::OBJ_NAME_RESOLVE));
    if (!fn) return nullptr;
    __try { return fn(1, objid); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// The game's own (current-locale) display name for a field object, via the master
// resolver. Empty on failure — caller falls back to a category word.
std::wstring ResolveObjectName(void* def, uint8_t kind) {
    uint16_t id16 = 0;
    if (!SafeReadU16(def, NavRva::DEF_ID_U16, &id16)) return L"";
    void* ctx = CallNameCtx(id16);
    if (!ctx) return L"";
    void* empty = Hooks::ResolveRva(NavRva::EMPTY_STRING);
    uint32_t primary = (kind == 0) ? NavRva::NAMECTX_NPC_OFF : NavRva::NAMECTX_GIMMICK_OFF;
    uint32_t alt     = (kind == 0) ? NavRva::NAMECTX_GIMMICK_OFF : NavRva::NAMECTX_NPC_OFF;
    void* p = PtrAt(ctx, primary);
    if (!p || p == empty) p = PtrAt(ctx, alt);
    if (!p || p == empty) return L"";
    std::wstring s = GameText::Decode(reinterpret_cast<const uint8_t*>(p), 256);
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

// Best-effort category from the resolved name (English keywords) + kind. The LABEL
// is always the game's own text; this only drives the category FILTER, so a
// non-English game simply falls back to Person/Object.
Category ClassifyByName(const std::wstring& name, uint8_t kind) {
    if (name.find(L"Save Crystal") != std::wstring::npos) return Category::SaveCrystal;
    if (name.find(L"Gate Crystal") != std::wstring::npos) return Category::GateCrystal;
    if (name.find(L"Treasure")     != std::wstring::npos) return Category::Treasure;
    return (kind == 0) ? Category::NPC : Category::Object;
}

// Re-read live positions for the current set; drop slots that went empty. Keeps
// entity identity (actor ptr) stable so focus survives. Caller holds g_mutex.
void RefreshPositionsLocked(const FVec3& playerPos) {
    for (auto it = g_entities.begin(); it != g_entities.end();) {
        void* def = PtrAt(it->actor, NavRva::ACTOR_DEF_PTR);
        FVec3 p;
        if (!def || !ReadActorPos(it->actor, p)) { it = g_entities.erase(it); continue; }
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

// Rebuild the set from the live actor pool. Caller holds g_mutex.
int RescanLocked() {
    g_entities.clear();
    if (!PlayerState::IsFieldActive()) return 0;

    void* base = PoolBase();
    uint32_t count = PoolCount();
    if (!base || count == 0 || count > 4096) return 0;   // sanity clamp

    void* leaderScene = PlayerState::ReadLeaderSceneObject();

    for (uint32_t i = 0; i < count; ++i) {
        void* actor = static_cast<char*>(base) + static_cast<size_t>(i) * NavRva::ACTOR_STRIDE;
        void* def = PtrAt(actor, NavRva::ACTOR_DEF_PTR);
        if (!def) continue;                               // empty slot

        Entity e;
        e.actor = actor;
        e.slot = i;
        e.sceneObj = PtrAt(actor, NavRva::ACTOR_SCENEOBJ);
        if (e.sceneObj && e.sceneObj == leaderScene) continue;   // skip the player

        SafeReadU8(def, NavRva::DEF_KIND_BYTE, &e.kind);
        SafeReadU16(def, NavRva::DEF_ID_U16, &e.defId);
        if (!ReadActorPos(actor, e.pos)) continue;

        e.label = ResolveObjectName(def, e.kind);            // game's own localized name
        e.category = ClassifyByName(e.label, e.kind);
        if (e.label.empty()) e.label = CategoryWord(e.category);
        g_entities.push_back(e);
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "rescan: %zu field objects", g_entities.size());
    Log::Write("NAV", msg);
    return static_cast<int>(g_entities.size());
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
    g_currentActor = nullptr;
}

int Rescan() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return RescanLocked();
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
    if (view.empty()) { g_currentActor = nullptr; SpeakNoTargets(); return; }

    // Find current focus within the view.
    int cur = -1;
    for (int i = 0; i < static_cast<int>(view.size()); ++i)
        if (g_entities[view[i]].actor == g_currentActor) { cur = i; break; }

    int next = (cur < 0) ? 0
                         : ((cur + dir) % static_cast<int>(view.size()) + static_cast<int>(view.size()))
                               % static_cast<int>(view.size());
    const Entity& e = g_entities[view[next]];
    g_currentActor = e.actor;
    SpeakEntityLocked(e, playerPos, yaw);
}

void CmdNext() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_entities.empty()) RescanLocked();   // lazy first scan
    FVec3 p; float yaw;
    if (!ReadPlayer(p, yaw)) { Speech::Output(L"Position unavailable"); return; }
    CycleLocked(+1, p, yaw);
}

void CmdPrev() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_entities.empty()) RescanLocked();
    FVec3 p; float yaw;
    if (!ReadPlayer(p, yaw)) { Speech::Output(L"Position unavailable"); return; }
    CycleLocked(-1, p, yaw);
}

void CmdDescribeCurrent() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_entities.empty()) RescanLocked();
    FVec3 p; float yaw;
    if (!ReadPlayer(p, yaw)) { Speech::Output(L"Position unavailable"); return; }
    RefreshPositionsLocked(p);
    std::vector<size_t> view = FilteredSortedLocked();
    if (view.empty()) { SpeakNoTargets(); return; }
    // Speak the current focus, or the nearest if no focus yet.
    size_t sel = view[0];
    for (size_t idx : view) if (g_entities[idx].actor == g_currentActor) { sel = idx; break; }
    g_currentActor = g_entities[sel].actor;
    SpeakEntityLocked(g_entities[sel], p, yaw);

    // Obstacle-aware hint toward the selection (<=5 rays; safe on the input thread).
    // A full A* grid is a later enhancement (thousands of rays => needs game-thread
    // execution to avoid racing the physics step).
    if (BulletQuery::HasWorld()) {
        const FVec3 tgt = g_entities[sel].pos;
        const float margin = 0.6f;   // ~capsule radius, meters
        if (BulletQuery::HorizontalClear(p, tgt, margin)) {
            Speech::SpeakQueued(L"Path clear");
        } else {
            const float base = std::atan2(tgt.x - p.x, tgt.z - p.z);
            float dist = NavCommon::Distance2D(p, tgt);
            const float probe = dist < 5.0f ? dist : 5.0f;   // look ~5 m per heading
            const float offs[4] = { 0.785398f, -0.785398f, 1.570796f, -1.570796f };  // +/-45, +/-90
            bool found = false;
            for (float o : offs) {
                const float a = base + o;
                const FVec3 pt{ p.x + std::sin(a) * probe, p.y, p.z + std::cos(a) * probe };
                if (BulletQuery::HorizontalClear(p, pt, margin)) {
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
    // Count matches + speak category name.
    size_t matches = 0;
    for (auto& e : g_entities)
        if (g_currentCategory == Category::All || e.category == g_currentCategory) ++matches;
    wchar_t buf[96];
    _snwprintf_s(buf, _TRUNCATE, L"%s, %zu", CategoryWord(g_currentCategory), matches);
    Speech::Output(buf);
    g_currentActor = nullptr;   // re-anchor to nearest on next cycle
}

void CmdNextCategory() { std::lock_guard<std::mutex> lk(g_mutex); ChangeCategoryLocked(+1); }
void CmdPrevCategory() { std::lock_guard<std::mutex> lk(g_mutex); ChangeCategoryLocked(-1); }

void SetEgocentric(bool on) { g_egocentric = on; }
bool IsEgocentric() { return g_egocentric; }

void LogDiagnostic() {
    std::lock_guard<std::mutex> lk(g_mutex);
    FVec3 p; bool haveP = PlayerState::ReadPlayerPos(p);
    if (haveP) RefreshPositionsLocked(p);

    char hdr[128];
    snprintf(hdr, sizeof(hdr), "==== entity diag: %zu objects, player=(%.2f,%.2f,%.2f) haveP=%d ====",
             g_entities.size(), p.x, p.y, p.z, haveP ? 1 : 0);
    Log::Write("NAV-DIAG", hdr);

    // Field-state manager (holds the field-sign category tables).
    void* mgr = PtrAt(Hooks::ResolveRva(NavRva::FIELD_STATE_BLOCK), 0);

    int idx = 0;
    for (auto& e : g_entities) {
        // First 16 def bytes so the save/treasure discriminator can be pinned.
        uint8_t db[16] = {};
        void* def = PtrAt(e.actor, NavRva::ACTOR_DEF_PTR);
        for (int b = 0; b < 16; ++b) SafeReadU8(def, b, &db[b]);
        // Field-sign category bytes for this slot (the icon the game floats over it).
        uint8_t catA = 0xFF, catB = 0xFF;
        if (mgr) {
            SafeReadU8(mgr, NavRva::FIELDSIGN_CAT_A_OFF + e.slot * 2, &catA);
            SafeReadU8(mgr, NavRva::FIELDSIGN_CAT_B_OFF + e.slot * 2, &catB);
        }
        char nlabel[48] = {};
        for (size_t k = 0; k < e.label.size() && k < 47; ++k)
            nlabel[k] = (e.label[k] < 128) ? static_cast<char>(e.label[k]) : '?';
        char line[320];
        snprintf(line, sizeof(line),
                 "  [%02d] slot=%u kind=%u defId=0x%04X \"%s\" pos=(%.2f,%.2f,%.2f) d=%.1f cat=%02X/%02X def[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X",
                 idx++, e.slot, e.kind, e.defId, nlabel, e.pos.x, e.pos.y, e.pos.z, e.dist2D,
                 catA, catB, db[0], db[1], db[2], db[3], db[4], db[5], db[6], db[7]);
        Log::Write("NAV-DIAG", line);
    }
    Log::Write("NAV-DIAG", "==== end entity diag ====");
}

} // namespace EntityList

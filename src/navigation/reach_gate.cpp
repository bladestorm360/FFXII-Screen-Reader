#include "navigation/reach_gate.h"
#include "navigation/nav_reach.h"
#include "navigation/nav_rva.h"
#include "navigation/map_names.h"
#include "navigation/player_state.h"
#include "ui/mod_menu.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/logger.h"
#include "core/stall_probe.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace ReachGate {

namespace {

using EntityList::Category;
using EntityScan::Entity;

constexpr int kPolysPerFrame = 256;                 // same slice NavReach uses, same reason
constexpr int kMaxPolys      = NavMesh::kMaxPolys;

// Rings an entity is judged on: its own point, then where a player would stand to press it. 3 m is the
// class-1 route reach (nav_commands), so "reachable" means "somewhere you could interact from".
const float   kJudgeRadii[] = { 0.0f, 1.5f, 3.0f };
constexpr int kJudgeDirs    = 8;

// The party class -> the effective-flags bit that refuses it (GameArchitecture.md: bit 23 class 0,
// 24 class 5, 25 class 1, 26 class 2, 27 class 3). 0 = no refusal bit known for this class, in which
// case nothing is ever called script-closed and the gate answers Reachable wherever NavReach does.
uint32_t RefuseBit(uint16_t cls) {
    switch (cls) {
    case 0: return 1u << 23;
    case 1: return 1u << 25;
    case 2: return 1u << 26;
    case 3: return 1u << 27;
    case 5: return 1u << 24;
    default: return 0;
    }
}

// ---- published answer ------------------------------------------------------------------------
std::mutex                                         g_pubMutex;
std::shared_ptr<const std::unordered_set<int32_t>> g_published;   // null while (re)filling

// ---- fill state (GAME THREAD ONLY) -----------------------------------------------------------
std::unordered_set<int32_t>  g_seen;    // visited, including refused closed polys
std::unordered_set<int32_t>  g_open;    // expanded: the component that gets published
std::vector<NavMesh::PolyId> g_stack;
uint32_t        g_epoch     = 0xFFFFFFFFu;
uint64_t        g_tableFp   = 0;
NavMesh::PolyId g_startPoly = NavMesh::kNoPoly;
bool            g_running   = false;
int             g_tableChangesLogged = 0;
// Evidence for the premise, gathered while filling: how many polys the flood refused as
// script-closed, which material ids they carried, and one sample of raw vs effective flags.
int      g_closedRefused = 0;
uint32_t g_closedMaterials = 0;
uint32_t g_sampleRaw = 0, g_sampleEff = 0;
int32_t  g_samplePoly = -1;

// FNV-1a over the two override banks FUN_00232020 applies (material 0x00-0x1F, group 0x40-0x4F).
uint64_t TableFingerprint() {
    void* tbl = Hooks::ResolveRva(NavRva::WALK_FLAG_TABLE);
    if (!tbl) return 0;
    uint8_t buf[NavRva::WALK_FLAG_ENTRIES * 8] = {};
    if (!MemRead::SafeReadBytes(tbl, buf, sizeof(buf))) return 0;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(buf); ++i) {
        const size_t entry = i / 8;
        if (entry >= 0x20 && entry < NavRva::WALK_FLAG_GROUP_BASE) continue;   // wall bank: not floor
        h ^= buf[i];
        h *= 1099511628211ull;
    }
    return h;
}

void Unpublish() {
    std::lock_guard<std::mutex> lk(g_pubMutex);
    g_published.reset();
}

bool ScriptClosedFor(NavMesh::PolyId p, uint32_t bit, uint32_t* rawOut, uint32_t* effOut) {
    if (bit == 0) return false;
    uint32_t raw = 0, eff = 0;
    if (!NavMesh::PolyFlags(p, raw, eff)) return false;
    if (rawOut) *rawOut = raw;
    if (effOut) *effOut = eff;
    return (raw & bit) == 0 && (eff & bit) != 0;
}

// ---- verdict log: once per change of verdict, per entity, per map ------------------------------
struct Seen { void* obj; std::wstring label; uint8_t verdict; };
std::mutex        g_logMutex;
int               g_logMap = -1;
std::vector<Seen> g_logSeen;

const char* Name(Verdict v) {
    switch (v) {
    case Verdict::Reachable:         return "reachable";
    case Verdict::BehindClosedFloor: return "BEHIND A SCRIPT-CLOSED FLOOR";
    case Verdict::Disconnected:      return "NOT CONNECTED (no walkable mesh path)";
    default:                         return "unknown";
    }
}

Verdict Judge(const FVec3& pos) {
    if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) return Verdict::Unknown;   // unplaced object
    std::shared_ptr<const std::unordered_set<int32_t>> open;
    {
        std::lock_guard<std::mutex> lk(g_pubMutex);
        open = g_published;
    }
    if (!open || open->empty()) return Verdict::Unknown;

    bool anyFound = false, inPermissive = false, permissiveAnswered = true;
    for (float r : kJudgeRadii) {
        const int dirs = (r == 0.0f) ? 1 : kJudgeDirs;
        for (int d = 0; d < dirs; ++d) {
            const float a = static_cast<float>(d) * (6.2831853f / static_cast<float>(kJudgeDirs));
            const NavMesh::PolyId p =
                NavMesh::FindPolyAt(pos.x + r * std::cos(a), pos.y, pos.z + r * std::sin(a));
            if (p == NavMesh::kNoPoly) continue;
            anyFound = true;
            if (open->count(p)) return Verdict::Reachable;
            bool answered = false;
            if (NavReach::ContainsPoly(p, &answered)) inPermissive = true;
            if (!answered) permissiveAnswered = false;
        }
    }
    if (!anyFound || !permissiveAnswered) return Verdict::Unknown;
    return inPermissive ? Verdict::BehindClosedFloor : Verdict::Disconnected;
}

std::string Ascii(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back((c < 128) ? static_cast<char>(c) : '?');
    return s;
}

} // namespace

bool ScriptClosed(NavMesh::PolyId p) {
    return ScriptClosedFor(p, RefuseBit(PlayerState::PartyMovementClass()), nullptr, nullptr);
}

void OnGameFrame(uint32_t epoch, const FVec3& playerPos) {
    NavMesh::EnsureEpoch(epoch);
    if (!NavMesh::Ready()) return;
    const NavMesh::PolyId here = NavMesh::FindPolyAt(playerPos.x, playerPos.y, playerPos.z);
    if (here == NavMesh::kNoPoly) return;

    const uint64_t fp = TableFingerprint();
    const char* why = nullptr;
    if (epoch != g_epoch) {
        why = "new map epoch";
        g_tableChangesLogged = 0;
    } else if (fp != g_tableFp) {
        why = "the floor override table changed (a door or barrier opened or closed)";
    } else if (!g_running) {
        std::lock_guard<std::mutex> lk(g_pubMutex);
        if (g_published && !g_published->count(here)) why = "the player left the published component";
    }

    if (why) {
        // A table that churns every frame would restart this every frame. Say so a bounded number of
        // times; the gate simply stays Unknown (never hides) while it cannot settle.
        if (epoch == g_epoch && fp != g_tableFp && ++g_tableChangesLogged > 8) {
            if (g_tableChangesLogged == 9)
                Log::Write("NAV-ROUTE", "reach-gate: override table keeps changing -- further restarts not logged this map");
        } else {
            char m[224];
            snprintf(m, sizeof(m), "reach-gate: (re)starting open-component flood from poly %d -- %s",
                     here, why);
            Log::Write("NAV-ROUTE", m);
        }
        Unpublish();
        g_epoch     = epoch;
        g_tableFp   = fp;
        g_startPoly = here;
        g_seen.clear();
        g_open.clear();
        g_stack.clear();
        g_seen.insert(here);                // seeded even if the player stands on a closed poly
        g_open.insert(here);
        g_stack.push_back(here);
        g_closedRefused = 0;
        g_closedMaterials = 0;
        g_samplePoly = -1;
        g_running   = true;
    }
    if (!g_running) return;

    STALL_SCOPE("ReachGate::fill");
    const uint32_t bit = RefuseBit(PlayerState::PartyMovementClass());
    int budget = kPolysPerFrame;
    while (!g_stack.empty() && budget-- > 0 && static_cast<int>(g_seen.size()) < kMaxPolys) {
        const NavMesh::PolyId cur = g_stack.back();
        g_stack.pop_back();
        for (int e = 0; e < 3; ++e) {
            const NavMesh::PolyId n = NavMesh::Neighbor(cur, e);
            if (n == NavMesh::kNoPoly || g_seen.count(n)) continue;
            if (!NavMesh::Walkable(n)) continue;
            uint32_t raw = 0, eff = 0;
            if (ScriptClosedFor(n, bit, &raw, &eff)) {
                g_seen.insert(n);            // visited, never expanded: a closed poly is a wall here
                ++g_closedRefused;
                g_closedMaterials |= 1u << ((raw >> 13) & 0x1F);
                if (g_samplePoly < 0) { g_samplePoly = n; g_sampleRaw = raw; g_sampleEff = eff; }
                continue;
            }
            g_seen.insert(n);
            g_open.insert(n);
            g_stack.push_back(n);
        }
    }

    if (g_stack.empty() || static_cast<int>(g_seen.size()) >= kMaxPolys) {
        g_running = false;
        auto snap = std::make_shared<const std::unordered_set<int32_t>>(g_open);
        const size_t openCount = snap->size();
        {
            std::lock_guard<std::mutex> lk(g_pubMutex);
            g_published = std::move(snap);
        }
        char mats[96]; int q = 0;
        for (int i = 0; i < 32 && q < 80; ++i)
            if ((g_closedMaterials >> i) & 1u) q += snprintf(mats + q, sizeof(mats) - q, "%s%d", q ? "," : "", i);
        if (q == 0) snprintf(mats, sizeof(mats), "none");
        char m[320];
        snprintf(m, sizeof(m),
                 "reach-gate: fill complete -- %zu polys open from poly %d | %d script-closed crossing(s) "
                 "refused, material id(s) %s | filter %s",
                 openCount, g_startPoly, g_closedRefused, mats,
                 ModMenu::UnreachableFilterOn() ? "ON" : "OFF (log only)");
        Log::Write("NAV-ROUTE", m);
        if (g_samplePoly >= 0) {
            // THE PREMISE, IN ONE LINE: raw class bit clear, effective class bit set. If a sample ever
            // shows the raw bit SET, the script-closed test is misreading and nothing may filter on it.
            char s[160];
            snprintf(s, sizeof(s), "reach-gate: closed sample poly %d raw=0x%08X eff=0x%08X (class bit 0x%08X)",
                     g_samplePoly, g_sampleRaw, g_sampleEff, bit);
            Log::Write("NAV-ROUTE", s);
        }
    }
}

void Invalidate() {
    g_running = false;
    g_epoch   = 0xFFFFFFFFu;
    g_seen.clear();
    g_open.clear();
    g_stack.clear();
    Unpublish();
}

void Annotate(std::vector<Entity>& list) {
    const bool filterOn = ModMenu::UnreachableFilterOn();
    std::lock_guard<std::mutex> lk(g_logMutex);
    const int map = MapNames::CurrentMapId();
    if (map != g_logMap) { g_logMap = map; g_logSeen.clear(); }

    for (auto& e : list) {
        Verdict v = Verdict::Unknown;
        if (e.category != Category::Enemy && !e.noBearing) v = Judge(e.pos);
        e.reach = static_cast<uint8_t>(v);
        if (v == Verdict::Unknown) continue;   // nothing decided, nothing to report

        // Identity for the log: the scene object, or -- for exits and drops, which have none -- the
        // label. `baseLabel` is empty on an exit whose name is unique, so fall back to `label`, or every
        // such exit would share one record and their verdicts would overwrite each other.
        const std::wstring& key = e.baseLabel.empty() ? e.label : e.baseLabel;
        Seen* s = nullptr;
        for (auto& x : g_logSeen)
            if ((e.sceneObj && x.obj == e.sceneObj) || (!e.sceneObj && !x.obj && x.label == key)) { s = &x; break; }
        if (s && s->verdict == e.reach) continue;
        const bool firstSight = (s == nullptr);
        if (!s) {
            if (g_logSeen.size() >= 256) continue;
            g_logSeen.push_back(Seen{ e.sceneObj, key, 0 });
            s = &g_logSeen.back();
        }
        s->verdict = e.reach;
        if (firstSight && v == Verdict::Reachable) continue;   // the normal case says nothing

        char m[320];
        snprintf(m, sizeof(m), "reach-gate: \"%s\" [%u:%u] at (%.1f,%.1f,%.1f) is %s -- %s",
                 Ascii(e.label).c_str(), e.container, e.slot, e.pos.x, e.pos.y, e.pos.z, Name(v),
                 !Hides(e.reach) ? "listed"
                                 : (filterOn ? "HIDDEN (filter on)" : "WOULD HIDE (filter off, still listed)"));
        Log::Write("NAV", m);
    }
}

} // namespace ReachGate

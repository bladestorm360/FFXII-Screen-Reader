#include "ui/battle_target_reader.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "speech/speech.h"
#include "core/logger.h"
#include "navigation/player_state.h"   // ReadSceneObjectPos (target world pos)

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

// Battle target-selection readout.
//
// Confirmed model (three decompile traces + Frida): target selection is a SEPARATE object on the
// battle-HUD context DAT_0209be80 (RVA 0x1F7BE80). The highlighted target's HANDLE is
//   P = *(void**)DAT_0209be80 ; handle = *(int)(P + 0x9FD8)     (nameplate/target-info manager)
// active while *(P + 0x10f78) != 0 (the single-target selector; absent during command navigation).
//
// The nameplate builder FUN_002bfd20 (0x19FD20) renders that target and, in the same call, resolves
// the handle to the real BtlChr and passes it to the vitals builder FUN_00329220 (0x209220). Rather
// than re-decode the handle (FUN_003588b0 was unreliable to call directly), we mark "this render is
// the current target" while FUN_002bfd20 runs, then capture the real BtlChr from the nested
// FUN_00329220 call -- the same object that resolves to the correct name + real HP via the actor
// pool (proven in the probe). Both hooks run on the render/game thread (synchronous, single-thread),
// so the flag needs no locking. Ally-vs-enemy uses the mod's confirmed faction test (scene-kind
// nibble), NOT a nameplate flag (panel+0x280 & 2 mis-classified allies).

namespace {

using MemRead::PtrAt;
using MemRead::SafeReadU8;
using MemRead::SafeReadU32;
using MemRead::SafeReadF32;

// ---- offline-derived RVAs / offsets (abs = RVA + 0x120000) --------------------------------------
constexpr uint32_t RVA_NAMEPLATE = 0x19FD20;  // FUN_002bfd20(panel, flag) -- target/nameplate render
constexpr uint32_t RVA_SNAPSHOT  = 0x209220;  // FUN_00329220(BtlChr, ...) -- vitals builder (nested)
constexpr uint32_t RVA_BSTATE    = 0x1F7BE80;  // DAT_0209be80 (ptr) -> P (battle-HUD context)

constexpr uint32_t OFF_TARGETID  = 0x9FD8;    // *(int)(P+0x9FD8) = highlighted target handle
constexpr uint32_t OFF_GATE      = 0x10F78;   // *(P+0x10f78) != 0 = target selection active
constexpr uint32_t OFF_PANEL_ID  = 0x288;     // *(u32)(panel+0x288) = the unit this nameplate draws

// actor pool -> combatant name + faction (mirrors src/navigation/nav_rva.h).
constexpr uint32_t ACTOR_POOL_BASE  = 0x1F6E688;
constexpr uint32_t ACTOR_POOL_COUNT = 0x1F6E6A0;
constexpr uint32_t ACTOR_STRIDE     = 0xF50;
constexpr uint32_t ACTOR_DEF_PTR    = 0x698;   // *(actor+0x698) = BtlChr
constexpr uint32_t ACTOR_NAME_STR   = 0x18;    // *(actor+0x18) = name codec
constexpr uint32_t ACTOR_SCENEOBJ   = 0x10;    // *(actor+0x10) = scene object
constexpr uint32_t ACTOR_POS_X      = 0xE0;    // cached world X on the actor (GameArchitecture.md:728)
constexpr uint32_t ACTOR_POS_Y      = 0xE4;    // cached world Y (elevation)
constexpr uint32_t ACTOR_POS_Z      = 0xE8;    // cached world Z
constexpr uint32_t SCENEOBJ_KIND    = 0x0E;    // *(u8)(sceneObj+0x0e) & 0x0f: 3 = ally, {1,2,7} = enemy
constexpr uint8_t  KIND_MASK        = 0x0F;
constexpr uint8_t  KIND_ALLY        = 3;
constexpr uint8_t  KIND_DEAD        = 5;       // dead/removed — same test the nav scanner uses
constexpr uint32_t DEF_KIND_BYTE    = 0x05;    // *(u8)(bc+5): 0 = player-controlled leader (self-target)
constexpr uint8_t  PLAYER_DEF_KIND  = 0;       // matches nav_rva.h; the leader's scene-kind is NOT 3

constexpr uint32_t BC_CURHP = 0x48;            // real current HP (confirmed vs Reks 135)
constexpr uint32_t BC_MAXHP = 0x24;            // real max HP

typedef void  (*Pfn_Nameplate)(void*, int);
typedef void* (*Pfn_Snapshot)(void*, int, void*, int);

Pfn_Nameplate s_origNameplate = nullptr;
Pfn_Snapshot  s_origSnapshot  = nullptr;

// Set by FUN_002bfd20 while it renders the CURRENT target; consumed by the nested FUN_00329220.
bool     g_wantTargetBc = false;
int32_t  g_targetHandle = 0;
int32_t  g_lastHandle   = 0;   // dedup: announce once per target change; reset when selection ends

// Target cache for the `p`-key route. Written on the render/game thread whenever the target
// nameplate redraws (HookedSnapshot — EVENT-driven, NOT per-frame); read on the input thread
// (GetLockedTarget). The `bc` pointer + `handle` are the handle->BtlChr bridge; `p` re-resolves a
// FRESH position from `bc` at press time and gates on the LIVE DAT_0209be80 state (not cache age,
// since the redraw is sparse). `bc`=nullptr => never captured. `pos` is a last-resort fallback.
struct TargetCache {
    void*        bc     = nullptr;   // target BtlChr (matches actor+0x698); for live pos re-resolve
    FVec3        pos;                 // last resolved pos (fallback if the live re-resolve fails)
    std::wstring label;
    int32_t      handle = 0;
    uint64_t     tickMs = 0;   // GetTickCount64() at capture (diagnostic only)
};
std::mutex  g_cacheMx;
TargetCache g_cache;

// Forget the selected target. Until now the ONLY thing that ever cleared this was Shutdown(), so a
// captured target outlived its own death, its deselection, and the whole battle.
void ClearCache() {
    std::lock_guard<std::mutex> lk(g_cacheMx);
    g_cache = TargetCache{};
}

void* Pstate() { return PtrAt(Hooks::ResolveRva(RVA_BSTATE), 0); }

inline bool NonZero(const FVec3& p) { return !(p.x == 0.0f && p.y == 0.0f && p.z == 0.0f); }

// Baked confirmation of the target position sources (for the diagnostic log).
struct PosDiag {
    bool  sceneOk = false;  FVec3 scenePos;   // sceneObj+0xB8 transform node
    bool  actorOk = false;  FVec3 actorPos;   // actor+0xE0/E4/E8 cached pos (GameArchitecture.md:728)
    int   source  = 0;      // 0 none, 1 scene node, 2 actor cache
};

// Target world position with a fallback: the scene node (sceneObj+0xB8) first — the same chain
// entity_list uses; if it fails or reads (0,0,0), as it does for a battle target during attack-menu
// selection, fall back to the actor's own cached world position (actor+0xE0/E4/E8). Returns havePos
// (a non-zero position from either source). Fills `d` for the confirmation log when non-null.
bool ResolveActorPos(void* actor, void* sceneObj, FVec3& out, PosDiag* d) {
    FVec3 sp;
    const bool sceneOk = PlayerState::ReadSceneObjectPos(sceneObj, sp) && NonZero(sp);
    FVec3 ap;
    const bool ax = SafeReadF32(actor, ACTOR_POS_X, &ap.x);
    const bool ay = SafeReadF32(actor, ACTOR_POS_Y, &ap.y);
    const bool az = SafeReadF32(actor, ACTOR_POS_Z, &ap.z);
    const bool actorOk = ax && ay && az && NonZero(ap);
    if (d) { d->sceneOk = sceneOk; d->scenePos = sp; d->actorOk = actorOk; d->actorPos = ap; }
    if (sceneOk) { out = sp; if (d) d->source = 1; return true; }
    if (actorOk) { out = ap; if (d) d->source = 2; return true; }
    if (d) d->source = 0;
    return false;
}

// Combatant name (actor+0x18) + faction (scene-kind nibble) from the actor pool, matching the BtlChr
// via *(actor+0x698)==bc. Returns the name (empty if not found / unprintable); *ally set from kind.
// When posOut is non-null, also reads the unit's live world position (scene node, else actor cache)
// and sets *havePosOut; fills *diagOut with both sources for the confirmation log.
// *deadOut is set when the unit's scene-kind says it has been removed/killed (KIND_DEAD), so callers
// can drop a target that died — an empty return means "not in the pool at all" (also not targetable).
std::wstring NameForBtlChr(void* bc, bool* ally, FVec3* posOut = nullptr, bool* havePosOut = nullptr,
                           PosDiag* diagOut = nullptr, bool* deadOut = nullptr) {
    *ally = false;
    if (havePosOut) *havePosOut = false;
    if (deadOut) *deadOut = false;
    if (!bc) return std::wstring();
    void* base = PtrAt(Hooks::ResolveRva(ACTOR_POOL_BASE), 0);
    if (!base) return std::wstring();
    uint32_t count = 0;
    SafeReadU32(Hooks::ResolveRva(ACTOR_POOL_COUNT), 0, &count);
    if (count == 0) return std::wstring();
    if (count > 64) count = 64;
    for (uint32_t i = 0; i < count; ++i) {
        void* actor = reinterpret_cast<char*>(base) + static_cast<size_t>(i) * ACTOR_STRIDE;
        if (PtrAt(actor, ACTOR_DEF_PTR) != bc) continue;
        const uint8_t* codec = reinterpret_cast<const uint8_t*>(PtrAt(actor, ACTOR_NAME_STR));
        std::wstring nm = GameText::Decode(codec, 128);
        if (!GameText::IsMostlyPrintable(nm)) return std::wstring();
        void* sceneObj = PtrAt(actor, ACTOR_SCENEOBJ);
        uint8_t kind = 0xFF, def5 = 0xFF;
        SafeReadU8(sceneObj, SCENEOBJ_KIND, &kind);
        SafeReadU8(bc, DEF_KIND_BYTE, &def5);
        // ally = the player-controlled leader (self-target: def+5==0, but its scene-kind is NOT 3)
        // OR a party-side unit (guests / AI party, kind==3). Mirrors the proven nav classifier
        // (entity_list.cpp) — KIND_ALLY alone misses the solo-Reks leader, so a curative on
        // yourself read as a percentage instead of a number.
        *ally = (def5 == PLAYER_DEF_KIND) || ((kind & KIND_MASK) == KIND_ALLY);
        if (deadOut) *deadOut = ((kind & KIND_MASK) == KIND_DEAD);
        if (posOut) {
            bool hp = ResolveActorPos(actor, sceneObj, *posOut, diagOut);
            if (havePosOut) *havePosOut = hp;
        }
        return nm;
    }
    return std::wstring();
}

// Speak the current target. Enemy = HP percentage (mirrors the gauge; no MP/numbers pre-Libra),
// ally = HP numbers. Real HP off the BtlChr (bc+0x48/0x24). Silent if the name can't resolve.
// name/ally are pre-resolved by the caller (single actor-pool lookup shared with the p-key cache).
void AnnounceTargetBc(void* bc, const std::wstring& name, bool ally) {
    if (name.empty()) return;

    uint32_t curHPu = 0, maxHPu = 0;
    SafeReadU32(bc, BC_CURHP, &curHPu);
    SafeReadU32(bc, BC_MAXHP, &maxHPu);
    int32_t curHP = static_cast<int32_t>(curHPu), maxHP = static_cast<int32_t>(maxHPu);

    std::wstring text = name;
    if (maxHP > 0) {
        if (ally) {
            text += L", HP " + std::to_wstring(curHP) + L" of " + std::to_wstring(maxHP);
        } else {
            int pct = static_cast<int>(static_cast<long long>(curHP) * 100 / maxHP);
            text += L", HP " + std::to_wstring(pct) + L" percent";
        }
    }

    char utf8[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    char line[320];
    snprintf(line, sizeof(line), "handle=0x%x %s \"%s\"", g_targetHandle, ally ? "ally" : "enemy", utf8);
    Log::Write("TARGET", line);

    Speech::Output(text, /*interrupt=*/true);
}

// FUN_002bfd20 render. If this call is drawing the CURRENT target (panel+0x288 == P+0x9FD8) while
// target selection is active, flag it so the nested FUN_00329220 grabs the real BtlChr.
void HookedNameplate(void* panel, int flag) {
    g_wantTargetBc = false;
    void* P = Pstate();
    if (P) {
        if (!PtrAt(P, OFF_GATE)) {
            g_lastHandle = 0;   // not selecting -> reset dedup (so re-entry re-announces)
            ClearCache();       // ...and drop the target itself, not just the dedup key
        } else {
            uint32_t th = 0, ph = 0;
            SafeReadU32(P, OFF_TARGETID, &th);
            SafeReadU32(panel, OFF_PANEL_ID, &ph);
            if (th != 0 && ph == th) {
                g_wantTargetBc = true;
                g_targetHandle = static_cast<int32_t>(th);
            }
        }
    }

    if (s_origNameplate) s_origNameplate(panel, flag);   // calls FUN_00329220 with the target BtlChr
    g_wantTargetBc = false;
}

// FUN_00329220 vitals build. When invoked for the flagged target, `bc` is the real target BtlChr.
// Resolve name+faction+world-pos once: refresh the p-key cache EVERY render while a target is live
// (so `p` always has the current target), but announce only on a target CHANGE (the dedup).
void* HookedSnapshot(void* bc, int p2, void* outBuf, int p4) {
    void* r = s_origSnapshot ? s_origSnapshot(bc, p2, outBuf, p4) : nullptr;
    if (g_wantTargetBc && bc) {
        g_wantTargetBc = false;                 // take only the first (the target) per render
        bool ally = false; FVec3 pos; bool havePos = false; PosDiag pd;
        std::wstring name = NameForBtlChr(bc, &ally, &pos, &havePos, &pd);
        if (!name.empty()) {
            // Always cache bc+handle+label (the handle->BtlChr bridge); pos when we have it. `p`
            // re-resolves a fresh pos from bc and gates on the live DAT_0209be80 state, so it no
            // longer depends on how often this (event-driven) render refreshes the cache.
            {
                std::lock_guard<std::mutex> lk(g_cacheMx);
                g_cache.bc     = bc;
                g_cache.label  = name;
                g_cache.handle = g_targetHandle;
                g_cache.tickMs = GetTickCount64();
                if (havePos) g_cache.pos = pos;
            }
            if (g_targetHandle != g_lastHandle) {
                g_lastHandle = g_targetHandle;
                // Baked confirmation: which position source populated the p-key cache (scene node
                // vs actor+0xE0 fallback). If both are 0/absent, havePos is false and `p` = No target.
                char pm[192];
                snprintf(pm, sizeof(pm),
                         "pos: sceneOk=%d scene=(%.1f,%.1f,%.1f) actorOk=%d actor=(%.1f,%.1f,%.1f) src=%d havePos=%d",
                         pd.sceneOk ? 1 : 0, pd.scenePos.x, pd.scenePos.y, pd.scenePos.z,
                         pd.actorOk ? 1 : 0, pd.actorPos.x, pd.actorPos.y, pd.actorPos.z,
                         pd.source, havePos ? 1 : 0);
                Log::Write("TARGET", pm);
                AnnounceTargetBc(bc, name, ally);
            }
        }
    }
    return r;
}

} // namespace

namespace BattleTargetReader {

bool Init() {
    bool ok = Hooks::InstallTyped(RVA_NAMEPLATE, &HookedNameplate, &s_origNameplate);
    ok     &= Hooks::InstallTyped(RVA_SNAPSHOT,  &HookedSnapshot,  &s_origSnapshot);
    Log::Write("TARGET", ok ? "BattleTargetReader: target readout installed (FUN_002bfd20 + FUN_00329220, DAT_0209be80+0x9FD8)"
                            : "BattleTargetReader: a target hook FAILED to install");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_SNAPSHOT);
    Hooks::Uninstall(RVA_NAMEPLATE);
    std::lock_guard<std::mutex> lk(g_cacheMx);
    g_cache = TargetCache{};   // drop any stale target
}

// Resolve the CURRENTLY SELECTED, CONFIRMED-LIVE target. Shared by `p` (route) and `;` (status), so
// both agree on what "the target" is and both refuse a dead one.
//
// Gates on the LIVE DAT_0209be80 target-selection state (read here, SEH-guarded, mirroring
// HookedNameplate) — NOT on cache age, because the nameplate redraw that fills the cache is
// event-driven and sparse. A target is "live" iff the game's gate (+0x10F78) is set and its selected
// handle (+0x9FD8) still matches the one we captured (the redraw updates both together on a target
// change) AND the unit has not died. Re-resolves a FRESH position from the cached BtlChr (handles a
// moving target), falling back to the last cached pos.
bool ResolveLiveTarget(void** bcOut, std::wstring& nameOut, bool& allyOut,
                       FVec3& posOut, bool& havePosOut) {
    // Live selection state (input thread; same reads HookedNameplate does on the game thread).
    void* P = Pstate();
    const bool gateActive = P && MemRead::PtrAt(P, OFF_GATE) != nullptr;
    uint32_t liveHandle = 0;
    if (P) MemRead::SafeReadU32(P, OFF_TARGETID, &liveHandle);

    void* bc = nullptr; int32_t cachedHandle = 0; FVec3 cachedPos; std::wstring label;
    {
        std::lock_guard<std::mutex> lk(g_cacheMx);
        bc = g_cache.bc; cachedHandle = g_cache.handle; cachedPos = g_cache.pos; label = g_cache.label;
    }

    const bool match = gateActive && liveHandle != 0 &&
                       static_cast<int32_t>(liveHandle) == cachedHandle && bc != nullptr;
    if (!match) {
        char lg[128];
        snprintf(lg, sizeof(lg), "ResolveLiveTarget: gate=%d liveHandle=0x%x cachedHandle=0x%x bc=%p match=0",
                 gateActive ? 1 : 0, liveHandle, cachedHandle, bc);
        Log::Write("TARGET", lg);
        // The selection is gone. Drop the cache too — otherwise it survives until DLL unload and a
        // later selection that happens to reuse this handle value would match a stale BtlChr.
        if (!gateActive) ClearCache();
        return false;
    }

    // LIVENESS. A matching handle is not enough: when the target dies the game leaves the selection
    // state alone for a moment, and the actor either flips to scene-kind 5 (dead/removed) or leaves
    // the pool entirely. Previously NameForBtlChr's return value was discarded here, so a departed
    // actor produced havePos=false -> posOut = the CACHED position, and `p` happily routed to the
    // corpse's last known spot. Three separate leaks, all closed below.
    bool ally = false, dead = false, havePos = false;
    FVec3 fresh;
    const std::wstring live = NameForBtlChr(bc, &ally, &fresh, &havePos, nullptr, &dead);

    uint32_t curHP = 0;
    const bool hpOk = MemRead::SafeReadU32(bc, BC_CURHP, &curHP);

    const bool gone = live.empty();                       // no longer in the actor pool
    const bool ko   = hpOk && curHP == 0;                 // killed but still pooled
    if (gone || dead || ko) {
        char lg[144];
        snprintf(lg, sizeof(lg), "ResolveLiveTarget: target no longer live (gone=%d dead=%d hp0=%d) -> cleared",
                 gone ? 1 : 0, dead ? 1 : 0, ko ? 1 : 0);
        Log::Write("TARGET", lg);
        ClearCache();
        return false;
    }

    char lg[144];
    snprintf(lg, sizeof(lg), "ResolveLiveTarget: gate=1 handle=0x%x bc=%p hp=%u havePos=%d match=1",
             liveHandle, bc, curHP, havePos ? 1 : 0);
    Log::Write("TARGET", lg);

    // Fresh position from the cached BtlChr (scene node -> actor+0xE0 fallback). The cached-pos
    // fallback is safe now: we only get here with a confirmed-live target.
    *bcOut     = bc;
    nameOut    = live.empty() ? label : live;
    allyOut    = ally;
    posOut     = havePos ? fresh : cachedPos;
    havePosOut = havePos;
    return true;
}

// Input-thread accessor for `p` (route to the selected target).
bool GetLockedTarget(FVec3& posOut, std::wstring& labelOut) {
    void* bc = nullptr; bool ally = false, havePos = false;
    std::wstring name;
    if (!ResolveLiveTarget(&bc, name, ally, posOut, havePos)) return false;
    labelOut = name;
    return true;
}

// Input-thread accessor for `;` (speak the selected target's status). Same liveness rules as `p`, so
// a dead target says "No target" rather than reporting a corpse. Reuses the announce formatting, so
// the readout matches what the target-change announcement says (enemy = HP %, ally = HP numbers).
void SpeakTargetStatus() {
    void* bc = nullptr; bool ally = false, havePos = false;
    std::wstring name; FVec3 pos;
    if (!ResolveLiveTarget(&bc, name, ally, pos, havePos) || name.empty()) {
        Speech::Output(L"No target", /*interrupt=*/true);
        return;
    }
    AnnounceTargetBc(bc, name, ally);
}

} // namespace BattleTargetReader

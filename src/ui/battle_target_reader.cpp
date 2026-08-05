#include "ui/battle_target_reader.h"
#include "battle/battle_state.h"
#include "battle/battle_state_diag.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "speech/speech.h"
#include "speech/phrasebook.h"
#include "speech/phrase_format.h"
#include "core/logger.h"
#include "core/stall_probe.h"
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

// Actor pool, BtlChr and scene-kind layout: core/phyre_types.h owns them (this file used to keep
// its own copy that "mirrored" nav_rva.h -- two mirrors of the same offsets is how they drift).
using namespace PhyreTypes;
constexpr uint32_t SCENEOBJ_KIND = PhyreTypes::SCENEOBJ_KIND_OFF;   // local spelling kept for the reads below

typedef void  (*Pfn_Nameplate)(void*, int);
typedef void* (*Pfn_Snapshot)(void*, int, void*, int);

Pfn_Nameplate s_origNameplate = nullptr;
Pfn_Snapshot  s_origSnapshot  = nullptr;

// Set by FUN_002bfd20 while it renders the CURRENT target; consumed by the nested FUN_00329220.
bool     g_wantTargetBc = false;
int32_t  g_targetHandle = 0;
// PERMITTED per-frame change-check (the no-dedup rule's one exception; see CLAUDE.md).
// GUARDS: FUN_002bfd20 (nameplate render) -> FUN_00329220 (vitals build), both called EVERY frame
// a target nameplate is on screen. Without this the reader would speak the target once per frame.
// It is keyed on the target HANDLE, not on the spoken text, and HookedNameplate resets it to 0 the
// moment selection ends — so leaving targeting and coming back re-announces.
int32_t  g_lastHandle   = 0;

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
            text += std::wstring(L", ") + Phrase::Get(Phrase::Id::HPPrefix) + std::to_wstring(curHP) + L"/" + std::to_wstring(maxHP);
        } else {
            text += L", " + PhraseFormat::Percent(Phrase::Id::HPPrefix, curHP, maxHP);
        }
    }

    char utf8[256];
    Log::ToUtf8(text, utf8, sizeof(utf8));
    char line[320];
    snprintf(line, sizeof(line), "handle=0x%x %s \"%s\"", g_targetHandle, ally ? "ally" : "enemy", utf8);
    Log::Write("TARGET", line);

    Speech::Output(text, /*interrupt=*/true);
}

// FUN_002bfd20 render. If this call is drawing the CURRENT target (panel+0x288 == P+0x9FD8) while
// target selection is active, flag it so the nested FUN_00329220 grabs the real BtlChr.
void HookedNameplate(void* panel, int flag) {
    STALL_SCOPE("BattleTarget::HookedNameplate");
    g_wantTargetBc = false;
    void* P = Pstate();
    if (P) {
        if (!PtrAt(P, OFF_GATE)) {
            g_lastHandle = 0;   // not selecting -> disarm the per-frame guard (re-entry re-announces)
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
// (so `p` always has the current target), but announce only on a target CHANGE — the permitted
// per-frame guard documented on g_lastHandle above.
void* HookedSnapshot(void* bc, int p2, void* outBuf, int p4) {
    void* r = s_origSnapshot ? s_origSnapshot(bc, p2, outBuf, p4) : nullptr;
    STALL_SCOPE("BattleTarget::HookedSnapshot");
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

struct ResolvedTarget {
    void*        actor    = nullptr;
    void*        bc       = nullptr;
    std::wstring name;                  // display name, instance letter included
    bool         ally     = false;
    bool         browsing = false;      // true = the cursor, NOT a commitment
    bool         acting   = false;      // committed and mid-action (vs queued)
    uint16_t     actionId = 0xFFFF;
    FVec3        pos;
    bool         havePos  = false;
};

// Resolve THE TARGET, committed first. Shared by `p` (route) and `;` (status).
//
// ===== WHY THIS CHANGED (Session 49) =====
// The old implementation resolved `*(P + 0x9FD8)`, which is written ONLY by browse handlers. A live
// capture settled it: scrolling the cursor across two enemies moved that field on every step while
// the character's actual commitment sat on a THIRD enemy the cursor never visited. So the previous
// readout could name a unit the character was not acting on at all.
//
// The real commitment lives on the ACTOR, and BattleState::CommittedTargetOf applies the corrected
// precedence: ACTIVE (+0x710/+0x714) only when it holds a real ability id and a non-null target --
// actor+0x714 can carry an AI/behaviour opcode from a 0x4000+ band with target 0 -- otherwise the
// QUEUED pair (+0xBB8/+0xBA0) gated on flag bit 0x4000, which must be tested because those fields
// retain stale values after it clears.
//
// The browse cursor is kept as an explicitly-labelled SECOND choice: while the select UI is open,
// what you are hovering is genuinely useful, it just is not "the target".
//
// No cache is needed any more. It existed because FUN_003588b0 was unreliable to call; handle ->
// actor is now a direct scan of actor+0x08, which is the actor's own handle (assigned outright in
// FUN_00322080).
bool ResolveTarget(ResolvedTarget& out) {
    out = ResolvedTarget{};

    void* actor = nullptr;

    // 1. the committed target
    void* leader = BattleState::LeaderActor();
    if (leader) {
        const BattleState::Committed c = BattleState::CommittedTargetOf(leader);
        if (c.valid) {
            actor = BattleState::ActorForHandle(c.targetHandle);
            if (actor) { out.acting = c.active; out.actionId = c.actionId; }
            else {
                // THE BLIND SPOT. A commitment that resolves but whose handle does not map to a
                // live actor falls through to the browse cursor below and is logged as "BROWSING"
                // -- indistinguishable from "there was no commitment". That ambiguity is why the
                // log appeared to show commitment almost never firing; it may in fact be firing
                // and this lookup failing. Never let these two look the same again.
                char m[160];
                snprintf(m, sizeof(m),
                         "commit: VALID but ActorForHandle(0x%X) FAILED -- falling back to browse",
                         static_cast<unsigned>(c.targetHandle));
                Log::Write("TARGET", m);
            }
        }
    }

    // 2. else the browse cursor, but ONLY while the select UI is genuinely open
    if (!actor) {
        void* P = Pstate();
        if (P && MemRead::PtrAt(P, OFF_GATE) != nullptr) {
            uint32_t h = 0;
            if (MemRead::SafeReadU32(P, OFF_TARGETID, &h) && h != 0) {
                actor = BattleState::ActorForHandle(static_cast<int32_t>(h));
                if (actor) out.browsing = true;
            }
        }
    }
    if (!actor) return false;

    // LIVENESS — a handle alone is not enough. When a target dies the game leaves the selection
    // state alone for a moment and the actor flips to scene-kind 5 (dead/removed) or leaves the
    // pool. Without this, `p` used to route to the corpse's last known spot.
    void* bc = BattleState::BtlChrForActor(actor);
    void* sceneObj = MemRead::PtrAt(actor, ACTOR_SCENEOBJ);
    uint8_t kind = 0xFF;
    MemRead::SafeReadU8(sceneObj, SCENEOBJ_KIND, &kind);
    if ((kind & KIND_MASK) == KIND_DEAD) return false;

    uint32_t curHP = 0;
    if (bc && MemRead::SafeReadU32(bc, BC_CURHP, &curHP) && curHP == 0) return false;

    out.name = BattleState::DisplayNameForActor(actor);   // includes the instance letter
    if (out.name.empty()) return false;

    const BattleState::Faction f = BattleState::FactionOf(actor);
    out.ally = (f == BattleState::Faction::Party || f == BattleState::Faction::Guest ||
                f == BattleState::Faction::Ally);

    out.actor = actor;
    out.bc = bc;
    out.havePos = ResolveActorPos(actor, sceneObj, out.pos, nullptr);

    char lg[224];
    char utf8[128];
    Log::ToUtf8(out.name, utf8, sizeof(utf8));
    snprintf(lg, sizeof(lg), "ResolveTarget: \"%s\" %s%s hp=%u havePos=%d",
             utf8, out.browsing ? "BROWSING" : (out.acting ? "committed/acting" : "committed/queued"),
             out.ally ? " ally" : " enemy", curHP, out.havePos ? 1 : 0);
    Log::Write("TARGET", lg);
    return true;
}

// Input-thread accessor for `p` (route to the target). Shares ResolveTarget with `;`, so the two
// keys agree on which unit they mean -- which was NOT true between 2026-07-20 and 2026-07-21, when
// `;` rejected the browsed target that `p` happily routed to. The comment claimed agreement the
// whole time; it is true again now.
bool GetLockedTarget(FVec3& posOut, std::wstring& labelOut) {
    ResolvedTarget t;
    if (!ResolveTarget(t) || !t.havePos) return false;
    posOut = t.pos;
    labelOut = t.name;
    return true;
}

// Input-thread accessor for `;` (speak the target's status): the unit the player is fighting.
//
// REPORTS WHATEVER ResolveTarget RESOLVED -- the commitment when there is one, the live select-UI
// target otherwise. It does NOT reject a "browsed" target.
//
// WHY (regression, fixed 2026-07-21). This used to be `if (!ResolveTarget(t) || t.browsing)`, added
// 2026-07-20 in eeffde5 on top of the 3903dbc rewrite that replaced `ResolveLiveTarget` with the
// commitment path. Before that rewrite the key resolved the live select-UI target (gate P+0x10F78
// open, handle from P+0x9FD8) and worked on every press -- the 07-20 12:11 log shows
// `ResolveLiveTarget: gate=1 handle=0x20000f ... hp=65 match=1` succeeding repeatedly. The
// `t.browsing` clause discarded exactly that state, so with an enemy targeted and Attack confirmed
// the key went silent: commitment resolution rejects the ACTIVE branch (the action-table row lookup
// returns null for Attack, id 0x96 -- see DiagnoseCommitment), the QUEUED bit is clear mid-swing,
// and the live target it fell back to was then thrown away.
//
// OUT OF BATTLE IT IS STILL SILENT, structurally and with no "am I in battle" flag: ResolveTarget
// returns false on its own out of combat, because there is no commitment AND the browse branch
// requires the select-UI gate to be open. The release-0.1 requirement is preserved by that, not by
// the clause removed here. Do NOT restore the old "No target" speech -- silence on
// nothing-to-report is a standing rule.
// Returns TRUE only when it actually spoke. `;` is shared with the field-side interact-target
// readout (InteractTarget::SpeakCurrent), which runs only when this had nothing -- so the caller
// needs to know the difference between "spoke" and "stayed silent". It is NOT an "in battle" flag:
// a battle with no committed or browsed target also returns false, and the field reader is silent
// there too, so the combined key stays silent exactly where it always did.
bool SpeakTargetStatus() {
    ResolvedTarget t;
    if (!ResolveTarget(t)) {
        // Say WHY, every link of it. A confirmed attack that reports "no commitment" is a bug in the
        // BtlWork -> leader -> actor-pool -> active/queued chain, and without this the whole chain
        // fails as one silent boolean with nothing to grep.
        BattleState::DiagnoseCommitment();
        Log::Write("TARGET", "; SILENT: no target (no commitment and no open select UI)");
        return false;
    }

    uint32_t curHPu = 0, maxHPu = 0;
    MemRead::SafeReadU32(t.bc, BC_CURHP, &curHPu);
    MemRead::SafeReadU32(t.bc, BC_MAXHP, &maxHPu);
    const int32_t curHP = static_cast<int32_t>(curHPu), maxHP = static_cast<int32_t>(maxHPu);

    std::wstring text = t.name;
    if (maxHP > 0) {
        if (t.ally) {
            // Allies show real numbers; enemies show a percentage, mirroring the gauge the game
            // draws (there is no pre-Libra HP-visible flag to read).
            text += std::wstring(L", ") + Phrase::Get(Phrase::Id::HPPrefix) + std::to_wstring(curHP) + L"/" + std::to_wstring(maxHP);
        } else {
            text += L", " + PhraseFormat::Percent(Phrase::Id::HPPrefix, curHP, maxHP);
        }
    }
    // Mod-emitted qualifier, and ONLY for a real commitment that has not started executing. A
    // browsed target reaches here now, and it is neither acting nor queued -- calling it "queued"
    // would be a fabricated state. It gets no suffix, matching what this key said when it worked.
    if (!t.browsing && !t.acting) text += std::wstring(L", ") + Phrase::Get(Phrase::Id::Queued);

    Speech::Output(text, /*interrupt=*/true);
    return true;
}

} // namespace BattleTargetReader

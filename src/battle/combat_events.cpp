#include "battle/combat_events.h"

#include "battle/battle_state.h"
#include "battle/combat_format.h"
#include "battle/combat_log.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"

#include <Windows.h>
#include <cstdio>

namespace CombatEvents {
namespace {

// ---- RVAs (abs = RVA + 0x120000) --------------------------------------------------------------
// FUN_00536410 -- the codec-sprintf. THE Tier-1 hook: it is the only point that sees the message
// ID and the finished, fully-substituted sentence in the SAME call frame. FUN_0028e110 (the
// ticker) receives only the string and a dwell class, never the id, so the realtime policy table
// would have nothing to key on there. This point is also UPSTREAM of the toast fork in
// FUN_0035b990, which skips the ticker entirely for some messages.
constexpr uint32_t RVA_SPRINTF = 0x416410;

// FUN_003112f0 -- the result applier. THE Tier-2 source: one call carries attacker, target, action
// and every number, so no correlation window is needed.
// ⚠ It runs PER ACTOR PER FRAME (~20/sec): the frame loop registers FUN_00233f70 as a per-object
// callback, which runs FUN_00310db0, which calls this with actionId 0xFFFF every tick. Measured
// live: 1400 calls to 3 real hits. The detour must therefore MONITOR ONLY and add no per-frame
// work -- see HookedApply.
constexpr uint32_t RVA_APPLY = 0x1F12F0;

// result struct fields
constexpr uint32_t R_OUTCOME = 0x04, R_VALID = 0x1C, R_TGT_HP = 0x24;
// BtlChr fields: core/phyre_types.h
using namespace PhyreTypes;

typedef void (*Pfn_Sprintf)(void*, void*, uint32_t, uint32_t);
typedef void (*Pfn_Apply)(void*, void*, void*, uint32_t, uint32_t);

Pfn_Sprintf s_origSprintf = nullptr;
Pfn_Apply   s_origApply   = nullptr;

// ---- edge latches, indexed by BtlChr slot ------------------------------------------------------
// Edge-triggered so each event fires once per crossing and re-arms only when the unit recovers.
// A state machine tracking a threshold crossing -- NOT a dedup/debounce; it suppresses no repeated
// event, it detects a transition.
//
// Keyed by BtlChr pointer, with round-robin reuse when the table fills.
//
// An earlier version derived the slot arithmetically from BtlWork's BtlChr array and REJECTED
// anything outside 0x28 entries. That bound is only provable for the PC path (FUN_0023a570 bounds
// the PC index with FUN_00322c50 category 2), so an enemy landing outside it was silently dropped
// -- which is exactly how enemy KO ended up neither spoken nor logged. This version assumes
// nothing about the array's size, and recycles rather than refusing, so it cannot quietly stop
// working the way a first-come table would.
constexpr int kLatchCount = 40;
struct Latch { void* bc = nullptr; bool low = false; };
Latch g_latch[kLatchCount];
int   g_latchNext = 0;

Latch& LatchFor(void* bc) {
    for (auto& l : g_latch) if (l.bc == bc) return l;
    Latch& l = g_latch[g_latchNext];
    g_latchNext = (g_latchNext + 1) % kLatchCount;
    l.bc = bc;
    l.low = false;
    return l;
}

// ---- critical-vitals watch, shared by real hits AND status ticks --------------------------------
//
// We do NOT announce KO ourselves. The GAME narrates it: FUN_00300530's KO path calls
// FUN_00469bb0, which emits message 0x10 "{0} has fallen" whenever FUN_002fa390 passes -- and that
// predicate is `(FUN_002f8e90(bc) & 7) != 0`, i.e. party | guest | ally. Because it hangs off the
// HP WRITER it fires for ANY cause, poison and doom included. That message is cull-exempt, is not
// dedup-eligible, and is already in the Tier-1 realtime list, so it reaches the player verbatim.
// Emitting our own "is KO'd" beside it would be a duplicate announcement of text the game supplies.
//
// What the game has NO text for is the 20% warning, so that one is ours. The latch is edge-
// triggered and re-arms on recovery: a state machine tracking a threshold crossing, NOT a dedup.
void CheckVitals(void* tgtBc, int32_t hpDelta) {
    if (!tgtBc || hpDelta >= 0) return;                 // only losses can cross downward

    uint32_t cur = 0, mx = 0;
    if (!MemRead::SafeReadU32(tgtBc, BC_CURHP, &cur) ||
        !MemRead::SafeReadU32(tgtBc, BC_MAXHP, &mx) || mx == 0) return;

    Latch& l = LatchFor(tgtBc);

    // We run on ENTRY -- the calculator fills the result struct before the applier applies it -- so
    // the BtlChr still holds the PRE-apply HP and the post value is pre + delta.
    const int32_t post  = static_cast<int32_t>(cur) + hpDelta;
    const bool    party = BattleState::IsPartySide(tgtBc);

    // One line per lethal blow, so a KO that fails to announce says WHY instead of just going
    // quiet. Cheap: only reached when something actually took damage.
    if (post <= 0) {
        char m[160];
        snprintf(m, sizeof(m), "lethal: pre=%d delta=%d post=%d party=%d latched=%d",
                 static_cast<int>(cur), hpDelta, post, party ? 1 : 0, l.low ? 1 : 0);
        Log::Write("COMBAT", m);
    }

    if (post > 0) {
        // Alive. Party members get the 20% warning; an enemy's HP fraction is not actionable.
        if (party && post <= static_cast<int32_t>(mx) / 5) {
            if (!l.low) {
                l.low = true;
                const std::wstring who =
                    BattleState::DisplayNameForActor(BattleState::ActorForBtlChr(tgtBc));
                if (!who.empty())
                    CombatLog::Append(CombatLog::Kind::System, who + L" below 20 percent",
                                      /*speakNow=*/true);
            }
        } else {
            l.low = false;                               // re-arm once recovered
        }
        return;
    }

    // ---- the unit is going down --------------------------------------------------------------
    if (l.low) return;                                   // already announced this death
    l.low = true;

    // PARTY side: the game narrates it itself -- FUN_00300530's KO path emits message 0x10
    // "{0} has fallen", gated to party|guest|ally, for ANY cause including poison and doom. Adding
    // our own line here would duplicate text the game supplies.
    if (party) return;

    // ENEMY side: that same gate means the game emits NOTHING when a foe dies, so this line is
    // ours.
    //
    // REALTIME (user, 2026-07-20). Knowing a foe is down lets you stop attacking a corpse and
    // retarget, which is worth an interruption. If it proves too chatty in a big fight the fix is a
    // config toggle, not silence -- so keep this a single flag rather than burying the decision.
    const std::wstring who = BattleState::DisplayNameForActor(BattleState::ActorForBtlChr(tgtBc));
    if (!who.empty())
        CombatLog::Append(CombatLog::Kind::System, CombatFormat::DefeatedLine(who),
                          /*speakNow=*/true);
}

// ---- the real work, reached only for genuine hits ----------------------------------------------
void OnRealHit(void* result, void* atkBc, void* tgtBc, uint16_t actionId) {
    uint8_t outcome = 0;
    uint32_t hpRaw = 0;
    MemRead::SafeReadU8(result, R_OUTCOME, &outcome);
    MemRead::SafeReadU32(result, R_TGT_HP, &hpRaw);
    const int32_t hpDelta = static_cast<int32_t>(hpRaw);

    void* atkActor = BattleState::ActorForBtlChr(atkBc);
    void* tgtActor = BattleState::ActorForBtlChr(tgtBc);

    const std::wstring line = CombatFormat::DamageLine(
        BattleState::DisplayNameForActor(atkActor),
        BattleState::DisplayNameForActor(tgtActor),
        BattleState::AbilityName(actionId),
        BattleState::AbilityCategory(actionId),
        hpDelta,
        outcome);

    // Log-only: this is the stream that made linear narration unusable, and it is why the log
    // exists at all. Critical events below get their own realtime treatment.
    if (!line.empty()) CombatLog::Append(CombatLog::Kind::Damage, line, /*speakNow=*/false);

    CheckVitals(tgtBc, hpDelta);
}

// ---- Tier 1: the game's own sentence -----------------------------------------------------------
void HookedSprintf(void* argBlock, void* dest, uint32_t size, uint32_t flag) {
    // The id must be read BEFORE the call; the string only exists after it.
    uint32_t idRaw = 0;
    const bool haveId = MemRead::SafeReadU32(argBlock, 4, &idRaw);

    s_origSprintf(argBlock, dest, size, flag);

    if (!haveId || !dest) return;
    const uint16_t msgId = static_cast<uint16_t>(idRaw & 0x7FFF);   // bit 15 = isPc

    // `dest` is a 0x180 stack buffer in the caller's frame holding RAW CODEC BYTES -- it must be
    // decoded and copied here, because it dies when that frame returns.
    std::wstring text = GameText::Decode(static_cast<const uint8_t*>(dest), 0x180);
    if (text.empty()) return;

    CombatLog::Append(CombatLog::Kind::GameMessage, text, CombatFormat::ShouldSpeakNow(msgId));
}

// ---- Tier 2: the applier. HOT PATH -- see the RVA note above. ----------------------------------
void HookedApply(void* result, void* atkBc, void* tgtBc, uint32_t actionId, uint32_t flags) {
    // Reject on the cheapest possible test FIRST. ~99.8% of calls are status ticks and die here on
    // a single compare, with no allocation, no lock, no SEH read and no string work.
    //
    // Filter the ACTION ID, never `attacker == 0`: FUN_00310db0 makes two REAL calls with a null
    // attacker, and keying on the attacker would silently drop them.
    if ((actionId & 0xFFFF) != 0xFFFF) {
        uint8_t valid = 0;
        // +0x1c == 1 is the emission gate -- FUN_00385f60 sets it as its LAST statement, so every
        // early bail leaves it 0.
        if (MemRead::SafeReadU8(result, R_VALID, &valid) && valid == 1)
            OnRealHit(result, atkBc, tgtBc, static_cast<uint16_t>(actionId & 0xFFFF));
    } else {
        // STATUS TICK. It must NEVER produce a log entry -- regen/poison/doom drift would flood the
        // log with low-information lines between the actions that matter (§9.1.3e), and the drift
        // is surfaced by the 4/5/6/7 vitals readout instead.
        //
        // We look at ticks for exactly ONE reason: poison and doom draining a party member toward
        // death. That costs a single read here, and a zero delta -- the overwhelming majority --
        // exits immediately.
        uint32_t d = 0;
        if (MemRead::SafeReadU32(result, R_TGT_HP, &d) && d != 0)
            CheckVitals(tgtBc, static_cast<int32_t>(d));
    }
    s_origApply(result, atkBc, tgtBc, actionId, flags);
}

} // namespace

bool Init() {
    CombatLog::Init();
    bool ok = Hooks::InstallTyped(RVA_SPRINTF, &HookedSprintf, &s_origSprintf);
    ok     &= Hooks::InstallTyped(RVA_APPLY,   &HookedApply,   &s_origApply);
    Log::Write("COMBAT", ok
        ? "CombatEvents: installed (FUN_00536410 Tier-1 messages, FUN_003112f0 Tier-2 damage)"
        : "CombatEvents: a hook FAILED to install");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_APPLY);
    Hooks::Uninstall(RVA_SPRINTF);
    CombatLog::Shutdown();
}

} // namespace CombatEvents

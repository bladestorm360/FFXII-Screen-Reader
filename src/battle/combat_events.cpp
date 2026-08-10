#include "battle/combat_events.h"

#include "battle/battle_state.h"
#include "battle/combat_format.h"
#include "battle/combat_log.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/stall_probe.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "speech/phrasebook.h"

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

// FUN_00312280(BtlChr* killer, Actor* dying) -- the REWARD BATCH, one call per enemy death. Its sole
// caller is FUN_0030e360's case 0 (the KO funnel, which fires for any cause of death), so this is
// also the truest "an enemy died" event available: unlike the applier, it runs after the HP write
// actually landed rather than off a damage CALCULATION.
//
// None of EXP/LP/gil/loot is an argument -- that claim was struck in Session 49. The function
// computes each reward internally and writes it straight into the BtlChr records, so we snapshot
// before and diff after. Cheap: 9 slots, twice, once per kill.
constexpr uint32_t RVA_REWARD = 0x1F2280;

// result struct fields
constexpr uint32_t R_OUTCOME = 0x04, R_VALID = 0x1C, R_TGT_HP = 0x24;
// BtlChr fields: core/phyre_types.h
using namespace PhyreTypes;

typedef void (*Pfn_Sprintf)(void*, void*, uint32_t, uint32_t);
typedef void (*Pfn_Apply)(void*, void*, void*, uint32_t, uint32_t);
typedef void (*Pfn_Reward)(void*, void*);

Pfn_Sprintf s_origSprintf = nullptr;
Pfn_Apply   s_origApply   = nullptr;
Pfn_Reward  s_origReward  = nullptr;

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

// ---- drop census -------------------------------------------------------------------------------
// Every early exit below used to return in SILENCE, so a battle that produced no combat-log entry
// was indistinguishable from a battle that never happened. That is not hypothetical: when the log
// was reported broken on 2026-08-03, all 14 sessions of that day held exactly two COMBAT lines --
// the init and the install -- and nothing in the file could say whether the producers had failed or
// the player had simply not fought. (They had not: every one of those logs reports Enemy=0.) An
// absent line means nobody wrote one; it is not evidence either way, and that ambiguity is the bug.
//
// Counters are bumped on the HOT PATH -- one add, no allocation, no lock, no SEH read -- and are
// REPORTED only when the count reaches a power of two. The file therefore grows O(log N) in the
// number of drops and never O(N), which is what the console-output budget requires of a hook that
// runs ~20 times a second per actor. 1 is a power of two, so the FIRST of each kind always prints,
// and that first line is the one that names the cause.
uint32_t g_dropNotValid  = 0;   // applier reached with a real action id, but +0x1c never armed
uint32_t g_dropEmptyLine = 0;   // genuine hit, but the formatter produced no sentence
uint32_t g_dropNoBuffer  = 0;   // Tier-1 sprintf with no readable id or no destination buffer
uint32_t g_dropNoText    = 0;   // Tier-1 sprintf whose buffer decoded to nothing
uint32_t g_realHits      = 0;   // POSITIVE control: hits that passed the gate and were formatted

bool CountAtPowerOfTwo(uint32_t& n) {
    ++n;
    return (n & (n - 1)) == 0;
}

void CountDrop(const char* reason, uint32_t& n) {
    if (!CountAtPowerOfTwo(n)) return;
    char m[128];
    snprintf(m, sizeof(m), "drop[%s] x%u", reason, n);
    Log::Write("COMBAT", m);
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

    // ---- THE 20% THRESHOLD IS 20% OF **MAX**, AND CURRENT IS ALLOWED TO EXCEED IT ---------------
    //
    // User's decision, S148, after working the alternatives through: *"you might just have to do 20%
    // of max hp regardless… that way when current HP does drop to 20% of max HP the low HP warning
    // will still fire."*
    //
    // WHAT MADE THIS LOOK BROKEN. With the licence-board `HP x2` augment, CURRENT sits above max --
    // Basch reads 14638 against a max of 7319 -- so the ratio starts at ~200% and the warning fires
    // at 1464, which is 10% of what he can actually absorb. That is a real consequence and it is
    // ACCEPTED, not overlooked: 20% of your max is a meaningful danger line, and an overmax buffer
    // is a temporary cushion sitting on top of it, not a bigger pool to re-scale against.
    //
    // `BC_MAXHP` IS THE RIGHT DENOMINATOR, and that is settled rather than assumed: `FUN_002fef30`
    // called as `FUN_002fef30(bc, 0, 0)` defaults its output pointer to `param_1 + 0x24`, so +0x24 is
    // that function's own computed result -- base plus twelve equipment/licence sources, already
    // through the party-side 9999 clamp. It is the game's own max, not a raw stat.
    //
    // DO NOT "FIX" THE >100% CASE. A future reader will see current above max and want to clamp the
    // numerator, or swap the denominator for the current-at-full-health value. Both change when the
    // warning fires, and neither is what was asked for. The comparison below is written to be
    // correct for post > denom rather than to prevent it.
    //
    // DisplayHp on the denominator is a no-op today (the game already clamped +0x24) and is here to
    // state the rule: the threshold is 20% of the max the player is shown, never of a raw field.
    //
    // PARTY ONLY. The comparison lives inside the `party` branch below and is not evaluated for
    // anything else, so nothing here reasons about enemy HP magnitudes -- an earlier revision of this
    // comment justified a widened integer type with a boss that can never reach the code.
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
        // Alive. THE WARNING IS PARTY-ONLY -- an enemy's HP fraction is not actionable, and the
        // threshold is never even computed for one. Everything below is therefore reasoning about
        // party HP alone: max is <= 9999 (the game's own clamp), current tops out around twice that
        // with an overmax buffer, so the multiply cannot overflow and needs no widening.
        //
        // Multiply rather than divide so the boundary is exact: 7319/5 truncates to 1463, and
        // `post * 5 <= 7319` gives the same answer without the rounding step.
        if (party &&
            static_cast<int32_t>(post) * 5 <=
                BattleState::DisplayHp(tgtBc, static_cast<int32_t>(mx))) {
            if (!l.low) {
                l.low = true;
                const std::wstring who =
                    BattleState::DisplayNameForActor(BattleState::ActorForBtlChr(tgtBc));
                if (!who.empty())
                    CombatLog::Append(CombatLog::Kind::System, who + Phrase::Get(Phrase::Id::BelowTwentyPercent),
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
    //
    // ENEMY side: the game emits nothing, so that line is ours -- but it is NOT emitted here any
    // more. It moved to HookedReward (FUN_00312280), the real death event, so the kill and its
    // EXP/LP can be spoken as ONE line. Announcing from here was announcing off a damage
    // CALCULATION, one step before the HP write and well before the rewards exist. The latch above
    // still runs for both sides: it is what re-arms the 20% warning on revive.
}

// ---- rewards: the enemy-defeated line, with the kill's EXP and LP -------------------------------
// FUN_00312280(killer, dying). Once per enemy death, off the KO funnel -- NOT a per-frame path.
//
// FFXII has no end-of-battle results screen: it grants rewards per corpse and shows them as
// floating +EXP/+LP sprite digits, so there is no game text to read and the sentence is ours.
// The NUMBERS are the game's own -- we snapshot the party's EXP/LP, let the batch run, and diff.
//
// Why the diff and not FUN_0028fb80(actorId, exp, lp), the popup itself: Ghidra renders that call
// site with what look like the gil accumulator in the value slots (dropped register args), leaving
// the argument identity at ~0.85 -- below this project's bar. The diff needs no such inference; it
// observes what the game actually wrote.
void HookedReward(void* killer, void* dying) {
    STALL_SCOPE("CombatEvents::HookedReward");

    // Roster list 3, slots 0-8 -- the SAME list FUN_00312280 walks to build its living-party set
    // (FUN_00320ab0(i, 3), capped at 9). Reserve members gain nothing, so their delta is simply 0.
    uint32_t beforeExp[BattleState::kRosterSlots] = {};
    uint32_t beforeLp [BattleState::kRosterSlots] = {};
    void*    slotBc   [BattleState::kRosterSlots] = {};
    for (int i = 0; i < BattleState::kRosterSlots; ++i) {
        slotBc[i] = BattleState::BtlChrForSlot(i);
        if (!slotBc[i]) continue;
        MemRead::SafeReadU32(slotBc[i], BC_EXP, &beforeExp[i]);
        MemRead::SafeReadU32(slotBc[i], BC_LP,  &beforeLp[i]);
    }

    // Resolve the name BEFORE the batch: this is a death path, and the actor's record is being torn
    // down around it.
    const std::wstring who = BattleState::DisplayNameForActor(dying);

    s_origReward(killer, dying);

    // Largest delta across the roster. EXP is divided among the survivors, so every living member
    // gets the same share and the max IS that share -- while a KO'd or absent member reads 0 and
    // cannot drag the answer down. An EXP-doubling aura makes one member exceed the on-screen
    // number; reporting what was actually gained is the more useful figure.
    uint32_t expGain = 0, lpGain = 0;
    for (int i = 0; i < BattleState::kRosterSlots; ++i) {
        if (!slotBc[i]) continue;
        uint32_t e = 0, l = 0;
        if (MemRead::SafeReadU32(slotBc[i], BC_EXP, &e) && e > beforeExp[i])
            expGain = (e - beforeExp[i] > expGain) ? e - beforeExp[i] : expGain;
        if (MemRead::SafeReadU32(slotBc[i], BC_LP, &l) && l > beforeLp[i])
            lpGain  = (l - beforeLp[i] > lpGain)  ? l - beforeLp[i]  : lpGain;
    }

    {
        char m[160];
        snprintf(m, sizeof(m), "reward: killer=%p dying=%p exp=+%u lp=+%u named=%d",
                 killer, dying, expGain, lpGain, who.empty() ? 0 : 1);
        Log::Write("COMBAT", m);
    }

    // No name means no line -- never announce an anonymous kill.
    if (who.empty()) return;

    // REALTIME (user, 2026-07-20). Knowing a foe is down lets you stop attacking a corpse and
    // retarget, which is worth an interruption. If it proves too chatty in a big fight the fix is a
    // config toggle, not silence -- so keep this a single flag rather than burying the decision.
    CombatLog::Append(CombatLog::Kind::System,
                      CombatFormat::DefeatedLine(who, expGain, lpGain), /*speakNow=*/true);
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

    // Resolved into locals rather than passed inline, so the drop diagnostic below can say WHICH
    // half failed. DamageLine returns nothing only when attacker AND target are both nameless, and
    // that distinction is the difference between a broken name chain and a broken formatter.
    const std::wstring atkName = BattleState::DisplayNameForActor(atkActor);
    const std::wstring tgtName = BattleState::DisplayNameForActor(tgtActor);
    const uint8_t      cat     = BattleState::AbilityCategory(actionId);

    const std::wstring line = CombatFormat::DamageLine(
        atkName,
        tgtName,
        BattleState::AbilityName(actionId),
        cat,
        hpDelta,
        outcome,
        BattleState::AbilityElements(actionId));

    // Log-only: this is the stream that made linear narration unusable, and it is why the log
    // exists at all. Critical events below get their own realtime treatment.
    if (!line.empty()) {
        CombatLog::Append(CombatLog::Kind::Damage, line, /*speakNow=*/false);
    } else if (CountAtPowerOfTwo(g_dropEmptyLine)) {
        // The one drop worth more than a counter: a hit passed every gate and still said nothing.
        // Both name flags at 0 points at the actor/name chain, not at this file.
        char m[192];
        snprintf(m, sizeof(m),
                 "drop[empty-line] x%u: action=0x%04X cat=%u atkNamed=%d tgtNamed=%d "
                 "hpDelta=%d outcome=%u",
                 g_dropEmptyLine, static_cast<unsigned>(actionId), static_cast<unsigned>(cat),
                 atkName.empty() ? 0 : 1, tgtName.empty() ? 0 : 1,
                 static_cast<int>(hpDelta), static_cast<unsigned>(outcome));
        Log::Write("COMBAT", m);
    }

    CheckVitals(tgtBc, hpDelta);
}

// ---- Tier 1: the game's own sentence -----------------------------------------------------------
void HookedSprintf(void* argBlock, void* dest, uint32_t size, uint32_t flag) {
    STALL_SCOPE("CombatEvents::HookedSprintf");
    // The id must be read BEFORE the call; the string only exists after it.
    uint32_t idRaw = 0;
    const bool haveId = MemRead::SafeReadU32(argBlock, 4, &idRaw);

    s_origSprintf(argBlock, dest, size, flag);

    if (!haveId || !dest) { CountDrop("sprintf-no-buffer", g_dropNoBuffer); return; }
    const uint16_t msgId = static_cast<uint16_t>(idRaw & 0x7FFF);   // bit 15 = isPc

    // `dest` is a 0x180 stack buffer in the caller's frame holding RAW CODEC BYTES -- it must be
    // decoded and copied here, because it dies when that frame returns.
    std::wstring text = GameText::Decode(static_cast<const uint8_t*>(dest), 0x180);
    if (text.empty()) { CountDrop("sprintf-no-text", g_dropNoText); return; }

    CombatLog::Append(CombatLog::Kind::GameMessage, text, CombatFormat::ShouldSpeakNow(msgId));
}

// ---- Tier 2: the applier. HOT PATH -- see the RVA note above. ----------------------------------
void HookedApply(void* result, void* atkBc, void* tgtBc, uint32_t actionId, uint32_t flags) {
    STALL_SCOPE("CombatEvents::HookedApply");
    // Reject on the cheapest possible test FIRST. ~99.8% of calls are status ticks and die here on
    // a single compare, with no allocation, no lock, no SEH read and no string work.
    //
    // Filter the ACTION ID, never `attacker == 0`: FUN_00310db0 makes two REAL calls with a null
    // attacker, and keying on the attacker would silently drop them.
    if ((actionId & 0xFFFF) != 0xFFFF) {
        uint8_t valid = 0;
        // +0x1c == 1 is the emission gate -- FUN_00385f60 sets it as its LAST statement, so every
        // early bail leaves it 0.
        if (MemRead::SafeReadU8(result, R_VALID, &valid) && valid == 1) {
            // POSITIVE control. Without it, "no drops and no entries" still has two readings --
            // the gate rejected everything, or nothing ever reached the gate. This line is what
            // makes "the applier saw N real hits" a fact instead of an inference.
            if (CountAtPowerOfTwo(g_realHits)) {
                char m[96];
                snprintf(m, sizeof(m), "realhit x%u (action=0x%04X)",
                         g_realHits, static_cast<unsigned>(actionId & 0xFFFF));
                Log::Write("COMBAT", m);
            }
            OnRealHit(result, atkBc, tgtBc, static_cast<uint16_t>(actionId & 0xFFFF));
        } else {
            // A real action id that never armed the emission gate. Expected sometimes; a battle
            // made ENTIRELY of these is the shape "the log stopped working" would actually take.
            CountDrop("apply-not-valid", g_dropNotValid);
        }
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
    ok     &= Hooks::InstallTyped(RVA_REWARD,  &HookedReward,  &s_origReward);
    Log::Write("COMBAT", ok
        ? "CombatEvents: installed (FUN_00536410 Tier-1 messages, FUN_003112f0 Tier-2 damage, "
          "FUN_00312280 enemy defeated + EXP/LP)"
        : "CombatEvents: a hook FAILED to install");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_REWARD);
    Hooks::Uninstall(RVA_APPLY);
    Hooks::Uninstall(RVA_SPRINTF);
    CombatLog::Shutdown();
}

} // namespace CombatEvents

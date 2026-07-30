#include "battle/battle_state.h"
#include "battle/battle_state_internal.h"
#include "battle/battle_state_diag.h"

#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "core/logger.h"
#include "navigation/nav_rva.h"

#include <Windows.h>
#include <cstdio>
#include <string>

// The two BattleState diagnostics, split out of battle_state.cpp (Session 93) which was 584 lines
// against a 500-line cap before the bidirectional-engagement work added to it. Same seam this repo
// already uses for exactly this purpose: entity_diag.cpp, exit_diag.cpp, map_script_diag.cpp.
//
// Both are LOG-ONLY and neither is on any hot path. They exist because the no-filler rule sends the
// answer to "why did that key say nothing" to the log rather than to speech -- DiagnoseSlot is what
// `4`/`5`/`6`/`7` print instead of announcing an empty slot, and DiagnoseCommitment is what `;` prints
// when there is no committed target to describe.
namespace BattleState {
namespace {

using MemRead::PtrAt;
using MemRead::SafeReadU8;
using MemRead::SafeReadU16;
using MemRead::SafeReadU32;
using namespace PhyreTypes;

using Internal::Reloc;
using Internal::PoolString;
using Internal::MasterRecord;
using Internal::RVA_ACTIONTBL;

// Copies of the handful of BtlWork/actor offsets these two print. Deliberately NOT re-exported from
// battle_state.cpp: a diagnostic that shared its subject's constants could not detect a wrong one, and
// telling the two apart is most of what DiagnoseCommitment is for.
constexpr uint32_t OFF_LEADER    = 0x5AA4;     // u8 leader BtlChr index
constexpr uint32_t OFF_BC_ARRAY  = 0x08;
constexpr uint32_t BC_STRIDE     = 0x1C8;
constexpr uint32_t BC_COUNT      = 0x28;
constexpr uint32_t OFF_ROSTER_L3 = 0x5A7E;     // list 3: 9 x u16 BtlChr indices (unmasked party)

constexpr uint32_t A_FLAGS = 0x00, A_PHASE = 0x6B4,
                   A_ACTIVE_TGT = 0x710, A_ACTIVE_ACT = 0x714,
                   A_QUEUED_ACT = 0xBA0, A_QUEUED_TGT = 0xBB8;
constexpr uint64_t A_FLAG_QUEUED = 0x4000;

// Category 0x14 = magicks / technicks / actions, the argument FUN_0035d330 takes beside an ability id.
constexpr uint32_t CAT_ABILITY = 0x14;

// st2e master-data header: +0x04 count, +0x08 stride, +0x0C -> records.
constexpr uint32_t HDR_COUNT = 0x04, HDR_STRIDE = 0x08, HDR_RECORDS = 0x0C;

} // namespace

SlotDiag DiagnoseSlot(int slot) {
    SlotDiag d;
    d.globalValue = PtrAt(Hooks::ResolveRva(BTLWORK_PTR), 0);
    if (!d.globalValue) return d;
    d.magicOk = SafeReadU32(d.globalValue, 0x00, &d.magic) && d.magic == BTLWORK_MAGIC;
    if (!d.magicOk || slot < 0 || slot >= kRosterSlots) return d;
    uint16_t e = 0xFFFF;
    d.rosterRead = SafeReadU16(d.globalValue, OFF_ROSTER_L3 + static_cast<uint32_t>(slot) * 2, &e);
    d.rosterEntry = e;
    return d;
}

void DiagnoseCommitment() {
    char m[320];

    void* w = Work();
    if (!w) {
        void* raw = PtrAt(Hooks::ResolveRva(BTLWORK_PTR), 0);
        uint32_t magic = 0;
        if (raw) MemRead::SafeReadU32(raw, 0, &magic);
        snprintf(m, sizeof(m), "commit-diag: BtlWork REJECTED raw=%p magic=0x%X (want 0x%X)",
                 raw, magic, BTLWORK_MAGIC);
        Log::Write("TARGET", m);
        return;
    }

    uint8_t leaderIdx = 0xFF;
    const bool idxOk = MemRead::SafeReadU8(w, OFF_LEADER, &leaderIdx);
    void* lbc = LeaderBtlChr();
    void* lact = LeaderActor();

    void* pool = PtrAt(Hooks::ResolveRva(NavRva::ACTOR_POOL_BASE), 0);
    uint32_t poolCount = 0;
    MemRead::SafeReadU32(Hooks::ResolveRva(NavRva::ACTOR_POOL_COUNT), 0, &poolCount);

    snprintf(m, sizeof(m),
             "commit-diag: W=%p leaderIdx=%u(read=%d,max=%u) leaderBc=%p leaderActor=%p pool=%p count=%u",
             w, leaderIdx, idxOk ? 1 : 0, BC_COUNT, lbc, lact, pool, poolCount);
    Log::Write("TARGET", m);

    if (!lact) {
        Log::Write("TARGET", "commit-diag: NO LEADER ACTOR -- the pool scan for actor+0x698 == leaderBc "
                             "found nothing, so no commitment can ever resolve.");
        return;
    }

    uint64_t flags = 0; uint8_t phase = 0;
    uint16_t aAct = 0xFFFF, qAct = 0xFFFF;
    uint32_t aTgt = 0, qTgt = 0;
    MemRead::SafeReadU64(lact, A_FLAGS, &flags);
    MemRead::SafeReadU8 (lact, A_PHASE, &phase);
    SafeReadU16(lact, A_ACTIVE_ACT, &aAct);
    SafeReadU32(lact, A_ACTIVE_TGT, &aTgt);
    SafeReadU16(lact, A_QUEUED_ACT, &qAct);
    SafeReadU32(lact, A_QUEUED_TGT, &qTgt);
    const bool activeRow = (aAct != 0xFFFF) && (MasterRecord(RVA_ACTIONTBL, aAct) != nullptr);

    snprintf(m, sizeof(m),
             "commit-diag: flags=0x%llX queuedBit=%d phase=%u | active act=0x%X tgt=0x%X row=%d "
             "| queued act=0x%X tgt=0x%X",
             (unsigned long long)flags, (flags & A_FLAG_QUEUED) ? 1 : 0, phase,
             aAct, aTgt, activeRow ? 1 : 0, qAct, qTgt);
    Log::Write("TARGET", m);

    // The ACTIVE branch's `row != nullptr` test is what rejects a live Attack (id 0x96), so dump the
    // table internals it depends on. AbilityName uses the SAME MasterRecord, so whatever this shows
    // also governs whether the combat log can name an ability. Report the numbers; do not guess.
    {
        void* hdr = PtrAt(Hooks::ResolveRva(RVA_ACTIONTBL), 0);
        uint32_t count = 0, recOff = 0; uint16_t stride = 0;
        bool cOk = false, sOk = false, rOk = false;
        if (hdr) {
            cOk = SafeReadU32(hdr, HDR_COUNT,   &count);
            sOk = SafeReadU16(hdr, HDR_STRIDE,  &stride);
            rOk = SafeReadU32(hdr, HDR_RECORDS, &recOff);
        }
        // The reloc base is printed as its RAW 8 BYTES plus a read-ok flag, because 0 is a
        // legitimate value here and the old code could not tell it from a failed read (that
        // conflation is the S87 Reloc() bug). This line is what answers "is the addend actually
        // zero on this build" WITHOUT a Frida probe -- one battle and the log says so.
        uint64_t relocRaw = 0;
        const bool relocOk = MemRead::SafeReadU64(Hooks::ResolveRva(MASTERDATA_RELOC_BASE), 0, &relocRaw);
        void* records = (recOff && relocOk)
                        ? reinterpret_cast<char*>(static_cast<uintptr_t>(relocRaw)) + recOff : nullptr;
        snprintf(m, sizeof(m),
                 "commit-diag: actionTbl hdr=%p count=%u(%d) stride=%u(%d) recOff=0x%X(%d) "
                 "relocRaw=0x%016llX(read=%d) records=%p | id=0x%X %s",
                 hdr, count, cOk ? 1 : 0, stride, sOk ? 1 : 0, recOff, rOk ? 1 : 0,
                 (unsigned long long)relocRaw, relocOk ? 1 : 0, records, aAct,
                 !hdr        ? "<-- TABLE PTR NULL"
               : !cOk        ? "<-- COUNT UNREADABLE"
               : (aAct >= count) ? "<-- ID >= COUNT (out of table)"
               : (stride == 0)   ? "<-- STRIDE 0"
               : !relocOk    ? "<-- RELOC BASE UNREADABLE"
               : !records    ? "<-- RECORDS NULL"
                             : "(id is in range -- row should resolve)");
        Log::Write("TARGET", m);

        // Both name chains side by side. The DEF chain (FUN_0035d330) is what AbilityName uses as
        // of S87 and is confirmed in play; the OLD action-table chain is printed only to show
        // whether Reloc()/PoolString() work at all, which is what governs AbilityCategory -- the
        // verb (casts/readies/uses). If the old chain is empty the verb degrades to "attacks" while
        // the name stays correct, which is exactly the pre-S87 behaviour and not a regression.
        if (aAct != 0xFFFF) {
            char defBuf[128] = {}, oldBuf[128] = {};
            Log::ToUtf8(DefName(CAT_ABILITY, aAct), defBuf, sizeof(defBuf));
            void* row = MasterRecord(RVA_ACTIONTBL, aAct);
            uint16_t nameIdx = 0; uint8_t catByte = 0;
            if (row) { SafeReadU16(row, 0x34, &nameIdx); SafeReadU8(row, 0x1E, &catByte); }
            Log::ToUtf8(row ? PoolString(nameIdx) : std::wstring(), oldBuf, sizeof(oldBuf));
            snprintf(m, sizeof(m),
                     "commit-diag: id=0x%X | DEF chain (shipped) = \"%s\" | action-table chain = "
                     "\"%s\" (row=%p nameIdx=%u cat=%u) %s",
                     aAct, defBuf, oldBuf, row, nameIdx, catByte,
                     row ? "" : "<-- action row unresolved: AbilityCategory will read 0 (verb -> \"attacks\")");
            Log::Write("TARGET", m);
        }
    }

    // Name the failing condition explicitly rather than leaving it to be inferred from the numbers.
    // When a path DOES match, the commitment is fine and the failure is downstream -- in
    // ActorForHandle -- so resolve the handle here too and say so. Reporting "should have matched"
    // without checking that was the gap that made a working commitment look like no commitment.
    if (activeRow && aTgt != 0) {
        void* ta = ActorForHandle(static_cast<int32_t>(aTgt));
        snprintf(m, sizeof(m), "commit-diag: ACTIVE path MATCHED -> ActorForHandle(0x%X)=%p%s",
                 aTgt, ta, ta ? "" : "  <-- HANDLE LOOKUP IS THE BUG");
        Log::Write("TARGET", m);
    } else if ((flags & A_FLAG_QUEUED) && qTgt != 0) {
        void* tq = ActorForHandle(static_cast<int32_t>(qTgt));
        snprintf(m, sizeof(m), "commit-diag: QUEUED path MATCHED -> ActorForHandle(0x%X)=%p%s",
                 qTgt, tq, tq ? "" : "  <-- HANDLE LOOKUP IS THE BUG");
        Log::Write("TARGET", m);
    } else {
        const char* why = (aTgt == 0 && qTgt == 0) ? "both target fields are 0"
                        : (!activeRow && !(flags & A_FLAG_QUEUED)) ? "active id not in the action table AND queued bit clear"
                        : (!activeRow) ? "active id not in the action table (AI opcode?), queued bit set but target 0"
                                       : "queued bit clear";
        snprintf(m, sizeof(m), "commit-diag: NO COMMITMENT -- %s", why);
        Log::Write("TARGET", m);
    }
}

} // namespace BattleState

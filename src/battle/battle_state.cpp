#include "battle/battle_state.h"

#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "core/game_text.h"
#include "core/logger.h"
#include "navigation/nav_rva.h"

#include <Windows.h>
#include <cstdio>

namespace BattleState {
namespace {

using MemRead::PtrAt;
using MemRead::SafeReadU8;
using MemRead::SafeReadU16;
using MemRead::SafeReadU32;

// ---- RVAs (abs = RVA + 0x120000) --------------------------------------------------------------
// BTLWORK_PTR / BTLWORK_MAGIC / MASTERDATA_RELOC_BASE: core/phyre_types.h (each had 2-3 names)
constexpr uint32_t RVA_POOL      = 0x2D9F170;  // DAT_02ebf170 -- shared codec string pool (word.bin)
constexpr uint32_t RVA_ACTIONTBL = 0x2D9F138;  // DAT_02ebf138 -- ability/action table
constexpr uint32_t RVA_STATUSTBL = 0x2D9F118;  // DAT_02ebf118 -- battle status-name table

// The DEF-record resolver: FUN_0035d330(category, id) fills a static record and returns it; the
// localized name is the codec pointer at record+0x18. This is the game's own name path -- the one
// ingame_menu_reader has been using for spell/technick names since S31 -- and it does NOT depend on
// Reloc()/MasterRecord()/PoolString(). Hoisted here so there is one copy, per the centralize rule.
constexpr uint32_t RVA_RESOLVE_DEF = 0x23D330;  // FUN_0035d330(cat, id) -> &record
constexpr uint32_t OFF_DEF_CODEC   = 0x18;      // record+0x18 = name codec source
constexpr uint32_t CAT_ABILITY     = 0x14;      // magicks / technicks / actions
typedef const uint8_t* (*Pfn_ResolveDef)(uint32_t, uint32_t);

// Party-member runtime records: &DAT_022c8080, stride 0xC0, live count at DAT_022c8064 (int).
// FUN_0035bb20 is the game's accessor; FUN_00272ee0 walks at most FOUR of them (3 party + guest).
// NOTE map_rva.h calls this same array a "field-sign site record table" (SITE_TABLE_*). Those
// constants are declared and never used; the derivation here -- +0x04 matched against a scene
// handle, +0x60 used as a BtlChr index by the gambit getter -- is the one backed by call sites.
constexpr uint32_t RVA_PARTY_RECS  = 0x21A8080;  // DAT_022c8080
constexpr uint32_t RVA_PARTY_COUNT = 0x21A8064;  // DAT_022c8064 (int)
constexpr uint32_t PARTY_STRIDE    = 0xC0;
constexpr uint32_t PARTY_HANDLE    = 0x04;       // i32 scene handle
constexpr uint32_t PARTY_BCIDX     = 0x60;       // i16 BtlChr index
constexpr uint32_t PARTY_SLOT_MAX  = 4;          // FUN_00272ee0 gives up after slot 3

constexpr uint32_t BC_FLAGS        = 0x00;       // u32 flag word (NOT previously named)
constexpr uint32_t BC_FLAG_GAMBIT  = 0x04;       // bit 2 -- gambit master toggle

constexpr uint32_t OFF_ROSTER_L3 = 0x5A7E;     // list 3: 9 x u16 BtlChr indices (unmasked party)
constexpr uint32_t OFF_LEADER    = 0x5AA4;     // u8 leader BtlChr index
constexpr uint32_t OFF_BC_ARRAY  = 0x08;
constexpr uint32_t BC_STRIDE     = 0x1C8;
constexpr uint32_t BC_COUNT      = 0x28;

// BC_CHARID / BC_KIND and the actor-pool / scene-kind layout: core/phyre_types.h
using namespace PhyreTypes;

// actor fields
constexpr uint32_t A_FLAGS = 0x00, A_HANDLE = 0x08, A_PHASE = 0x6B4,
                   A_ACTIVE_TGT = 0x710, A_ACTIVE_ACT = 0x714,
                   A_QUEUED_ACT = 0xBA0, A_QUEUED_TGT = 0xBB8;
constexpr uint64_t A_FLAG_QUEUED = 0x4000;

// The aggro pair (GameArchitecture / combat_system.md section 7.1, confidence 0.97):
//   +0xEA4  u32  bit i set <=> the actor whose OWN pool index is i has committed an action at me
//   +0xEA9  u8   this actor's own pool index, i.e. which bit it sets in everyone else's mask
constexpr uint32_t A_ENGAGED_BY = 0xEA4, A_POOL_INDEX = 0xEA9;

// sceneObj fields
constexpr uint32_t SO_INST = 0x100, SO_NAMEKEY = 0x102;

// st2e master-data header: +0x04 count, +0x08 stride, +0x0C -> records.
constexpr uint32_t HDR_COUNT = 0x04, HDR_STRIDE = 0x08, HDR_RECORDS = 0x0C;

// Every "pointer" in master data is a u32 offset needing this relocation (FUN_0020e600, which is
// literally `return off + _DAT_01f83530`).
//
// S87 FIX: this used to bail when the base READ BACK AS ZERO, conflating "the read failed" with
// "the addend is 0" -- and 0 is a legitimate addend. Branch on the read succeeding instead. The
// width is right: FUN_0020e600 is 10 bytes (`mov eax,ecx` + `add rax, qword [rip+d]` + `ret`), a
// genuine 64-bit load. Its inverse FUN_0020e620 is 9 bytes doing a 32-bit `sub eax, dword [rip+d]`,
// and for that to invert the 64-bit add the high dword must be zero -- so the base is below 4 GB.
// Whether it is exactly zero is a runtime fact; DiagnoseSlot prints it (see the actionTbl block).
//
// A wrong resulting pointer is safe: every consumer reads through MemRead's SEH-guarded helpers.
void* Reloc(uint32_t off) {
    if (off == 0) return nullptr;
    uint64_t base = 0;
    if (!MemRead::SafeReadU64(Hooks::ResolveRva(MASTERDATA_RELOC_BASE), 0, &base)) return nullptr;
    return reinterpret_cast<char*>(static_cast<uintptr_t>(base)) + off;
}

// Shared-pool lookup: chunk = pool[1 + (idx >> 11)], string = chunk[idx & 0x7FF].
// Both levels are [u32 count][u32 offset...] with offsets relative to their own table base, and
// the string itself carries FUN_002b58b0's 00 00 variant prefix.
std::wstring PoolString(uint16_t idx) {
    void* pool = PtrAt(Hooks::ResolveRva(RVA_POOL), 0);
    if (!pool) return std::wstring();

    uint32_t chunkOff = 0;
    if (!SafeReadU32(pool, 4 + static_cast<uint32_t>(idx >> 11) * 4, &chunkOff)) return std::wstring();
    void* chunk = Reloc(chunkOff);
    if (!chunk) return std::wstring();

    uint32_t strOff = 0;
    if (!SafeReadU32(chunk, 4 + static_cast<uint32_t>(idx & 0x7FF) * 4, &strOff)) return std::wstring();
    const uint8_t* str = static_cast<const uint8_t*>(Reloc(strOff));
    if (!str) return std::wstring();

    return GameText::Decode(GameText::SkipVariantPrefix(str), 128);
}

// Resolve one record out of an st2e table, with the count guard that keeps a non-ability id
// (the 0x4000+ AI-opcode band) from indexing outside the table.
void* MasterRecord(uint32_t tableRva, uint32_t index) {
    void* hdr = PtrAt(Hooks::ResolveRva(tableRva), 0);
    if (!hdr) return nullptr;
    uint32_t count = 0; uint16_t stride = 0; uint32_t recOff = 0;
    if (!SafeReadU32(hdr, HDR_COUNT, &count) || index >= count) return nullptr;
    if (!SafeReadU16(hdr, HDR_STRIDE, &stride) || stride == 0) return nullptr;
    if (!SafeReadU32(hdr, HDR_RECORDS, &recOff)) return nullptr;
    void* records = Reloc(recOff);
    if (!records) return nullptr;
    return static_cast<char*>(records) + static_cast<size_t>(index) * stride;
}

} // namespace

// ================================================================================================

void* Work() {
    void* w = PtrAt(Hooks::ResolveRva(BTLWORK_PTR), 0);      // THE DEREFERENCE
    if (!w) return nullptr;
    uint32_t magic = 0;
    if (!SafeReadU32(w, 0x00, &magic) || magic != BTLWORK_MAGIC) return nullptr;
    return w;
}

void* BtlChrForSlot(int slot) {
    if (slot < 0 || slot >= kRosterSlots) return nullptr;
    void* w = Work();
    if (!w) return nullptr;
    // Entries are u16 but a valid index is < 0x28, so the low byte carries it and the 0xFFFF
    // empty sentinel shows up as 0xFF -- which the >= BC_COUNT test rejects either way.
    uint8_t idx = 0xFF;
    if (!SafeReadU8(w, OFF_ROSTER_L3 + static_cast<uint32_t>(slot) * 2, &idx)) return nullptr;
    if (idx >= BC_COUNT) return nullptr;
    return static_cast<char*>(w) + OFF_BC_ARRAY + static_cast<size_t>(idx) * BC_STRIDE;
}

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

void* LeaderBtlChr() {
    void* w = Work();
    if (!w) return nullptr;
    uint8_t id = 0xFF;
    if (!SafeReadU8(w, OFF_LEADER, &id) || id >= BC_COUNT) return nullptr;
    return static_cast<char*>(w) + OFF_BC_ARRAY + static_cast<size_t>(id) * BC_STRIDE;
}

void* LeaderActor() { return ActorForBtlChr(LeaderBtlChr()); }

void* BtlChrForActor(void* actor) { return PtrAt(actor, NavRva::ACTOR_DEF_PTR); }

bool GambitsEnabled(uint32_t sceneHandle, bool* outResolved) {
    if (outResolved) *outResolved = false;
    if (sceneHandle == 0) return false;

    // The record array is a STATIC array, so ResolveRva gives its address directly -- no deref.
    void* recs  = Hooks::ResolveRva(RVA_PARTY_RECS);
    void* countp = Hooks::ResolveRva(RVA_PARTY_COUNT);
    if (!recs || !countp) return false;
    uint32_t count = 0;
    if (!SafeReadU32(countp, 0, &count)) return false;
    if (count > PARTY_SLOT_MAX) count = PARTY_SLOT_MAX;   // same bound the game's own walk uses

    for (uint32_t i = 0; i < count; ++i) {
        char* rec = static_cast<char*>(recs) + static_cast<size_t>(i) * PARTY_STRIDE;
        uint32_t h = 0;
        if (!SafeReadU32(rec, PARTY_HANDLE, &h) || h != sceneHandle) continue;

        uint16_t idx = 0;
        if (!SafeReadU16(rec, PARTY_BCIDX, &idx) || idx >= BC_COUNT) return false;
        void* w = Work();
        if (!w) return false;
        void* bc = static_cast<char*>(w) + OFF_BC_ARRAY + static_cast<size_t>(idx) * BC_STRIDE;

        uint32_t flags = 0;
        if (!SafeReadU32(bc, BC_FLAGS, &flags)) return false;
        if (outResolved) *outResolved = true;
        return (flags & BC_FLAG_GAMBIT) != 0;
    }
    return false;   // handle not in the party table -- unresolved, and the caller stays silent
}

void* ActorForBtlChr(void* bc) {
    if (!bc) return nullptr;
    void* pool = PtrAt(Hooks::ResolveRva(NavRva::ACTOR_POOL_BASE), 0);
    if (!pool) return nullptr;
    uint32_t count = 0;
    SafeReadU32(Hooks::ResolveRva(NavRva::ACTOR_POOL_COUNT), 0, &count);
    if (count == 0 || count > 128) return nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        void* actor = static_cast<char*>(pool) + static_cast<size_t>(i) * NavRva::ACTOR_STRIDE;
        if (PtrAt(actor, NavRva::ACTOR_DEF_PTR) == bc) return actor;
    }
    return nullptr;
}

void* ActorForHandle(int32_t handle) {
    if (handle == 0) return nullptr;
    void* pool = PtrAt(Hooks::ResolveRva(NavRva::ACTOR_POOL_BASE), 0);
    if (!pool) return nullptr;
    uint32_t count = 0;
    SafeReadU32(Hooks::ResolveRva(NavRva::ACTOR_POOL_COUNT), 0, &count);
    if (count == 0 || count > 128) return nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        void* actor = static_cast<char*>(pool) + static_cast<size_t>(i) * NavRva::ACTOR_STRIDE;
        uint32_t h = 0;
        // actor+0x08 is the actor's OWN handle (assigned directly in FUN_00322080).
        if (SafeReadU32(actor, A_HANDLE, &h) && static_cast<int32_t>(h) == handle) return actor;
    }
    return nullptr;
}

std::wstring NameForActor(void* actor) {
    if (!actor) return std::wstring();
    const uint8_t* codec = static_cast<const uint8_t*>(PtrAt(actor, NavRva::ACTOR_NAME_STR));
    // NO SkipVariantPrefix here: FUN_0023a570 stores this already variant-selected.
    std::wstring nm = GameText::Decode(codec, 128);
    return GameText::IsMostlyPrintable(nm) ? nm : std::wstring();
}

std::wstring NameForBtlChr(void* bc) { return NameForActor(ActorForBtlChr(bc)); }

uint16_t InstanceIndex(void* actor) {
    void* so = PtrAt(actor, NavRva::ACTOR_SCENEOBJ);
    if (!so) return 0;
    uint16_t key = 0;
    if (!SafeReadU16(so, SO_NAMEKEY, &key)) return 0;
    if (static_cast<int16_t>(key) >= 0) return 0;      // sign bit clear => no instance letter
    uint16_t inst = 0;
    if (!SafeReadU16(so, SO_INST, &inst)) return 0;
    return inst;
}

std::wstring DisplayNameForActor(void* actor) {
    std::wstring nm = NameForActor(actor);
    if (nm.empty()) return nm;
    const uint16_t inst = InstanceIndex(actor);
    if (inst >= 1 && inst <= 26) {
        nm += L' ';
        nm += static_cast<wchar_t>(L'A' + (inst - 1));   // FUN_002b58f0 indexes value-1
    }
    return nm;
}

bool IsPartySide(void* bc) {
    uint8_t kind = 0xFF;
    return bc && SafeReadU8(bc, BC_KIND, &kind) && kind == 0;
}

Faction FactionOf(void* actor) {
    if (!actor) return Faction::Unknown;
    void* bc = BtlChrForActor(actor);
    if (!bc) return Faction::Neutral;                  // FUN_002f8e90: null BtlChr => 0x10

    uint8_t kind = 0xFF;
    if (!SafeReadU8(bc, BC_KIND, &kind)) return Faction::Unknown;
    if (kind == 0) {
        uint8_t charId = 0;
        SafeReadU8(bc, BC_CHARID, &charId);
        // guests are charId 0x1B..0x27
        return (static_cast<uint8_t>(charId - 0x1B) <= 0x0C) ? Faction::Guest : Faction::Party;
    }
    if (kind == 1) {
        void* so = PtrAt(actor, NavRva::ACTOR_SCENEOBJ);
        uint8_t n = 0;
        if (!so || !SafeReadU8(so, NavRva::SCENEOBJ_KIND_OFF, &n)) return Faction::Neutral;
        n &= 0x0F;
        if (n == 3) return Faction::Ally;
        if (n == 1 || n == 2 || n == 7) return Faction::Foe;
        return Faction::Neutral;
    }
    return Faction::Neutral;
}

// "Is the party actually under attack right now" — FFXII has no in-battle global to read.
//
// WHY THIS SHAPE. combat_system.md section 7.1 records `+0xEA4` at 0.97 as "who has committed an
// action against me", and then STRIKES the obvious use of it — "in battle = any party actor has
// +0xEA4 != 0" — because FUN_0030f760:84-88 sets the bit with NO hostility gate, so an ally's
// out-of-combat Cure trips it. The strike is about the missing filter, not about the field.
//
// Filtering by faction removes exactly that false positive and needs nothing new: FactionOf is
// already a shipped read-only reimplementation of FUN_002f8e90. The doc's suggested replacement
// `*(u32*)(actor+4) & 0x100000` sits at 0.90 with a "may lag the engage edge" caveat and is
// deliberately NOT used here.
//
// Two passes over a <=40-entry pool, no allocation, no game calls. Cheap enough for the audio
// beacon to ask once per field frame.
bool PartyEngaged() {
    void* pool = PtrAt(Hooks::ResolveRva(NavRva::ACTOR_POOL_BASE), 0);
    if (!pool) return false;
    uint32_t count = 0;
    SafeReadU32(Hooks::ResolveRva(NavRva::ACTOR_POOL_COUNT), 0, &count);
    if (count == 0 || count > 128) return false;

    // Pass 1: which pool-index bits belong to a FOE. Anything above bit 31 cannot be represented in
    // the mask the engine itself uses, so it cannot be an attacker either.
    uint32_t foeMask = 0;
    for (uint32_t i = 0; i < count; ++i) {
        void* actor = static_cast<char*>(pool) + static_cast<size_t>(i) * NavRva::ACTOR_STRIDE;
        if (FactionOf(actor) != Faction::Foe) continue;
        uint8_t idx = 0xFF;
        if (!SafeReadU8(actor, A_POOL_INDEX, &idx) || idx >= 32) continue;
        foeMask |= (1u << idx);
    }
    if (foeMask == 0) return false;

    // Pass 2: is any LIVING party-side actor targeted by one of them? Dead members are skipped —
    // a KO'd character still carries whatever mask it had when it went down.
    for (uint32_t i = 0; i < count; ++i) {
        void* actor = static_cast<char*>(pool) + static_cast<size_t>(i) * NavRva::ACTOR_STRIDE;
        const Faction f = FactionOf(actor);
        if (f != Faction::Party && f != Faction::Guest) continue;
        void* bc = BtlChrForActor(actor);
        int32_t hp = 0;
        if (bc && SafeReadU32(bc, BC_CURHP, reinterpret_cast<uint32_t*>(&hp)) && hp <= 0) continue;
        uint32_t engagedBy = 0;
        if (!SafeReadU32(actor, A_ENGAGED_BY, &engagedBy)) continue;
        if (engagedBy & foeMask) return true;
    }
    return false;
}

Committed CommittedTargetOf(void* actor) {
    Committed out;
    if (!actor) return out;

    // ACTIVE first -- but ONLY when it holds a real ability id and a non-null target. actor+0x714
    // can carry an AI/behaviour opcode (a whole 0x4000+ band was observed live) with target 0, and
    // preferring it blindly would report nothing at all.
    uint16_t aAct = 0xFFFF; uint32_t aTgt = 0; uint8_t phase = 0;
    if (SafeReadU16(actor, A_ACTIVE_ACT, &aAct) &&
        SafeReadU32(actor, A_ACTIVE_TGT, &aTgt) &&
        SafeReadU8(actor, A_PHASE, &phase)) {
        void* row = MasterRecord(RVA_ACTIONTBL, aAct);   // null when aAct is out of the table
        if (aAct != 0xFFFF && row != nullptr && aTgt != 0) {
            out.targetHandle = static_cast<int32_t>(aTgt);
            out.actionId = aAct;
            out.active = true;
            out.valid = true;
            return out;
        }
    }

    // Else the queued commitment. The flag MUST be tested: +0xBA0/+0xBB8 retain their last
    // committed values after it clears (seen live as QUEUED(0) with the fields still populated).
    uint64_t flags = 0; uint16_t qAct = 0; uint32_t qTgt = 0;
    if (MemRead::SafeReadU64(actor, A_FLAGS, &flags) && (flags & A_FLAG_QUEUED) &&
        SafeReadU16(actor, A_QUEUED_ACT, &qAct) &&
        SafeReadU32(actor, A_QUEUED_TGT, &qTgt) && qTgt != 0) {
        out.targetHandle = static_cast<int32_t>(qTgt);
        out.actionId = qAct;
        out.active = false;
        out.valid = true;
    }
    return out;
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

// POD-only half: makes the game call and returns the codec pointer. Split out because __try cannot
// live in a function that needs object unwinding, and Decode below builds a std::wstring.
const uint8_t* DefCodec(uint32_t category, uint32_t id) {
    auto fn = reinterpret_cast<Pfn_ResolveDef>(Hooks::ResolveRva(RVA_RESOLVE_DEF));
    if (!fn) return nullptr;
    __try {
        const uint8_t* rec = fn(category, id);
        if (!rec) return nullptr;
        const uint8_t* src = *reinterpret_cast<const uint8_t* const*>(rec + OFF_DEF_CODEC);
        // Shared-pool strings carry a 2-byte 00 00 variant prefix; without skipping it every one of
        // them decodes empty (S49). Harmless when absent -- a valid string never starts with 0x00.
        return (src && src[0] == 0 && src[1] == 0) ? src + 2 : src;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

std::wstring DefName(uint32_t category, uint32_t id) {
    const uint8_t* codec = DefCodec(category, id);
    if (!codec) return std::wstring();
    std::wstring s = GameText::Decode(codec, 256);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

std::wstring AbilityName(uint16_t actionId) {
    if (actionId == 0xFFFF) return std::wstring();
    // Use the chain the mod ALREADY proves works: this is byte-for-byte the resolver that speaks
    // spell and technick names in the battle menu (confirmed in play, S31). The previous
    // implementation walked a DIFFERENT chain -- MasterRecord(action table) -> row+0x34 -> the
    // shared pool -- and every step of that one goes through Reloc(), which is why the combat log
    // said "attacks" for everything. Do not "restore" the old chain; it was never the working one.
    //
    // This is a GAME CALL, so it is game-thread only. That is already true of every caller:
    // combat_log renders its line text at append time on the game thread, the same discipline
    // item_names.cpp documents for FUN_00272cb0.
    return DefName(CAT_ABILITY, actionId);
}

uint8_t AbilityCategory(uint16_t actionId) {
    if (actionId == 0xFFFF) return 0;
    void* row = MasterRecord(RVA_ACTIONTBL, actionId);
    if (!row) return 0;
    uint8_t cat = 0;
    if (!SafeReadU8(row, 0x1E, &cat)) return 0;
    return cat;
}

std::wstring StatusName(int bitIndex) {
    if (bitIndex < 0 || bitIndex > 31) return std::wstring();
    void* rec = MasterRecord(RVA_STATUSTBL, static_cast<uint32_t>(bitIndex));
    if (!rec) return std::wstring();
    // rec+0x02 == 0xFF marks a status the game suppresses (KO, Invisible, HP Critical, X-Zone).
    uint8_t suppress = 0;
    if (SafeReadU8(rec, 0x02, &suppress) && suppress == 0xFF) return std::wstring();
    uint16_t nameIdx = 0;
    if (!SafeReadU16(rec, 0x00, &nameIdx)) return std::wstring();
    return PoolString(nameIdx);
}

std::wstring StatusNames(uint32_t statusWord) {
    std::wstring out;
    for (int bit = 0; bit < 32; ++bit) {
        if ((statusWord & (1u << bit)) == 0) continue;
        std::wstring nm = StatusName(bit);
        if (nm.empty()) continue;                    // suppressed or unresolvable
        if (!out.empty()) out += L", ";
        out += nm;
    }
    return out;
}

} // namespace BattleState

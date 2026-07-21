#include "battle/battle_state.h"

#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "core/game_text.h"
#include "navigation/nav_rva.h"

namespace BattleState {
namespace {

using MemRead::PtrAt;
using MemRead::SafeReadU8;
using MemRead::SafeReadU16;
using MemRead::SafeReadU32;

// ---- RVAs (abs = RVA + 0x120000) --------------------------------------------------------------
constexpr uint32_t RVA_BTLWORK   = 0x2D9F190;  // DAT_02ebf190 -- POINTER to BtlWork
constexpr uint32_t RVA_RELOC     = 0x1E63530;  // _DAT_01f83530 -- master-data relocation base
constexpr uint32_t RVA_POOL      = 0x2D9F170;  // DAT_02ebf170 -- shared codec string pool (word.bin)
constexpr uint32_t RVA_ACTIONTBL = 0x2D9F138;  // DAT_02ebf138 -- ability/action table
constexpr uint32_t RVA_STATUSTBL = 0x2D9F118;  // DAT_02ebf118 -- battle status-name table

constexpr uint32_t BTLWORK_MAGIC = 0x5071901;  // stamped at W+0x00 by FUN_002370c0
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

// sceneObj fields
constexpr uint32_t SO_INST = 0x100, SO_NAMEKEY = 0x102;

// st2e master-data header: +0x04 count, +0x08 stride, +0x0C -> records.
constexpr uint32_t HDR_COUNT = 0x04, HDR_STRIDE = 0x08, HDR_RECORDS = 0x0C;

// Every "pointer" in master data is a u32 offset needing this relocation (FUN_0020e600).
void* Reloc(uint32_t off) {
    if (off == 0) return nullptr;
    void* base = PtrAt(Hooks::ResolveRva(RVA_RELOC), 0);
    if (!base) return nullptr;
    return static_cast<char*>(base) + off;
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
    void* w = PtrAt(Hooks::ResolveRva(RVA_BTLWORK), 0);      // THE DEREFERENCE
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
    d.globalValue = PtrAt(Hooks::ResolveRva(RVA_BTLWORK), 0);
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

std::wstring AbilityName(uint16_t actionId) {
    if (actionId == 0xFFFF) return std::wstring();
    void* row = MasterRecord(RVA_ACTIONTBL, actionId);
    if (!row) return std::wstring();                 // out of table: an AI opcode, not an ability
    uint16_t nameIdx = 0;
    if (!SafeReadU16(row, 0x34, &nameIdx)) return std::wstring();   // 0x34 = NAME (not 0x00)
    return PoolString(nameIdx);
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

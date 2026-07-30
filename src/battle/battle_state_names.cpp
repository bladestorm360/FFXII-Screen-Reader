#include "battle/battle_state.h"
#include "battle/battle_state_internal.h"

#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "core/game_text.h"

#include <Windows.h>
#include <string>

// Master-data NAME resolution: abilities, statuses, and the DEF-record resolver everything else keys
// off. Split out of battle_state.cpp (Session 93) to bring it back under the 500-line cap -- a distinct
// concern with one dependency (the Internal:: master-data plumbing) and no state.
//
// `BattleState::DefName` is the project's one choke point for game-supplied master-data names; new
// callers extend it with a category rather than resolving records themselves.
namespace BattleState {
namespace {

using MemRead::PtrAt;
using MemRead::SafeReadU8;
using MemRead::SafeReadU16;
using MemRead::SafeReadU32;
using namespace PhyreTypes;

using Internal::PoolString;
using Internal::MasterRecord;
using Internal::RVA_ACTIONTBL;

constexpr uint32_t RVA_STATUSTBL = 0x2D9F118;  // DAT_02ebf118 -- battle status-name table

// DAT_02ebf130 -- the CHARACTER master table, and rec+0x30 is its shared-pool name index. Read off
// FUN_0031c5d0 `case 1`, which is the arm category 2 takes:
//     base   = Reloc(*(u32*)(DAT_02ebf130 + 0xc));
//     row    = base + *(u16*)(DAT_02ebf130 + 8) * id;
//     poolId = *(u16*)(row + 0x30);              // -> DAT_02ebf170, the shared pool
// That header shape (count / stride+8 / records+0xc) is exactly Internal::MasterRecord, and the
// sibling arm for category 3 reads its own table the same way at +0x08 -- so the layout is the
// table's, not this category's. Confidence 0.98.
constexpr uint32_t RVA_CHARTBL      = 0x2D9F130;
constexpr uint32_t OFF_CHAR_NAMEIDX = 0x30;

// FUN_0035d330(category, id) fills a static record and returns it; the localized name is the codec
// pointer at record+0x18. The game's own name path, and it does NOT depend on the Reloc/PoolString
// chain -- which is why an ability name can resolve when a status name does not.
constexpr uint32_t RVA_RESOLVE_DEF = 0x23D330;  // FUN_0035d330(cat, id) -> &record
constexpr uint32_t OFF_DEF_CODEC   = 0x18;      // record+0x18 = name codec source
constexpr uint32_t CAT_ABILITY     = 0x14;      // magicks / technicks / actions
typedef const uint8_t* (*Pfn_ResolveDef)(uint32_t, uint32_t);

} // namespace

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

// The pure-read twin of DefName(0x02, id) -- see battle_state.h for WHY it has to be pure. Same two
// helpers StatusName uses, so there is no third master-data walk in the codebase.
std::wstring CharacterName(uint8_t charId) {
    void* rec = MasterRecord(RVA_CHARTBL, charId);
    if (!rec) return std::wstring();          // MasterRecord's count guard already rejected the id
    uint16_t nameIdx = 0;
    if (!SafeReadU16(rec, OFF_CHAR_NAMEIDX, &nameIdx)) return std::wstring();
    return PoolString(nameIdx);
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

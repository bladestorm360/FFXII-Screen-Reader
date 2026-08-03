#include "battle/battle_state.h"
#include "battle/battle_state_internal.h"

#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "core/game_text.h"

#include <Windows.h>
#include <cstdio>
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

// row+0x13 is the ELEMENT mask. Same table and same pure-read technique AbilityCategory uses at
// row+0x1E, so this adds no new chain -- just a second offset on a walk that already ships.
//
// Derived offline against the shipped action_data.bin (543 rows, stride 60), confidence 0.99:
//   * 15/15 named elemental spells match, whole families included -- Fire/Fira/Firaga 0x01,
//     Thunder/Thundara/Thundaga 0x02, Blizzard/Blizzara/Blizzaga 0x04, Aero/Aeroga 0x20,
//     Holy 0x40, Dark/Darkra/Darkga 0x80.
//   * Correct negatives: Cure, Shock, Scathe and Bio all read 0x00.
//   * Across ALL 543 rows the byte only ever takes {0, and the eight single-bit values} -- never a
//     2-bit combination. 447 non-elemental, then 19/11/10/8/13/13/9/13 across the eight elements.
//     A byte that meant something else would not distribute that way.
//
// That last property is also the runtime self-check: a multi-bit mask means this offset is wrong,
// so ElementNames logs one and the caller is expected to treat the annotation as unproven.
uint8_t AbilityElements(uint16_t actionId) {
    if (actionId == 0xFFFF) return 0;
    void* row = MasterRecord(RVA_ACTIONTBL, actionId);
    if (!row) return 0;
    uint8_t mask = 0;
    if (!SafeReadU8(row, 0x13, &mask)) return 0;
    return mask;
}

std::wstring StatusName(int bitIndex, bool includeSuppressed) {
    if (bitIndex < 0 || bitIndex > 31) return std::wstring();
    void* rec = MasterRecord(RVA_STATUSTBL, static_cast<uint32_t>(bitIndex));
    if (!rec) return std::wstring();
    // rec+0x02 == 0xFF marks a status the game suppresses (KO, Invisible, HP Critical, X-Zone).
    uint8_t suppress = 0;
    if (!includeSuppressed && SafeReadU8(rec, 0x02, &suppress) && suppress == 0xFF)
        return std::wstring();
    uint16_t nameIdx = 0;
    if (!SafeReadU16(rec, 0x00, &nameIdx)) return std::wstring();
    return PoolString(nameIdx);
}

// The eight elements live in the SAME shared pool StatusName reads, at chunk 4 index 23+bit --
// i.e. pool index (4 << 11) | (23 + bit) = 0x2017 + bit.
//
// That is the game's OWN binding, not an inference. `attribute_data.bin` is an st2e table of
// exactly 8 records x u16, and its eight values are literally 0x2017..0x201E in order. Decoding
// them out of the shipped word.bin gives Fire, Lightning, Ice, Earth, Water, Wind, Holy, Dark --
// the same order the description panel's own bit loop walks (FUN_00293ce0:41 emits
// FUN_002f9860(0x4B27 + bit)), and the same order the game's prose confirms in help_action.bin
// ("Deal <fire sprite> fire damage to one foe.", once per element). Confidence 0.99.
//
// Nothing here is hardcoded English: an unresolvable index yields an empty string and the caller
// says nothing, which is the correct degradation.
std::wstring ElementName(int bitIndex) {
    if (bitIndex < 0 || bitIndex > 7) return std::wstring();

    // Re-entrancy guard. This is the resolver GameText calls while decoding, and it decodes a pool
    // string to answer -- so a pool string that itself carried a sprite escape would come back
    // through here. Element names are plain text so it would terminate anyway, but the guard makes
    // that a fact rather than a property of the data.
    static thread_local bool s_resolving = false;
    if (s_resolving) return std::wstring();
    s_resolving = true;
    std::wstring name = PoolString(static_cast<uint16_t>(0x2017 + bitIndex));
    s_resolving = false;

    // One-shot census. This is what stands in for a confirmation probe: if the pool binding is
    // wrong the eight names come back blank or scrambled and it says so once, in the log, instead
    // of silently mis-labelling every elemental item in the game. Written on the first resolve
    // because the pool is not populated at Init.
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;                       // set first: the loop below re-enters ElementName
        std::wstring all;
        for (int b = 0; b < 8; ++b) {
            wchar_t hdr[8];
            swprintf_s(hdr, L"%s%d=", b ? L" " : L"", b);
            all += hdr;
            std::wstring nm = ElementName(b);
            all += nm.empty() ? L"?" : nm;
        }
        Log::WriteW("BATTLE", "elements", all);
    }
    return name;
}

std::wstring ElementNames(uint8_t elementMask) {
    if (elementMask == 0) return std::wstring();   // non-elemental: add NOTHING

    // Self-check for AbilityElements' offset -- see there. No shipped row has two bits set, so one
    // of these lines means row+0x13 is not the element mask and the annotation cannot be trusted.
    // Log-only and unconditional: this is diagnostics, not speech.
    int bits = 0;
    for (int b = 0; b < 8; ++b) if (elementMask & (1u << b)) ++bits;
    if (bits > 1) {
        char msg[96];
        snprintf(msg, sizeof(msg), "element mask 0x%02X has %d bits set -- row+0x13 suspect",
                 elementMask, bits);
        Log::Write("BATTLE", msg);
    }

    std::wstring out;
    for (int bit = 0; bit < 8; ++bit) {
        if ((elementMask & (1u << bit)) == 0) continue;
        std::wstring nm = ElementName(bit);
        if (nm.empty()) continue;                    // unresolvable: drop it, never guess
        if (!out.empty()) out += L", ";
        out += nm;
    }
    return out;
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

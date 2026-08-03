#include "ui/equip_detail.h"
#include "ui/text_capture.h"

#include "battle/battle_state.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>

namespace {

using MemRead::SafeReadU32;

// FUN_0035d330(category, packedId) -> &DAT_022ca520, the shared 0x98-byte scratch record.
// The id lives in the HIGH half of the second argument: FUN_0031c5d0 case 6 reads it as
// `*(u16*)((char*)param_1 + 6)`, which is why every caller passes `id << 16` (FUN_00292b70:93).
constexpr uint32_t RVA_RESOLVE_DEF = 0x23D330;
constexpr uint32_t CAT_EQUIPMENT   = 6;
typedef const uint8_t* (*Pfn_ResolveDef)(uint32_t, uint32_t);

// The description formatter's category byte. 3 = equipment, before FUN_00292b70 remaps it to a
// per-slot builder index. Anything else has no status masks to expand.
constexpr int DESC_CAT_EQUIPMENT = 3;

constexpr uint32_t REC_IMMUNE = 0x44;   // u32 status mask: ailments this item blocks
constexpr uint32_t REC_EQUIP  = 0x48;   // u32 status mask: statuses it grants while worn

// listhelp_common string ids. The base is 0x2328 and the id is `0x2328 + <index into that file>` --
// verified against the shipped listhelp_common.bin on ALL 18 ids the panel builders use
// (License Needed / On Hit / None / Immune / Absorb / Half Damage / Weak / separator / Equip /
// both "Various status effects" phrases / the five stat labels / Element: None). 18/18, conf 1.00.
constexpr int TXT_SEPARATOR        = 0x2332;  // ", "
constexpr int TXT_IMMUNE           = 0x2333;  // "Immune: "
constexpr int TXT_EQUIP            = 0x2334;  // "Equip: "
constexpr int TXT_IMMUNE_COLLAPSED = 0x2335;  // "Immune: Various status effects"
constexpr int TXT_EQUIP_COLLAPSED  = 0x2336;  // "Equip: Various status effects"

// The game's own threshold: FUN_00293310 enumerates only when fewer than four bits are set.
constexpr int kCollapseAt = 4;

int PopCount(uint32_t v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

const uint8_t* ResolveRecord(uint16_t itemId) {
    auto fn = reinterpret_cast<Pfn_ResolveDef>(Hooks::ResolveRva(RVA_RESOLVE_DEF));
    if (!fn) return nullptr;
    __try {
        return fn(CAT_EQUIPMENT, static_cast<uint32_t>(itemId) << 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// POD-only under __try (the caller builds std::wstrings, which cannot unwind through SEH).
bool ReadDescParams(const void* params, int* category, uint16_t* itemId) {
    if (!params) return false;
    __try {
        const uint32_t* p = static_cast<const uint32_t*>(params);
        *category = static_cast<int>(p[0]);
        *itemId   = static_cast<uint16_t>(p[1] >> 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

namespace EquipDetail {

bool ExpandCollapsedStatuses(const void* descParams, std::wstring& text) {
    int category = 0;
    uint16_t itemId = 0;
    if (!ReadDescParams(descParams, &category, &itemId)) return false;
    if (category != DESC_CAT_EQUIPMENT || itemId == 0xFFFF) return false;

    const uint8_t* rec = ResolveRecord(itemId);
    if (!rec) return false;
    uint32_t immune = 0, granted = 0;
    if (!SafeReadU32(const_cast<uint8_t*>(rec), REC_IMMUNE, &immune)) return false;
    if (!SafeReadU32(const_cast<uint8_t*>(rec), REC_EQUIP,  &granted)) return false;

    // Mirror the game's own choice exactly (FUN_00293310:139-185): a non-zero IMMUNE mask wins and
    // the granted mask is not shown at all; only when immune is empty does the granted one appear.
    // Reproducing that is what keeps the phrase we are about to replace the one actually on screen.
    const bool useImmune = (immune != 0);
    const uint32_t mask  = useImmune ? immune : granted;
    if (mask == 0) return false;

    // A second, SEPARATE gap: when both masks are set the game silently drops the granted one. Not
    // expanded here -- that would invent a row the panel never had. Logged so the decision rests on
    // evidence if it is ever worth adding.
    if (immune != 0 && granted != 0) {
        char hdr[96];
        snprintf(hdr, sizeof(hdr), "item 0x%04X hides its granted-status mask 0x%08X behind immune",
                 itemId, granted);
        Log::Write("DESC", hdr);
    }

    if (PopCount(mask) < kCollapseAt) return false;   // the game already listed them

    const std::wstring needle = TextCapture::ResolveStringById(
        useImmune ? TXT_IMMUNE_COLLAPSED : TXT_EQUIP_COLLAPSED);
    if (needle.empty()) return false;
    const size_t at = text.find(needle);
    if (at == std::wstring::npos) {
        // Our mask says "collapsed" but the phrase is not on screen, so the two disagree and the
        // text is left exactly as the game wrote it. Never patch a string we cannot locate.
        char hdr[96];
        snprintf(hdr, sizeof(hdr), "item 0x%04X mask 0x%08X reads collapsed but the phrase is absent",
                 itemId, mask);
        Log::Write("DESC", hdr);
        return false;
    }

    std::wstring sep = TextCapture::ResolveStringById(TXT_SEPARATOR);
    if (sep.empty()) sep = L", ";                     // the game's separator, with a plain fallback
    std::wstring names;
    for (int bit = 0; bit < 32; ++bit) {
        if ((mask & (1u << bit)) == 0) continue;
        // includeSuppressed: the battle HUD hides KO / Invisible / HP Critical / X-Zone as noise,
        // but an accessory that blocks KO is exactly what a buyer needs to hear, and the game's own
        // panel lists them here (it reads the master name with no suppression check).
        const std::wstring nm = BattleState::StatusName(bit, /*includeSuppressed=*/true);
        if (nm.empty()) continue;
        if (!names.empty()) names += sep;
        names += nm;
    }
    if (names.empty()) return false;                  // nothing resolved -> leave the game's text

    const std::wstring label = TextCapture::ResolveStringById(useImmune ? TXT_IMMUNE : TXT_EQUIP);
    text.replace(at, needle.size(), label + names);
    return true;
}

} // namespace EquipDetail

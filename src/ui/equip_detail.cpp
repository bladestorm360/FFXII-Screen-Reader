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

// Every set bit's name, joined with the game's own separator.
//
// includeSuppressed: the battle HUD hides KO / Invisible / HP Critical / X-Zone as noise, but an
// accessory that blocks KO is exactly what a buyer needs to hear, and the game's own panel lists
// them here (it reads the master name with no suppression check).
std::wstring JoinNames(uint32_t mask) {
    std::wstring sep = TextCapture::ResolveStringById(TXT_SEPARATOR);
    if (sep.empty()) sep = L", ";
    std::wstring out;
    for (int bit = 0; bit < 32; ++bit) {
        if ((mask & (1u << bit)) == 0) continue;
        const std::wstring nm = BattleState::StatusName(bit, /*includeSuppressed=*/true);
        if (nm.empty()) continue;
        if (!out.empty()) out += sep;
        out += nm;
    }
    return out;
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

    if (immune == 0 && granted == 0) return false;
    bool changed = false;

    // WHICH ROW THE GAME DREW. All three equipment builders (FUN_00293310, FUN_00293fe0,
    // FUN_00294b50) share one idiom, checked in all three: `if (immune == 0) { show granted } else
    // { show immune }` — a non-zero IMMUNE mask wins outright and the granted mask is not drawn at
    // all. Reproducing that is what keeps the phrase we replace below the one actually on screen.
    const bool showedImmune  = (immune != 0);
    const uint32_t shownMask = showedImmune ? immune : granted;

    // (1) The row the game DID draw, but collapsed past three entries. Substitute rather than
    // append: the needle is the game's own phrase, so the result is locale-correct by construction
    // and reads "Immune: Sleep, Confuse, ..." instead of saying "Immune" twice.
    if (PopCount(shownMask) >= kCollapseAt) {
        const std::wstring needle = TextCapture::ResolveStringById(
            showedImmune ? TXT_IMMUNE_COLLAPSED : TXT_EQUIP_COLLAPSED);
        const size_t at = needle.empty() ? std::wstring::npos : text.find(needle);
        if (at == std::wstring::npos) {
            // Our mask says "collapsed" but the phrase is not on screen: the two disagree, so the
            // text is left exactly as the game wrote it. Never patch a string you cannot locate.
            char hdr[96];
            snprintf(hdr, sizeof(hdr), "item 0x%04X mask 0x%08X reads collapsed, phrase absent",
                     itemId, shownMask);
            Log::Write("DESC", hdr);
        } else {
            const std::wstring names = JoinNames(shownMask);
            if (!names.empty()) {
                text.replace(at, needle.size(),
                             TextCapture::ResolveStringById(showedImmune ? TXT_IMMUNE : TXT_EQUIP)
                             + names);
                changed = true;
            }
        }
    }

    // (2) The row the game drew NOTHING for. When an item both blocks ailments AND confers statuses
    // while worn, the builder's either/or means the granted list never appears — at any count, not
    // just past the collapse threshold. Both facts matter when deciding what to wear, so the hidden
    // row is restored rather than merely logged.
    //
    // Appended rather than substituted, because there is no phrase on screen to replace. The label
    // and the names are all game text, so nothing here is invented — only un-hidden.
    if (immune != 0 && granted != 0) {
        const std::wstring names = JoinNames(granted);
        const std::wstring label = TextCapture::ResolveStringById(TXT_EQUIP);
        if (!names.empty() && !label.empty()) {
            const std::wstring row = label + names;
            if (text.find(row) == std::wstring::npos) {   // never duplicate a row already present
                if (!text.empty() && text.back() != L'\n') text += L'\n';
                text += row;
                changed = true;
            }
        }
        char hdr[112];
        snprintf(hdr, sizeof(hdr), "item 0x%04X both masks set (immune 0x%08X, equip 0x%08X)%s",
                 itemId, immune, granted, changed ? " -- granted row restored" : "");
        Log::Write("DESC", hdr);
    }

    return changed;
}

} // namespace EquipDetail

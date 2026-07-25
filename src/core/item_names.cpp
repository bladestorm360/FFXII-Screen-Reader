#include "core/item_names.h"

#include "core/game_text.h"
#include "core/hooks.h"

#include <Windows.h>

namespace ItemNames {
namespace {

constexpr uint32_t RVA_RESOLVE_ITEM = 0x152CB0;   // FUN_00272cb0(id) -> item name codec

typedef const uint8_t* (*Pfn_ResolveItem)(uint32_t);

} // namespace

const uint8_t* ResolveCodec(uint32_t itemId) {
    auto fn = reinterpret_cast<Pfn_ResolveItem>(Hooks::ResolveRva(RVA_RESOLVE_ITEM));
    if (!fn) return nullptr;
    __try { return fn(itemId); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

std::wstring Resolve(uint32_t itemId) {
    const uint8_t* codec = ResolveCodec(itemId);
    if (!codec) return std::wstring();
    // The resolver hands back an already-selected string (the same surface the item menu draws), so
    // it needs no SkipVariantPrefix -- that is only for raw shared-pool entries.
    std::wstring s = GameText::Decode(codec, 128);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

} // namespace ItemNames

#include "ui/equip_target_reader.h"
#include "ui/equip_compare.h"

#include "battle/battle_state.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "speech/phrasebook.h"
#include "speech/speech.h"

#include <Windows.h>
#include <cstdio>
#include <string>

namespace {

using MemRead::Obj0;
using MemRead::PtrAt;
using MemRead::SafeReadU16;
using MemRead::SafeReadU32;

constexpr uint32_t RVA_REFRESH  = 0x45B1A0;  // FUN_0057b1a0(win)
constexpr uint32_t RVA_WINCLS   = 0x45AC10;  // FUN_0057ac10 -- the window's class
constexpr uint32_t RVA_WINPTR   = 0x2B897A0; // DAT_02ca97a0 -- live instance
constexpr uint32_t RVA_MENUCTX  = 0x1F7AC30; // DAT_0209ac30 -- pause/menu context

constexpr uint32_t CTX_MEMBER   = 0xDE0;     // menuCtx+0xDE0 = selected member, written by the L/R
                                             // handlers FUN_0027f360 / FUN_0027ed10
constexpr uint32_t CTX_BLOCKS   = 0xAC8;
constexpr uint32_t BLK_CHARID   = 0x60;
constexpr uint32_t W_ARRAY      = 0x60;      // win+0x60 -> the window's widget array
constexpr uint32_t WA_SLOTITEM  = 0x38;      // arr+0x38 -> widget holding the CURRENT slot item
constexpr uint32_t WA_CANNOT    = 0x80;      // arr+0x80 -> the "cannot equip" notice widget
constexpr uint32_t W_TEXT       = 0x18;
constexpr uint32_t W_FLAGS      = 0x08;      // bit 0 = visible
constexpr uint32_t WIN_REMAIN   = 0xC4;      // i16, how many are left to equip

typedef uint64_t (*Pfn_Refresh)(void*);
Pfn_Refresh s_origRefresh = nullptr;

// Validate by CLASS off the window the hook was handed. Deliberately NOT via DAT_02ca97a0: the
// probe caught the very first FUN_0057b1a0 firing while that global was still null, so a reader
// keyed on it would miss the screen's own entry announcement.
bool IsOurWindow(void* win) {
    return win && Obj0(win) == Hooks::ResolveRva(RVA_WINCLS);
}

std::wstring DecodeWidgetText(void* widget) {
    if (!widget) return std::wstring();
    const uint8_t* codec = static_cast<const uint8_t*>(PtrAt(widget, W_TEXT));
    if (!codec) return std::wstring();
    std::wstring s = GameText::Decode(codec, 128);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// ONE emit point for this surface, per the one-choke-point rule.
void Speak(const std::wstring& line, const char* tag) {
    if (line.empty()) return;
    Log::WriteW("EQUIP", tag, line);
    Speech::Output(line, /*interrupt=*/true);
}

// FUN_0057b1a0(win): the screen's refresh. Run the original FIRST -- it is what writes the slot-item
// widget and re-drives the compare panel, so everything below reads the state the player is about
// to see, not the one they just left.
//
// NO dedup and no entry latch. Entry, every Left/Right, and the post-equip refresh all want the
// full line: after equipping, the slot-item read IS the confirmation that it went on.
uint64_t HookedRefresh(void* win) {
    const uint64_t ret = s_origRefresh ? s_origRefresh(win) : 0;
    STALL_SCOPE("EquipTarget::HookedRefresh");
    if (!IsOurWindow(win)) return ret;

    void* ctx = PtrAt(Hooks::ResolveRva(RVA_MENUCTX), 0);
    if (!ctx) return ret;
    uint16_t rawMem = 0;
    if (!SafeReadU16(ctx, CTX_MEMBER, &rawMem)) return ret;
    const int memberIdx = static_cast<int16_t>(rawMem);
    if (memberIdx < 0 || memberIdx > 8) return ret;

    void* block = PtrAt(ctx, CTX_BLOCKS + static_cast<uint32_t>(memberIdx) * 8);
    if (!block) return ret;
    uint16_t rawChar = 0;
    if (!SafeReadU16(block, BLK_CHARID, &rawChar)) return ret;
    const int charId = static_cast<int16_t>(rawChar);
    if (charId < 0) return ret;

    const std::wstring name = BattleState::CharacterName(static_cast<uint8_t>(charId));
    if (name.empty()) return ret;                    // no game-supplied name -> stay silent

    void* arr = PtrAt(win, W_ARRAY);
    // The "cannot equip" notice is a widget the game SHOWS rather than a flag it sets, so its
    // visibility bit is the answer. Cross-checked below against the compare panel's own witness.
    bool cannot = false;
    if (arr) {
        void* w = PtrAt(arr, WA_CANNOT);
        uint32_t flags = 0;
        cannot = w && SafeReadU32(w, W_FLAGS, &flags) && (flags & 1) != 0;
    }

    std::wstring line = name + L": ";
    if (cannot) {
        line += Phrase::Get(Phrase::Id::CannotEquip);
    } else {
        // What they are wearing in that slot right now. FUN_0057b1a0:71-90 writes either the item's
        // name or the game's own empty-slot string into this one widget, so a single read covers
        // both cases and neither is invented.
        const std::wstring slotItem = arr ? DecodeWidgetText(PtrAt(arr, WA_SLOTITEM)) : std::wstring();
        if (!slotItem.empty()) line += slotItem;

        const std::wstring delta = EquipCompare::LineForMember(memberIdx);
        if (!delta.empty()) {
            // LineForMember re-states the name; keep only the stats half so the line does not say
            // the character twice.
            const size_t sep = delta.find(L": ");
            const std::wstring stats = (sep == std::wstring::npos) ? delta : delta.substr(sep + 2);
            if (!stats.empty()) {
                if (line.back() != L' ') line += L", ";
                line += stats;
            }
        }
    }

    // Two witnesses on one line: the screen's own notice widget and the compare panel's colB+0xE4.
    // They are independent reads of the same fact, so a disagreement means one of them is wrong and
    // should be chased rather than trusted.
    const std::wstring panelSays = EquipCompare::LineForMember(memberIdx);
    const bool panelCannot = panelSays.find(Phrase::Get(Phrase::Id::CannotEquip)) != std::wstring::npos;
    if (!panelSays.empty() && panelCannot != cannot) {
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "can-equip DISAGREE mem=%d widget=%d panel=%d",
                 memberIdx, cannot ? 1 : 0, panelCannot ? 1 : 0);
        Log::Write("EQUIP", hdr);
    }

    int16_t remaining = 0;
    if (SafeReadU16(win, WIN_REMAIN, &rawMem)) remaining = static_cast<int16_t>(rawMem);
    char tag[48];
    snprintf(tag, sizeof(tag), "target mem=%d left=%d:", memberIdx, remaining);
    Speak(line, tag);
    return ret;
}

} // namespace

namespace EquipTargetReader {

bool Init() {
    const bool ok = Hooks::InstallTyped(RVA_REFRESH, &HookedRefresh, &s_origRefresh);
    Log::Write("EQUIP", ok ? "equip-target screen hook installed (FUN_0057b1a0)"
                           : "equip-target hook FAILED -- that screen stays silent");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_REFRESH);
    s_origRefresh = nullptr;
}

bool IsActive() {
    void* win = PtrAt(Hooks::ResolveRva(RVA_WINPTR), 0);
    return IsOurWindow(win);
}

} // namespace EquipTargetReader

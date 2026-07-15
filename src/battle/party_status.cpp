#include "party_status.h"

#include "../core/hooks.h"
#include "../core/logger.h"
#include "../core/mem_read.h"
#include "../core/game_text.h"
#include "../speech/speech.h"
#include "../navigation/nav_rva.h"

#include <cstdio>

namespace PartyStatus {
namespace {

// ---- Party roster + BtlChr, read straight from the game's own tables -------------------------
//
// Derived from the seven `btlAtel*FromPartySlot` natives (FUN_0050fd00..FUN_0050ff20), which were
// resolved offline by fingerprint: seven consecutive functions that all funnel through the same
// slot->BtlChr resolver FUN_00320ab0, with address order matching the .dbg declaration order and
// every body doing what its name says. We read what they read rather than calling them (they are
// athena-VM natives: they pop their arg off the VM stack via FUN_00267db0/FUN_00267e10 — which
// aborts on underflow — and push their result through the VM context).
//
// FUN_00320ab0(slot, listId):
//     bVar1 = FUN_0031b9f0(slot, listId);            // roster lookup -> BtlChr index
//     if (0x27 < bVar1) return 0;                    // 0x28 entries; >= 0x28 means empty
//     return DAT_02ebf190 + 8 + bVar1 * 0x1c8;
//
// FUN_0031b9f0 exposes five parallel 9 x u16 roster lists off DAT_02ebf190; all seven natives use
// LIST 3 (+0x5a7e). That matters: list 3 is the UNMASKED copy of the master party, while lists
// 1/2/4 get slots blanked to 0xffff per game mode (list 2 is the HUD-masked one). Using list 3 is
// what makes this work on the field, in battle, and with the HUD hidden alike — no visibility gate.
constexpr uint32_t RVA_BTL_BASE   = 0x2D9F190;  // DAT_02ebf190
constexpr uint32_t OFF_ROSTER_L3  = 0x5a7e;     // list 3: 9 x u16, value = BtlChr index (0xffff empty)
constexpr uint32_t OFF_BC_ARRAY   = 0x08;       // BtlChr array starts here
constexpr uint32_t BC_STRIDE      = 0x1c8;
constexpr uint32_t BC_COUNT       = 0x28;       // 40 entries

// BtlChr fields. NOTE the width asymmetry, confirmed in the natives' own bodies: HP is i32, MP is
// i16. Reading MP as i32 pulls in the neighbouring field as garbage in the high half.
constexpr uint32_t BC_CHARID      = 0x04;   // u8
constexpr uint32_t BC_MAXHP       = 0x24;   // i32   (btlAtelGetHpMaxFromPartySlot)
constexpr uint32_t BC_MAXMP       = 0x28;   // i16   (btlAtelGetMpMaxFromPartySlot)
constexpr uint32_t BC_STATUS_A    = 0x3c;   // u32
constexpr uint32_t BC_CURHP       = 0x48;   // i32   (btlAtelGetHpNowFromPartySlot)
constexpr uint32_t BC_CURMP       = 0x4c;   // i16   (btlAtelGetMpNowFromPartySlot)
constexpr uint32_t BC_STATUS_B    = 0x64;   // u32
// MP-enabled guard: btlAtelGetMpMaxFromPartySlot returns 0 unless BOTH of these have their sign bit
// clear. Same guard appears independently in the HUD builder FUN_00329220 and the MP clamp
// FUN_00300ce0, so it is the game's own "does this character have an MP gauge" test.
constexpr uint32_t BC_MP_GUARD_A  = 0x6c;   // i8
constexpr uint32_t BC_MP_GUARD_B  = 0x7c;   // i8

// Resolve a party slot to its BtlChr. Returns null for an empty slot.
void* BtlChrForSlot(int slot) {
    using MemRead::SafeReadU8;
    if (slot < 0 || slot >= kMaxSlots) return nullptr;
    void* base = Hooks::ResolveRva(RVA_BTL_BASE);
    if (!base) return nullptr;
    // Roster entries are u16, but a valid BtlChr index is < 0x28, so the low byte carries it and the
    // 0xffff empty sentinel shows up as 0xff — which the >= BC_COUNT test rejects either way.
    uint8_t bcIdx = 0xFF;
    if (!SafeReadU8(base, OFF_ROSTER_L3 + static_cast<uint32_t>(slot) * 2, &bcIdx)) return nullptr;
    if (bcIdx >= BC_COUNT) return nullptr;   // empty slot
    return static_cast<char*>(base) + OFF_BC_ARRAY + static_cast<size_t>(bcIdx) * BC_STRIDE;
}

// Combatant name for a BtlChr, by matching the actor pool (actor+0x698 == bc) and reading the
// localized name the game already resolved at actor+0x18. Pure memory reads — deliberately NOT
// FUN_0035d330(2, charId), which writes a shared static (DAT_022ca520) and is not reentrant: these
// hotkeys run on the input thread, so a game call here could race the game thread's own use of it.
std::wstring NameForBtlChr(void* bc) {
    using MemRead::PtrAt; using MemRead::SafeReadU32;
    if (!bc) return std::wstring();
    void* pool = PtrAt(Hooks::ResolveRva(NavRva::ACTOR_POOL_BASE), 0);
    if (!pool) return std::wstring();
    uint32_t count = 0;
    SafeReadU32(Hooks::ResolveRva(NavRva::ACTOR_POOL_COUNT), 0, &count);
    if (count == 0) return std::wstring();
    if (count > 64) count = 64;
    for (uint32_t i = 0; i < count; ++i) {
        void* actor = static_cast<char*>(pool) + static_cast<size_t>(i) * NavRva::ACTOR_STRIDE;
        if (PtrAt(actor, NavRva::ACTOR_DEF_PTR) != bc) continue;
        const uint8_t* codec = reinterpret_cast<const uint8_t*>(PtrAt(actor, NavRva::ACTOR_NAME_STR));
        std::wstring nm = GameText::Decode(codec, 128);
        return GameText::IsMostlyPrintable(nm) ? nm : std::wstring();
    }
    return std::wstring();
}

} // namespace

bool ReadSlot(int slot, SlotVitals& out) {
    using MemRead::SafeReadU8; using MemRead::SafeReadU32; using MemRead::SafeReadS16;
    out = SlotVitals{};

    void* bc = BtlChrForSlot(slot);
    if (!bc) return false;   // empty slot -> caller stays silent

    uint32_t curHP = 0, maxHP = 0, sa = 0, sb = 0;
    int16_t  curMP = 0, maxMP = 0;
    uint8_t  charId = 0;
    if (!SafeReadU32(bc, BC_CURHP, &curHP) || !SafeReadU32(bc, BC_MAXHP, &maxHP)) return false;
    SafeReadU8(bc, BC_CHARID, &charId);
    SafeReadS16(bc, BC_CURMP, &curMP);
    SafeReadS16(bc, BC_MAXMP, &maxMP);
    SafeReadU32(bc, BC_STATUS_A, &sa);
    SafeReadU32(bc, BC_STATUS_B, &sb);

    // The game's own MP-gauge test (see BC_MP_GUARD_*): both guards' sign bits clear.
    uint8_t ga = 0xFF, gb = 0xFF;
    const bool guardsRead = SafeReadU8(bc, BC_MP_GUARD_A, &ga) && SafeReadU8(bc, BC_MP_GUARD_B, &gb);
    const bool mpEnabled  = guardsRead && (static_cast<int8_t>(ga) >= 0) && (static_cast<int8_t>(gb) >= 0);

    out.present = true;
    out.charId  = charId;
    out.curHP   = static_cast<int32_t>(curHP);
    out.maxHP   = static_cast<int32_t>(maxHP);
    out.curMP   = curMP;
    out.maxMP   = maxMP;
    out.haveMP  = mpEnabled && maxMP > 0;
    out.status  = sa | sb;   // the natives' own status word is the OR of both (btlAtelGetNowStatus...)
    out.name    = NameForBtlChr(bc);
    return true;
}

void SpeakSlot(int slot) {
    SlotVitals v;
    if (!ReadSlot(slot, v) || !v.present) {
        char m[64];
        snprintf(m, sizeof(m), "party slot %d: empty (silent)", slot + 1);
        Log::Write("PARTY", m);
        return;   // empty slot speaks nothing
    }

    // "HP"/"MP" are mod-emitted labels (the game draws them as baked gauge art, not readable text).
    std::wstring text = v.name;
    if (!text.empty()) text += L", ";
    text += L"HP " + std::to_wstring(v.curHP) + L" of " + std::to_wstring(v.maxHP);
    if (v.haveMP)
        text += L", MP " + std::to_wstring(v.curMP) + L" of " + std::to_wstring(v.maxMP);

    char utf8[192] = {};
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    char line[256];
    snprintf(line, sizeof(line), "slot %d charId=%u \"%s\"", slot + 1, v.charId, utf8);
    Log::Write("PARTY", line);

    Speech::Output(text, /*interrupt=*/true);
}

} // namespace PartyStatus

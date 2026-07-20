#include "party_status.h"

#include "battle/battle_state.h"
#include "../core/logger.h"
#include "../core/mem_read.h"
#include "../speech/speech.h"

#include <cstdio>

namespace PartyStatus {
namespace {

// BtlChr fields. NOTE the width asymmetry, confirmed in the natives' own bodies: HP is i32, MP is
// i16. Reading MP as i32 pulls in the neighbouring field as garbage in the high half.
constexpr uint32_t BC_CHARID     = 0x04;   // u8
constexpr uint32_t BC_MAXHP      = 0x24;   // i32   (btlAtelGetHpMaxFromPartySlot)
constexpr uint32_t BC_MAXMP      = 0x28;   // i16   (btlAtelGetMpMaxFromPartySlot)
constexpr uint32_t BC_STATUS_A   = 0x3c;   // u32
constexpr uint32_t BC_CURHP      = 0x48;   // i32   (btlAtelGetHpNowFromPartySlot)
constexpr uint32_t BC_CURMP      = 0x4c;   // i16   (btlAtelGetMpNowFromPartySlot)
constexpr uint32_t BC_STATUS_B   = 0x64;   // u32
// MP-enabled guard: btlAtelGetMpMaxFromPartySlot returns 0 unless BOTH of these have their sign bit
// clear. The same guard appears independently in the HUD builder FUN_00329220 and the MP clamp
// FUN_00300ce0, so it is the game's own "does this character have an MP gauge" test.
constexpr uint32_t BC_MP_GUARD_A = 0x6c;   // i8
constexpr uint32_t BC_MP_GUARD_B = 0x7c;   // i8

} // namespace

bool ReadSlot(int slot, SlotVitals& out) {
    using MemRead::SafeReadU8; using MemRead::SafeReadU32; using MemRead::SafeReadS16;
    out = SlotVitals{};

    // BattleState::BtlChrForSlot dereferences DAT_02ebf190 and validates the engine's own magic.
    // The previous code treated that global as the struct itself, so every roster read landed in
    // unrelated memory, bcIdx came back >= 0x28, and this function returned silently -- the whole
    // reason 4/5/6 did nothing.
    void* bc = BattleState::BtlChrForSlot(slot);
    if (!bc) return false;

    uint32_t curHP = 0, maxHP = 0, sa = 0, sb = 0;
    int16_t  curMP = 0, maxMP = 0;
    uint8_t  charId = 0;
    if (!SafeReadU32(bc, BC_CURHP, &curHP) || !SafeReadU32(bc, BC_MAXHP, &maxHP)) return false;
    SafeReadU8(bc, BC_CHARID, &charId);
    SafeReadS16(bc, BC_CURMP, &curMP);
    SafeReadS16(bc, BC_MAXMP, &maxMP);
    SafeReadU32(bc, BC_STATUS_A, &sa);
    SafeReadU32(bc, BC_STATUS_B, &sb);

    uint8_t ga = 0xFF, gb = 0xFF;
    const bool guardsRead = SafeReadU8(bc, BC_MP_GUARD_A, &ga) && SafeReadU8(bc, BC_MP_GUARD_B, &gb);
    const bool mpEnabled  = guardsRead && (static_cast<int8_t>(ga) >= 0) && (static_cast<int8_t>(gb) >= 0);

    out.present     = true;
    out.charId      = charId;
    out.curHP       = static_cast<int32_t>(curHP);
    out.maxHP       = static_cast<int32_t>(maxHP);
    out.curMP       = curMP;
    out.maxMP       = maxMP;
    out.haveMP      = mpEnabled && maxMP > 0;
    out.status      = sa | sb;   // the natives' own status word is the OR of both
    out.name        = BattleState::NameForBtlChr(bc);
    out.statusNames = BattleState::StatusNames(out.status);
    return true;
}

void SpeakSlot(int slot) {
    SlotVitals v;
    if (!ReadSlot(slot, v) || !v.present) {
        // SILENT when the slot is empty or unavailable (user decision, 2026-07-20). combat_system.md
        // §8.2 argued for speaking "Empty slot" so a broken mod could not masquerade as an empty
        // slot -- but that reasoning came from a session where the mod WAS broken, and announcing
        // an empty guest slot on every press is just noise. The diagnostic now lives in the log
        // instead, and PARTY is in the logger's flush list so it survives a hard exit.
        const BattleState::SlotDiag d = BattleState::DiagnoseSlot(slot);
        char m[240];
        snprintf(m, sizeof(m),
                 "party slot %d: SILENT. W=%p magic=0x%08X(%s) rosterEntry=0x%04X(%s) [%s]",
                 slot + 1, d.globalValue, d.magic, d.magicOk ? "ok" : "BAD",
                 d.rosterEntry, d.rosterRead ? "read" : "unreadable",
                 !d.globalValue ? "battle-work subsystem not up"
                 : !d.magicOk   ? "MAGIC MISMATCH - wrong base or not initialized"
                 : (d.rosterEntry >= 0x28) ? "slot genuinely empty"
                                           : "roster ok but BtlChr rejected");
        Log::Write("PARTY", m);
        return;
    }

    // ORDER: name, STATUSES, HP, MP.
    // Statuses sit directly after the name so they are reachable the instant the line starts
    // speaking -- in real-time combat you need to know you are poisoned or stopped before you need
    // the exact HP figure, and waiting through two numbers to hear it is too slow (user, 2026-07-20).
    //
    // "HP"/"MP" are mod-emitted labels: the game draws those as baked gauge art, so there is no game
    // text to read. The status NAMES are the game's own, from its battle status table.
    std::wstring text = v.name;
    if (!v.statusNames.empty()) {
        if (!text.empty()) text += L", ";
        text += v.statusNames;
    }
    if (!text.empty()) text += L", ";
    text += L"HP " + std::to_wstring(v.curHP) + L"/" + std::to_wstring(v.maxHP);
    if (v.haveMP)
        text += L", MP " + std::to_wstring(v.curMP) + L"/" + std::to_wstring(v.maxMP);

    char utf8[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    char line[320];
    snprintf(line, sizeof(line), "slot %d charId=%u status=0x%08X \"%s\"",
             slot + 1, v.charId, v.status, utf8);
    Log::Write("PARTY", line);

    Speech::Output(text, /*interrupt=*/true);
}

} // namespace PartyStatus

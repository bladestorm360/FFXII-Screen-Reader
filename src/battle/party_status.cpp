#include "party_status.h"

#include "battle/battle_state.h"
#include "../core/logger.h"
#include "../core/mem_read.h"
#include "../core/phyre_types.h"
#include "../speech/speech.h"
#include "../speech/phrasebook.h"

#include <cstdio>

namespace PartyStatus {
namespace {

// BtlChr field offsets (widths, the MP guard and their provenance): core/phyre_types.h.
using namespace PhyreTypes;

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
    text += Phrase::Get(Phrase::Id::HPPrefix) + std::to_wstring(v.curHP) + L"/" + std::to_wstring(v.maxHP);
    if (v.haveMP)
        text += std::wstring(L", ") + Phrase::Get(Phrase::Id::MPPrefix) + std::to_wstring(v.curMP) + L"/" + std::to_wstring(v.maxMP);

    char utf8[256];
    Log::ToUtf8(text, utf8, sizeof(utf8));
    char line[320];
    snprintf(line, sizeof(line), "slot %d charId=%u status=0x%08X \"%s\"",
             slot + 1, v.charId, v.status, utf8);
    Log::Write("PARTY", line);

    Speech::Output(text, /*interrupt=*/true);
}

} // namespace PartyStatus

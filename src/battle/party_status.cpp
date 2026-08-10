#include "party_status.h"

#include "battle/battle_state.h"
#include "battle/battle_state_diag.h"
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

bool ReadBtlChr(void* bc, SlotVitals& out) {
    using MemRead::SafeReadU8; using MemRead::SafeReadU32; using MemRead::SafeReadS16;
    out = SlotVitals{};
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
    // BOTH numbers through the game's own clamp, or the line reads as nonsense: a bubbled character
    // whose CURRENT was capped but whose MAX was not would speak "9999 of 7319", and Belias would
    // have said "12786 of 9999". The screen shows 9999/9999 for the Esper and 9999/7319 for Basch --
    // one rule, applied to both halves, reproduces every row of the party screen exactly.
    out.rawCurHP    = static_cast<int32_t>(curHP);
    out.rawMaxHP    = static_cast<int32_t>(maxHP);
    out.curHP       = BattleState::DisplayHp(bc, out.rawCurHP);
    out.maxHP       = BattleState::DisplayHp(bc, out.rawMaxHP);
    out.curMP       = curMP;
    out.maxMP       = maxMP;
    out.haveMP      = mpEnabled && maxMP > 0;
    out.status      = sa | sb;   // the natives' own status word is the OR of both
    // NAME BY CHAR ID, not by actor. NameForBtlChr resolves through ActorForBtlChr, which scans the
    // FIELD ACTOR POOL for an actor whose def pointer is this BtlChr -- and only the LEADER has one.
    // So every non-leader slot came back nameless while HP/MP/status, which are read straight off the
    // BtlChr a few lines up, were always fine. That is exactly the reported defect: "only reads the
    // status effects and the vitals", and it surfaced on an explicit party swap because a swap is what
    // changes which BtlChr sits in each roster slot.
    //
    // A reader that resolves a ROSTER member through the actor pool is broken by construction, not
    // intermittently -- the one slot that worked hid it.
    out.name = BattleState::CharacterName(charId);
    if (out.name.empty()) {
        // A guest may not be in the character master table. The actor path is still the right answer
        // for anyone the field has actually spawned, so keep it as the fallback rather than a guess --
        // and if both are empty the line simply starts with the statuses, which already worked.
        out.name = BattleState::NameForBtlChr(bc);
    }
    out.statusNames = BattleState::StatusNames(out.status);
    return true;
}

bool ReadSlot(int slot, SlotVitals& out) {
    // BattleState::BtlChrForSlot dereferences DAT_02ebf190 and validates the engine's own magic.
    // The previous code treated that global as the struct itself, so every roster read landed in
    // unrelated memory, bcIdx came back >= 0x28, and this function returned silently -- the whole
    // reason 4/5/6 did nothing.
    return ReadBtlChr(BattleState::BtlChrForSlot(slot), out);
}

namespace {

// THE ONE PLACE THE LINE IS WORDED. Two callers reach it -- a roster slot and the summoned Esper --
// and they must not drift apart: a second copy of this would be a second wording, a second interrupt
// policy and a second log format for what the player hears as the same readout.
//
// ORDER: name, STATUSES, HP, MP, gauge.
// Statuses sit directly after the name so they are reachable the instant the line starts speaking --
// in real-time combat you need to know you are poisoned or stopped before you need the exact HP
// figure, and waiting through two numbers to hear it is too slow (user, 2026-07-20).
//
// "HP"/"MP" are mod-emitted labels: the game draws those as baked gauge art, so there is no game
// text to read. The status NAMES are the game's own, from its battle status table.
void SpeakVitals(const SlotVitals& v, const char* what) {
    std::wstring text = v.name;
    if (!v.statusNames.empty()) {
        if (!text.empty()) text += L", ";
        text += v.statusNames;
    }
    if (!text.empty()) text += L", ";
    text += Phrase::Get(Phrase::Id::HPPrefix) + std::to_wstring(v.curHP) + L"/" + std::to_wstring(v.maxHP);
    if (v.haveMP)
        text += std::wstring(L", ") + Phrase::Get(Phrase::Id::MPPrefix) + std::to_wstring(v.curMP) + L"/" + std::to_wstring(v.maxMP);
    // The gauge is spoken as whole pips, which is what the HUD draws; the raw floats go to the log
    // below so the fraction is still recoverable when the unit question gets settled.
    if (v.haveGauge && v.maxGauge > 0.0f)
        text += std::wstring(L", ") + Phrase::Get(Phrase::Id::SummonGauge) +
                std::to_wstring(static_cast<int>(v.curGauge)) + Phrase::Get(Phrase::Id::OfJoiner) +
                std::to_wstring(static_cast<int>(v.maxGauge));

    char utf8[256];
    Log::ToUtf8(text, utf8, sizeof(utf8));
    // Raw HP goes in ONLY when the clamp actually moved something, so the normal line stays short and
    // a clamped one is greppable. Anything here that is not a party-side value over 9999 is a bug in
    // DisplayHp's party-side test, not in the reading.
    char rawNote[48] = "";
    if (v.rawCurHP != v.curHP || v.rawMaxHP != v.maxHP)
        snprintf(rawNote, sizeof(rawNote), " rawHP=%d/%d", v.rawCurHP, v.rawMaxHP);
    char line[416];
    if (v.haveGauge)
        snprintf(line, sizeof(line), "%s charId=%u status=0x%08X gauge=%.2f/%.2f%s \"%s\"",
                 what, v.charId, v.status, v.curGauge, v.maxGauge, rawNote, utf8);
    else
        snprintf(line, sizeof(line), "%s charId=%u status=0x%08X%s \"%s\"",
                 what, v.charId, v.status, rawNote, utf8);
    Log::Write("PARTY", line);

    Speech::Output(text, /*interrupt=*/true);
}

} // namespace

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

    char what[24];
    snprintf(what, sizeof(what), "slot %d", slot + 1);
    SpeakVitals(v, what);
}

void SpeakEsper() {
    void* bc = BattleState::EsperBtlChr();
    SlotVitals v;
    if (!bc || !ReadBtlChr(bc, v) || !v.present) {
        // SILENT, and for the same reason key 7 is silent with no guest: there is nothing to report.
        // Nearly every press of `8` outside a summon lands here, so anything spoken would be noise.
        //
        // The log still has to distinguish the three ways this can happen, because two of them are
        // bugs and one is the normal case: no battle-work subsystem, no Esper out (the summon-mode
        // bit is clear, which is the normal case), or a BtlChr that resolved but would not read.
        void* w = BattleState::Work();
        char m[200];
        snprintf(m, sizeof(m), "esper: SILENT. W=%p bc=%p [%s]", w, bc,
                 !w  ? "battle-work subsystem not up"
                 : !bc ? "no Esper summoned (summon-mode bit clear, or index out of range)"
                       : "BtlChr resolved but its HP pair would not read");
        Log::Write("PARTY", m);
        return;
    }

    // The gauge is the one thing an Esper has and a party member does not. Absent or unreadable, the
    // clause is simply left off -- the HP figure is the part the player asked for.
    v.haveGauge = BattleState::EsperGauge(&v.curGauge, &v.maxGauge);

    SpeakVitals(v, "esper");
}

} // namespace PartyStatus

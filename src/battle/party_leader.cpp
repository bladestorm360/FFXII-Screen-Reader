#include "battle/party_leader.h"

#include "battle/battle_state.h"
#include "battle/party_status.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/stall_probe.h"
#include "navigation/player_state.h"
#include "speech/phrasebook.h"
#include "speech/speech.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

// FUN_00358bc0 -- the leader-handle commit. `void(void)`: no parameters in the decompile, and the
// bytes agree (its prologue reads the stage flag and no argument register). abs 0x358BC0
// - 0x120000 = RVA 0x238BC0, beside FUN_003590d0 at 0x2390D0 (the leader accessor, same page).
constexpr uint32_t RVA_LEADER_COMMIT = 0x238BC0;

typedef void (*Pfn_Commit)();
Pfn_Commit s_origCommit = nullptr;
bool       g_initialized = false;

// The last leader's character id. Game thread only -- the commit is its only reader and writer.
constexpr int kNoChar = -1;
int g_lastCharId = kNoChar;

void OnLeaderChanged(uint32_t before, uint32_t after) {
    STALL_SCOPE("PartyLeader::Changed");
    char m[192];

    // handle -> scene object -> component (the actor) -> BtlChr, and the party readout's own reader
    // for the id and the name, so this line names a character exactly as keys 4-7 do.
    PartyStatus::SlotVitals v;
    void* actor = PlayerState::ComponentForHandle(after);
    void* bc    = actor ? BattleState::BtlChrForActor(actor) : nullptr;
    if (!bc || !PartyStatus::ReadBtlChr(bc, v)) {
        snprintf(m, sizeof(m), "leader handle 0x%X -> 0x%X, new leader did not resolve "
                 "(actor=%p bc=%p) -- silent", before, after, actor, bc);
        Log::Write("PARTY", m);
        g_lastCharId = kNoChar;
        return;
    }

    const int prevCharId = g_lastCharId;
    g_lastCharId = v.charId;
    if (before == 0) {
        snprintf(m, sizeof(m), "leader set from handle 0 (a spawn, not a change): 0x%X charId %u -- "
                 "silent", after, v.charId);
        Log::Write("PARTY", m);
        return;
    }
    if (prevCharId == v.charId) {
        snprintf(m, sizeof(m), "leader handle 0x%X -> 0x%X, same character (charId %u) -- silent",
                 before, after, v.charId);
        Log::Write("PARTY", m);
        return;
    }
    if (v.name.empty()) {
        snprintf(m, sizeof(m), "leader changed to charId %u (handle 0x%X) but it has no name -- silent",
                 v.charId, after);
        Log::Write("PARTY", m);
        return;
    }

    const std::wstring line = v.name + L", " + Phrase::Get(Phrase::Id::PartyLeader);
    snprintf(m, sizeof(m), "leader changed: handle 0x%X -> 0x%X, charId %d -> %u: ",
             before, after, prevCharId, v.charId);
    Log::WriteW("PARTY", m, line);
    Speech::Output(line, /*interrupt=*/true);
}

void HookedCommit() {
    // Every frame. Two reads of one global and nothing else unless the handle moved.
    const uint32_t before = PlayerState::ReadLeaderHandle();
    if (s_origCommit) s_origCommit();
    const uint32_t after = PlayerState::ReadLeaderHandle();
    if (after != before && after != 0) OnLeaderChanged(before, after);
}

} // namespace

namespace PartyLeader {

bool Init() {
    if (g_initialized) return true;
    const bool ok = Hooks::InstallTyped(RVA_LEADER_COMMIT, &HookedCommit, &s_origCommit);
    g_initialized = ok;
    Log::Write("PARTY", ok
        ? "PartyLeader initialized (leader-handle commit FUN_00358bc0: speaks '<name>, leader')"
        : "PartyLeader: hook failed to install -- see Hooks log. Leader changes are silent.");
    return ok;
}

void Shutdown() {
    if (!g_initialized) return;
    Hooks::Uninstall(RVA_LEADER_COMMIT);
    g_initialized = false;
}

} // namespace PartyLeader

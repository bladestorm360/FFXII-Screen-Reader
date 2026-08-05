#include "navigation/shout_fill.h"

#include <cstdio>

#include "core/logger.h"

namespace ShoutFill {
namespace {

// Game-thread only (the gauge detour and the teardown hook both run there), so plain state.
bool s_filled       = false;   // the once-per-visit latch: set only by a SUCCESSFUL write
int  s_attemptsLogged = 0;     // console/log budget: the loop can run 30+ times a burst

constexpr int kMaxAttemptLogs = 6;

} // namespace

void TryInstantFill(const ShoutScript::Module& mod, int preSet, int newValue) {
    if (s_filled || !mod.valid || !mod.row) return;

    void*    addr     = nullptr;
    uint8_t  elemType = 0xFF;
    uint32_t desc     = 0;
    const bool resolved = ShoutScript::VarAddress(mod, mod.row->meterVarIdx, &addr, &elemType, &desc);

    int32_t storage = 0;
    const bool read = resolved && ShoutScript::ReadVar(addr, elemType, &storage);

    // THE FALSIFIER. `storage == newValue - 1` is what the bytecode guarantees while the increment
    // loop is running (see shout_fill.h); the range test refuses a meter that is already at or past
    // the goal, so a late burst cannot re-trigger the write.
    const int  goal      = static_cast<int>(mod.row->fillValue);
    const bool relation  = read && (storage == newValue - 1);
    const bool inRange   = read && (storage >= 0) && (storage < goal);
    const bool ok        = resolved && read && relation && inRange;

    if (ok) {
        const bool wrote = ShoutScript::WriteVar(addr, elemType, goal);
        s_filled = wrote;
        char m[288];
        snprintf(m, sizeof(m),
                 "%s %s var 0x%02X -> %d  (addr=%p type=%u desc=0x%08X, storage was %d, gauge "
                 "pre=%d new=%d). The game's own loop now clamps and runs its success branch.",
                 wrote ? "WROTE" : "WRITE FAULTED -- minigame plays vanilla;",
                 mod.srcName, mod.row->meterVarIdx, goal, addr, elemType, desc, storage,
                 preSet, newValue);
        Log::Write("SHOUT-FILL", m);
        return;
    }

    // Declined. Logged with every operand so a wrong expectation is a one-line fix next build --
    // and capped, because the burst that reaches here does so once per frame for thirty frames.
    if (s_attemptsLogged < kMaxAttemptLogs) {
        ++s_attemptsLogged;
        char m[320];
        snprintf(m, sizeof(m),
                 "DECLINED on %s: resolved=%d read=%d storage=%d expected=%d (gauge pre=%d new=%d) "
                 "relation=%d inRange=%d goal=%d var=0x%02X type=%u desc=0x%08X addr=%p -- nothing "
                 "written, minigame plays vanilla",
                 mod.srcName, resolved ? 1 : 0, read ? 1 : 0, storage, newValue - 1, preSet,
                 newValue, relation ? 1 : 0, inRange ? 1 : 0, goal, mod.row->meterVarIdx,
                 elemType, desc, addr);
        Log::Write("SHOUT-FILL", m);
    }
}

void OnMapTeardown() {
    s_filled = false;
    s_attemptsLogged = 0;
}

} // namespace ShoutFill

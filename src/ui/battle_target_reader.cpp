#include "ui/battle_target_reader.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "speech/speech.h"
#include "core/logger.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

// Battle target-selection readout.
//
// Confirmed model (three decompile traces + Frida): target selection is a SEPARATE object on the
// battle-HUD context DAT_0209be80 (RVA 0x1F7BE80). The highlighted target's HANDLE is
//   P = *(void**)DAT_0209be80 ; handle = *(int)(P + 0x9FD8)     (nameplate/target-info manager)
// active while *(P + 0x10f78) != 0 (the single-target selector; absent during command navigation).
//
// The nameplate builder FUN_002bfd20 (0x19FD20) renders that target and, in the same call, resolves
// the handle to the real BtlChr and passes it to the vitals builder FUN_00329220 (0x209220). Rather
// than re-decode the handle (FUN_003588b0 was unreliable to call directly), we mark "this render is
// the current target" while FUN_002bfd20 runs, then capture the real BtlChr from the nested
// FUN_00329220 call -- the same object that resolves to the correct name + real HP via the actor
// pool (proven in the probe). Both hooks run on the render/game thread (synchronous, single-thread),
// so the flag needs no locking. Ally-vs-enemy uses the mod's confirmed faction test (scene-kind
// nibble), NOT a nameplate flag (panel+0x280 & 2 mis-classified allies).

namespace {

using MemRead::PtrAt;
using MemRead::SafeReadU8;
using MemRead::SafeReadU32;

// ---- offline-derived RVAs / offsets (abs = RVA + 0x120000) --------------------------------------
constexpr uint32_t RVA_NAMEPLATE = 0x19FD20;  // FUN_002bfd20(panel, flag) -- target/nameplate render
constexpr uint32_t RVA_SNAPSHOT  = 0x209220;  // FUN_00329220(BtlChr, ...) -- vitals builder (nested)
constexpr uint32_t RVA_BSTATE    = 0x1F7BE80;  // DAT_0209be80 (ptr) -> P (battle-HUD context)

constexpr uint32_t OFF_TARGETID  = 0x9FD8;    // *(int)(P+0x9FD8) = highlighted target handle
constexpr uint32_t OFF_GATE      = 0x10F78;   // *(P+0x10f78) != 0 = target selection active
constexpr uint32_t OFF_PANEL_ID  = 0x288;     // *(u32)(panel+0x288) = the unit this nameplate draws

// actor pool -> combatant name + faction (mirrors src/navigation/nav_rva.h).
constexpr uint32_t ACTOR_POOL_BASE  = 0x1F6E688;
constexpr uint32_t ACTOR_POOL_COUNT = 0x1F6E6A0;
constexpr uint32_t ACTOR_STRIDE     = 0xF50;
constexpr uint32_t ACTOR_DEF_PTR    = 0x698;   // *(actor+0x698) = BtlChr
constexpr uint32_t ACTOR_NAME_STR   = 0x18;    // *(actor+0x18) = name codec
constexpr uint32_t ACTOR_SCENEOBJ   = 0x10;    // *(actor+0x10) = scene object
constexpr uint32_t SCENEOBJ_KIND    = 0x0E;    // *(u8)(sceneObj+0x0e) & 0x0f: 3 = ally, {1,2,7} = enemy
constexpr uint8_t  KIND_MASK        = 0x0F;
constexpr uint8_t  KIND_ALLY        = 3;

constexpr uint32_t BC_CURHP = 0x48;            // real current HP (confirmed vs Reks 135)
constexpr uint32_t BC_MAXHP = 0x24;            // real max HP

typedef void  (*Pfn_Nameplate)(void*, int);
typedef void* (*Pfn_Snapshot)(void*, int, void*, int);

Pfn_Nameplate s_origNameplate = nullptr;
Pfn_Snapshot  s_origSnapshot  = nullptr;

// Set by FUN_002bfd20 while it renders the CURRENT target; consumed by the nested FUN_00329220.
bool     g_wantTargetBc = false;
int32_t  g_targetHandle = 0;
int32_t  g_lastHandle   = 0;   // dedup: announce once per target change; reset when selection ends

void* Pstate() { return PtrAt(Hooks::ResolveRva(RVA_BSTATE), 0); }

// Combatant name (actor+0x18) + faction (scene-kind nibble) from the actor pool, matching the BtlChr
// via *(actor+0x698)==bc. Returns the name (empty if not found / unprintable); *ally set from kind.
std::wstring NameForBtlChr(void* bc, bool* ally) {
    *ally = false;
    if (!bc) return std::wstring();
    void* base = PtrAt(Hooks::ResolveRva(ACTOR_POOL_BASE), 0);
    if (!base) return std::wstring();
    uint32_t count = 0;
    SafeReadU32(Hooks::ResolveRva(ACTOR_POOL_COUNT), 0, &count);
    if (count == 0) return std::wstring();
    if (count > 64) count = 64;
    for (uint32_t i = 0; i < count; ++i) {
        void* actor = reinterpret_cast<char*>(base) + static_cast<size_t>(i) * ACTOR_STRIDE;
        if (PtrAt(actor, ACTOR_DEF_PTR) != bc) continue;
        const uint8_t* codec = reinterpret_cast<const uint8_t*>(PtrAt(actor, ACTOR_NAME_STR));
        std::wstring nm = GameText::Decode(codec, 128);
        if (!GameText::IsMostlyPrintable(nm)) return std::wstring();
        uint8_t kind = 0xFF;
        SafeReadU8(PtrAt(actor, ACTOR_SCENEOBJ), SCENEOBJ_KIND, &kind);
        *ally = ((kind & KIND_MASK) == KIND_ALLY);
        return nm;
    }
    return std::wstring();
}

// Speak the current target. Enemy = HP percentage (mirrors the gauge; no MP/numbers pre-Libra),
// ally = HP numbers. Real HP off the BtlChr (bc+0x48/0x24). Silent if the name can't resolve.
void AnnounceTargetBc(void* bc) {
    bool ally = false;
    std::wstring name = NameForBtlChr(bc, &ally);
    if (name.empty()) return;

    uint32_t curHPu = 0, maxHPu = 0;
    SafeReadU32(bc, BC_CURHP, &curHPu);
    SafeReadU32(bc, BC_MAXHP, &maxHPu);
    int32_t curHP = static_cast<int32_t>(curHPu), maxHP = static_cast<int32_t>(maxHPu);

    std::wstring text = name;
    if (maxHP > 0) {
        if (ally) {
            text += L", HP " + std::to_wstring(curHP) + L" of " + std::to_wstring(maxHP);
        } else {
            int pct = static_cast<int>(static_cast<long long>(curHP) * 100 / maxHP);
            text += L", HP " + std::to_wstring(pct) + L" percent";
        }
    }

    char utf8[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    char line[320];
    snprintf(line, sizeof(line), "handle=0x%x %s \"%s\"", g_targetHandle, ally ? "ally" : "enemy", utf8);
    Log::Write("TARGET", line);

    Speech::Output(text, /*interrupt=*/true);
}

// FUN_002bfd20 render. If this call is drawing the CURRENT target (panel+0x288 == P+0x9FD8) while
// target selection is active, flag it so the nested FUN_00329220 grabs the real BtlChr.
void HookedNameplate(void* panel, int flag) {
    g_wantTargetBc = false;
    void* P = Pstate();
    if (P) {
        if (!PtrAt(P, OFF_GATE)) {
            g_lastHandle = 0;   // not selecting -> reset dedup (so re-entry re-announces)
        } else {
            uint32_t th = 0, ph = 0;
            SafeReadU32(P, OFF_TARGETID, &th);
            SafeReadU32(panel, OFF_PANEL_ID, &ph);
            if (th != 0 && ph == th) {
                g_wantTargetBc = true;
                g_targetHandle = static_cast<int32_t>(th);
            }
        }
    }

    if (s_origNameplate) s_origNameplate(panel, flag);   // calls FUN_00329220 with the target BtlChr
    g_wantTargetBc = false;
}

// FUN_00329220 vitals build. When invoked for the flagged target, `bc` is the real target BtlChr.
void* HookedSnapshot(void* bc, int p2, void* outBuf, int p4) {
    void* r = s_origSnapshot ? s_origSnapshot(bc, p2, outBuf, p4) : nullptr;
    if (g_wantTargetBc && bc && g_targetHandle != g_lastHandle) {
        g_wantTargetBc = false;                 // take only the first (the target) per render
        g_lastHandle = g_targetHandle;
        AnnounceTargetBc(bc);
    }
    return r;
}

} // namespace

namespace BattleTargetReader {

bool Init() {
    bool ok = Hooks::InstallTyped(RVA_NAMEPLATE, &HookedNameplate, &s_origNameplate);
    ok     &= Hooks::InstallTyped(RVA_SNAPSHOT,  &HookedSnapshot,  &s_origSnapshot);
    Log::Write("TARGET", ok ? "BattleTargetReader: target readout installed (FUN_002bfd20 + FUN_00329220, DAT_0209be80+0x9FD8)"
                            : "BattleTargetReader: a target hook FAILED to install");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_SNAPSHOT);
    Hooks::Uninstall(RVA_NAMEPLATE);
}

} // namespace BattleTargetReader

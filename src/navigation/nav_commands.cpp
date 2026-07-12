#include "navigation/nav_commands.h"
#include "navigation/entity_list.h"
#include "navigation/player_state.h"
#include "navigation/nav_common.h"
#include "navigation/path_planner.h"
#include "navigation/map_query.h"
#include "navigation/nav_rva.h"
#include "navigation/nav_types.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/logger.h"
#include "speech/speech.h"

#include <Windows.h>
#include <cstdio>
#include <string>

namespace NavCommands {

namespace {

// `\` — request a turn-by-turn route to the current selection. The actual A* runs on
// the game thread (crash-safe on transitions); the legs (or "No path") are spoken from
// there a frame or two later. We only capture the fixed world target here.
void RouteToCurrent() {
    FVec3 pos; std::wstring label;
    if (!EntityList::GetCurrentTarget(pos, label)) {
        // Front-of-pipeline diagnostic: distinguishes "/ produced no target" from
        // "/ never reached us" (no NAV-ROUTE line at all) when tracing the route failure.
        Log::Write("NAV-ROUTE", "'/' pressed: GetCurrentTarget returned no target -> \"No target\"");
        Speech::Output(L"No target");
        return;
    }
    Log::Write("NAV-ROUTE", "'/' pressed: target acquired -> PathPlanner::Request");
    PathPlanner::Request(pos, label);
}

void SpeakFacing() {
    float yaw = 0.0f;
    if (!PlayerState::ReadPlayerYaw(yaw)) { Speech::Output(L"Facing unavailable"); return; }
    std::wstring s = L"Facing ";
    s += NavCommon::CardinalOfHeading(yaw);
    Speech::Output(s);
}

void DiagnosticDump() {
    FVec3 p;
    bool haveP = PlayerState::ReadPlayerPos(p);
    if (haveP) {
        char msg[96];
        snprintf(msg, sizeof(msg), "player pos=(%.2f, %.2f, %.2f)", p.x, p.y, p.z);
        Log::Write("NAV-DIAG", msg);
    } else {
        Log::Write("NAV-DIAG", "player pos unavailable");
    }

    // Which route-gate condition(s) fail RIGHT NOW (works even when NOT nav-safe — that
    // is the point; it names the exact blocker the game-thread route would hit).
    uint8_t fm = PlayerState::NavSafeFailMask();
    char names[96];
    PlayerState::FormatNavSafeMask(fm, names, sizeof(names));
    char sm[160];
    snprintf(sm, sizeof(sm), "nav-safe failMask=0x%02X[%s] (0x00 = ready)", fm, names);
    Log::Write("NAV-DIAG", sm);

    // Walkmap self-test against the SQEX field collision (the system the AI NPCs walk on;
    // live even with no Bullet world). Logs the collision-manager gate + ctx0, a ground
    // probe, and 4 cardinal wall probes at the player — the confirmation that the map
    // oracle is usable in this scene (esp. the prologue). All SEH-guarded / read-only.
    void* mapGate = MemRead::PtrAt(Hooks::ResolveRva(NavRva::MAP_COLL_GATE), 0);
    void* mapCtx0 = MemRead::PtrAt(Hooks::ResolveRva(NavRva::MAP_COLL_CTX0), 0);
    char gm[128];
    snprintf(gm, sizeof(gm), "map-coll: gate=%p ctx0=%p hasWorld=%d",
             mapGate, mapCtx0, MapQuery::HasWorld() ? 1 : 0);
    Log::Write("NAV-DIAG", gm);

    if (haveP && MapQuery::HasWorld()) {
        float fy = 0.0f;
        bool floorHit = MapQuery::GroundAt(p.x, p.z, fy);
        char fl[112];
        snprintf(fl, sizeof(fl), "selftest ground: hit=%d y=%.2f (playerY=%.2f dy=%.2f)",
                 floorHit ? 1 : 0, floorHit ? fy : 0.0f, p.y, floorHit ? (fy - p.y) : 0.0f);
        Log::Write("NAV-DIAG", fl);

        // Cardinal wall clearance ~5 m each, at body height. north=-Z, east=+X, south=+Z, west=-X.
        // Log BOTH the walk class (mask=4, what routing uses) and the camera class
        // (mask=0xffff) — the runtime confirmation for Fix A: walk must read CLEAR toward
        // directions you can actually walk; camera falsely blocks on camera-only planes.
        const float R = 5.0f, bodyPad = 0.9f;
        const float baseY = (floorHit ? fy : p.y) + bodyPad;
        const FVec3 base{ p.x, baseY, p.z };
        auto clr = [&](float dx, float dz, uint16_t mask, uint32_t flags) -> int {
            return MapQuery::SegmentHit(base, FVec3{ p.x + dx, baseY, p.z + dz }, mask, flags) < 0 ? 1 : 0;
        };
        const uint16_t mW = NavRva::MAP_MASK_WALK, mC = NavRva::MAP_MASK_CAM;
        const uint32_t fW = NavRva::MAP_SEG_FLAGS, fC = NavRva::MAP_SEG_FLAGS_CAM;
        char hz[176];
        snprintf(hz, sizeof(hz),
                 "selftest walls walk(4): N=%d E=%d S=%d W=%d | cam(0xffff): N=%d E=%d S=%d W=%d (1=clear)",
                 clr(0.0f, -R, mW, fW), clr(R, 0.0f, mW, fW), clr(0.0f, R, mW, fW), clr(-R, 0.0f, mW, fW),
                 clr(0.0f, -R, mC, fC), clr(R, 0.0f, mC, fC), clr(0.0f, R, mC, fC), clr(-R, 0.0f, mC, fC));
        Log::Write("NAV-DIAG", hz);
    } else {
        Log::Write("NAV-DIAG", "selftest skipped (no walkmap or no player pos)");
    }

    EntityList::Rescan();
    EntityList::LogDiagnostic();
    Speech::Output(L"Diagnostic logged");
}

} // namespace

void OnNavKey(int vk, bool /*shift*/) {
    switch (vk) {
        case VK_OEM_5:      RouteToCurrent();                 break;  // \  turn-by-turn route
        case VK_OEM_4:      EntityList::CmdPrev();            break;  // [  previous object
        case VK_OEM_6:      EntityList::CmdNext();            break;  // ]  next object
        case VK_OEM_3:      EntityList::CmdRescan();          break;  // `  rescan + area
        case VK_OEM_MINUS:  EntityList::CmdPrevCategory();    break;  // -  previous category
        case VK_OEM_PLUS:   EntityList::CmdNextCategory();    break;  // =  next category
        case VK_OEM_1:      SpeakFacing();                    break;  // ;  facing readout
        case VK_OEM_2:      EntityList::CmdDescribeCurrent(); break;  // /  describe current
        case VK_OEM_7:      DiagnosticDump();                 break;  // '  diagnostic dump
        default:            break;
    }
}

} // namespace NavCommands

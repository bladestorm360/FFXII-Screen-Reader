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
#include <cmath>
#include <string>

namespace NavCommands {

namespace {

// `\` — request a turn-by-turn route to the current selection. The actual A* runs on
// the game thread (crash-safe on transitions); the legs (or "No path") are spoken from
// there a frame or two later. We only capture the fixed world target here.
void RouteToCurrent() {
    FVec3 pos; std::wstring label;
    if (!EntityList::GetCurrentTarget(pos, label)) {
        // Front-of-pipeline diagnostic: distinguishes "\\ produced no target" from
        // "\\ never reached us" (no NAV-ROUTE line at all) when tracing the route failure.
        Log::Write("NAV-ROUTE", "'\\' (route) pressed: GetCurrentTarget returned no target -> \"No target\"");
        Speech::Output(L"No target");
        return;
    }
    Log::Write("NAV-ROUTE", "'\\' (route) pressed: target acquired -> PathPlanner::Request");
    PathPlanner::Request(pos, label);
}

// `;` — speak which real-world (true-north) direction "forward"/UP currently points ("Forward points
// north"). The spoken route/scan directions are EGOCENTRIC ("North" = forward = where UP takes you);
// this is the absolute-orientation companion. Reads the camera up-direction (ReadCameraForward), the
// SAME reference those directions use — not the character's facing.
void SpeakFacing() {
    float upRad = 0.0f;
    if (!PlayerState::ReadCameraForward(upRad)) { Speech::Output(L"Facing unavailable"); return; }
    std::wstring s = L"Forward points ";
    s += NavCommon::CardinalOfFacing(upRad);
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

    // Movement-frame snapshot — pins the egocentric "forward" reference sign against GROUND TRUTH.
    // Protocol: (1) stand, tap '; (2) hold W and tap ' WHILE MOVING; (3) hold D and tap ' while
    // moving. On the W leg, walkedYawF (the actual direction UP took you, from the position delta) and
    // moveYawDeg and faceNodeDeg should all agree, and match camFwdNegDeg OR camFwdRawDeg — whichever
    // matches is the correct up-direction formula (locks the 180deg sign). All degrees, signed,
    // atan2(x,z) convention except walkedYawB which is the BearingDeg atan2(x,-z) convention.
    {
        PlayerState::MoveFrame mf;
        PlayerState::ReadMoveFrame(mf);
        const float R2D = 57.2957795f;
        char fn[24], md[48], cfn[24], cfr[24], cd[24];
        if (mf.haveFaceNode)  snprintf(fn, sizeof(fn), "%.1f", mf.faceNodeRad * R2D);  else snprintf(fn, sizeof(fn), "n/a");
        if (mf.haveMove && mf.moving) snprintf(md, sizeof(md), "%.1f(%.2f,%.2f)", mf.moveYawRad * R2D, mf.moveX, mf.moveZ);
        else if (mf.haveMove)         snprintf(md, sizeof(md), "idle");
        else                          snprintf(md, sizeof(md), "n/a");
        if (mf.haveCamFwd) snprintf(cfn, sizeof(cfn), "%.1f", mf.camFwdNegRad * R2D); else snprintf(cfn, sizeof(cfn), "n/a");
        if (mf.haveCamFwd) snprintf(cfr, sizeof(cfr), "%.1f", mf.camFwdRawRad * R2D); else snprintf(cfr, sizeof(cfr), "n/a");
        if (mf.haveCamLook) snprintf(cd, sizeof(cd), "%.1f", mf.camLookRad * R2D);    else snprintf(cd, sizeof(cd), "n/a");
        char fmsg[224];
        snprintf(fmsg, sizeof(fmsg),
                 "move-frame: faceNodeDeg=%s moveYawDeg=%s camFwdNegDeg=%s camFwdRawDeg=%s camScalarDeg=%s",
                 fn, md, cfn, cfr, cd);
        Log::Write("NAV-DIAG", fmsg);

        // Ground truth: the world direction actually walked since the PREVIOUS ' press.
        char db[64];
        if (mf.haveDPos)
            snprintf(db, sizeof(db), "walkedYawF=%.1f walkedYawB=%.1f dist=%.2f (d=%.2f,%.2f)",
                     mf.dPosYawF * R2D, mf.dPosYawB * R2D, mf.dPosDist, mf.dPosX, mf.dPosZ);
        else
            snprintf(db, sizeof(db), "walked=n/a (first tap or no movement since last tap)");
        Log::Write("NAV-DIAG", (std::string("move-frame dPos: ") + db).c_str());
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

    // Walkmap GRID self-test (confirmation for the whole-map overlay). Logs the grid header
    // (=> exact map extent) and cross-checks the DIRECT cell read (ReadCellFloor, the zero-
    // raycast overlay source, conf 0.92) against the confirmed GroundAt oracle on a sampled
    // stride of cells. High walkAgree% promotes the direct read to the >=0.98 ship bar; a low
    // rate means the struct offsets are off (fix them, or flip NavGrid to the GroundAt fill).
    if (MapQuery::HasWorld()) {
        MapQuery::WalkGridInfo g;
        if (MapQuery::GetGridInfo(g)) {
            const float extX = static_cast<float>(g.nCols * g.cellSizeX);
            const float extZ = static_cast<float>(g.nRows * g.cellSizeZ);
            char gh[192];
            snprintf(gh, sizeof(gh),
                     "grid: %dx%d cells cell=%dx%dm origin=(%d,%d) extent=%.0fx%.0fm minCorner=(%d,%d)",
                     g.nCols, g.nRows, g.cellSizeX, g.cellSizeZ, g.originX, g.originZ,
                     extX, extZ, -g.originX, -g.originZ);
            Log::Write("NAV-DIAG", gh);

            int sampled = 0, walkAgree = 0, yAgree = 0, directWalk = 0, oracleWalk = 0;
            const int total = g.nCols * g.nRows;
            const int stride = total > 400 ? total / 400 : 1;   // ~<=400 samples
            for (int cell = 0; cell < total; cell += stride) {
                const int col = cell % g.nCols, row = cell / g.nCols;
                float dy = 0.0f; const bool dW = MapQuery::ReadCellFloor(g, col, row, dy);
                float wx, wz; MapQuery::CellCenter(g, col, row, wx, wz);
                float oy = 0.0f; const bool oW = MapQuery::GroundAt(wx, wz, oy);
                ++sampled;
                if (dW) ++directWalk;
                if (oW) ++oracleWalk;
                if (dW == oW) {
                    ++walkAgree;
                    if (dW && std::fabs(dy - oy) <= 0.5f) ++yAgree;
                }
            }
            char gc[208];
            snprintf(gc, sizeof(gc),
                     "grid xcheck: sampled=%d walkAgree=%d (%.0f%%) yAgree=%d directWalk=%d oracleWalk=%d",
                     sampled, walkAgree, sampled ? 100.0f * static_cast<float>(walkAgree) / sampled : 0.0f,
                     yAgree, directWalk, oracleWalk);
            Log::Write("NAV-DIAG", gc);
        } else {
            Log::Write("NAV-DIAG", "grid: GetGridInfo failed (no grid header)");
        }
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
        case VK_OEM_2:      EntityList::CmdDescribeCurrent(); break;  // /  describe current
        case VK_OEM_1:      SpeakFacing();                    break;  // ;  facing readout (true north)
        case VK_OEM_7:      DiagnosticDump();                 break;  // '  diagnostic dump
        default:            break;
    }
}

} // namespace NavCommands

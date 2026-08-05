#include "navigation/nav_probe.h"
#include "navigation/exit_diag.h"
#include "navigation/shout_diag.h"
#include "navigation/nav_mesh.h"
#include "navigation/map_query.h"
#include "navigation/map_seams.h"
#include "navigation/map_names.h"
#include "navigation/nav_common.h"
#include "navigation/nav_trace.h"
#include "navigation/player_state.h"
#include "navigation/interact_target.h"
#include "navigation/entity_list.h"
#include "navigation/nav_rva.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "speech/speech.h"
#include "speech/phrasebook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

namespace NavProbe {

namespace {

constexpr const char* kTag = "NAV-PROBE";

std::atomic<bool> g_pending{false};
int               g_framesLeft = 0;      // game thread only
constexpr int     kWaitFrames  = 90;     // ~1.5 s while a map fades in

// Polys listed individually around the player. The whole component is flooded regardless; this only
// bounds how much of it is printed.
constexpr int kNeighbourDepth = 2;

void LogPoly(const char* what, NavMesh::PolyId p) {
    char m[288];
    if (p == NavMesh::kNoPoly) {
        snprintf(m, sizeof(m), "   %s: NO POLY (not standing on any readable floor triangle)", what);
        Log::Write(kTag, m);
        return;
    }
    uint32_t raw = 0, eff = 0;
    NavMesh::PolyFlags(p, raw, eff);
    FVec3 c{};
    NavMesh::PolyCentroid(p, c);
    char nb[128]; int off = 0;
    for (int e = 0; e < 3; ++e) {
        const NavMesh::PolyId n = NavMesh::Neighbor(p, e);
        off += snprintf(nb + off, sizeof(nb) - static_cast<size_t>(off), "%se%d=%d%s",
                        e ? " " : "", e, n,
                        (n == NavMesh::kNoPoly) ? "(edge)"
                                                : (NavMesh::Walkable(n) ? "(walk)" : "(BLOCKED)"));
    }
    snprintf(m, sizeof(m),
             "   %s: poly=%d centroid=(%.2f,%.2f,%.2f) raw=%08X eff=%08X type=%u mj=%d walk=%d | %s",
             what, p, c.x, c.y, c.z, raw, eff, eff & 7u, NavMesh::MapJumpGroup(p),
             NavMesh::Walkable(p) ? 1 : 0, nb);
    Log::Write(kTag, m);
}

// THE question the rebuild rests on: is the mesh actually connected from where the player stands to
// where each exit is? Everything says it should be -- stairs are triangles like any other -- but that
// is an assertion about this map's data, and asserting things about exit data is exactly how six
// sessions of the exit saga went wrong. So flood it and print the answer.
void DumpMeshConnectivity(const FVec3& player, int mapId) {
    Log::Write(kTag, "==== MESH: the game's own navmesh (poly adjacency at +0x16/+0x18/+0x1A) ====");

    const NavMesh::PolyId here = NavMesh::FindPolyAt(player.x, player.y, player.z);
    LogPoly("player", here);

    if (here == NavMesh::kNoPoly) {
        Log::Write(kTag, "   cannot flood without a start poly");
        return;
    }

    // One ring or two of neighbours, so a broken adjacency read is visible as garbage ids rather
    // than as a silently small flood.
    std::vector<NavMesh::PolyId> ring{ here };
    std::unordered_set<NavMesh::PolyId> shown{ here };
    for (int d = 0; d < kNeighbourDepth; ++d) {
        std::vector<NavMesh::PolyId> next;
        for (NavMesh::PolyId p : ring)
            for (int e = 0; e < 3; ++e) {
                const NavMesh::PolyId n = NavMesh::Neighbor(p, e);
                if (n == NavMesh::kNoPoly || shown.count(n)) continue;
                shown.insert(n);
                next.push_back(n);
                char lbl[32];
                snprintf(lbl, sizeof(lbl), "ring%d", d + 1);
                LogPoly(lbl, n);
            }
        ring.swap(next);
    }

    std::vector<NavMesh::PolyId> comp;
    const int n = NavMesh::FloodFrom(here, comp);
    char m[224];
    snprintf(m, sizeof(m), "   FLOOD: %d polys reachable from the player's poly%s",
             n, (n >= NavMesh::kMaxPolys) ? "  <== HIT THE CAP" : "");
    Log::Write(kTag, m);

    std::unordered_set<NavMesh::PolyId> reach(comp.begin(), comp.end());

    // Per seam: are its own polys in that component? A "no" here is the whole bug, stated in one
    // line, and it names the group so the next question is which edge the flood stopped at.
    std::vector<MapQuery::MapJumpSurface> surf;
    if (!MapQuery::CachedMapJumpSurfaces(mapId, surf))
        Log::Write(kTag, "   seams: NOT SWEPT YET for this map -- not the same thing as 'this map has none'");
    for (const auto& s : surf) {
        int inComp = 0;
        for (int pid : s.polys) if (reach.count(pid)) ++inComp;
        snprintf(m, sizeof(m),
                 "   seam group=%d polys=%zu inPlayerComponent=%d  -> %s",
                 s.group, s.polys.size(), inComp,
                 inComp > 0 ? "REACHABLE" : "NOT REACHABLE FROM HERE");
        Log::Write(kTag, m);
        if (!s.polys.empty()) {
            char ids[160]; int off = 0;
            for (size_t i = 0; i < s.polys.size() && off < 140; ++i)
                off += snprintf(ids + off, sizeof(ids) - static_cast<size_t>(off), "%s%d",
                                i ? "," : "", s.polys[i]);
            snprintf(m, sizeof(m), "     polys: %s", ids);
            Log::Write(kTag, m);
        }
    }
}

void DumpInteractReach() {
    Log::Write(kTag, "==== INTERACT-REACH: the number kApproachRadius=4.0 used to stand in for ====");
    InteractTarget::LogChosen();

    // How often the interaction-anchor offset is actually present. The offset's semantics are not
    // established, so this counts rather than asserts: if it never fires, the anchor change is a
    // no-op and should be reported as one instead of standing as a fix.
    {
        int withOff = 0, plain = 0; float maxOff = 0.0f;
        PlayerState::GetAnchorStats(withOff, plain, maxOff);
        char am[192];
        snprintf(am, sizeof(am),
                 "   anchor offset (xform+0x107): applied=%d plain=%d maxMagnitude=%.3f%s",
                 withOff, plain, maxOff,
                 (withOff == 0) ? "   <== NEVER SET; the anchor change is a no-op so far" : "");
        Log::Write(kTag, am);
    }

    const InteractTarget::Chosen c = InteractTarget::Read();
    char m[320];

    if (!c.valid || !c.sceneObj) {
        Log::Write(kTag, "   no chosen target in reach -- stand next to something and press ' again");
    } else {
        const InteractTarget::Reach r = InteractTarget::ReadReachFor(c.sceneObj);
        if (!r.valid) {
            Log::Write(kTag, "   ReadReachFor failed (no leader xform or no target xform)");
        } else {
            snprintf(m, sizeof(m),
                     "   replica: dist2D=%.3f | ellipsePl=%.3f + extraPl=%.3f + ellipseTg=%.3f + "
                     "extraTg=%.3f -> reach=%.3f (min=%.3f) gate=%s",
                     r.dist2D, r.ellipsePlayer, r.extraPlayer, r.ellipseTarget, r.extraTarget,
                     r.radius, r.radiusMin, r.passes ? "PASS" : "FAIL");
            Log::Write(kTag, m);

            if (r.haveMeasured) {
                const float delta = r.radius - r.measured;
                snprintf(m, sizeof(m),
                         "   engine : score=%.4f -> measured = 2*%.3f - score = %.3f | "
                         "replica-measured=%+.3f  %s",
                         c.score, r.dist2D, r.measured, delta,
                         (std::fabs(delta) <= 0.02f)
                             ? "<== CONFIRMED, replica is the engine's reach"
                             : "<== MISMATCH, do NOT narrow the goal set with the replica");
                Log::Write(kTag, m);
            } else {
                Log::Write(kTag, "   engine : no measured value (this object is not the chosen one)");
            }
            Log::Write(kTag, "   prior bracket from play: 0.51 PASSED the distance gate, 1.70 did not");
        }
        InteractTarget::LogGatesFor(c.sceneObj, "chosen");
    }

    FVec3 tp; std::wstring tl; void* tobj = nullptr;
    if (EntityList::GetCurrentTarget(tp, tl, nullptr, &tobj) && tobj && tobj != c.sceneObj) {
        const InteractTarget::Reach fr = InteractTarget::ReadReachFor(tobj);
        if (fr.valid) {
            snprintf(m, sizeof(m),
                     "   focused route target: dist2D=%.3f reach=%.3f (min=%.3f) gate=%s "
                     "-> the route should stop %.2fm from it, not on it",
                     fr.dist2D, fr.radius, fr.radiusMin, fr.passes ? "PASS" : "FAIL", fr.radiusMin);
            Log::Write(kTag, m);
        }
    }
}

void DumpExitAim(const FVec3& player, int mapId) {
    std::vector<MapQuery::MapJumpSurface> surf;
    const bool swept = MapQuery::CachedMapJumpSurfaces(mapId, surf);

    char m[320];
    snprintf(m, sizeof(m),
             "==== EXIT-AIM: seam centroid vs near edge | %zu group(s), mask now 0xF (was 0x1F)%s ====",
             surf.size(), swept ? "" : " -- NOT SWEPT YET for this map");
    Log::Write(kTag, m);

    for (const auto& s : surf) {
        FVec3 nearPt{};   // NOT `near`: <windef.h> defines it as an empty macro
        const bool haveNear = MapQuery::NearestPointOnSurface(s, player, nearPt);
        const float dCentroid = NavCommon::Distance2D(player, s.centroid);
        const float dNear     = haveNear ? NavCommon::Distance2D(player, nearPt) : dCentroid;

        snprintf(m, sizeof(m),
                 "   group=%d polys=%d verts=%zu box x[%.1f..%.1f] y[%.1f..%.1f] z[%.1f..%.1f]",
                 s.group, s.polyCount, s.verts.size(),
                 s.min.x, s.max.x, s.min.y, s.max.y, s.min.z, s.max.z);
        Log::Write(kTag, m);

        snprintf(m, sizeof(m),
                 "     centroid=(%.1f,%.1f,%.1f) d=%.1fm %d steps | nearest=(%.1f,%.1f,%.1f) "
                 "d=%.1fm %d steps | OVERSHOOT %.1fm %d steps",
                 s.centroid.x, s.centroid.y, s.centroid.z, dCentroid,
                 NavCommon::DistanceToSteps(dCentroid),
                 nearPt.x, nearPt.y, nearPt.z, dNear, NavCommon::DistanceToSteps(dNear),
                 dCentroid - dNear,
                 NavCommon::DistanceToSteps(dCentroid) - NavCommon::DistanceToSteps(dNear));
        Log::Write(kTag, m);
    }
}

// ---- FUN_00231690 DYNAMIC-OBSTACLE DIAGNOSTIC (Session 100) --------------------------------------
// The S100 decompile sweep found exactly ONE callable path that can see dynamic obstacle prims
// (>= 0x5000 -- doors, sluice gates, moving platforms; every ray/sphere callback hard-excludes
// them): FUN_00231690, the generic volume push-out, conditionally write-free at conf 0.97 -- BELOW
// the 0.98 bar. This diagnostic exists to settle that FROM C++ (user directive: C++ diagnostics,
// not Frida): snapshot the conditional-write targets the research identified, make one safe-path
// call (caller-owned body object with the ellipse semi-axes at 0.27, far under the 2.0 spill
// threshold), re-snapshot, and log every delta plus anything the call returned. Verdict rule,
// recorded in the plan: deltas confined to the per-use scratch (which every engine caller rewrites
// before use) across several presses/maps -> promoted; ANY unexpected delta -> the function is
// dropped and the dynamic-obstacle census dies. NOTHING ROUTES ON THIS.
typedef int(__fastcall* Pfn_VolumePushOut)(void* ctx, float* outPush, short* outPrim,
                                           float* shapeMat, float* pos,
                                           uint32_t flags, int enableDyn, void* bodyObj);

struct ScratchSnap {
    uint32_t visit[8] = {};   // head of the 128-int visit array
    uint32_t count = 0, aux = 0;
    uint32_t build[4] = {};   // FUN_00230790's OBB build scratch
};

bool SnapScratch(ScratchSnap& s) {
    void* va = Hooks::ResolveRva(NavRva::COLL_VISIT_ARRAY);
    void* vc = Hooks::ResolveRva(NavRva::COLL_VISIT_COUNT);
    void* vx = Hooks::ResolveRva(NavRva::COLL_VISIT_AUX);
    void* vb = Hooks::ResolveRva(NavRva::COLL_BUILD_SCRATCH);
    if (!va || !vc || !vx || !vb) return false;
    bool ok = true;
    for (uint32_t i = 0; i < 8; ++i) ok = ok && MemRead::SafeReadU32(va, i * 4u, &s.visit[i]);
    ok = ok && MemRead::SafeReadU32(vc, 0, &s.count);
    ok = ok && MemRead::SafeReadU32(vx, 0, &s.aux);
    for (uint32_t i = 0; i < 4; ++i) ok = ok && MemRead::SafeReadU32(vb, i * 4u, &s.build[i]);
    return ok;
}

// POD-only SEH scope, same convention as map_query.cpp's engine-call shims.
int CallPushOut(Pfn_VolumePushOut fn, void* ctx, float* outPush, short* outPrim, float* shapeMat,
                float* pos, uint32_t flags, int dyn, void* body, int* faulted) {
    __try { return fn(ctx, outPush, outPrim, shapeMat, pos, flags, dyn, body); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *faulted = 1; return 0; }
}

void DumpDynObstacleProbe(const FVec3& p) {
    void* fn  = Hooks::ResolveRva(NavRva::MAP_VOLUME_PUSHOUT);
    void* ctx = MapQuery::DebugCollisionCtx();
    char m[288];
    if (!fn || !ctx) {
        Log::Write(kTag, "   dynprobe: FUN_00231690/ctx unresolved -- skipped");
        return;
    }
    ScratchSnap before{}, after{};
    const bool sb = SnapScratch(before);

    // Everything caller-owned. Shape = 3x4 sphere matrix diag(0.27), the sweep's own idiom; body =
    // a zeroed block with ONLY the ellipse semi-axes (+0x80/+0x84) and the party class (+0x50) set
    // -- the safe-path condition is those axes <= 2.0, and we hold them at 0.27.
    float outPush[4] = {};
    short outPrim[8]; for (short& s : outPrim) s = -1;
    float shape[12] = {};
    shape[0] = shape[5] = shape[10] = 0.27f;
    float pos4[4] = { p.x, p.y + 0.9f, p.z, 1.0f };
    alignas(16) unsigned char body[0x100] = {};
    *reinterpret_cast<float*>(body + 0x80) = 0.27f;
    *reinterpret_cast<float*>(body + 0x84) = 0.27f;
    *reinterpret_cast<uint16_t*>(body + 0x50) = 4;   // party movement class

    int faulted = 0;
    const int r = CallPushOut(reinterpret_cast<Pfn_VolumePushOut>(fn), ctx, outPush, outPrim,
                              shape, pos4, 0x13u, /*enableDyn=*/1, body, &faulted);
    const bool sa = SnapScratch(after);

    snprintf(m, sizeof(m),
             "   dynprobe: ret=%d faulted=%d prim[0]=%d push=(%.3f,%.3f,%.3f) at=(%.1f,%.2f,%.1f)"
             "%s",
             r, faulted, static_cast<int>(outPrim[0]), outPush[0], outPush[1], outPush[2],
             pos4[0], pos4[1], pos4[2],
             (outPrim[0] >= 0x5000) ? "  <== DYNAMIC OBSTACLE PRIM" : "");
    Log::Write(kTag, m);

    if (!sb || !sa) {
        Log::Write(kTag, "   dynprobe: scratch UNREADABLE -- write-freedom NOT ANSWERED this press");
        return;
    }
    int deltas = 0;
    for (int i = 0; i < 8; ++i)
        if (before.visit[i] != after.visit[i] && deltas < 8) {
            snprintf(m, sizeof(m), "   dynprobe: DELTA visit[%d] 0x%08X -> 0x%08X",
                     i, before.visit[i], after.visit[i]);
            Log::Write(kTag, m);
            ++deltas;
        }
    if (before.count != after.count) {
        snprintf(m, sizeof(m), "   dynprobe: DELTA visitCount %u -> %u", before.count, after.count);
        Log::Write(kTag, m); ++deltas;
    }
    if (before.aux != after.aux) {
        snprintf(m, sizeof(m), "   dynprobe: DELTA visitAux 0x%08X -> 0x%08X", before.aux, after.aux);
        Log::Write(kTag, m); ++deltas;
    }
    for (int i = 0; i < 4; ++i)
        if (before.build[i] != after.build[i]) {
            snprintf(m, sizeof(m), "   dynprobe: DELTA buildScratch[%d] 0x%08X -> 0x%08X",
                     i, before.build[i], after.build[i]);
            Log::Write(kTag, m); ++deltas;
        }
    snprintf(m, sizeof(m),
             "   dynprobe: scratch deltas=%d -- %s", deltas,
             deltas == 0 ? "SAFE-PATH CALL WROTE NOTHING WE WATCH (one data point toward >=0.98)"
                         : "writes observed; record them, do NOT promote");
    Log::Write(kTag, m);
}

void RunProbe() {
    STALL_SCOPE("NavProbe::RunProbe");
    FVec3 p{};
    const bool haveP = PlayerState::ReadPlayerPos(p);
    const int  mapId = MapNames::CurrentMapId();

    char m[288];
    snprintf(m, sizeof(m), "======== NAV-PROBE mapId=%d player=(%.2f,%.2f,%.2f)%s ========",
             mapId, p.x, p.y, p.z, haveP ? "" : " (POSITION UNAVAILABLE)");
    Log::Write(kTag, m);

    uint8_t fm = PlayerState::NavSafeFailMask();
    char names[96];
    PlayerState::FormatNavSafeMask(fm, names, sizeof(names));
    snprintf(m, sizeof(m), "   nav-safe failMask=0x%02X[%s] (0x00 = ready) | hasWorld=%d meshReady=%d",
             fm, names, MapQuery::HasWorld() ? 1 : 0, NavMesh::Ready() ? 1 : 0);
    Log::Write(kTag, m);

    if (haveP && NavMesh::Ready()) DumpMeshConnectivity(p, mapId);
    else Log::Write(kTag, "   mesh not ready -- MESH block skipped");

    DumpInteractReach();
    if (haveP) DumpExitAim(p, mapId);
    if (haveP && MapQuery::HasWorld()) DumpDynObstacleProbe(p);

    // The RAW handle-table walk -- every object the game registered, including the ones the scan
    // REJECTED. Re-keyed here in Session 77: it lost its only caller when the `'` dump was stripped
    // in Session 74, and without it the log can only show what PASSED the filters. That is how four
    // deleted NPCs went unnoticed. Its filter is deliberately wider than the scan's, so an object
    // the scan drops still appears here with its flags, kind, category and position.
    EntityList::LogDiagnostic();

    // THE TERRAIN CENSUS -- every distinct polygon flags word on this map, with the effective flags and
    // whether the party is refused by bit 23. Revived in Session 96: it had been written, declared and
    // then left with zero callers, while three research passes argued about terrain attributes it would
    // have answered in one keypress.
    ExitDiag::LogWalkPolyClasses();

    // The shout minigame's own state: which script module is live, the gauge and its shown bit, the
    // two toggles, and the full npcdic census. That census is how the guard's identity gets measured
    // -- run this standing beside whoever punished the last shout.
    ShoutDiag::Dump();

    NavTrace::DumpTrail();
    Log::Write(kTag, "======== end NAV-PROBE ========");
}

} // namespace

void Request() { g_pending.store(true, std::memory_order_release); }

void OnGameFrame() {
    if (!g_pending.load(std::memory_order_acquire)) return;

    if (g_framesLeft <= 0) g_framesLeft = kWaitFrames;

    if (!PlayerState::IsFieldNavSafe()) {
        if (--g_framesLeft > 0) return;
        g_pending.store(false, std::memory_order_release);
        uint8_t fm = PlayerState::NavSafeFailMask();
        char names[96];
        PlayerState::FormatNavSafeMask(fm, names, sizeof(names));
        char m[176];
        snprintf(m, sizeof(m), "probe abandoned: field never became nav-safe (failMask=0x%02X[%s])",
                 fm, names);
        Log::Write(kTag, m);
        Speech::Output(Phrase::Get(Phrase::Id::DiagnosticUnavailable), true);
        return;
    }

    g_pending.store(false, std::memory_order_release);
    g_framesLeft = 0;
    RunProbe();
    Speech::Output(Phrase::Get(Phrase::Id::DiagnosticLogged), true);
}

} // namespace NavProbe

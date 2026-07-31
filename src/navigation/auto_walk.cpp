#include "navigation/auto_walk.h"
#include "navigation/audio_beacon.h"
#include "navigation/nav_common.h"
#include "navigation/nav_mesh.h"
#include "navigation/player_state.h"
#include "input/input_tracker.h"
#include "ui/mod_menu.h"
#include "ui/menu_state.h"
#include "speech/phrasebook.h"
#include "speech/speech.h"
#include "core/logger.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>

namespace AutoWalk {
namespace {

// DIK scan codes -- the same constants input_tracker.cpp reads. W/A/S/D are the only keys this
// module may ever WRITE; the arrows appear only in the CANCEL set (they are the game's camera keys,
// Controls.md -- camera rotation is auto-compensated by camera-relative steering, so cancelling on
// them is conservative rather than necessary; one-array tester-taste tunable).
constexpr unsigned char kDikW = 0x11, kDikA = 0x1E, kDikS = 0x1F, kDikD = 0x20;
constexpr unsigned char kDikUp = 0xC8, kDikDown = 0xD0, kDikLeft = 0xCB, kDikRight = 0xCD;

// ---- tuning --------------------------------------------------------------------------------------
constexpr uint64_t kPendingFreshMs = 5000;   // covers the planner's own wait-for-nav-safe retry
// The passive master safety: the mask is only refreshed by the field-frame tick, so anything that
// stops that tick (map transition, pause, a menu that freezes field sim, a stall) stops injection
// within this window with no code needing to know why.
constexpr uint64_t kMaskStaleMs    = 250;
// Steering hysteresis: the current octant sticks until the bearing is this far PAST its sector
// boundary (bounding heading error at 22.5 + 10 deg, under half a sector), and an ADJACENT-octant
// change applies at most once per hold window. A >= 2-octant change (a real corner) is immediate.
constexpr float    kHysteresisDeg  = 10.0f;
constexpr uint64_t kMinHoldMs      = 250;
// Field-tick self-gap: OnGameFrame stopped being called (dialogue, cutscene, pause paths that stop
// the field tick without any signal the mod can read). No auto-resume -- on the next tick it
// disengages rather than walking the character again unprompted.
constexpr uint64_t kFieldGapMs     = 600;
// The hard cap: stop commanding a character that is not getting anywhere. The stuck detector
// (1.8 s + 2 s cooldown) keeps firing replans underneath; this is the "give up and say so" bound,
// measured as closing on the FINAL destination.
constexpr uint64_t kNoProgressMs   = 15000;
constexpr float    kProgressM      = 1.0f;
constexpr float    kWalkDeltaCapM  = 5.0f;   // per-frame delta above this = teleport, not walking

// ---- cross-thread state (the ONLY state the input-poll thread touches) ---------------------------
std::atomic<bool>     g_engaged{false};
std::atomic<uint8_t>  g_keyMask{0};          // bit0 W, bit1 A, bit2 S, bit3 D
std::atomic<uint64_t> g_maskStampMs{0};
std::atomic<bool>     g_cancelReq{false};    // hook saw a real movement key; game thread formalises
std::atomic<uint64_t> g_pendingEngageMs{0};  // `\` pressed with the toggle on

// ---- game-thread state ---------------------------------------------------------------------------
int      g_octant        = -1;
uint64_t g_octantSinceMs = 0;
uint64_t g_lastFrameMs   = 0;
FVec3    g_lastPos{};
bool     g_havePos       = false;
float    g_walkedM       = 0.0f;
float    g_routeLenM     = 0.0f;
size_t   g_lastLegIndex  = 0;
size_t   g_lastLegCount  = 0;
int      g_stuckFires    = 0;
uint64_t g_engageMs      = 0;
float    g_bestDestDist  = -1.0f;
uint64_t g_lastImproveMs = 0;

enum class Reason {
    PlayerInput, Combat, Arrived, RouteLost, MapChange,
    ToggleOff, ModMenuOpen, GameMenu, FocusLost, FieldInterrupted, NoProgress
};

const char* ReasonName(Reason r) {
    switch (r) {
        case Reason::PlayerInput:      return "PlayerInput";
        case Reason::Combat:           return "Combat";
        case Reason::Arrived:          return "Arrived";
        case Reason::RouteLost:        return "RouteLost";
        case Reason::MapChange:        return "MapChange";
        case Reason::ToggleOff:        return "ToggleOff";
        case Reason::ModMenuOpen:      return "ModMenuOpen";
        case Reason::GameMenu:         return "GameMenu";
        case Reason::FocusLost:        return "FocusLost";
        case Reason::FieldInterrupted: return "FieldInterrupted";
        case Reason::NoProgress:       return "NoProgress";
    }
    return "?";
}

// Octant -> key mask. Octants are clockwise from forward (0 fwd, 2 right, 4 behind, 6 left);
// bit0 W, bit1 A, bit2 S, bit3 D.
constexpr uint8_t kOctMask[8] = {
    0x1,        // 0 forward         = W
    0x1 | 0x8,  // 1 forward-right   = W+D
    0x8,        // 2 right           = D
    0x4 | 0x8,  // 3 behind-right    = S+D
    0x4,        // 4 behind          = S
    0x4 | 0x2,  // 5 behind-left     = S+A
    0x2,        // 6 left            = A
    0x1 | 0x2,  // 7 forward-left    = W+A
};

const char* OctWord(int o) {
    static const char* kW[8] = { "forward", "fwd-right", "right", "back-right",
                                 "behind", "back-left", "left", "fwd-left" };
    return (o >= 0 && o < 8) ? kW[o] : "none";
}

void Engage(const AudioBeacon::LegSnapshot& snap, uint64_t now) {
    g_octant        = -1;
    g_octantSinceMs = 0;
    g_havePos       = false;
    g_walkedM       = 0.0f;
    g_routeLenM     = snap.routeLenM;
    g_lastLegIndex  = snap.legIndex;
    g_lastLegCount  = snap.legCount;
    g_stuckFires    = 0;
    g_engageMs      = now;
    g_bestDestDist  = -1.0f;
    g_lastImproveMs = now;
    g_cancelReq.store(false, std::memory_order_relaxed);
    g_keyMask.store(0, std::memory_order_relaxed);
    g_maskStampMs.store(now, std::memory_order_relaxed);
    g_engaged.store(true, std::memory_order_relaxed);
    char m[176];
    snprintf(m, sizeof(m), "engage: legs=%zu (starting at %zu) route=%.1fm epoch=%u",
             snap.legCount, snap.legIndex + 1, snap.routeLenM, snap.epoch);
    Log::Write("AUTOWALK", m);
}

// The per-engagement summary is the validation payload this feature exists to produce: reason,
// ground covered vs route length, and how often the engine refused a commanded walk.
void Disengage(Reason why) {
    g_engaged.store(false, std::memory_order_relaxed);
    g_keyMask.store(0, std::memory_order_relaxed);
    g_cancelReq.store(false, std::memory_order_relaxed);
    const uint64_t now = GetTickCount64();
    const size_t legsDone = (why == Reason::Arrived) ? g_lastLegCount : g_lastLegIndex;
    char m[256];
    snprintf(m, sizeof(m),
             "summary: reason=%s legs=%zu/%zu walked=%.1fm route=%.1fm stuckFires=%d "
             "duration=%.1fs final=(%.1f,%.1f,%.1f)",
             ReasonName(why), legsDone, g_lastLegCount, g_walkedM, g_routeLenM, g_stuckFires,
             (now - g_engageMs) / 1000.0f, g_lastPos.x, g_lastPos.y, g_lastPos.z);
    Log::Write("AUTOWALK", m);
    // Speech ONLY where nothing else announces the stop: the no-progress cap and a lost route.
    // Player-initiated stops (own keys, toggle, menus) and self-announcing ones (the arrival cue,
    // combat's own announcements, the map transition) stay silent -- silence is the mod's normal.
    if (why == Reason::NoProgress || why == Reason::RouteLost)
        Speech::Output(Phrase::Get(Phrase::Id::AutoWalkStopped));
}

} // namespace

bool Engaged() { return g_engaged.load(std::memory_order_relaxed); }

void NotifyRoutePressed() {
    if (!ModMenu::AutoWalkOn()) return;
    g_pendingEngageMs.store(GetTickCount64(), std::memory_order_relaxed);
}

void OnDevicePoll(unsigned char* dik) {
    if (!g_engaged.load(std::memory_order_relaxed)) return;   // toggle off / idle: buffer untouched
    // THE PLAYER ALWAYS WINS, IN THIS SAME POLL: a real movement key means nothing is injected on
    // the read that carries it, and the latch cancels the feature before the next poll can resume.
    if (((dik[kDikW] | dik[kDikA] | dik[kDikS] | dik[kDikD] |
          dik[kDikUp] | dik[kDikDown] | dik[kDikLeft] | dik[kDikRight]) & 0x80) != 0) {
        g_cancelReq.store(true, std::memory_order_relaxed);
        return;
    }
    if (g_cancelReq.load(std::memory_order_relaxed)) return;
    if (GetTickCount64() - g_maskStampMs.load(std::memory_order_relaxed) > kMaskStaleMs) return;
    const uint8_t m = g_keyMask.load(std::memory_order_relaxed);
    if (m & 0x1) dik[kDikW] |= 0x80;
    if (m & 0x2) dik[kDikA] |= 0x80;
    if (m & 0x4) dik[kDikS] |= 0x80;
    if (m & 0x8) dik[kDikD] |= 0x80;
}

void OnStuckFired(const FVec3& pos, size_t legIndex, size_t legCount, float distToCorner) {
    if (!g_engaged.load(std::memory_order_relaxed)) return;
    ++g_stuckFires;
    // The one line four sessions of map 315 never had: EXACTLY where the engine refused a commanded
    // walk, on which poly, steering which way.
    const NavMesh::PolyId poly = NavMesh::FindPolyAt(pos.x, pos.y, pos.z);
    char m[224];
    snprintf(m, sizeof(m),
             "stuck: pos=(%.2f,%.2f,%.2f) poly=%d leg=%zu/%zu distToCorner=%.1fm "
             "injected=%s mask=0x%X fires=%d",
             pos.x, pos.y, pos.z, static_cast<int>(poly), legIndex + 1, legCount, distToCorner,
             OctWord(g_octant), g_keyMask.load(std::memory_order_relaxed), g_stuckFires);
    Log::Write("AUTOWALK", m);
}

void OnGameFrame() {
    const uint64_t now  = GetTickCount64();
    const uint64_t prev = g_lastFrameMs;
    g_lastFrameMs = now;

    if (!g_engaged.load(std::memory_order_relaxed)) {
        // Pending engage from `\`? Completed only once the beacon reports an active route -- which
        // already means plan == Route (Frontier/NoPath never seed). Combat blocks the engage.
        const uint64_t pend = g_pendingEngageMs.load(std::memory_order_relaxed);
        if (!pend) return;
        if (now - pend > kPendingFreshMs) { g_pendingEngageMs.store(0, std::memory_order_relaxed); return; }
        AudioBeacon::LegSnapshot snap;
        if (AudioBeacon::GetLegSnapshot(snap) && snap.routeActive && !snap.engagedCombat) {
            g_pendingEngageMs.store(0, std::memory_order_relaxed);
            Engage(snap, now);
        }
        return;
    }

    // Engaged. A fresh `\` while walking just re-requests the route; the beacon reseeds and the
    // snapshot below follows the new legs -- consume the stamp so it cannot fire a stale engage.
    g_pendingEngageMs.store(0, std::memory_order_relaxed);

    // Disengage checks, most-specific reason first.
    if (!ModMenu::AutoWalkOn())            { Disengage(Reason::ToggleOff);   return; }
    if (g_cancelReq.load(std::memory_order_relaxed)) { Disengage(Reason::PlayerInput); return; }
    if (!InputTracker::GameForeground())   { Disengage(Reason::FocusLost);   return; }
    if (ModMenu::IsOpen())                 { Disengage(Reason::ModMenuOpen); return; }
    if (MenuState::IsAnyMenuOpen())        { Disengage(Reason::GameMenu);    return; }
    if (prev != 0 && now - prev > kFieldGapMs) { Disengage(Reason::FieldInterrupted); return; }

    AudioBeacon::LegSnapshot snap;
    const bool haveRoute = AudioBeacon::GetLegSnapshot(snap);
    // COMBAT STOPS THE WALK, ALWAYS, WITHIN ONE FRAME (user requirement, recorded S100). The
    // beacon's own PartyEngagement edge is the authority -- no second actor-pool scan, and the
    // walker never re-engages on its own after combat: the player presses `\` again.
    if (snap.engagedCombat)                { Disengage(Reason::Combat);      return; }
    if (!haveRoute || !snap.routeActive) {
        const AudioBeacon::StopReason sr = AudioBeacon::LastStopReason();
        Disengage(sr == AudioBeacon::StopReason::Arrived   ? Reason::Arrived
                : sr == AudioBeacon::StopReason::MapChange ? Reason::MapChange
                                                           : Reason::RouteLost);
        return;
    }
    g_lastLegIndex = snap.legIndex;
    g_lastLegCount = snap.legCount;

    FVec3 me;
    if (!PlayerState::IsFieldNavSafe() || !PlayerState::ReadPlayerPos(me)) {
        // Not a readable frame: publish nothing. The mask goes stale in 250 ms and injection stops
        // on its own; steering resumes when the field is readable again.
        return;
    }
    float facingRad = 0.0f;
    PlayerState::ReadCameraForwardStable(facingRad);

    // Ground actually covered (teleports rejected), and the no-progress cap on the DESTINATION.
    if (g_havePos) {
        const float dx = me.x - g_lastPos.x, dz = me.z - g_lastPos.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d < kWalkDeltaCapM) g_walkedM += d;
    }
    g_lastPos = me;
    g_havePos = true;

    const float destDist = NavCommon::Distance2D(me, snap.finalTarget);
    if (g_bestDestDist < 0.0f || destDist < g_bestDestDist - kProgressM) {
        g_bestDestDist  = destDist;
        g_lastImproveMs = now;
    } else if (now - g_lastImproveMs >= kNoProgressMs) {
        Disengage(Reason::NoProgress);
        return;
    }

    // ---- steering: render the SAME bearing the spoken legs and the pan render (S92 rule) --------
    int desired = NavCommon::RelativeOctant(me, snap.legTarget, facingRad);
    if (g_octant >= 0 && desired != g_octant) {
        const float rel = NavCommon::RelativeBearingDeg(me, snap.legTarget, facingRad);
        float d = rel - static_cast<float>(g_octant) * 45.0f;
        while (d > 180.0f)  d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        const bool past     = std::fabs(d) > 22.5f + kHysteresisDeg;
        const int  odist    = std::abs(desired - g_octant);
        const bool adjacent = (odist == 1 || odist == 7);
        if (!past)                                              desired = g_octant;
        else if (adjacent && now - g_octantSinceMs < kMinHoldMs) desired = g_octant;
    }
    if (desired != g_octant) { g_octant = desired; g_octantSinceMs = now; }

    g_keyMask.store(kOctMask[g_octant & 7], std::memory_order_relaxed);
    g_maskStampMs.store(now, std::memory_order_relaxed);
}

} // namespace AutoWalk

#include "navigation/audio_beacon.h"
#include "navigation/player_state.h"
#include "navigation/path_planner.h"
#include "navigation/nav_common.h"
#include "audio/audio_engine.h"
#include "audio/audio_clips.h"
#include "battle/battle_state.h"
#include "core/phyre_types.h"
#include "core/mem_read.h"
#include "core/logger.h"
#include "ui/mod_menu.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>

namespace AudioBeacon {
namespace {

// ---- tuning ------------------------------------------------------------------------------------
// World units are ~metres; NavCommon::g_unitsPerStep is 0.75, so 20 m is about 27 steps.
constexpr float  kFarInterval  = 1.00f;   // seconds between pings at kFarDist or beyond
constexpr float  kNearInterval = 0.20f;   // ...and when practically on top of the target
constexpr float  kFarDist      = 20.0f;
constexpr float  kNearDist     = 2.0f;
// Arriving at a leg corner. XZ is generous because the corners come from a 1.5 m simplify
// tolerance; the Y band matches path_planner's kAtExitDy, which exists to separate storeys.
constexpr float  kLegReached   = 2.0f;
constexpr float  kLegReachedDy = 3.0f;
// The arrival cue: same sound, pitched up, once.
constexpr float  kArrivalPitch = 1.5f;
// Off-route detection. Deliberately slack -- this fires a whole re-plan, and a beacon that re-aims
// every time the player rounds a pillar would be worse than one that is briefly stale.
constexpr float  kStrayDist    = 6.0f;
constexpr int    kStrayFrames  = 45;      // ~0.75 s at 60 fps
constexpr uint64_t kReplanCooldownMs = 2000;

// ---- state (game thread except where noted) ----------------------------------------------------
std::atomic<bool>  g_active{false};       // read first thing every frame, hence atomic
std::vector<FVec3> g_legs;
size_t             g_current   = 0;
uint32_t           g_epoch     = 0;
FVec3              g_legStart  {};        // where the current leg began, for the stray test
uint64_t           g_nextPingMs = 0;
int                g_strayCount = 0;
uint64_t           g_lastReplanMs = 0;
bool               g_wasEngaged = false;  // edge-detect combat so the log says when it flipped

// Distance -> repeat period. Linear between the two anchors, clamped outside them.
float IntervalFor(float dist) {
    if (dist >= kFarDist)  return kFarInterval;
    if (dist <= kNearDist) return kNearInterval;
    const float t = (dist - kNearDist) / (kFarDist - kNearDist);   // 0 near .. 1 far
    return kNearInterval + t * (kFarInterval - kNearInterval);
}

constexpr float kDegToRad = 3.14159265358979f / 180.0f;

// Bearing to `to` in the player's movement frame -> the pan/front pair AudioEngine wants.
//
// facingRad is the camera forward, which is the frame an UP push acts in and the frame the spoken
// legs are phrased in. The angle comes from NavCommon::RelativeBearingDeg -- the SAME call the
// spoken octant word is a rendering of -- so the ping and the words cannot disagree.
//
// SESSION 92 BUG, fixed here: this used to compute its own `atan2(dx, dz) - facingRad`, which is
// the exact NEGATION of NavCommon's `atan2(dx, -dz) - CompassFaceDeg(facingRad)` (the Z axis is
// reflected AND the yaw enters as `180 - yaw`, not `-yaw`). Negating an angle leaves `cos` alone
// but flips `sin`, so front/back stayed correct while the pan came out MIRRORED: the route spoke
// "Northwest" and the beacon panned hard right. Do not re-introduce a local bearing here -- the
// plan for this feature called the pan and the word "two encodings of one value" that "must never
// be computed twice", and computing it twice is precisely what broke it.
void BearingToPan(const FVec3& from, const FVec3& to, float facingRad, float& pan, float& front) {
    const float dx = to.x - from.x;
    const float dz = to.z - from.z;
    if (std::fabs(dx) < 1e-4f && std::fabs(dz) < 1e-4f) { pan = 0.0f; front = 1.0f; return; }
    // 0 deg = forward, 90 = right, 180 = behind, 270 = left -> sin is the L/R axis, cos front/back.
    const float rel = NavCommon::RelativeBearingDeg(from, to, facingRad) * kDegToRad;
    pan   = std::sin(rel);
    front = std::cos(rel);
}

// Perpendicular distance from p to the segment a->b on the ground plane. Same helper shape as
// PathDirections::PerpDist; kept local because that one is in an anonymous namespace and this file
// needs only the one line of it.
float PerpDist(const FVec3& p, const FVec3& a, const FVec3& b) {
    const float vx = b.x - a.x, vz = b.z - a.z;
    const float wx = p.x - a.x, wz = p.z - a.z;
    const float len2 = vx * vx + vz * vz;
    if (len2 <= 1e-6f) return std::sqrt(wx * wx + wz * wz);
    float t = (wx * vx + wz * vz) / len2;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float dx = wx - t * vx, dz = wz - t * vz;
    return std::sqrt(dx * dx + dz * dz);
}

// The live world position of the unit the leader is committed to acting on, if any.
//
// This is the COMMITTED target -- what `;` speaks and what Controls.md calls the active combat
// target -- deliberately not the browse cursor at P+0x9FD8, which Session 49 caught sitting on one
// enemy while the commitment was on a third.
bool ActiveTargetPos(FVec3& out) {
    void* leader = BattleState::LeaderActor();
    if (!leader) return false;
    BattleState::Committed c = BattleState::CommittedTargetOf(leader);
    if (!c.valid || c.targetHandle == 0) return false;
    void* actor = BattleState::ActorForHandle(c.targetHandle);
    if (!actor) return false;
    void* sceneObj = MemRead::PtrAt(actor, PhyreTypes::ACTOR_SCENEOBJ);
    if (!sceneObj) return false;
    return PlayerState::ReadSceneObjectPos(sceneObj, out);
}

void ResetPhase() {
    g_nextPingMs = 0;          // 0 == ping on the very next frame
    g_strayCount = 0;
}

} // namespace

void Seed(const std::vector<FVec3>& legPoints, uint32_t epoch) {
    if (legPoints.empty()) { Stop(); return; }
    g_legs    = legPoints;
    g_current = 0;
    g_epoch   = epoch;
    g_strayCount = 0;
    g_lastReplanMs = GetTickCount64();
    ResetPhase();
    FVec3 p;
    g_legStart = PlayerState::ReadPlayerPos(p) ? p : legPoints.front();
    g_active.store(true, std::memory_order_release);

    // Log the FIRST leg's relative octant beside the pan the beacon will actually use, so a
    // pan-vs-word disagreement is a grep instead of a play session. Octant 0 = forward/"North",
    // 2 = right/"East", 4 = behind/"South", 6 = left/"West", clockwise -- so pan must share the sign
    // of the octant's half: octants 1-3 pan POSITIVE (right), 5-7 NEGATIVE (left), 0 and 4 ~zero.
    // Session 92 shipped with those inverted and it took the tester's ear to catch it.
    float facingRad = 0.0f;
    PlayerState::ReadCameraForwardStable(facingRad);
    float pan0 = 0.0f, front0 = 1.0f;
    BearingToPan(g_legStart, g_legs.front(), facingRad, pan0, front0);

    char m[240];
    snprintf(m, sizeof(m),
             "seed: %zu leg point(s), epoch=%u, goal=(%.1f,%.1f,%.1f); leg 1 octant=%d "
             "pan=%+.2f front=%+.2f",
             g_legs.size(), epoch, g_legs.back().x, g_legs.back().y, g_legs.back().z,
             NavCommon::RelativeOctant(g_legStart, g_legs.front(), facingRad), pan0, front0);
    Log::Write("BEACON", m);
}

void Stop() {
    if (g_active.exchange(false, std::memory_order_acq_rel)) {
        AudioEngine::SilenceAll();
        Log::Write("BEACON", "stop");
    }
    g_legs.clear();
    g_current = 0;
}

bool Active() { return g_active.load(std::memory_order_acquire); }

void OnGameFrame() {
    if (!g_active.load(std::memory_order_acquire)) return;    // O(1) idle cost, the common case

    if (!ModMenu::AudioBeaconOn() || !AudioEngine::Available()) { Stop(); return; }

    // The map changed under us. PathPlanner bumps the epoch on teardown, so this needs no hook of
    // its own and cannot be missed.
    if (g_epoch != PathPlanner::CurrentEpoch()) {
        Log::Write("BEACON", "map changed -> stop");
        Stop();
        return;
    }

    FVec3 me;
    if (!PlayerState::IsFieldNavSafe() || !PlayerState::ReadPlayerPos(me)) return;   // skip the frame

    float facingRad = 0.0f;
    PlayerState::ReadCameraForwardStable(facingRad);

    const uint64_t now = GetTickCount64();

    // ---- in combat, the beacon tracks the target instead of the route --------------------------
    // FFXII is seamless-battle, so this is a state the player walks into and out of rather than a
    // screen transition. The route is NOT discarded: legs and index survive untouched, and the
    // objective beacon resumes on the same leg the moment the party is clear.
    const bool engaged = BattleState::PartyEngaged();
    if (engaged != g_wasEngaged) {
        g_wasEngaged = engaged;
        Log::Write("BEACON", engaged ? "party engaged -> tracking active target"
                                     : "party clear -> resuming objective");
        ResetPhase();
    }

    if (engaged) {
        FVec3 tgt;
        if (!ActiveTargetPos(tgt)) {
            // Engaged but nothing committed: say nothing at all. A beacon still leading you to a
            // shop while something is chewing on you is worse than silence.
            AudioEngine::SilenceAll();
            g_nextPingMs = 0;
            return;
        }
        if (now < g_nextPingMs) return;
        const float dist = NavCommon::Distance2D(me, tgt);
        float pan = 0.0f, front = 1.0f;
        BearingToPan(me, tgt, facingRad, pan, front);
        AudioEngine::PlayPing(AudioClips::ActiveTarget(), pan, front, 1.0f, 1.0f);
        g_nextPingMs = now + static_cast<uint64_t>(IntervalFor(dist) * 1000.0f);
        return;   // no arrival cue in combat -- you do not "arrive" at an enemy
    }

    // ---- objective beacon ----------------------------------------------------------------------
    if (g_current >= g_legs.size()) { Stop(); return; }
    const FVec3 goal = g_legs[g_current];

    const float dist = NavCommon::Distance2D(me, goal);
    const float dy   = std::fabs(me.y - goal.y);

    if (dist <= kLegReached && dy <= kLegReachedDy) {
        const bool last = (g_current + 1 >= g_legs.size());
        if (last) {
            // Arrival: the same sound pitched up, once, then done.
            AudioEngine::PlayPing(AudioClips::Objective(), 0.0f, 1.0f, 1.0f, kArrivalPitch);
            Log::Write("BEACON", "arrived at destination -> final cue, stop");
            g_active.store(false, std::memory_order_release);   // not Stop(): let the cue ring out
            g_legs.clear();
            g_current = 0;
            return;
        }
        ++g_current;
        g_legStart = me;
        ResetPhase();
        char m[128];
        snprintf(m, sizeof(m), "leg reached -> advancing to leg %zu of %zu (silent)",
                 g_current + 1, g_legs.size());
        Log::Write("BEACON", m);
        return;
    }

    // Off-route: re-plan silently. Measured perpendicular to the leg the player is supposed to be
    // walking, not as raw distance to the corner -- walking the leg correctly increases the latter
    // for the whole first half of a dog-leg.
    if (PerpDist(me, g_legStart, goal) > kStrayDist) {
        if (++g_strayCount >= kStrayFrames && (now - g_lastReplanMs) >= kReplanCooldownMs) {
            g_strayCount   = 0;
            g_lastReplanMs = now;
            if (PathPlanner::RequestReplan()) {
                Log::Write("BEACON", "off route -> silent re-plan requested");
            }
        }
    } else {
        g_strayCount = 0;
    }

    if (now < g_nextPingMs) return;
    float pan = 0.0f, front = 1.0f;
    BearingToPan(me, goal, facingRad, pan, front);
    AudioEngine::PlayPing(AudioClips::Objective(), pan, front, 1.0f, 1.0f);
    g_nextPingMs = now + static_cast<uint64_t>(IntervalFor(dist) * 1000.0f);
}

} // namespace AudioBeacon

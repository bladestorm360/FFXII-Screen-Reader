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
#include "navigation/nav_blocked.h"
#include "navigation/auto_walk.h"
#include "input/input_tracker.h"
#include "ui/dialogue_reader.h"
#include "ui/ingame_menu_reader.h"
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
// BEHIND PITCHES THE PING DOWN TOO, and that lives in audio_engine.cpp beside the gain drop and the
// low-pass -- NOT here. It was briefly computed in this file from its own reading of `front`, which is
// the exact shape of the Session 92 pan bug: one fact derived twice, and the copies disagreed. Every
// ping below passes `front`, so both beacons get all three behind cues from one definition without
// either call site having to ask.
//
// Off-route detection. Deliberately slack -- this fires a whole re-plan, and a beacon that re-aims
// every time the player rounds a pillar would be worse than one that is briefly stale.
//
// WALL-CLOCK, AND THAT SLACK IS THE WHOLE DESIGN. This was `kStrayFrames = 45` -- "~0.75 s at
// 60 fps" -- which INVERTED the sentence above at any other rate: the field tick fires once per
// FUN_0022a770 call, so at 144 fps the deliberate slack becomes a third of a second, which is
// roughly "every time the player rounds a pillar". The intent survived only at exactly 60 fps.
// Game speed is not a factor -- 2x/4x runs the sim loop inside that one call more times.
constexpr float  kStrayDist    = 6.0f;
constexpr uint64_t kStrayMs    = 750;
constexpr uint64_t kReplanCooldownMs = 2000;

// ---- STUCK: trying to move and going nowhere -----------------------------------------------------
// The stray test above measures PERPENDICULAR distance from the leg, so it structurally cannot see
// this: a player jammed at the leg's own start is zero metres from the line. The tester walked into a
// wall, pressed `\` four times over twelve seconds from one position, and got the identical route
// every time, because nothing in the mod had noticed they had stopped moving.
//
// POSITION-BASED SINCE SESSION 100. The old gate was `InputTracker::MovementHeld()`, whose only
// writer is the DirectInput KEYBOARD buffer -- the mechanism could never fire for a pad player, and
// in the S99 log the player oscillated at one spot for ~40 s across eleven route requests without a
// single `stuck ->` line. The clock now runs whenever a route is live; FIRING needs evidence that
// movement was being ATTEMPTED, any of:
//   * a movement key held (keyboard players -- the old gate, kept);
//   * auto-walk engaged (the mod itself is commanding movement);
//   * accumulated 2D displacement >= kStuckMotionMinM since the clock started (pad players: pushing
//     a wall produces depenetration jitter and slides without closing; standing still accumulates
//     ~nothing, so an idle player can NEVER read as stuck -- no replan storm, no NavBlocked spam).
// Accepted blind spot, so it is not rediscovered: a pad player pushing PERFECTLY head-on produces
// zero slide and no readable input; nothing observational can see that case (S93: head-on cancels).
constexpr float    kProgressEpsilon = 0.6f;    // metres of closing that counts as progress
constexpr uint64_t kStuckMs         = 1800;    // trying, no closing -> stuck
constexpr float    kStuckMotionMinM = 1.0f;    // displacement that proves movement was attempted
constexpr float    kMotionTeleportM = 5.0f;    // per-frame delta above this = teleport, not walking

// ---- state (game thread except where noted) ----------------------------------------------------
std::atomic<bool>  g_active{false};       // read first thing every frame, hence atomic
std::vector<FVec3> g_legs;
size_t             g_current   = 0;
uint32_t           g_epoch     = 0;
FVec3              g_legStart  {};        // where the current leg began, for the stray test
uint64_t           g_nextPingMs = 0;
uint64_t           g_straySinceMs = 0;   // first frame of the CURRENT stray run; 0 == on route
uint64_t           g_lastReplanMs = 0;
bool               g_wasEngaged = false;  // edge-detect combat so the log says when it flipped
bool               g_wasEscaping = false; // edge-detect escape mode, for the same reason (S179)
// Why the beacon is currently suspended, or null when it is not. Compared BY POINTER on purpose:
// every value is one of the string literals below, so identity is a clean state test and the log
// line fires on the TRANSITION only. That is a state machine detecting a change, not a speech dedup
// (nothing here speaks) -- see the CLAUDE.md carve-out for exactly this shape.
const char*        g_busyWhy    = nullptr;
float              g_stuckBestDist = -1.0f;   // closest we have come to the current leg point
uint64_t           g_stuckSinceMs  = 0;       // when that closest approach happened
float              g_motionAccum   = 0.0f;    // 2D displacement since the stuck clock (re)started
FVec3              g_lastPos{};               // previous frame's position, for the accumulator
bool               g_motionValid   = false;   // g_lastPos holds a real position
StopReason         g_lastStopReason = StopReason::None;
// S183: automatic re-plans held on this leg after one failed and the route was kept (HoldAutoReplans).
// `g_holdLogged` makes the "held" log line fire once per hold -- a transition, not a speech dedup.
bool               g_replanHeld     = false;
size_t             g_replanHeldLeg  = 0;
bool               g_holdLogged     = false;

void ClearReplanHold() {
    g_replanHeld = false;
    g_holdLogged = false;
}

// True when this frame's stuck / off-route re-plan must not be requested. Logs the first refusal.
bool ReplanHeld(const char* which) {
    if (!g_replanHeld || g_replanHeldLeg != g_current) return false;
    if (!g_holdLogged) {
        g_holdLogged = true;
        char m[200];
        snprintf(m, sizeof(m),
                 "%s on leg %zu/%zu -- automatic re-plan HELD: the last one from this leg found no route and "
                 "the live route was kept (clears at this corner, on a new route, or on the player's own \\)",
                 which, g_current + 1, g_legs.size());
        Log::Write("BEACON", m);
    }
    return true;
}

// Distance -> repeat period. Linear between the two anchors, clamped outside them.
float IntervalFor(float dist) {
    if (dist >= kFarDist)  return kFarInterval;
    if (dist <= kNearDist) return kNearInterval;
    const float t = (dist - kNearDist) / (kFarDist - kNearDist);   // 0 near .. 1 far
    return kNearInterval + t * (kFarInterval - kNearInterval);
}

// BEARING -> PAN MOVED TO NavCommon (Session 191), with `kDegToRad`, which had no other user here.
// It was private to this file until the soundscape needed the identical two lines; the reasoning
// that used to sit here now sits on NavCommon::BearingToPan, where both callers can read it.
//
// The short version, because it is the one thing never to undo: `facingRad` is the camera forward,
// which is the frame an UP push acts in and the frame the spoken legs are phrased in, and the angle
// comes from NavCommon::RelativeBearingDeg -- the SAME call the spoken octant word is a rendering
// of -- so the ping and the words cannot disagree. Do NOT re-introduce a local bearing here: the
// Session 92 bug was exactly that, and `cos` being even hid the flipped `sin` for a whole release.

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
// The live world position of an actor the engagement scan already resolved.
//
// The actor arrives ALREADY RESOLVED, from the same scan that decided we are in combat -- so the beacon
// structurally cannot ping at a unit other than the one that put it in combat mode. It used to walk
// LeaderActor -> CommittedTargetOf -> ActorForHandle itself, a second derivation of a value the gate had
// already computed, and that split was the whole shape of the S92 bug.
//
// Deliberately NOT the browse cursor at P+0x9FD8, which Session 49 caught sitting on one enemy while the
// commitment was on a third.
bool TargetPos(void* actor, FVec3& out) {
    if (!actor) return false;
    void* sceneObj = MemRead::PtrAt(actor, PhyreTypes::ACTOR_SCENEOBJ);
    if (!sceneObj) return false;
    return PlayerState::ReadSceneObjectPos(sceneObj, out);
}

void ResetPhase() {
    g_nextPingMs = 0;          // 0 == ping on the very next frame
    g_straySinceMs  = 0;
    g_stuckBestDist = -1.0f;   // a new leg: nothing has been approached yet
    g_stuckSinceMs  = 0;
    g_motionAccum   = 0.0f;
    g_motionValid   = false;
}

} // namespace

void Seed(const std::vector<FVec3>& legPoints, uint32_t epoch) {
    if (legPoints.empty()) { Stop(StopReason::External); return; }   // a failed plan/re-plan
    g_lastStopReason = StopReason::Reseeded;   // a NEW route replaced the old one
    g_legs    = legPoints;
    g_current = 0;
    g_epoch   = epoch;
    g_lastReplanMs = GetTickCount64();
    ClearReplanHold();          // a new route: its legs have not been re-planned from yet
    ResetPhase();               // owns g_straySinceMs, along with the leg and stuck state
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
    NavCommon::BearingToPan(g_legStart, g_legs.front(), facingRad, pan0, front0);

    char m[240];
    snprintf(m, sizeof(m),
             "seed: %zu leg point(s), epoch=%u, goal=(%.1f,%.1f,%.1f); leg 1 octant=%d "
             "pan=%+.2f front=%+.2f",
             g_legs.size(), epoch, g_legs.back().x, g_legs.back().y, g_legs.back().z,
             NavCommon::RelativeOctant(g_legStart, g_legs.front(), facingRad), pan0, front0);
    Log::Write("BEACON", m);
}

void Stop(StopReason reason) {
    if (g_active.exchange(false, std::memory_order_acq_rel)) {
        AudioEngine::SilenceBeacon();
        Log::Write("BEACON", "stop");
        g_lastStopReason = reason;
    }
    g_legs.clear();
    g_current = 0;
    ClearReplanHold();
}

bool Active() { return g_active.load(std::memory_order_acquire); }

bool RemainingCorners(std::vector<FVec3>& out) {
    out.clear();
    if (!g_active.load(std::memory_order_acquire) || g_current >= g_legs.size()) return false;
    out.assign(g_legs.begin() + static_cast<ptrdiff_t>(g_current), g_legs.end());
    return true;
}

void HoldAutoReplans() {
    if (!g_active.load(std::memory_order_acquire)) return;
    g_replanHeld    = true;
    g_replanHeldLeg = g_current;
    g_holdLogged    = false;
}

StopReason LastStopReason() { return g_lastStopReason; }

bool GetLegSnapshot(LegSnapshot& out) {
    out = LegSnapshot{};
    out.routeActive   = g_active.load(std::memory_order_acquire);
    out.engagedCombat = g_wasEngaged;
    out.epoch         = g_epoch;
    if (!out.routeActive || g_current >= g_legs.size()) return false;
    out.legIndex    = g_current;
    out.legCount    = g_legs.size();
    out.legStart    = g_legStart;
    out.legTarget   = g_legs[g_current];
    out.finalTarget = g_legs.back();
    FVec3 me;
    if (PlayerState::ReadPlayerPos(me)) {
        float len = NavCommon::Distance2D(me, g_legs[g_current]);
        for (size_t i = g_current + 1; i < g_legs.size(); ++i)
            len += NavCommon::Distance2D(g_legs[i - 1], g_legs[i]);
        out.routeLenM = len;
    }
    return true;
}

void OnGameFrame() {
    // ---- O(1) idle -------------------------------------------------------------------------------
    // TWO INDEPENDENT REASONS TO RUN (Session 95). The target ping used to live behind `g_active`, so
    // it only ever sounded if a route beacon happened to be running -- the tester asked for it to be
    // its own feature, on its own switch, playing "regardless of whether or not there was a beacon
    // before". Both loads are relaxed atomics; this is still the cheap common case.
    // FOLLOWING A ROUTE IS NAVIGATION; PLAYING A SOUND IS AUDIO (Session 96). These used to be one
    // flag: the very first line returned unless the beacon SETTING was on, and the kill switch called
    // Stop(), which clears the legs. So turning a sound off threw the navigation state away, and with
    // the beacon off the mod had no idea where the player was on the route and could not notice them
    // getting stuck. `objective` now means "a route is being followed" and is independent of whether
    // anything is audible.
    const bool audioUp   = AudioEngine::Available();
    const bool routeAudio = audioUp && ModMenu::AudioBeaconOn();
    const bool targetOn   = audioUp && ModMenu::TargetBeaconOn();
    bool objective = g_active.load(std::memory_order_acquire);
    if (!objective && !targetOn) return;

    // The map changed under us. PathPlanner bumps the epoch on teardown, so this needs no hook of
    // its own and cannot be missed. Only the route is map-bound; an enemy you are fighting is not.
    if (objective && g_epoch != PathPlanner::CurrentEpoch()) {
        Log::Write("BEACON", "map changed -> stop");
        Stop(StopReason::MapChange);
        objective = false;
    }
    if (!objective && !targetOn) return;

    FVec3 me;
    if (!PlayerState::IsFieldNavSafe() || !PlayerState::ReadPlayerPos(me)) return;   // skip the frame

    // ---- SUSPEND WHILE THE PLAYER IS NOT DRIVING (Session 157) ---------------------------------
    //
    // EVERY GATE ABOVE THIS LINE ASKS WHETHER THE FIELD EXISTS. None of them asks whether the player
    // is in control of it. `IsFieldNavSafe()` is six LIVENESS predicates -- field sim live, module
    // started, actor pool, leader pointer, world, leader object -- and every one stays true through a
    // conversation and through a cutscene. So the beacon pinged through both, and had done since it
    // was written. **This is not the S152 stray-timer change; the gate never existed.**
    //
    // The field pause menu was quiet only BY ACCIDENT: opening it stops the field tick that calls
    // this function at all. The battle command menu does not stop it -- and FFXII lets that menu be
    // opened OUT OF COMBAT on any map where battles can happen, so `PartyEngagement()` reads clear
    // and the objective beacon kept running underneath it. That is exactly where the tester still
    // had it firing, and it is why "in combat" was never the right question.
    //
    // BOTH beacons are covered, deliberately: this sits ahead of the combat branch, so the target
    // ping is silenced by an open command menu too (tester's call).
    //
    // SUSPEND, NEVER STOP. `Stop()` throws the legs away; a player closing a menu expects to be back
    // on the same leg, the same way they are after a fight -- which is how the combat branch below
    // has always behaved. Silence what is ringing and drop the ping phase so nothing bursts on resume.
    //
    // Both predicates are the GAME's own state, not mod-side bookkeeping: `BattleCommandActive()`
    // re-validates its panel against the window class on every read, and `IsBoxLive()` asks the
    // engine's message-window registry. Note what is deliberately NOT used --
    // `MenuState::IsAnyMenuOpen()` reads a global the decompile writes once and never clears, so it
    // answers "open" forever; a gate built on it once killed the field object scan for a whole fight.
    {
        const char* busy = nullptr;
        if (IngameMenuReader::BattleCommandActive()) busy = "battle command menu open";
        else if (DialogueReader::IsBoxLive())        busy = "dialogue or message box on screen";

        if (busy != g_busyWhy) {
            g_busyWhy = busy;
            char m[160];
            snprintf(m, sizeof(m), "%s%s", busy ? "suspended -- " : "resumed", busy ? busy : "");
            Log::Write("BEACON", m);
        }
        if (busy) {
            AudioEngine::SilenceBeacon();
            g_nextPingMs = 0;
            return;
        }
    }

    float facingRad = 0.0f;
    PlayerState::ReadCameraForwardStable(facingRad);

    const uint64_t now = GetTickCount64();

    // ---- in combat, the beacon tracks the target instead of the route --------------------------
    // FFXII is seamless-battle, so this is a state the player walks into and out of rather than a
    // screen transition. The route is NOT discarded: legs and index survive untouched, and the
    // objective beacon resumes on the same leg the moment the party is clear. That half is unchanged.
    const BattleState::Engagement eng = BattleState::PartyEngagement();
    const bool engaged = eng.engaged;
    if (engaged != g_wasEngaged) {
        g_wasEngaged = engaged;
        // BOTH HALVES ARE LOGGED, because the S92 fix was exactly about which one fired. One play
        // session proves the player-attacks-first case flips the beacon (`committed=1 targeted=0`),
        // proves an out-of-combat ally heal does NOT (the S49 filter), and makes any flapping visible
        // as repeated engaged/clear pairs rather than as a vague report.
        char m[192];
        snprintf(m, sizeof(m), "%s (targeted=%d committed=%d action=0x%04X)",
                 engaged  ? "party engaged -> tracking active target"
                 : objective ? "party clear -> resuming objective"
                             : "party clear -> idle (no objective route)",
                 eng.targeted ? 1 : 0, eng.committed ? 1 : 0, eng.actionId);
        Log::Write("BEACON", m);
        ResetPhase();
    }

    // ESCAPE MODE ALWAYS RESUMES THE ROUTE BEACON (S179, the user's rule: "when escape mode is on, the
    // beacon should ***always*** resume" -- the route beacon, not the target one). A fleeing party is
    // still `engaged`: foes keep targeting it until the flight actually breaks away, which is the
    // deviation S92 recorded. So while escape is on, the combat branch below is skipped outright and
    // the frame falls through to the objective beacon on the same leg. No route running: silence, as
    // out of combat. The flag is the game's own (BattleState::EscapeModeOn), logged on every change.
    const bool escaping = BattleState::EscapeModeOn();
    if (escaping != g_wasEscaping) {
        g_wasEscaping = escaping;
        char m[160];
        snprintf(m, sizeof(m), "escape mode %s (engaged=%d, objective=%d)%s",
                 escaping ? "ON" : "off", engaged ? 1 : 0, objective ? 1 : 0,
                 escaping ? (objective ? " -> objective beacon resumes" : " -> no route to resume")
                          : (engaged ? " -> back to the target beacon" : ""));
        Log::Write("BEACON", m);
        ResetPhase();
    }

    if (engaged && !escaping) {
        FVec3 tgt;
        // Only a COMMITTED target is ever pinged. When the party is merely being attacked with nothing
        // committed the beacon stays silent -- the tester's decision in S92, and the reason
        // `eng.targetActor` is null in that state rather than filled with the attacker.
        if (!targetOn || !TargetPos(eng.targetActor, tgt)) {
            // Engaged but nothing committed, or the target ping switched off: say nothing at all. A
            // beacon still leading you to a shop while something is chewing on you is worse than
            // silence, so the route half stays suspended here either way.
            AudioEngine::SilenceBeacon();
            g_nextPingMs = 0;
            return;
        }
        if (now < g_nextPingMs) return;
        const float dist = NavCommon::Distance2D(me, tgt);
        float pan = 0.0f, front = 1.0f;
        NavCommon::BearingToPan(me, tgt, facingRad, pan, front);
        AudioEngine::PlayPing(AudioClips::Get(AudioClips::Sound::ActiveTarget), pan, front,
                              ModMenu::TargetVolume(), 1.0f);
        g_nextPingMs = now + static_cast<uint64_t>(IntervalFor(dist) * 1000.0f);
        return;   // no arrival cue in combat -- you do not "arrive" at an enemy
    }

    // ---- objective beacon ----------------------------------------------------------------------
    // Reached when out of combat, or in escape mode (S179). With the target ping on and no route running, this is where
    // the frame ends -- there is nothing to lead anybody along.
    if (!objective) return;
    if (g_current >= g_legs.size()) { Stop(); return; }
    const FVec3 goal = g_legs[g_current];

    const float dist = NavCommon::Distance2D(me, goal);
    const float dy   = std::fabs(me.y - goal.y);

    if (dist <= kLegReached && dy <= kLegReachedDy) {
        const bool last = (g_current + 1 >= g_legs.size());
        if (last) {
            // Arrival: the same sound pitched up, once, then done. Centred and ahead by construction,
            // so the behind cue cannot apply -- the arrival pitch is the whole point of this one.
            if (routeAudio)
                AudioEngine::PlayPing(AudioClips::Get(AudioClips::Sound::Objective), 0.0f, 1.0f,
                                      ModMenu::BeaconVolume(), kArrivalPitch);
            Log::Write("BEACON", "arrived at destination -> final cue, stop");
            g_lastStopReason = StopReason::Arrived;
            g_active.store(false, std::memory_order_release);   // not Stop(): let the cue ring out
            g_legs.clear();
            g_current = 0;
            return;
        }
        ++g_current;
        // THE LEG'S START IS THE PREVIOUS CORNER, NOT WHERE THE PLAYER HAPPENED TO BE (Session 93).
        //
        // This used to be `g_legStart = me`, and that is what made `off route -> silent re-plan
        // requested` fire every 3-7 seconds during ordinary walking. The stray test measures the
        // player's perpendicular distance from `g_legStart -> goal`, so seeding it with the player's own
        // position defines the reference line as "wherever I was standing when the leg advanced" rather
        // than as the route's leg. Advance a leg while standing a few metres to one side -- which is
        // normal, since a leg completes on a radius -- and the reference line is skewed by that offset;
        // walking the ACTUAL route then reads as deviation, trips the re-plan, and the re-plan re-seeds
        // the same skew. A self-sustaining loop that cost a game-thread search every few seconds and
        // reset the ping phase each time.
        //
        // The route's own corners are right here in g_legs, so the leg is corner[N-1] -> corner[N].
        g_legStart = g_legs[g_current - 1];
        ClearReplanHold();      // a new leg: a re-plan from here is a new question
        ResetPhase();
        char m[128];
        snprintf(m, sizeof(m), "leg reached -> advancing to leg %zu of %zu (silent)",
                 g_current + 1, g_legs.size());
        Log::Write("BEACON", m);
        return;
    }

    // ---- STUCK: trying to move and getting no closer -------------------------------------------
    // Distinct from the stray test below in the one way that matters: this measures CLOSING on the
    // leg point, so it fires for a player jammed at the leg's own start, where perpendicular distance
    // is zero and the stray test is blind by construction. POSITION-BASED since Session 100 -- see
    // the constants block for the firing evidence and the accepted blind spot.
    {
        // Per-frame displacement accumulator (teleports and map snaps rejected, not accumulated).
        if (g_motionValid) {
            const float ddx = me.x - g_lastPos.x, ddz = me.z - g_lastPos.z;
            const float d = std::sqrt(ddx * ddx + ddz * ddz);
            if (d < kMotionTeleportM) g_motionAccum += d;
        }
        g_lastPos     = me;
        g_motionValid = true;
    }
    if (g_stuckBestDist < 0.0f || dist < g_stuckBestDist - kProgressEpsilon) {
        g_stuckBestDist = dist;              // real progress -- restart the clock
        g_stuckSinceMs  = now;
        g_motionAccum   = 0.0f;
    } else if (g_stuckSinceMs != 0 && (now - g_stuckSinceMs) >= kStuckMs &&
               (now - g_lastReplanMs) >= kReplanCooldownMs &&
               (InputTracker::MovementHeld() || AutoWalk::Engaged() ||
                g_motionAccum >= kStuckMotionMinM) &&
               !ReplanHeld("stuck")) {
        g_lastReplanMs  = now;
        g_stuckSinceMs  = now;
        const float motion   = g_motionAccum;
        const bool  walking  = AutoWalk::Engaged();
        g_motionAccum   = 0.0f;
        // RECORD IT FIRST, then re-plan -- the record is what makes the player's own next `\`
        // come back with a different route, which matters more than this silent re-plan does.
        NavBlocked::Note(me, g_epoch);
        char m[224];
        snprintf(m, sizeof(m),
                 "stuck -> blocked spot recorded and re-planning: %.1fs with no progress "
                 "on leg %zu/%zu, %.1fm from its corner, motion=%.1fm%s",
                 kStuckMs / 1000.0f, g_current + 1, g_legs.size(), dist, motion,
                 walking ? " [auto-walk]" : "");
        Log::Write("BEACON", m);
        // The ground-truth line the pathing track consumes: exactly where the engine refused a
        // commanded walk. AutoWalk owns its wording (poly, injected direction, replan count).
        if (walking) AutoWalk::OnStuckFired(me, g_current, g_legs.size(), dist);
        PathPlanner::RequestReplan();
        return;
    }

    // Off-route: re-plan silently. Measured perpendicular to the leg the player is supposed to be
    // walking, not as raw distance to the corner -- walking the leg correctly increases the latter
    // for the whole first half of a dog-leg.
    const float perp = PerpDist(me, g_legStart, goal);
    if (perp > kStrayDist) {
        if (g_straySinceMs == 0) g_straySinceMs = now;
        if ((now - g_straySinceMs) >= kStrayMs && (now - g_lastReplanMs) >= kReplanCooldownMs &&
            !ReplanHeld("off route")) {
            g_straySinceMs = 0;
            g_lastReplanMs = now;
            if (PathPlanner::RequestReplan()) {
                // Print the MEASUREMENT, not just the event. A re-plan storm and a genuine detour look
                // identical in a bare line, which is why the previous one sat in the log for a whole
                // session without anyone reading it as a defect.
                char m[176];
                snprintf(m, sizeof(m),
                         "off route -> silent re-plan requested: %.1fm perpendicular to leg %zu/%zu "
                         "(%.1f,%.1f)->(%.1f,%.1f), threshold %.1fm",
                         perp, g_current + 1, g_legs.size(),
                         g_legStart.x, g_legStart.z, goal.x, goal.z, kStrayDist);
                Log::Write("BEACON", m);
            }
        }
    } else {
        // BACK ON ROUTE -- DISARM. This clear is load-bearing and is the half the audit's sketch
        // left out: the test measures ONE CONTINUOUS stray run, exactly as the frame counter did.
        // Without it the deadline would mean "has been stray at some point in the last 750 ms",
        // which is looser than the code it replaced and would fire on a player oscillating across
        // the 6 m boundary.
        g_straySinceMs = 0;
    }

    // TRACKING IS DONE; the rest of this function is sound. With the route beacon switched off the
    // leg advance, the stray test and the stuck detector above have all still run.
    if (!routeAudio) return;
    if (now < g_nextPingMs) return;
    float pan = 0.0f, front = 1.0f;
    NavCommon::BearingToPan(me, goal, facingRad, pan, front);
    AudioEngine::PlayPing(AudioClips::Get(AudioClips::Sound::Objective), pan, front,
                          ModMenu::BeaconVolume(), 1.0f);
    g_nextPingMs = now + static_cast<uint64_t>(IntervalFor(dist) * 1000.0f);
}

} // namespace AudioBeacon

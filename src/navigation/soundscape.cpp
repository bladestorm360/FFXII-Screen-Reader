#include "navigation/soundscape.h"

#include "audio/audio_clips.h"
#include "audio/audio_engine.h"
#include "core/logger.h"
#include "navigation/entity_list.h"
#include "navigation/nav_common.h"
#include "navigation/path_planner.h"
#include "navigation/player_state.h"
#include "ui/dialogue_reader.h"
#include "ui/ingame_menu_reader.h"
#include "ui/mod_menu.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <vector>

namespace Soundscape {
namespace {

using EntityList::Category;

// ---- tuning --------------------------------------------------------------------------------------
// RADIUS, and it is the ONLY limit there is. The user's number, and their ruling on the nearest-N cap
// an earlier draft had: *"not nearest 10 interactibles. there should be no limit other than the 20
// step range."* Kept in STEPS and converted through NavCommon, because a step is the unit every spoken
// distance in the mod already uses -- writing 15.0f here would silently stop meaning "20 steps" the
// moment `g_unitsPerStep` is ever re-measured.
constexpr float kRadiusSteps = 20.0f;

// PERIOD and PITCH per slot -- the user's specification, transcribed:
//     NPC1: sound plays 1S apart at normal pitch
//     NPC2: plays 1.1S apart at 5% pitch increase
// so slot N plays every (1000 + 100*N) ms, and slot 1 is +5%.
//
// THE PERIOD IS WHAT KEEPS THEM OFF EACH OTHER, and it needs no scheduler to do it: 1.0 s and 1.1 s
// drift 100 ms apart every cycle and then wrap, so two voices cannot stay on top of one another. The
// pitch is what makes them two *identifiable* things rather than one thing with a stutter.
//
// PITCH GOES 30% EACH WAY, ALTERNATING OUTWARD FROM NORMAL (user's instruction, 2026-09-18: *"must be
// 30% each way, higher and lower"*). Slot 0 is unshifted, then +5, -5, +10, -10 ... out to +-30%,
// which is thirteen distinguishable voices per category rather than the seven that climbing in one
// direction gave. Alternating keeps the user's own example intact (slot 1 IS +5%) and puts adjacent
// slots on opposite sides of normal, so the two nearest of a kind are 10% apart rather than 5%.
//
// +-30% IS A CEILING, NOT A STARTING POINT: past roughly that much a clip stops sounding like itself,
// which would trade "which NPC is that" for "what was that", a worse confusion than the one the pitch
// is here to solve. Slots past the ceiling share the extremes and are told apart by period alone.
constexpr uint64_t kBasePeriodMs = 1000;
constexpr uint64_t kPeriodStepMs = 100;
constexpr float    kPitchStep    = 0.05f;
constexpr int      kPitchMaxStep = 6;      // 6 * 5% = 30% up and 30% down

// A VOICE MUST FINISH BEFORE IT REPEATS, and that is not automatic -- it is arithmetic between two
// numbers nobody thinks of together. `npc` is 1.307 s long and slot 0's period is 1.0 s, so the
// nearest NPC was re-triggering 0.3 s before its own sound had finished, layering the same clip on
// itself at the same pitch through a second voice of the pool. That was true the moment this file
// was first written; PITCHING DOWN MAKES IT WORSE AND MORE COMMON, because a clip at 0.70x runs 43%
// LONGER -- at the floor, four of the ten sounds outlast a one-second period.
//
// So the period has a FLOOR at "however long this clip actually is at this slot's pitch, plus a gap",
// and the per-slot step is added on top of that floor rather than to a bare constant. Short clips --
// six of the ten, including every one a map is likely to have several of -- never reach the floor and
// keep the user's 1.0 / 1.1 / 1.2 s exactly. Long ones stretch only as far as they must.
//
// The floor is computed per SLOT, not per category, which matters: two slots of one category must not
// come out with the same period or the drift that keeps them apart stops working. Adding the step
// after the floor guarantees they differ even when both are floored.
constexpr uint64_t kMinGapMs = 200;        // silence between one voice's own repeats

// INITIAL PHASE. A voice joining the soundscape starts a beat after whoever joined last, so a map load
// or a walk round a corner does not fire thirty sounds on one frame. It only has to break the initial
// pile-up -- the differing periods do the rest from there, forever.
constexpr uint64_t kPhaseStepMs = 70;
constexpr int      kPhaseWrap   = 16;      // offsets cycle 0..15 * 70 ms, i.e. just over a second

// MEMBERSHIP refresh: how often "who is within the radius" is re-asked. NOT how often anything sounds,
// and NOT how fresh a voice's direction is -- a ping reads its own entity's live position at the
// instant it plays. 200 ms is four times a second for a boundary nobody can hear themselves cross, and
// it is the one periodic cost in this file.
constexpr uint64_t kTrackRefreshMs = 200;

// A track survives this long after the last refresh that saw it in range. Hysteresis, and it is load-
// bearing: without it an entity hovering at exactly 20 steps would lose and regain its slot over and
// over, and since the slot IS the voice's pitch and period, that would be an audible stutter of
// identity rather than a quiet edge. It also rides out the entity list's own streaming noise.
constexpr uint64_t kDropGraceMs = 1500;

// Safety bounds, NOT design caps -- the user's instruction is that the radius is the only limit.
//   kMaxTracked        bounds the gather vector. Sized past any plausible crowd; if it ever clips,
//                      that is a real loss of entities and the log says so.
//   kMaxStartsPerFrame is a cost valve, not a cull. A voice that cannot start this frame starts on the
//                      next one, ~7-16 ms later, which is inaudible -- but it means a hundred voices
//                      coming due together can never become a hundred interleaves in one game frame.
constexpr int kMaxTracked        = 96;
constexpr int kMaxStartsPerFrame = 4;

// DISTANCE -> GAIN. Full at the player's feet, kFarGain at the edge of the radius, linear between.
// It does not fall to zero: an entity at 19 steps is still inside the radius the player asked to hear,
// and fading it to silence would make the radius mean something different from what it says.
constexpr float kFarGain = 0.30f;

// ---- state (GAME THREAD ONLY; g_sounding is atomic only so Stop can read it as a plain flag) -------
//
// ONE TRACK PER AUDIBLE ENTITY, living for as long as that entity is in range. This is the whole
// difference between this design and a sweep: there is no global cycle, no ordering and no shared
// phase -- each voice is an independent clock, and the list is only ever walked to ask "is this one
// due yet".
struct Track {
    void*                   id      = nullptr;  // scene object; identity AND the live-position handle.
                                                // Null for fixed exits -- see `pos`.
    Category                cat     = Category::Object;
    const AudioClips::Clip* clip    = nullptr;  // resolved once, on arrival
    int                     slot    = 0;        // index among its CATEGORY -> period + pitch
    uint64_t                period  = kBasePeriodMs;
    float                   pitch   = 1.0f;
    uint64_t                nextMs  = 0;        // when this voice next sounds
    uint64_t                seenMs  = 0;        // last membership refresh that found it in range
    bool                    inRange = false;    // ...that refresh's verdict. A track out of range keeps
                                                // its slot for the grace window but stays silent.
    FVec3                   pos{};              // last known position. For a fixed exit this IS the
                                                // position: there is no scene node to re-read.
};

std::atomic<bool>  g_sounding{false};   // something of ours may be ringing; read from Stop()
std::vector<Track> g_tracks;
uint64_t           g_nextRefreshMs = 0;
uint64_t           g_phaseSeq      = 0;      // hands out initial offsets; global, not per category
uint32_t           g_epoch         = 0;      // PathPlanner map epoch the tracks belong to
bool               g_wasOn         = false;
bool               g_clipWarned    = false;  // the "gather is full" line, once per set of tracks
// Why the soundscape is currently suspended, or null. Compared BY POINTER, so the log line fires on
// the TRANSITION only: every value is one of the string literals below. That is a state machine
// detecting a change, not a speech dedup -- nothing here speaks. Same shape as the beacon's.
const char*        g_busyWhy       = nullptr;

// Scratch for the membership gather, kept alive so a refresh costs no allocation.
std::vector<EntityList::NearbyEntity> g_near;

// ---- category -> sound ------------------------------------------------------------------------------
// The whole mapping, in one place. `Category::All` is a filter pseudo-category and never appears on an
// entity; `Category::Trap` has NO sound and returns null, which keeps it out of the soundscape.
//
// TRAPS ARE SILENT ON PURPOSE, and it is not an oversight to fix by borrowing a neighbour's sound.
// There is no trap sound in `FF 12 SFX\`, every sound that IS there already means a specific thing,
// and a floor trap that announced itself with the interactable chime would be telling the player
// something false about a thing that hurts them. Lessons `L-35` / `L-37`: ship nothing rather than
// ship wrong. When a trap sound exists, it is one row here and one line in the .rc.
const AudioClips::Clip* CategorySound(Category c) {
    switch (c) {
        case Category::Exit:        return AudioClips::Get(AudioClips::Sound::Exit);
        case Category::Door:        return AudioClips::Get(AudioClips::Sound::Door);
        case Category::Shop:        return AudioClips::Get(AudioClips::Sound::Shop);
        case Category::SaveCrystal: return AudioClips::Get(AudioClips::Sound::SaveCrystal);
        case Category::GateCrystal: return AudioClips::Get(AudioClips::Sound::GateCrystal);
        case Category::Treasure:    return AudioClips::Get(AudioClips::Sound::Treasure);
        case Category::NPC:         return AudioClips::Get(AudioClips::Sound::NPC);
        case Category::Object:      return AudioClips::Get(AudioClips::Sound::Object);
        case Category::Enemy:       return AudioClips::Get(AudioClips::Sound::Enemy);
        case Category::Items:       return AudioClips::Get(AudioClips::Sound::Items);
        default:                    return nullptr;   // All (not an entity), Trap (no sound yet)
    }
}

// Slot -> pitch. 0 -> 1.00, 1 -> 1.05, 2 -> 0.95, 3 -> 1.10, 4 -> 0.90 ... clamped at +-30%.
// Odd slots go up, even slots go down, both walking outward one 5% step at a time.
float PitchForSlot(int slot) {
    if (slot <= 0) return 1.0f;
    int step = (slot + 1) / 2;                       // 1,1,2,2,3,3,...
    if (step > kPitchMaxStep) step = kPitchMaxStep;
    const float mag = kPitchStep * static_cast<float>(step);
    return (slot & 1) ? 1.0f + mag : 1.0f - mag;
}

// Slot -> repeat period, floored so the clip finishes first. See the kMinGapMs note above for why the
// floor exists and why the step is added AFTER it.
uint64_t PeriodForSlot(int slot, const AudioClips::Clip* clip, float pitch) {
    uint64_t base = kBasePeriodMs;
    if (clip && clip->freq > 0 && pitch > 0.0f) {
        // A playback rate of `pitch` divides the clip's duration; SDL_SetAudioStreamFrequencyRatio is
        // exactly that, so this is the length the player will actually hear.
        const double secs = static_cast<double>(clip->samples.size()) / clip->freq / pitch;
        const uint64_t needed = static_cast<uint64_t>(secs * 1000.0) + kMinGapMs;
        if (needed > base) base = needed;
    }
    return base + kPeriodStepMs * static_cast<uint64_t>(slot);
}

// The lowest slot not currently held by a live track of this category. Linear in the track count,
// which is fine: it runs once per entity ARRIVAL, not per frame and not per ping.
int ClaimSlot(Category cat) {
    for (int slot = 0; ; ++slot) {
        bool taken = false;
        for (const Track& t : g_tracks)
            if (t.cat == cat && t.slot == slot) { taken = true; break; }
        if (!taken) return slot;
    }
}

Track* FindTrack(void* id, Category cat, const FVec3& pos) {
    for (Track& t : g_tracks) {
        // A scene object is identity outright. A FIXED EXIT has no scene node, so it is identified by
        // its category and its position -- which for an exit never changes. That is what makes the
        // comparison sound here and unsound for anything that moves.
        if (id != nullptr) { if (t.id == id) return &t; continue; }
        if (t.id == nullptr && t.cat == cat &&
            NavCommon::Distance2D(t.pos, pos) < 0.05f) return &t;
    }
    return nullptr;
}

// Re-ask who is in range and reconcile that against what is being tracked. Arrivals claim a slot;
// departures keep theirs until the grace window runs out, then release it.
void RefreshMembership(const FVec3& me, uint64_t now) {
    const float radius = kRadiusSteps * NavCommon::GetUnitsPerStep();
    const int found = EntityList::CollectNearby(me, radius, kMaxTracked, g_near);

    if (found >= kMaxTracked) {
        if (!g_clipWarned) {
            g_clipWarned = true;
            char m[176];
            snprintf(m, sizeof(m),
                     "gather hit its %d-entry bound -- entities INSIDE the radius are being dropped, "
                     "which the no-limit rule says must not happen. Raise kMaxTracked.", kMaxTracked);
            Log::Write("SCAPE", m);
        }
    } else {
        g_clipWarned = false;   // re-arm, so a later map that does clip is reported too
    }

    for (const EntityList::NearbyEntity& e : g_near) {
        const AudioClips::Clip* clip = CategorySound(e.cat);
        if (!clip) continue;                  // soundless category (trap), or a clip that failed to
                                              // decode: never tracked, so it costs nothing per frame

        Track* t = FindTrack(e.id, e.cat, e.pos);
        if (!t) {
            Track fresh;
            fresh.id     = e.id;
            fresh.cat    = e.cat;
            fresh.clip   = clip;
            fresh.slot   = ClaimSlot(e.cat);
            fresh.pitch  = PitchForSlot(fresh.slot);
            fresh.period = PeriodForSlot(fresh.slot, clip, fresh.pitch);
            // Start a beat after whoever joined last. GLOBAL, not per category, so a door and an NPC
            // that both took slot 0 -- and therefore share a period exactly -- do not sound in unison
            // for as long as they are both in range.
            fresh.nextMs = now + kPhaseStepMs * (g_phaseSeq++ % kPhaseWrap);
            g_tracks.push_back(fresh);
            t = &g_tracks.back();
        }
        t->pos     = e.pos;
        t->seenMs  = now;
        t->inRange = true;
    }

    // Anything the gather did not report is out of range (or gone). It falls silent at once and keeps
    // its slot until the grace window expires, so stepping back and forth across the boundary does not
    // churn a voice's identity.
    for (size_t i = 0; i < g_tracks.size(); ) {
        Track& t = g_tracks[i];
        if (t.seenMs != now) t.inRange = false;
        if (!t.inRange && now - t.seenMs > kDropGraceMs) {
            g_tracks[i] = g_tracks.back();
            g_tracks.pop_back();
        } else {
            ++i;
        }
    }
}

void ClearTracks() {
    g_tracks.clear();
    g_nextRefreshMs = 0;
    g_clipWarned    = false;
}

} // namespace

void OnGameFrame() {
    // ---- O(1) idle ---------------------------------------------------------------------------------
    // The shipped default is OFF, so for almost every player this function is one relaxed atomic load
    // and a return, on every field frame, forever. Everything below is paid for only by someone who
    // switched the feature on.
    const bool on = AudioEngine::Available() && ModMenu::SoundscapeOn();
    if (on != g_wasOn) {
        g_wasOn = on;
        Log::Write("SCAPE", on ? "soundscape on" : "soundscape off");
        if (!on) Stop();
    }
    if (!on) return;

    // The map changed under us. Every track holds a scene-object pointer and a world position from the
    // old area, both meaningless now. PathPlanner bumps the epoch on teardown, so this needs no hook of
    // its own and cannot be missed.
    const uint32_t epoch = PathPlanner::CurrentEpoch();
    if (epoch != g_epoch) {
        g_epoch = epoch;
        if (!g_tracks.empty()) Log::Write("SCAPE", "map changed -> tracks dropped");
        Stop();
    }

    FVec3 me;
    if (!PlayerState::IsFieldNavSafe() || !PlayerState::ReadPlayerPos(me)) return;   // skip the frame

    // ---- SUSPEND WHILE THE PLAYER IS NOT DRIVING ----------------------------------------------------
    // The same two predicates, for the same reason, as the beacon's Session 157 gate: IsFieldNavSafe
    // asks whether the field EXISTS, not whether the player is in control of it, and both stay true
    // through a conversation and through a cutscene. The field pause menu stops this tick outright; the
    // battle command menu does not, and FFXII lets that menu be opened out of combat on any map where
    // battles can happen.
    //
    // Both are the GAME's own state: BattleCommandActive re-validates its panel against the window
    // class on every read, and IsBoxLive asks the engine's message-window registry.
    // MenuState::IsAnyMenuOpen is deliberately NOT used -- it reads a global the game writes once and
    // never clears, so it answers "open" forever.
    {
        const char* busy = nullptr;
        if (IngameMenuReader::BattleCommandActive()) busy = "battle command menu open";
        else if (DialogueReader::IsBoxLive())        busy = "dialogue or message box on screen";

        if (busy != g_busyWhy) {
            g_busyWhy = busy;
            char m[160];
            snprintf(m, sizeof(m), "%s%s", busy ? "suspended -- " : "resumed", busy ? busy : "");
            Log::Write("SCAPE", m);
        }
        if (busy) {
            // SUSPEND: silence everything and drop the tracks, so coming back out of a conversation
            // re-reads the room rather than resuming voices aimed at where things used to be.
            Stop();
            return;
        }
    }

    const uint64_t now = GetTickCount64();

    if (now >= g_nextRefreshMs) {
        RefreshMembership(me, now);
        g_nextRefreshMs = now + kTrackRefreshMs;
    }
    if (g_tracks.empty()) return;

    // ---- sound every voice that has come due --------------------------------------------------------
    // The camera is read at most once per frame, and only on a frame where something is actually going
    // to sound -- there is no point paying for it on the frames where every clock is still running down,
    // which is most of them.
    bool  haveFacing = false;
    float facingRad  = 0.0f;
    int   started    = 0;
    const float radius = kRadiusSteps * NavCommon::GetUnitsPerStep();
    const float vol    = ModMenu::SoundscapeVolume();

    for (Track& t : g_tracks) {
        if (!t.inRange || now < t.nextMs) continue;
        if (started >= kMaxStartsPerFrame) break;   // the rest start next frame; nothing is skipped

        if (!haveFacing) {
            PlayerState::ReadCameraForwardStable(facingRad);
            haveFacing = true;
        }

        // LIVE POSITION, read at the instant the voice sounds -- this is what makes a walking NPC's
        // sound follow the NPC instead of following the last membership refresh. A fixed exit has no
        // scene node and does not move, so its stored position IS live. A failed read is streaming
        // noise: keep the last known position rather than dropping a voice mid-stride, which is the
        // same rule RefreshPositionsLocked already applies.
        FVec3 live;
        if (t.id && PlayerState::ReadSceneObjectPos(t.id, live)) t.pos = live;

        float pan = 0.0f, front = 1.0f;
        NavCommon::BearingToPan(me, t.pos, facingRad, pan, front);

        const float d = NavCommon::Distance2D(me, t.pos);
        const float k = (radius > 0.0f && d < radius) ? d / radius : 1.0f;   // 0 at you, 1 at the edge
        const float gain = vol * (1.0f - k * (1.0f - kFarGain));

        AudioEngine::PlayScape(t.clip, pan, front, gain, t.pitch);
        g_sounding.store(true, std::memory_order_relaxed);

        // Re-phase from NOW rather than advancing `nextMs += period`. The latter would make a voice
        // that had been held (a conversation, a frame-rate stall) fire a burst of catch-up pings to
        // pay off a debt nobody wants paid.
        t.nextMs = now + t.period;
        ++started;
    }
}

void Stop() {
    ClearTracks();
    if (g_sounding.exchange(false, std::memory_order_acq_rel))
        AudioEngine::SilenceScape();
}

} // namespace Soundscape

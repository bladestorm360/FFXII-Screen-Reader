#include "audio/audio_engine.h"
#include "core/logger.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace AudioEngine {
namespace {

// ---- voices -------------------------------------------------------------------------------------
// One SDL stream per voice, every one of them bound to the single logical device below. The beacon
// has a voice to itself and retriggers it; the soundscape round-robins over the pool, taking whichever
// is least busy. `srcFreq` is per voice because SDL_SetAudioStreamFormat is a per-stream setting -- a
// single shared g_srcFreq was correct only while there was one stream, and would have re-pointed the
// wrong voice the moment a clip at another rate arrived.
struct Voice {
    SDL_AudioStream* stream  = nullptr;
    int              srcFreq = 0;
};

// Sixteen scape voices. The soundscape has NO entity cap -- every interactable within 20 steps gets
// its own repeating voice -- so the number that can be sounding at one instant is set by how long the
// clips are against how often each repeats: roughly `sum(clipLength / period)` over everything in
// range. With clips averaging ~0.6 s and periods from 1.0 s upward, twenty tracked entities work out
// at about six sounding at once and forty at about twelve, so sixteen carries a crowded street with
// headroom. Past that the pool cuts the voice nearest to finishing, which is a clipped tail rather
// than a lost sound.
//
// Not a tuning knob: raise it on a measurement, not on a worry. Each stream is a small SDL allocation
// and an entry in the device's mix list, and one that never plays costs nothing at all.
constexpr int kScapeVoices = 16;

SDL_AudioDeviceID g_device = 0;      // the LOGICAL device every voice below is bound to
Voice g_beacon;
Voice g_scape[kScapeVoices];
int   g_scapeNext = 0;               // round-robin cursor, used only to break ties
bool  g_ready     = false;

// Create one voice and bind it to g_device. Logs and leaves the voice null on failure; a caller
// that got a null voice plays nothing, which is the whole degradation strategy here.
bool OpenVoice(Voice& v, const SDL_AudioSpec& src, const char* what);

// Reusable interleave buffer. Game-thread only (see the threading note in the header), so it needs
// no lock; keeping it around avoids a ~200 KB heap churn on every ping.
std::vector<float> g_scratch;

// ---- BEHIND: three cues, ONE definition ---------------------------------------------------------
//
// BEHIND IS THE WHOLE REAR HEMISPHERE (Session 95, the tester's correction). `front` is the cosine of
// the relative bearing, so `front < 0` IS behind — anything from just past the player's shoulder round
// to dead astern, a 180-degree sweep. The pitch cue first shipped scaled by `-front`, which meant a
// target 100 degrees round barely differed from one at 80 and only a target exactly astern got the
// full treatment. That is "directly behind", not "behind".
//
// THE PAN IS NOT TOUCHED BY ANY OF THIS, and that is a requirement rather than an oversight: there is
// no spatial audio here, so left/right is the only bearing information the player has and it has to
// keep working all the way round. Behind-left stays panned left and simply gains the behind cues on
// top — the attenuation and the low-pass scale both channels equally, and pitch is a playback rate.
//
// The short ramp is the one concession: the cue reaches full strength within kBehindRampDeg of the
// abeam line rather than snapping at it, so an enemy circling the player does not chatter between
// two timbres as it crosses. Everything past that band is fully "behind".
constexpr float kBehindGain      = 0.55f;
constexpr float kBehindLowpass   = 0.85f;   // 0 = no filtering behind, 1 = maximum
constexpr float kBehindPitchDrop = 0.20f;   // playback rate x0.80 behind — a few semitones, not an octave
constexpr float kBehindRampDeg   = 15.0f;   // degrees past abeam at which the cues are at full strength

// 0 ahead and abeam, 1 anywhere in the rear hemisphere past the ramp band.
float BehindAmount(float front) {
    if (front >= 0.0f) return 0.0f;
    // cos(90 + ramp) is how far `front` has travelled below zero at the end of the band.
    const float full = std::sin(kBehindRampDeg * 3.14159265358979f / 180.0f);
    const float t = -front / full;
    return t > 1.0f ? 1.0f : t;
}

// The source spec every voice is created with. The clips themselves are mono and are widened at play
// time; stereo is where the pan lives. The device may be running at some entirely different rate and
// format — the stream converts, which is the whole reason not to hand-roll any of this.
SDL_AudioSpec SourceSpec(int freq) {
    SDL_AudioSpec src = {};
    src.format   = SDL_AUDIO_F32;
    src.channels = 2;
    src.freq     = freq;
    return src;
}

// ---- THE ONE RENDER PATH ------------------------------------------------------------------------
// Both channels come through here, so pan, the three behind cues, the pitch clamp and the mono->stereo
// interleave exist exactly once. Standing up a second copy for the soundscape is what CLAUDE.md's
// CENTRALIZE rule and debug.md's Session 92 note ("the pan / behind math IS the reusable part —
// extend it, do not stand up a second spatialization path") both forbid, and it is also how the
// Session 92 pan bug happened: one fact derived twice, and the copies disagreed.
void Render(Voice& v, const AudioClips::Clip* clip, float pan, float front, float gain, float pitch) {
    if (!v.stream || !clip || clip->samples.empty() || clip->freq <= 0) return;

    pan   = std::clamp(pan,   -1.0f, 1.0f);
    front = std::clamp(front, -1.0f, 1.0f);
    gain  = std::clamp(gain,   0.0f, 1.0f);
    if (gain <= 0.0f) return;
    // Pitch is clamped AFTER the behind cue below multiplies it, so the clamp is a real bound on what
    // reaches SDL rather than a bound on what the caller asked for.

    // Equal-power pan, the form DQ7R's engine uses: constant perceived loudness as the image moves
    // across, where a naive linear (1-p)/(p) pan dips in the middle.
    const float panNorm = (pan + 1.0f) * 0.5f;                 // 0..1
    float gainL = std::cos(panNorm * 1.5707963f) * gain;
    float gainR = std::sin(panNorm * 1.5707963f) * gain;

    // Behind: quieter, duller and LOWER. Three cues from ONE `behind` value, because any one alone is
    // easy to mistake for "further away" — together they read as behind. Deriving them from a single
    // number here is also what stops them drifting apart: the pitch cue was briefly computed in
    // audio_beacon.cpp from its own reading of `front`, which is the exact shape of the Session 92 pan
    // bug (one fact, two derivations, and the copies disagreed).
    //
    // Applied to EVERY ping on EVERY channel, so both beacons and every soundscape voice get identical
    // treatment without any caller having to remember to ask.
    float lp = 0.0f;                                            // one-pole coefficient, 0 = off
    const float behind = BehindAmount(front);
    if (behind > 0.0f) {
        const float att = 1.0f - behind * (1.0f - kBehindGain);
        gainL *= att;
        gainR *= att;
        lp     = behind * kBehindLowpass;
        // Multiplies the caller's pitch rather than replacing it, so the arrival cue keeps its
        // pitched-UP character and the soundscape's same-type pitch spread survives behind the player.
        pitch *= 1.0f - behind * kBehindPitchDrop;
    }
    if (pitch < 0.05f) pitch = 0.05f;
    if (pitch > 8.0f)  pitch = 8.0f;

    // Re-point this voice if this clip is at a different rate than the last one it played. Every
    // shipped sound is 44100 so this normally never fires, but it keeps a future clip from playing at
    // the wrong speed rather than silently sounding wrong.
    if (clip->freq != v.srcFreq) {
        SDL_AudioSpec src = SourceSpec(clip->freq);
        if (SDL_SetAudioStreamFormat(v.stream, &src, nullptr)) v.srcFreq = clip->freq;
    }

    // Pitch is SDL's job, and it is per stream — which is what lets two soundscape voices sound the
    // same clip at two pitches at the same time.
    SDL_SetAudioStreamFrequencyRatio(v.stream, pitch);

    // Mono -> stereo with the pan gains applied, plus the behind filter. This is the only place
    // samples are touched.
    const size_t n = clip->samples.size();
    g_scratch.resize(n * 2);
    const float* in = clip->samples.data();
    float* out = g_scratch.data();
    if (lp > 0.0f) {
        const float a = 1.0f - lp;                              // a=1 passes through, smaller = duller
        float y = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            y += a * (in[i] - y);
            out[i * 2 + 0] = y * gainL;
            out[i * 2 + 1] = y * gainR;
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            out[i * 2 + 0] = in[i] * gainL;
            out[i * 2 + 1] = in[i] * gainR;
        }
    }

    // Drop whatever is still queued on THIS voice, then enqueue this ping.
    SDL_ClearAudioStream(v.stream);
    SDL_PutAudioStreamData(v.stream, g_scratch.data(),
                           static_cast<int>(g_scratch.size() * sizeof(float)));
}

// The scape voice to use next: the one with the least audio still queued, so a new ping lands on a
// silent voice whenever one exists and only ever cuts the nearest-to-finished voice when none does.
// Ties go to the round-robin cursor, which spreads consecutive pings over the pool when everything is
// idle rather than hammering voice 0 -- SDL's own resampler keeps a little state per stream, and
// reusing one voice for back-to-back pings is the case most likely to click.
Voice* PickScapeVoice() {
    Voice*  best      = nullptr;
    int     bestQueued = 0;
    for (int k = 0; k < kScapeVoices; ++k) {
        Voice& v = g_scape[(g_scapeNext + k) % kScapeVoices];
        if (!v.stream) continue;
        // Bytes still QUEUED AS INPUT, i.e. not yet pulled by the device -- SDL's own words, and the
        // right question here. It reaches zero a device-buffer's worth (~10-20 ms) BEFORE the tail
        // has finished sounding, so "idle" is approximate; with eight voices that slack never
        // matters, and the alternative (SDL_GetAudioStreamAvailable) answers a different question.
        // -1 is the failure return and is read as idle, which is also what the FFPR engine does:
        // a stream that cannot be queried is not one to protect from being cut.
        const int q = SDL_GetAudioStreamQueued(v.stream);
        const int queued = q > 0 ? q : 0;
        if (!best || queued < bestQueued) {
            best       = &v;
            bestQueued = queued;
            if (queued <= 0) break;                 // silent voice: nothing can beat it
        }
    }
    g_scapeNext = (g_scapeNext + 1) % kScapeVoices;
    return best;
}

} // namespace

// Declared inside the anonymous namespace above; defined here so it sits beside Init, its only
// caller. `dst` is null on purpose: SDL_BindAudioStream sets the output end to whatever the device
// is actually running, and the stream converts our 44100 F32 stereo source to it.
namespace {
bool OpenVoice(Voice& v, const SDL_AudioSpec& src, const char* what) {
    SDL_AudioStream* s = SDL_CreateAudioStream(&src, nullptr);
    if (!s) {
        char msg[192];
        snprintf(msg, sizeof(msg), "AudioEngine: SDL_CreateAudioStream failed for %s voice: %s",
                 what, SDL_GetError());
        Log::Write("AUDIO", msg);
        return false;
    }
    if (!SDL_BindAudioStream(g_device, s)) {
        char msg[192];
        snprintf(msg, sizeof(msg), "AudioEngine: SDL_BindAudioStream failed for %s voice: %s",
                 what, SDL_GetError());
        Log::Write("AUDIO", msg);
        SDL_DestroyAudioStream(s);
        return false;
    }
    v.stream  = s;
    v.srcFreq = src.freq;
    return true;
}
} // namespace

bool Init() {
    if (g_ready) return true;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "AudioEngine: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
        Log::Write("AUDIO", msg);
        return false;
    }

    // ONE DEVICE, N STREAMS BOUND TO IT -- the shape the sibling FFPR mods already ship, ported from
    // `D:\Games\Dev\Unity\FFPR\ff1\ff1-screen-reader\Utils\AudioEngine.cs`, whose own header reads:
    // "One logical playback device with several SDL_AudioStreams bound to it; SDL mixes every bound
    // stream automatically". Their eight streams are one per named source -- footstep, wall bump,
    // beacon, the four wall tones, counter -- and it is the same call sequence here: open, create,
    // bind, resume.
    //
    // Deliberately NOT SDL_OpenAudioDeviceStream, which this file used while the beacon was the only
    // sound. That opens a device AND a stream together and ties the device's lifetime to that one
    // stream, so the beacon would have owned the device and destroying it would have closed the
    // soundscape's voices with it. Symmetrical is both simpler and harder to tear down wrong.
    const SDL_AudioSpec src = SourceSpec(44100);   // every shipped clip; corrected per clip in Render
    g_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &src);
    if (g_device == 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "AudioEngine: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        Log::Write("AUDIO", msg);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    // A voice that fails to create or bind is left null and skipped for the session, and each channel
    // degrades on its own: a missing beacon voice costs the beacon and nothing else, fewer soundscape
    // voices cost a little overlap. Only losing EVERY voice is a real failure.
    int voices = 0;
    if (OpenVoice(g_beacon, src, "beacon")) ++voices;
    int scapeVoices = 0;
    for (int i = 0; i < kScapeVoices; ++i)
        if (OpenVoice(g_scape[i], src, "soundscape")) { ++voices; ++scapeVoices; }

    if (voices == 0) {
        Log::Write("AUDIO", "AudioEngine: no audio streams could be bound - audio disabled");
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    // A freshly opened device starts PAUSED. Skipping this is a silent no-audio failure -- the FFPR
    // engine carries the same warning over the same line, which is how it was found there.
    if (!SDL_ResumeAudioDevice(g_device)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "AudioEngine: SDL_ResumeAudioDevice failed: %s", SDL_GetError());
        Log::Write("AUDIO", msg);
    }

    g_ready = true;

    // Clips are decoded AFTER the device is up: AudioClips uses SDL_LoadWAV_IO.
    if (!AudioClips::Init()) {
        // Not fatal on its own -- Available() still reports true and Render simply gets handed a
        // null clip and returns. The log line from AudioClips names which sound failed.
        Log::Write("AUDIO", "AudioEngine: device is open but no clips decoded");
    }

    char msg[224];
    snprintf(msg, sizeof(msg),
             "AudioEngine: ready (device=%u, source 44100 Hz stereo F32, beacon=%s, %d/%d soundscape voices)",
             (unsigned)g_device, g_beacon.stream ? "yes" : "NO", scapeVoices, kScapeVoices);
    Log::Write("AUDIO", msg);
    if (scapeVoices == 0) Log::Write("AUDIO", "AudioEngine: no soundscape voices - the soundscape will stay silent");
    return true;
}

void Shutdown() {
    if (!g_ready) return;
    // Streams first, then the device: a bound stream outliving its device is the one ordering that
    // bites, and with no stream owning the device there is nothing else to get right.
    for (Voice& v : g_scape) {
        if (!v.stream) continue;
        SDL_ClearAudioStream(v.stream);
        SDL_DestroyAudioStream(v.stream);
        v.stream = nullptr;
    }
    if (g_beacon.stream) {
        SDL_ClearAudioStream(g_beacon.stream);
        SDL_DestroyAudioStream(g_beacon.stream);
        g_beacon.stream = nullptr;
    }
    if (g_device != 0) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }
    AudioClips::Shutdown();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    g_scratch.clear();
    g_scratch.shrink_to_fit();
    g_ready = false;
    Log::Write("AUDIO", "AudioEngine shut down");
}

bool Available() { return g_ready; }

void PlayPing(const AudioClips::Clip* clip, float pan, float front, float gain, float pitch) {
    if (!g_ready) return;
    Render(g_beacon, clip, pan, front, gain, pitch);
}

void SilenceBeacon() {
    if (!g_ready || !g_beacon.stream) return;
    SDL_ClearAudioStream(g_beacon.stream);
}

void PlayScape(const AudioClips::Clip* clip, float pan, float front, float gain, float pitch) {
    if (!g_ready) return;
    Voice* v = PickScapeVoice();
    if (v) Render(*v, clip, pan, front, gain, pitch);
}

void SilenceScape() {
    if (!g_ready) return;
    for (Voice& v : g_scape)
        if (v.stream) SDL_ClearAudioStream(v.stream);
}

} // namespace AudioEngine

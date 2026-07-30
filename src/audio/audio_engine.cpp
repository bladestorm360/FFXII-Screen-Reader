#include "audio/audio_engine.h"
#include "core/logger.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace AudioEngine {
namespace {

SDL_AudioStream* g_stream  = nullptr;
bool             g_ready   = false;
int              g_srcFreq = 0;      // sample rate the stream is currently configured to read

// Reusable interleave buffer. Game-thread only (see the threading note in the header), so it needs
// no lock; keeping it around avoids a ~200 KB heap churn on every ping.
std::vector<float> g_scratch;

// How much a sound directly behind is attenuated, and how hard it is low-passed. Together these
// are what separate "ahead" from "behind" — see the header.
constexpr float kBehindGain    = 0.55f;
constexpr float kBehindLowpass = 0.85f;   // 0 = no filtering behind, 1 = maximum

} // namespace

bool Init() {
    if (g_ready) return true;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "AudioEngine: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
        Log::Write("AUDIO", msg);
        return false;
    }

    // Source spec. Stereo because that is where the pan lives; the clips themselves are mono and
    // are widened at play time. The device may be running at some entirely different rate and
    // format — the stream converts, which is the whole reason not to hand-roll any of this.
    SDL_AudioSpec src = {};
    src.format   = SDL_AUDIO_F32;
    src.channels = 2;
    src.freq     = 44100;             // every shipped clip; corrected per clip in PlayPing if it differs
    g_srcFreq    = src.freq;

    // No callback: we push a whole ping at a time and an empty stream plays silence by itself.
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &src, nullptr, nullptr);
    if (!g_stream) {
        char msg[256];
        snprintf(msg, sizeof(msg), "AudioEngine: SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
        Log::Write("AUDIO", msg);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    if (!SDL_ResumeAudioStreamDevice(g_stream)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "AudioEngine: SDL_ResumeAudioStreamDevice failed: %s", SDL_GetError());
        Log::Write("AUDIO", msg);
        SDL_DestroyAudioStream(g_stream);
        g_stream = nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    g_ready = true;

    // Clips are decoded AFTER the device is up: AudioClips uses SDL_LoadWAV_IO.
    if (!AudioClips::Init()) {
        // Not fatal on its own -- Available() still reports true and PlayPing simply gets handed a
        // null clip and returns. The log line from AudioClips names which sound failed.
        Log::Write("AUDIO", "AudioEngine: device is open but no clips decoded");
    }

    const SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(g_stream);
    char msg[192];
    snprintf(msg, sizeof(msg), "AudioEngine: ready (device=%u, source 44100 Hz stereo F32)", (unsigned)dev);
    Log::Write("AUDIO", msg);
    return true;
}

void Shutdown() {
    if (!g_ready) return;
    if (g_stream) {
        SDL_DestroyAudioStream(g_stream);
        g_stream = nullptr;
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
    if (!g_ready || !g_stream || !clip || clip->samples.empty() || clip->freq <= 0) return;

    pan   = std::clamp(pan,   -1.0f, 1.0f);
    front = std::clamp(front, -1.0f, 1.0f);
    gain  = std::clamp(gain,   0.0f, 1.0f);
    if (pitch < 0.05f) pitch = 0.05f;
    if (pitch > 8.0f)  pitch = 8.0f;
    if (gain <= 0.0f) return;

    // Equal-power pan, the form DQ7R's engine uses: constant perceived loudness as the image moves
    // across, where a naive linear (1-p)/(p) pan dips in the middle.
    const float panNorm = (pan + 1.0f) * 0.5f;                 // 0..1
    float gainL = std::cos(panNorm * 1.5707963f) * gain;
    float gainR = std::sin(panNorm * 1.5707963f) * gain;

    // Behind: quieter and duller. Both cues, because either alone is easy to mistake for "further
    // away" — together they read as behind.
    float lp = 0.0f;                                            // one-pole coefficient, 0 = off
    if (front < 0.0f) {
        const float behind = -front;                            // 0 abeam .. 1 directly behind
        const float att    = 1.0f - behind * (1.0f - kBehindGain);
        gainL *= att;
        gainR *= att;
        lp = behind * kBehindLowpass;
    }

    // Re-point the stream if this clip is at a different rate than the last one. Both shipped
    // sounds are 44100 so this normally never fires, but it keeps a future clip from playing at
    // the wrong speed rather than silently sounding wrong.
    if (clip->freq != g_srcFreq) {
        SDL_AudioSpec src = {};
        src.format   = SDL_AUDIO_F32;
        src.channels = 2;
        src.freq     = clip->freq;
        if (SDL_SetAudioStreamFormat(g_stream, &src, nullptr)) {
            g_srcFreq = clip->freq;
        }
    }

    // Pitch is SDL's job.
    SDL_SetAudioStreamFrequencyRatio(g_stream, pitch);

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

    // Retrigger: drop whatever is still queued, then enqueue this ping.
    SDL_ClearAudioStream(g_stream);
    SDL_PutAudioStreamData(g_stream, g_scratch.data(),
                           static_cast<int>(g_scratch.size() * sizeof(float)));
}

void SilenceAll() {
    if (!g_ready || !g_stream) return;
    SDL_ClearAudioStream(g_stream);
}

} // namespace AudioEngine

#include "audio/audio_clips.h"
#include "audio/beacon_assets.h"
#include "core/logger.h"

#include <SDL3/SDL.h>

#include <windows.h>

#include <cstdio>

namespace AudioClips {
namespace {

Clip g_objective;
Clip g_activeTarget;
bool g_ready = false;

// The HMODULE of THIS dll, resolved from the address of a function inside it.
//
// dllmain.cpp keeps the module handle in a file-static and exposes only the DIRECTORY
// (Log::GetGameDir). Rather than widen that or add a second GetModuleFileName helper — the header
// for the existing one warns specifically against a second copy — this asks the loader which module
// owns an address we already have. No name lookup, so it cannot be fooled by another dinput8.dll
// somewhere on the search path.
HMODULE SelfModule() {
    HMODULE h = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&SelfModule), &h);
    return h;
}

// Point `outData`/`outSize` at an embedded RCDATA blob. The pointer stays valid for the life of
// the module and must not be freed — LockResource hands back an address inside the mapped image.
bool FindEmbedded(int resId, const void*& outData, DWORD& outSize) {
    HMODULE self = SelfModule();
    if (!self) return false;
    HRSRC info = FindResourceA(self, MAKEINTRESOURCEA(resId), RT_RCDATA);
    if (!info) return false;
    HGLOBAL res = LoadResource(self, info);
    if (!res) return false;
    const void* p = LockResource(res);
    const DWORD n = SizeofResource(self, info);
    if (!p || n == 0) return false;
    outData = p;
    outSize = n;
    return true;
}

// Decode one embedded .wav into mono float.
//
// SDL_LoadWAV_IO walks the RIFF chunk list properly. That matters even though assets/*.wav are
// stripped: a hand-rolled "skip 44 bytes and the rest is samples" loader would play any surviving
// metadata as noise, and the ORIGINALS in `FF 12 SFX\` are ~70-90% metadata. SDL_ConvertAudioSamples
// then does s16 -> f32 and any channel count -> mono, so nothing here has to know the source format.
bool LoadOne(int resId, const char* name, Clip& out) {
    const void* blob = nullptr;
    DWORD       blobSize = 0;
    if (!FindEmbedded(resId, blob, blobSize)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "AudioClips: embedded resource %d (%s) not found", resId, name);
        Log::Write("AUDIO", msg);
        return false;
    }

    SDL_IOStream* io = SDL_IOFromConstMem(blob, static_cast<size_t>(blobSize));
    if (!io) {
        char msg[192];
        snprintf(msg, sizeof(msg), "AudioClips: SDL_IOFromConstMem failed for %s: %s", name, SDL_GetError());
        Log::Write("AUDIO", msg);
        return false;
    }

    SDL_AudioSpec spec = {};
    Uint8*        wav  = nullptr;
    Uint32        wavLen = 0;
    // closeio = true: SDL frees the IOStream for us on both the success and failure paths.
    if (!SDL_LoadWAV_IO(io, true, &spec, &wav, &wavLen)) {
        char msg[192];
        snprintf(msg, sizeof(msg), "AudioClips: SDL_LoadWAV_IO failed for %s: %s", name, SDL_GetError());
        Log::Write("AUDIO", msg);
        return false;
    }

    SDL_AudioSpec want = {};
    want.format   = SDL_AUDIO_F32;
    want.channels = 1;
    want.freq     = spec.freq;        // keep the clip's own rate; the voice folds the ratio into its step

    Uint8* conv    = nullptr;
    int    convLen = 0;
    const bool ok = SDL_ConvertAudioSamples(&spec, wav, static_cast<int>(wavLen), &want, &conv, &convLen);
    SDL_free(wav);
    if (!ok) {
        char msg[192];
        snprintf(msg, sizeof(msg), "AudioClips: SDL_ConvertAudioSamples failed for %s: %s", name, SDL_GetError());
        Log::Write("AUDIO", msg);
        return false;
    }

    const size_t frames = static_cast<size_t>(convLen) / sizeof(float);
    out.samples.assign(reinterpret_cast<const float*>(conv),
                       reinterpret_cast<const float*>(conv) + frames);
    out.freq = want.freq;
    SDL_free(conv);

    char msg[192];
    snprintf(msg, sizeof(msg), "AudioClips: %s loaded — %zu frames @ %d Hz (%.2f s), src %d ch %d Hz",
             name, frames, out.freq,
             out.freq > 0 ? static_cast<double>(frames) / out.freq : 0.0,
             spec.channels, spec.freq);
    Log::Write("AUDIO", msg);
    return !out.samples.empty();
}

} // namespace

bool Init() {
    if (g_ready) return true;
    const bool a = LoadOne(IDR_SND_OBJECTIVE,     "objective",     g_objective);
    const bool b = LoadOne(IDR_SND_ACTIVE_TARGET, "active_target", g_activeTarget);
    g_ready = a || b;   // one bad clip must not silence the other
    if (!g_ready) Log::Write("AUDIO", "AudioClips: no sounds decoded — the beacon will stay silent");
    return g_ready;
}

void Shutdown() {
    g_objective   = Clip{};
    g_activeTarget = Clip{};
    g_ready = false;
}

const Clip* Objective()    { return g_objective.samples.empty()    ? nullptr : &g_objective; }
const Clip* ActiveTarget() { return g_activeTarget.samples.empty() ? nullptr : &g_activeTarget; }

} // namespace AudioClips

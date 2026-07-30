#pragma once

#include <vector>

// The beacon's sound samples, decoded once at startup from RCDATA embedded in this DLL.
//
// WHY RCDATA and not files on disk: there is nothing extra for the user to place, nothing extra in
// the release zip, no path to resolve, and no "asset missing" state to degrade through. The mod has
// no other on-disk asset and this deliberately does not become the first one. See beacon_assets.h.
//
// Samples are kept as MONO float at the clip's own rate. They are NOT resampled to the device rate:
// a playing voice already needs a fractional read position to do pitch, so it costs nothing to fold
// the rate ratio into that same number (see AudioEngine::Voice::rate). One less buffer, one less
// pass, and no quality difference — the interpolation happens either way.
namespace AudioClips {

struct Clip {
    std::vector<float> samples;   // mono, [-1, 1]
    int                freq = 0;  // the clip's own sample rate (44100 for all shipped sounds)
};

// Decode the embedded sounds. Requires SDL's audio subsystem to be up (it uses SDL_LoadWAV_IO),
// so call it from AudioEngine::Init AFTER the device opens, never before.
// Returns false and logs if a resource is missing or undecodable; accessors then return nullptr
// and the beacon silently does nothing.
bool Init();
void Shutdown();

// Null until Init() has succeeded. Callers must handle null rather than assume audio exists.
const Clip* Objective();      // the route beacon
const Clip* ActiveTarget();   // the in-combat target beacon

} // namespace AudioClips

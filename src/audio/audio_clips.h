#pragma once

#include <vector>

// The mod's sound samples, decoded once at startup from RCDATA embedded in this DLL.
//
// WHY RCDATA and not files on disk: there is nothing extra for the user to place, nothing extra in
// the release zip, no path to resolve, and no "asset missing" state to degrade through. The mod has
// no other on-disk asset and this deliberately does not become the first one. See beacon_assets.h.
//
// Samples are kept as MONO float at the clip's own rate. They are NOT resampled to the device rate:
// a playing voice already needs a fractional read position to do pitch, so it costs nothing to fold
// the rate ratio into that same number. One less buffer, one less pass, and no quality difference --
// the interpolation happens either way.
namespace AudioClips {

struct Clip {
    std::vector<float> samples;   // mono, [-1, 1]
    int                freq = 0;  // the clip's own sample rate (44100 for all shipped sounds)
};

// Every sound the mod owns. ONE enum and ONE lookup, deliberately: the first two used to have a
// named accessor each, and adding ten more would have meant twelve near-identical functions and
// twelve chances for a caller to reach the wrong one.
//
// This enum is AUDIO's vocabulary, not navigation's. It happens to mirror EntityList::Category for
// the ten soundscape entries, but the mapping is made once, in soundscape.cpp, so this header does
// not have to include a navigation header and the two can diverge without either breaking.
enum class Sound {
    Objective = 0,   // the route beacon
    ActiveTarget,    // the in-combat target beacon
    Exit,
    Door,
    Shop,
    SaveCrystal,
    GateCrystal,
    Treasure,
    NPC,
    Object,
    Enemy,
    Items,
    Count
};

// Decode the embedded sounds. Requires SDL's audio subsystem to be up (it uses SDL_LoadWAV_IO),
// so call it from AudioEngine::Init AFTER the device opens, never before.
// Returns false and logs if EVERY resource is missing or undecodable; one bad clip never silences
// the others. Accessors return nullptr for anything that did not decode and every caller handles
// null by playing nothing.
bool Init();
void Shutdown();

// Null until Init() has succeeded, and null for any individual sound that failed to decode.
// Callers must handle null rather than assume audio exists.
const Clip* Get(Sound s);

} // namespace AudioClips

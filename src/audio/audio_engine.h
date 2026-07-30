#pragma once

#include "audio/audio_clips.h"

// SDL3 playback for the navigation beacon.
//
// This is a THIN WRAPPER, not a mixer. SDL plays the .wav data directly and already provides
// everything the beacon needs except one thing:
//
//   pitch      -> SDL_SetAudioStreamFrequencyRatio
//   retrigger  -> SDL_ClearAudioStream
//   conversion -> the stream converts our source spec to whatever the device wants
//   silence    -> an empty stream simply outputs nothing; gaps need no code
//
// The one gap is PAN: SDL3 has no pan or balance API (checked against 3.4.4 — there is gain and a
// channel map, neither of which is per-channel gain). So the single place this touches samples is
// interleaving the mono clip into stereo with a left/right gain, which is applying volume rather
// than mixing. The distance/behind filtering rides along in that same loop for free.
//
// THREADING: everything here is called from the GAME THREAD only, from the beacon's field-frame
// tick. There is no audio callback and no worker thread. SDL_PutAudioStreamData is a buffered
// enqueue into SDL's own stream — it does not block on the device and does not touch a lock the
// game needs — and the beacon pings at most five times a second, so this is not the "our lock in
// their hot path" shape that `feedback_stall_is_always_mod_side` warns about.
namespace AudioEngine {

// Open the device and decode the embedded clips. Safe to call twice. Returns false (logging under
// "AUDIO") if SDL or the device is unavailable; every other entry point then no-ops silently.
bool Init();
void Shutdown();

// True once Init() has succeeded. The beacon tests this rather than assuming audio exists.
bool Available();

// Play `clip` once, immediately, cutting anything already sounding.
//
// Retrigger rather than overlap is deliberate: the clips run ~0.5-0.6 s and the beacon's fastest
// repeat is 0.2 s, so letting pings ring over each other would turn into mush exactly when the
// player is closest and needs the most precision. Cutting gives the crisp accelerating blip of a
// parking sensor.
//
//   pan   -1 hard left .. 0 centre .. +1 hard right
//   front cos of the bearing: +1 dead ahead, 0 abeam, -1 directly behind. A pure pan renders ahead
//         and behind IDENTICALLY, and this player cannot turn to disambiguate (they treat the game
//         as top-down and do not drive the camera), so behind is attenuated and low-passed. That is
//         the only cue distinguishing the two; it is not decoration.
//   gain  0..1
//   pitch playback-rate multiplier; >1 is higher and shorter. Used once, for the arrival cue.
void PlayPing(const AudioClips::Clip* clip, float pan, float front, float gain, float pitch);

// Cut anything sounding right now. Used when the beacon is switched off mid-ping.
void SilenceAll();

} // namespace AudioEngine

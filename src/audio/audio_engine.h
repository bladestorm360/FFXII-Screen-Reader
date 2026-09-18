#pragma once

#include "audio/audio_clips.h"

// SDL3 playback for the navigation beacon and the entity soundscape.
//
// This is a THIN WRAPPER, not a mixer. SDL plays the .wav data directly and already provides
// everything we need except one thing:
//
//   pitch      -> SDL_SetAudioStreamFrequencyRatio
//   retrigger  -> SDL_ClearAudioStream
//   conversion -> the stream converts our source spec to whatever the device wants
//   silence    -> an empty stream simply outputs nothing; gaps need no code
//   OVERLAP    -> several streams bound to one logical device; SDL sums them into the device buffer
//
// The one gap is PAN: SDL3 has no pan or balance API (checked against 3.4.4 — there is gain and a
// channel map, neither of which is per-channel gain). So the single place this touches samples is
// interleaving the mono clip into stereo with a left/right gain, which is applying volume rather
// than mixing. The distance/behind filtering rides along in that same loop for free.
//
// ---------------------------------------------------------------------------------------------
// WHY SEVERAL STREAMS AND NOT A SOFTWARE MIXER (Session 191)
//
// `debug.md`'s Session 92 design said a many-source version "needs either several streams or a real
// mixer". It is several streams, and the choice is not a toss-up:
//
// A mixer would have to sum every sounding clip into one buffer on the GAME THREAD, which the mod is
// forbidden to stall — and the soundscape has NO cap on how many voices exist, so that cost would be
// bounded by how crowded the map is rather than by anything we control. Handing each ping to its own
// stream costs exactly what one beacon ping already costs (interleave one clip, push it), and SDL does
// the summing on its own audio thread where it belongs. There is no per-frame render callback and no
// audio-side state for the mod to keep in step — which matters most precisely because the voices
// are independent: nothing here has to know how many there are or when the next one is due.
//
// SDL3 mixes every stream BOUND TO A LOGICAL DEVICE ("Playback devices will take data from bound
// audio streams, mix it, and send it to the hardware" — SDL_audio.h). So the scape voices are
// created with SDL_CreateAudioStream and bound to one device opened up front, rather than each opening
// a device of its own. See Init() for why that device is NOT opened through the beacon's stream.
//
// ---------------------------------------------------------------------------------------------
// THREADING: everything here is called from the GAME THREAD only — the beacon's and the soundscape's
// field-frame ticks. There is no audio callback and no worker thread. SDL_PutAudioStreamData is a
// buffered enqueue into SDL's own stream — it does not block on the device and does not touch a lock
// the game needs — and the soundscape caps how many voices may START in one frame, so this is not the
// "our lock in their hot path" shape that `feedback_stall_is_always_mod_side` warns about.
namespace AudioEngine {

// Open the device, create the voices, and decode the embedded clips. Safe to call twice. Returns
// false (logging under "AUDIO") if SDL or the device is unavailable; every other entry point then
// no-ops silently.
bool Init();
void Shutdown();

// True once Init() has succeeded. Callers test this rather than assuming audio exists.
bool Available();

// ---- the BEACON channel -------------------------------------------------------------------------
// Play `clip` once, immediately, cutting anything already sounding ON THIS CHANNEL. The soundscape
// is untouched: they are separate streams precisely so that turning one on cannot chop the other.
//
// Retrigger rather than overlap is deliberate here: the clips run ~0.5-0.6 s and the beacon's
// fastest repeat is 0.2 s, so letting pings ring over each other would turn into mush exactly when
// the player is closest and needs the most precision. Cutting gives the crisp accelerating blip of a
// parking sensor.
//
//   pan   -1 hard left .. 0 centre .. +1 hard right
//   front cos of the bearing: +1 dead ahead, 0 abeam, -1 directly behind. A pure pan renders ahead
//         and behind IDENTICALLY, and this player cannot turn to disambiguate (they treat the game
//         as top-down and do not drive the camera), so behind is attenuated and low-passed. That is
//         the only cue distinguishing the two; it is not decoration.
//   gain  0..1
//   pitch playback-rate multiplier; >1 is higher and shorter. Used for the arrival cue.
void PlayPing(const AudioClips::Clip* clip, float pan, float front, float gain, float pitch);

// Cut whatever the BEACON is sounding right now. Used when the beacon is switched off or suspended
// mid-ping. Named for its channel on purpose — it was `SilenceAll` when there was only one channel,
// and a function that silenced half of the mod's audio while still claiming "all" is exactly the
// kind of stale name that gets called in the wrong place later.
void SilenceBeacon();

// ---- the SOUNDSCAPE channel ---------------------------------------------------------------------
// Play `clip` on the soundscape's voice pool. Same spatial treatment as PlayPing — one definition of
// pan and of behind, shared — but OVERLAPPING: the voice chosen is the one with the least audio still
// queued, so a ping only ever cuts another when every voice is genuinely busy.
//
// Arguments mean exactly what they mean above. `pitch` carries information here rather than emphasis:
// see soundscape.cpp for what a shifted pitch is telling the player.
void PlayScape(const AudioClips::Clip* clip, float pan, float front, float gain, float pitch);

// Cut every soundscape voice. Used when the soundscape is switched off, or suspended while voices are
// still sounding.
void SilenceScape();

} // namespace AudioEngine

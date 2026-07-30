#pragma once

// Resource ids for the beacon sounds embedded in the DLL as RCDATA.
//
// The sounds ship INSIDE dinput8.dll rather than as loose files beside it: there is nothing extra
// to deploy, nothing extra in the release zip, no path to resolve at runtime and no missing-file
// case to degrade through. See beacon_assets.rc for the embed and audio_clips.cpp for the load.
//
// The .wav files under assets/ are STRIPPED to bare `fmt ` + `data`. The originals in
// `FF 12 SFX\` are DAW-library exports whose metadata (Soundminer / Pro Tools LIST, bext, iXML,
// filr, SMED, ...) is most of the file -- objective.wav is 188,498 bytes of which 54,240 is audio.
// Do not re-copy the originals here.
//
// This header is included by BOTH the .rc and the C++ loader, so the ids cannot drift apart.

#define IDR_SND_OBJECTIVE      101
#define IDR_SND_ACTIVE_TARGET  102

#pragma once

// Resource ids for the mod's sounds, embedded in the DLL as RCDATA.
//
// The sounds ship INSIDE dinput8.dll rather than as loose files beside it: there is nothing extra
// to deploy, nothing extra in the release zip, no path to resolve at runtime and no missing-file
// case to degrade through. See beacon_assets.rc for the embed and audio_clips.cpp for the load.
//
// The .wav files under assets/ are STRIPPED to bare `fmt ` + `data`. The originals in
// `FF 12 SFX\` are DAW-library exports whose metadata (Soundminer / Pro Tools LIST, bext, iXML,
// filr, SMED, ...) is most of the file -- objective.wav is 188,498 bytes of which 54,240 is audio.
// Do not re-copy the originals here. The strip is byte-faithful on the AUDIO: the `data` chunk is
// copied verbatim, so `gate_crystal` and `save_crystal` stay stereo and are down-mixed at load time
// by SDL, exactly as they always were.
//
// This header is included by BOTH the .rc and the C++ loader, so the ids cannot drift apart.
//
// 101-102 are the two BEACON sounds (Session 92). 103-112 are the SOUNDSCAPE sounds (Session 191) --
// one per EntityList::Category that has one. Ids are append-only: an .rc id is not a user-visible
// thing, but renumbering one silently re-points a sound, so new sounds take the next free number.

#define IDR_SND_OBJECTIVE      101
#define IDR_SND_ACTIVE_TARGET  102

#define IDR_SND_EXIT           103   // Category::Exit          (Entrance.wav)
#define IDR_SND_DOOR           104   // Category::Door
#define IDR_SND_SHOP           105   // Category::Shop
#define IDR_SND_SAVE_CRYSTAL   106   // Category::SaveCrystal
#define IDR_SND_GATE_CRYSTAL   107   // Category::GateCrystal
#define IDR_SND_TREASURE       108   // Category::Treasure
#define IDR_SND_NPC            109   // Category::NPC
#define IDR_SND_OBJECT         110   // Category::Object        (interactable.wav)
#define IDR_SND_ENEMY          111   // Category::Enemy
#define IDR_SND_ITEM           112   // Category::Items

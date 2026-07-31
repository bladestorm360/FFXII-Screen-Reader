#pragma once

#include <cstddef>
#include <cstdint>
#include "navigation/nav_types.h"

// AUTO-WALK (Session 100) -- the mod walks the character along the route `\` just planned.
//
// THE ONE RECORDED EXCEPTION to the strictly-read-only-input rule, user-authorized 2026-07-31 (see
// CLAUDE.md). The whole write surface is `OnDevicePoll`: it may OR the four movement-key bits
// (DIK W/A/S/D and nothing else) into the keyboard state buffer inside `HookedGetDeviceState`,
// AFTER the real state has been read and AFTER `InputTracker::FeedDInputKeyboard` was fed the
// PRE-injection buffer -- so every mod-side observation sees only the player's real keys. Bounds,
// all non-negotiable: default OFF (ModMenu toggle); with the toggle off the injection function
// returns on its first line and the input path is byte-identical to the read-only mod; a real
// movement key in the same poll SUPPRESSES injection in that poll and cancels the feature -- the
// player always wins, instantly; and it disengages on combat, route loss, map change, focus loss,
// menu open, field-tick stall, and a no-progress cap. Extending injection to any other key is a
// NEW category change requiring new explicit permission.
//
// ONE ROUTE AUTHORITY. Auto-walk consumes the audio beacon's leg state (AudioBeacon::
// GetLegSnapshot) -- it never plans, never keeps its own copy of the route, and never re-derives
// the bearing: steering renders NavCommon::RelativeOctant, the SAME number the spoken legs and the
// beacon pan are renderings of (the S92 rule).
//
// THREADING. All state lives on the game thread (`OnGameFrame`, called from the field-frame hook
// right after AudioBeacon::OnGameFrame, so the leg snapshot is same-frame fresh). The input-poll
// thread touches four relaxed atomics and OR's at most two bits; it takes no locks and reads no
// leg state. A mask stamp makes injection die passively within 250 ms whenever the field tick
// stops (map transition, pause, stall) -- no code has to know why.
//
// No Init/Shutdown: static atomics only, no hooks of its own, nothing to register. Do not add one
// to navigation.cpp reflexively.
namespace AutoWalk {

// True while auto-walk is commanding movement. Relaxed atomic -- any thread. The beacon's stuck
// detector OR's this in as "movement is being attempted".
bool Engaged();

// INPUT THREAD, from `\` (NavCommands::RouteToCurrent). Arms a pending engage if the ModMenu
// toggle is ON; the game thread completes the engage once the beacon reports an active route.
// A plan that fails (Frontier / NoPath) never seeds the beacon, so auto-walk structurally cannot
// engage on one -- the pending stamp just expires.
void NotifyRoutePressed();

// GAME THREAD, once per field frame, AFTER AudioBeacon::OnGameFrame.
void OnGameFrame();

// INPUT-POLL THREAD, from HookedGetDeviceState, AFTER FeedDInputKeyboard. `dik256` is the game's
// freshly-filled 256-byte DIK buffer. The only function in the mod that writes input.
void OnDevicePoll(unsigned char* dik256);

// GAME THREAD, from the beacon's stuck detector while engaged: the ground-truth impassibility
// record -- exactly where the engine refused a commanded walk, with what was being injected.
void OnStuckFired(const FVec3& pos, size_t legIndex, size_t legCount, float distToCorner);

} // namespace AutoWalk

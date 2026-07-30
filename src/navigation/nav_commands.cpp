#include "navigation/nav_commands.h"
#include "navigation/entity_list.h"
#include "ui/mod_menu.h"
#include "navigation/path_planner.h"
#include "navigation/nav_probe.h"
#include "navigation/nav_types.h"
#include "ui/battle_target_reader.h"
#include "navigation/interact_target.h"
#include "battle/party_status.h"
#include "battle/combat_log.h"
#include "core/logger.h"
#include "speech/speech.h"
#include "speech/phrasebook.h"

#include <Windows.h>
#include <cstdio>
#include <string>

namespace NavCommands {

namespace {

// `\` — request a turn-by-turn route to the current selection. The actual A* runs on
// the game thread (crash-safe on transitions); the legs (or "No path") are spoken from
// there a frame or two later. We only capture the fixed world target here.
void RouteToCurrent() {
    FVec3 pos; std::wstring label;
    bool isTransition = false;   // exits only: the target is the map-jump surface itself
    void* sceneObj = nullptr;    // needed for the interaction band -- see below
    if (!EntityList::GetCurrentTarget(pos, label, &isTransition, &sceneObj)) {
        // Front-of-pipeline diagnostic: distinguishes "\\ produced no target" from
        // "\\ never reached us" (no NAV-ROUTE line at all) when tracing the route failure.
        Log::Write("NAV-ROUTE", "'\\' (route) pressed: GetCurrentTarget returned no target -> \"No target\"");
        Speech::Output(Phrase::Get(Phrase::Id::NoTarget));
        return;
    }
    Log::Write("NAV-ROUTE", "'\\' (route) pressed: target acquired -> PathPlanner::Request");
    // Route to where you could STAND and interact, not to where the object is. Both halves of the
    // engine's own predicate come along: the vertical BAND says which surfaces you could interact
    // from, and the horizontal REACH says how close you have to be -- so the search can stop exactly
    // where `;` starts answering instead of walking you onto the target.
    //
    // A transition is excluded on purpose: its destination IS the surface you walk onto.
    InteractTarget::Band  band;
    InteractTarget::Reach reach;
    if (!isTransition) {
        band  = InteractTarget::ReadBandFor(sceneObj);
        reach = InteractTarget::ReadReachFor(sceneObj);
    }
    // radiusMin, not radius: the ellipse radius is direction-dependent, and a goal poly has to be
    // interactable from whatever angle the route happens to arrive at.
    const float reachRadius = reach.valid ? reach.radiusMin : 0.0f;
    // seedBeacon=true: `\` is the "lead me there" key, so its route arms the audio beacon.
    if (band.valid) PathPlanner::Request(pos, label, isTransition, band.lo, band.hi, reachRadius, true);
    else            PathPlanner::Request(pos, label, isTransition, 1.0f, -1.0f, 0.0f, true);
}

// `p` — request a turn-by-turn route to the game's LOCKED/SELECTED battle target (bypasses the
// mod's [ / ] cursor, addressing "loses focus in combat"). Same PathPlanner pipe as `\`; the only
// difference is the target source: the battle target-selection/lock reader (DAT_0209be80), read
// from its live cache. Fresh cache (<=300 ms) => a target is currently selected/locked; otherwise
// "No target". The NAV-ROUTE lines here + the drain log answer the PRE-SHIP CHECK (does Lock-On
// keep the object populated, and does the field stay nav-safe in battle-state mode).
void RouteToLockedTarget() {
    FVec3 tgt; std::wstring label;
    // Gates on the LIVE DAT_0209be80 selection state (not a cache age window) so a held target keeps
    // routing on every press, and re-resolves a fresh position for a moving target.
    if (!BattleTargetReader::GetLockedTarget(tgt, label)) {
        Log::Write("NAV-ROUTE", "'p' (route to locked target) pressed: no live locked target -> \"No target\"");
        Speech::Output(Phrase::Get(Phrase::Id::NoTarget));
        return;
    }
    char m[160];
    snprintf(m, sizeof(m), "'p' (route to locked target) pressed: target acquired at (%.2f,%.2f,%.2f) -> PathPlanner::Request",
             tgt.x, tgt.y, tgt.z);
    Log::Write("NAV-ROUTE", m);
    PathPlanner::Request(tgt, label);
}

// `;` — speak the ACTIVE TARGET's status (name + vitals), for whatever the game currently has
// selected. Works out of battle too: the target-selection state is what the cursor keys (I/J/K/L,
// Q/E) drive, not something battle-only.
//
// (This key used to be the true-north facing readout. Dropped: orientation isn't needed — the route
// directions are egocentric and pathfinding works without knowing where the camera points.)
// (implemented in battle_target_reader, which owns the target state + the vitals formatting)

// `'` -- the navigation diagnostic probe. Only raises a flag here; the probe itself runs on the GAME
// THREAD (nav_probe.cpp), because it needs MapQuery::GroundAt and that is a game call.
//
// This replaced a ~165-line inline dump that emitted the move-frame snapshot, the wall self-test, the
// walkmap grid cross-check, the +0x70 / +0x54 legacy exit tables, ExitDiag::DumpCoverage and
// MapExits::DiagScanScriptMapjumps -- the last of which alone hex-dumped 0x9000 bytes as ~1,152 lines.
// All of it measured questions that are already answered; none of it measured the ones still open.
// See nav_probe.h for what the key reports now.

} // namespace

// NOTE: no `shift` parameter. The game binds Left Shift to Toggle Walk/Run and the mod cannot
// swallow keys, so a Shift chord would silently flip walk/run on every press (input_tracker.cpp).
// The old parameter was passed `false` at every call site and could never be honoured -- a dead
// argument that made an impossible chord look like a working feature. Do not reintroduce it.
void OnNavKey(int vk) {
    switch (vk) {
        case VK_OEM_5:      RouteToCurrent();                 break;  // \  turn-by-turn route
        case 'P':           RouteToLockedTarget();           break;  // p  route to locked battle target
        case VK_OEM_4:      EntityList::CmdPrev();            break;  // [  previous object
        case VK_OEM_6:      EntityList::CmdNext();            break;  // ]  next object
        case VK_OEM_3:      EntityList::CmdRescan();          break;  // `  rescan + area
        // F4 speaks the new value; F8 opens/closes the menu that holds the same setting. Both route
        // through ModMenu so there is exactly one place a value changes, persists and is announced.
        case VK_F4:         ModMenu::CycleSetting(ModMenu::SettingId::CombatVerbosity); break;
        // Same arrangement for the beacon: F9 is the shortcut, the F8 menu holds the same value.
        // Turning it OFF silences a running beacon; turning it ON only re-arms the feature, since an
        // On press has no destination to aim at -- press `\` to seed one.
        case VK_F9:         ModMenu::CycleSetting(ModMenu::SettingId::AudioBeacon);     break;
        case VK_F8:         ModMenu::Toggle();                break;  // F8 mod menu
        case VK_OEM_MINUS:  EntityList::CmdPrevCategory();    break;  // -  previous category
        case VK_OEM_PLUS:   EntityList::CmdNextCategory();    break;  // =  next category
        case VK_F5:         EntityList::CmdToggleAvailability(); break; // F5 all <-> story-gated
        case VK_F6:         EntityList::CmdLabelFromClipboard(); break;  // F6 label focus from clipboard
        case VK_OEM_2:      EntityList::CmdDescribeCurrent(); break;  // /  describe current
        // `;` is context-gated, not double-bound: the battle reader is STRUCTURALLY silent in the
        // field (it needs a commitment or an open select UI), so its false return is the field
        // case. In battle you get the battle target; outside it, who Confirm will address.
        case VK_OEM_1:
            if (!BattleTargetReader::SpeakTargetStatus()) InteractTarget::SpeakCurrent();
            break;                                                // ;  target status / interact target
        case VK_OEM_7:      NavProbe::Request();              break;  // '  diagnostic probe (game thread)
        case '4':           PartyStatus::SpeakSlot(0);        break;  // 4  party slot 1 status
        case '5':           PartyStatus::SpeakSlot(1);        break;  // 5  party slot 2 status
        case '6':           PartyStatus::SpeakSlot(2);        break;  // 6  party slot 3 status
        case '7':           PartyStatus::SpeakSlot(3);        break;  // 7  guest slot (silent if none)
        case VK_OEM_COMMA:  CombatLog::StepBack();            break;  // ,  combat log: older
        case VK_OEM_PERIOD: CombatLog::StepForward();         break;  // .  combat log: newer
        case VK_HOME:       CombatLog::JumpOldest();          break;  // Home  oldest entry
        case VK_END:        CombatLog::JumpNewest();          break;  // End   newest entry
        default:            break;
    }
}

} // namespace NavCommands

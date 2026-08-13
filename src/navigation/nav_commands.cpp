#include "navigation/nav_commands.h"
#include "navigation/entity_list.h"
#include "ui/mod_menu.h"
#include "navigation/auto_walk.h"
#include "navigation/path_planner.h"
#include "navigation/nav_probe.h"
#include "navigation/shout_meter.h"
#include "navigation/statue_guide.h"
#include "navigation/nav_types.h"
#include "ui/battle_target_reader.h"
#include "ui/equip_compare.h"
#include "ui/equip_target_reader.h"
#include "navigation/interact_target.h"
#include "battle/party_status.h"
#include "ui/save_reader.h"
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
// How near an exit counts as arriving at it, for routing purposes. Deliberately the SAME numbers
// PathPlanner uses to say "At the exit" (kAtExitDist / kAtExitDy) -- if the planner would call the
// player arrived there, the search must be allowed to finish there.
constexpr float kExitArriveDist = 3.0f;
constexpr float kExitArriveDy   = 3.0f;

void RouteToCurrent() {
    FVec3 pos; std::wstring label;
    bool isTransition = false;   // exits only: the target is the map-jump surface itself
    void* sceneObj = nullptr;    // needed for the interaction band -- see below
    // The map-jump group, 0 unless this is a walk-onto surface. `pos` is one vertex of that surface;
    // this is the handle to the rest of it, for the failure path in PathSearch. See Entity::seamGroup.
    int seamGroup = 0;
    if (!EntityList::GetCurrentTarget(pos, label, &isTransition, &sceneObj, &seamGroup)) {
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
    } else {
        // AN EXIT IS ARRIVED AT, NOT LANDED ON (Session 96).
        //
        // Transitions used to be given no band and no reach at all, which makes PathSearch require A*
        // to finish on the exit's OWN polygon and nothing else. That is stricter than the rest of the
        // mod: PathPlanner already calls anything within kAtExitDist "At the exit" and stops routing.
        //
        // It produced a FALSE "No path" on an exit the tester then walked to by hand. The log shows why:
        // `reach=1` (NavReach found a reachable poly within its 4.5 m slack) while A* failed, and
        // `edge=6` -- the edge tests were barely refusing anything, so the search was not walled in, it
        // simply could not finish on the one polygon it was told to finish on. A map-jump surface can
        // easily be bordered by water on the sides you would never approach from.
        //
        // Supplying a band and a reach turns on `NoteFallback`, the machinery that already records the
        // first poly A* pops that you could stand on and interact from -- proven code, used by every
        // non-transition target since S73. Nothing else changes: if the exit's own poly IS reachable,
        // IsGoal still matches it first and the fallback is never consulted.
        band.valid  = true;
        band.lo     = pos.y - kExitArriveDy;
        band.hi     = pos.y + kExitArriveDy;
        reach.valid = true;
        reach.radiusMin = kExitArriveDist;
    }
    // radiusMin, not radius: the ellipse radius is direction-dependent, and a goal poly has to be
    // interactable from whatever angle the route happens to arrive at.
    const float reachRadius = reach.valid ? reach.radiusMin : 0.0f;
    // seedBeacon=true: `\` is the "lead me there" key, so its route arms the audio beacon.
    if (band.valid) PathPlanner::Request(pos, label, isTransition, band.lo, band.hi, reachRadius, true, seamGroup);
    else            PathPlanner::Request(pos, label, isTransition, 1.0f, -1.0f, 0.0f, true, seamGroup);
    // S100: with the Auto-walk toggle ON, `\` also walks the route. Only a pending stamp here --
    // the game thread engages once the beacon reports an active route, so a plan that fails
    // (Frontier / "No path") structurally cannot start the character walking. `p` deliberately
    // gets no equivalent call: its route never arms the beacon.
    AutoWalk::NotifyRoutePressed();
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

// Column `n` (1-based) of the equipment comparison, if one is on screen. Returns false when there
// is no live panel, so the caller falls through to whatever the key means elsewhere.
//
// Requesting a column that does not exist is SILENT, not "no such character" -- a party of four
// leaves keys 8 and 9 addressing nothing, and filler there would be worse than nothing.
bool EquipColumnKey(int n) {
    if (!EquipCompare::IsLive()) return false;
    const std::wstring line = EquipCompare::LineFor(n);
    if (line.empty()) return true;                 // panel is live but that column is not: swallow
    Log::WriteW("EQUIP", "column:", line);
    Speech::Output(line, /*interrupt=*/true);
    return true;
}

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
        // F11, not F9: the game owns F9 ("Hide On-Screen Keyboard", S112). Bare press only --
        // Shift+F11 belongs to NVDA; the guard is in input_tracker's edge registration.
        case VK_F11:        ModMenu::CycleSetting(ModMenu::SettingId::AudioBeacon);     break;
        // F7, same arrangement again: the shortcut for the setting the F8 menu also holds. It
        // changes only what the mod VOLUNTEERS -- the shop's `4`-`9` columns and `o`'s Libra readout
        // answer identically whichever way it is set, because a toggle that took away a way to ASK
        // would be a regression rather than a setting.
        case VK_F7:         ModMenu::CycleSetting(ModMenu::SettingId::AutoDetail);      break;
        // F10 IS NOT BOUND (Session 115). It held the sneak-assist toggle from S107 to S114; that
        // feature now acts automatically on the maps `path_danger.cpp` names and has no setting to
        // switch, so the key went back to the game. Do not re-bind it without checking the
        // on-screen-keyboard overlay first -- the config screen is not evidence a key is free
        // (S112, the F9 collision).
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
        // 4-9: CONTEXT-GATED. While an equipment comparison is on screen (a shop list highlight or
        // the equip-to-whom screen) these address its per-character COLUMNS; everywhere else 4-7
        // keep their party-slot meaning, 8 reads the summoned Esper and 9 does nothing. The gate is
        // structural -- a live, class-validated panel -- not a cached flag, so leaving the shop
        // restores party status with no state to get stuck.
        case '4': case '5': case '6': case '7':
        case '8': case '9': {
            const int n = vk - '4' + 1;                       // 4 -> column 1 ... 9 -> column 6
            if (EquipColumnKey(n)) break;
            // Save/load slot list: address the HIGHLIGHTED save's party instead of the live one.
            // Same structural gate as the equipment columns -- a class-validated live window, no
            // cached flag -- so leaving the screen restores party status with nothing to get stuck.
            if (SaveReader::PartyMemberKey(n)) break;
            // 8 = the summoned Esper (S148). It is NOT roster slot 4: an Esper is absent from roster
            // list 3 entirely, which is why 4-7 could never reach it. Silent when none is out, the
            // same way 7 is silent with no guest -- so outside a summon 8 behaves exactly as before.
            if (vk == '8')      PartyStatus::SpeakEsper();
            else if (vk <= '7') PartyStatus::SpeakSlot(vk - '4');   // 9 stays silent outside a shop
            break;
        }
        case VK_OEM_COMMA:  CombatLog::StepBack();            break;  // ,  combat log: older
        case VK_OEM_PERIOD: CombatLog::StepForward();         break;  // .  combat log: newer
        // Bhujerba shout minigame. Both only raise a flag: the work needs live transforms and the
        // game's own name tables, so it drains on the next field frame (the `'` probe's arrangement
        // above). Off a shout map both are silent no-ops -- the dispatcher always accepts the key
        // and ShoutMeter decides, which is what keeps the no-op quiet rather than "not available
        // here".
        // `B` IS CONTEXT-GATED, NOT DOUBLE-BOUND. Both requests are raised and each drains on the
        // next field frame against its own structural gate -- a live shout sequence for the meter, a
        // live `mrm_` script for the statues -- and the two contexts can never both be live (one is
        // the Bhujerba streets, the other is the Stilshrine of Miriam). Raising both here rather
        // than asking which applies keeps the decision on the game thread, where reading script
        // modules is safe; deciding on the input thread is what the `'` probe's arrangement avoids.
        case 'B':
            ShoutMeter::RequestMeterCheck();                          // B  infamy meter
            StatueGuide::RequestCheck();                              // B  statue puzzle status
            break;
        case 'N':           ShoutMeter::RequestGuardCheck();  break;  // N  nearest NPCs
        case VK_HOME:       CombatLog::JumpOldest();          break;  // Home  oldest entry
        case VK_END:        CombatLog::JumpNewest();          break;  // End   newest entry
        default:            break;
    }
}

} // namespace NavCommands

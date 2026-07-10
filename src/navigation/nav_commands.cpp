#include "navigation/nav_commands.h"
#include "navigation/entity_list.h"
#include "navigation/player_state.h"
#include "navigation/nav_common.h"
#include "navigation/path_planner.h"
#include "navigation/nav_types.h"
#include "core/logger.h"
#include "speech/speech.h"

#include <Windows.h>
#include <cstdio>
#include <string>

namespace NavCommands {

namespace {

// `/` — request a turn-by-turn route to the current selection. The actual A* runs on
// the game thread (crash-safe on transitions); the legs (or "No path") are spoken from
// there a frame or two later. We only capture the fixed world target here.
void RouteToCurrent() {
    FVec3 pos; std::wstring label;
    if (!EntityList::GetCurrentTarget(pos, label)) {
        // Front-of-pipeline diagnostic: distinguishes "/ produced no target" from
        // "/ never reached us" (no NAV-ROUTE line at all) when tracing the route failure.
        Log::Write("NAV-ROUTE", "'/' pressed: GetCurrentTarget returned no target -> \"No target\"");
        Speech::Output(L"No target");
        return;
    }
    Log::Write("NAV-ROUTE", "'/' pressed: target acquired -> PathPlanner::Request");
    PathPlanner::Request(pos, label);
}

void SpeakFacing() {
    float yaw = 0.0f;
    if (!PlayerState::ReadPlayerYaw(yaw)) { Speech::Output(L"Facing unavailable"); return; }
    std::wstring s = L"Facing ";
    s += NavCommon::CardinalOfHeading(yaw);
    Speech::Output(s);
}

void DiagnosticDump() {
    FVec3 p;
    if (PlayerState::ReadPlayerPos(p)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "player pos=(%.2f, %.2f, %.2f)", p.x, p.y, p.z);
        Log::Write("NAV-DIAG", msg);
    } else {
        Log::Write("NAV-DIAG", "player pos unavailable");
    }
    EntityList::Rescan();
    EntityList::LogDiagnostic();
    Speech::Output(L"Diagnostic logged");
}

} // namespace

void OnNavKey(int vk, bool /*shift*/) {
    switch (vk) {
        case VK_OEM_5:      EntityList::CmdDescribeCurrent(); break;  // \  describe current
        case VK_OEM_4:      EntityList::CmdPrev();            break;  // [  previous object
        case VK_OEM_6:      EntityList::CmdNext();            break;  // ]  next object
        case VK_OEM_3:      EntityList::CmdRescan();          break;  // `  rescan + area
        case VK_OEM_MINUS:  EntityList::CmdPrevCategory();    break;  // -  previous category
        case VK_OEM_PLUS:   EntityList::CmdNextCategory();    break;  // =  next category
        case VK_OEM_1:      SpeakFacing();                    break;  // ;  facing readout
        case VK_OEM_2:      RouteToCurrent();                 break;  // /  turn-by-turn route
        case VK_OEM_7:      DiagnosticDump();                 break;  // '  diagnostic dump
        default:            break;
    }
}

} // namespace NavCommands

#include "navigation/nav_commands.h"
#include "navigation/entity_list.h"
#include "navigation/player_state.h"
#include "navigation/nav_common.h"
#include "core/logger.h"
#include "speech/speech.h"

#include <Windows.h>
#include <cstdio>
#include <string>

namespace NavCommands {

namespace {

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
        case VK_OEM_7:      DiagnosticDump();                 break;  // '  diagnostic dump
        default:            break;
    }
}

} // namespace NavCommands

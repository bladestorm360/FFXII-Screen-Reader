#include "navigation/navigation.h"
#include "navigation/nav_hooks.h"
#include "navigation/nav_commands.h"
#include "navigation/entity_list.h"
#include "core/logger.h"
#include "input/input_tracker.h"

namespace Navigation {

bool Init() {
    // Physics-context capture hook (for turn-by-turn walkability, Layer 3).
    bool ok = NavHooks::Init();
    // Field-object list (walks the game's actor pool on demand).
    EntityList::Init();
    // Route nav hotkeys (\ [ ] ` + Shift variants) to the command dispatcher.
    InputTracker::SetNavKeyCallback(&NavCommands::OnNavKey);
    Log::Write("NAV", "navigation ready: \\=describe  [/]=object  -/==category  "
                      "`=rescan  ;=facing  '=diagnostic");
    return ok;
}

void Shutdown() {
    EntityList::Shutdown();
    NavHooks::Shutdown();
}

} // namespace Navigation

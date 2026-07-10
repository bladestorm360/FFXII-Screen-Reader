#include "navigation/navigation.h"
#include "navigation/nav_hooks.h"
#include "navigation/nav_commands.h"
#include "navigation/entity_list.h"
#include "navigation/path_planner.h"
#include "core/logger.h"
#include "input/input_tracker.h"

namespace Navigation {

bool Init() {
    // Turn-by-turn route planner (Layer 3) — must init before the hooks, which drain it.
    PathPlanner::Init();
    // Physics-context capture + game-thread route-planner hooks.
    bool ok = NavHooks::Init();
    // Field-object list (walks the game's actor pool on demand).
    EntityList::Init();
    // Route nav hotkeys to the command dispatcher.
    InputTracker::SetNavKeyCallback(&NavCommands::OnNavKey);
    Log::Write("NAV", "navigation ready: \\=describe  [/]=object  -/==category  "
                      "`=rescan  ;=facing  /=route  '=diagnostic");
    return ok;
}

void Shutdown() {
    EntityList::Shutdown();
    NavHooks::Shutdown();
    PathPlanner::Shutdown();
}

} // namespace Navigation

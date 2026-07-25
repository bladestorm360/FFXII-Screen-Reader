#include "navigation/navigation.h"
#include "navigation/nav_hooks.h"
#include "navigation/nav_commands.h"
#include "navigation/entity_list.h"
#include "navigation/item_scan.h"
#include "navigation/path_planner.h"
#include "core/logger.h"
#include "input/input_tracker.h"

namespace Navigation {

bool Init() {
    // Turn-by-turn route planner (Layer 3) — must init before the hooks, which drain it.
    PathPlanner::Init();
    // Physics-context capture + game-thread route-planner hooks.
    bool ok = NavHooks::Init();
    // Ground-loot hooks — must init BEFORE the entity list, which scans the pool they feed.
    ok &= ItemScan::Init();
    // Field-object list (walks the game's actor pool on demand).
    EntityList::Init();
    // Route nav hotkeys to the command dispatcher.
    InputTracker::SetNavKeyCallback(&NavCommands::OnNavKey);
    Log::Write("NAV", "navigation ready: \\=route  [/]=object  -/==category  F5=all/story-gated  "
                      "`=rescan  /=describe  ;=target status  '=diagnostic  "
                      "(directions are EGOCENTRIC: ahead/left/right/behind, relative to where UP walks you)");
    return ok;
}

void Shutdown() {
    EntityList::Shutdown();
    ItemScan::Shutdown();
    NavHooks::Shutdown();
    PathPlanner::Shutdown();
}

} // namespace Navigation

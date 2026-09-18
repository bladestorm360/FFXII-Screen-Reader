#include "navigation/navigation.h"
#include "navigation/nav_hooks.h"
#include "navigation/soundscape.h"
#include "navigation/nav_commands.h"
#include "navigation/entity_list.h"
#include "navigation/item_scan.h"
#include "navigation/treasure_state.h"
#include "navigation/path_planner.h"
#include "navigation/sneak_assist.h"
#include "navigation/shout_meter.h"
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
    // Collected-treasure record — must init BEFORE the entity list for the same reason ItemScan
    // does: the scan consults it on every rescan. Non-fatal by design, and fail-safe: a failed
    // install means collected treasure stays listed, which is exactly the behaviour before it.
    TreasureState::Init();
    // Sneak assist (S106; always-on for the danger-table maps since S115). Non-fatal by design: a
    // failed install just means the guards behave exactly as the game wrote them.
    SneakAssist::Init();
    // Bhujerba shout minigame: the spoken infamy meter and its two keys. Non-fatal by design --
    // a failed install means the gauge simply never speaks, exactly as before the feature existed.
    ShoutMeter::Init();
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
    TreasureState::Shutdown();
    ItemScan::Shutdown();
    NavHooks::Shutdown();
    // AFTER the hooks are gone, so the field tick can no longer be mid-sweep while we clear it.
    Soundscape::Stop();
    PathPlanner::Shutdown();
}

} // namespace Navigation

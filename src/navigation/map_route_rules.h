#pragma once

#include <cstdint>

// PER-MAP ROUTING RULES -- a small table, one row per map, one flag per MECHANISM (S183).
//
// WHY A TABLE AND NOT A GLOBAL CHANGE. The user's ruling on Falls of Time (2026-09-17): a route through
// its water is invalid -- "waterfall or ordinary water, it's an invalid route" -- and the fix must be MAP
// SPECIFIC, "so you don't change routing on every map". Everywhere else the S96 rule stands: ground the
// leader's class refuses is a PRICE, because the party wades water the flag test refuses (map 311).
//
// WHY NOT THE DANGER TABLE. `PathDanger`'s rows are sneak assist's whole map whitelist; a row there would
// switch sneak assist on for the map. This is the S121 shape (`PathDanger::MapUsesEngineCatch`): a
// constexpr table whose rows carry a flag per mechanism, and new machinery keys on ITS OWN flag, never on
// "the map has a row". L-49 bans map-id special cases inside algorithms; a per-map feature table with its
// own mechanism flag is the exception the user sanctioned.
//
// Widening to another map needs that map's own play evidence and its own row (L-48).
namespace MapRouteRules {

// On this map, A* CUTS class-refused ground (a poly that is not walkable, or that the leader's floor class
// refuses) instead of pricing it, and the adjacency march refuses to graze across it. The route's own start
// and goal polys, and the goal's own map-jump surface, stay exempt. One linear scan of a constexpr table;
// no game reads, any thread.
bool MapRefusesTerrain(uint32_t mapId);

} // namespace MapRouteRules

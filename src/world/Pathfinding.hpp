#pragma once
//
// A* pathfinding on the 8-directional tile grid.
//
#include <optional>
#include <vector>

#include "Map.hpp"

namespace sh {

// Returns the single best next step from `from` toward `to`, or nullopt if no
// path exists. Tiles in `blocked` (e.g. other monsters) are treated as
// impassable, except `to` itself which is always reachable so a monster can
// path right up to the player.
std::optional<Vec2> next_step_towards(const Map& map, Vec2 from, Vec2 to,
                                      const std::vector<Vec2>& blocked);

} // namespace sh

#pragma once
//
// Procedural dungeon generation via Binary Space Partitioning (BSP).
//
#include <vector>

#include "../core/Rng.hpp"
#include "Map.hpp"

namespace sh {

struct Room {
    int x{0};
    int y{0};
    int w{0};
    int h{0};

    [[nodiscard]] Vec2 center() const { return {x + w / 2, y + h / 2}; }
};

struct DungeonResult {
    Map map;
    std::vector<Room> rooms;
    Vec2 player_start{};
    Vec2 stairs{};
    std::vector<Vec2> barrels;  // explosive-barrel spawn positions
    std::vector<Vec2> crates;   // pushable crate spawn positions
};

// Builds a fully connected dungeon of the requested size. `depth` is the floor
// number, available for future difficulty scaling of the layout.
DungeonResult generate_dungeon(int w, int h, Rng& rng, int depth);

} // namespace sh

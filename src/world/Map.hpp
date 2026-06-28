#pragma once
//
// Tile + Map — the static level geometry.
//
// A Tile knows its type plus two visibility flags maintained by the FOV system:
//   visible  : currently lit / in line-of-sight this turn
//   explored : has ever been seen (drawn dim once out of sight, "memory")
//
#include "../core/Grid.hpp"
#include "../core/Vec2.hpp"

namespace sh {

enum class TileType : unsigned char {
    Wall,
    Floor,
    StairsDown,
};

struct Tile {
    TileType type{TileType::Wall};
    bool explored{false};
    bool visible{false};
};

class Map {
public:
    Map() = default;
    Map(int w, int h) : tiles_(w, h, Tile{}) {}

    Grid<Tile>& grid() { return tiles_; }
    const Grid<Tile>& grid() const { return tiles_; }

    [[nodiscard]] int width() const { return tiles_.width(); }
    [[nodiscard]] int height() const { return tiles_.height(); }

    [[nodiscard]] bool in_bounds(Vec2 p) const { return tiles_.in_bounds(p); }

    Tile& at(Vec2 p) { return tiles_.at(p); }
    const Tile& at(Vec2 p) const { return tiles_.at(p); }

    [[nodiscard]] bool is_wall(Vec2 p) const {
        return !in_bounds(p) || tiles_.at(p).type == TileType::Wall;
    }
    [[nodiscard]] bool blocks_sight(Vec2 p) const { return is_wall(p); }
    [[nodiscard]] bool walkable(Vec2 p) const {
        return in_bounds(p) && tiles_.at(p).type != TileType::Wall;
    }

private:
    Grid<Tile> tiles_;
};

} // namespace sh

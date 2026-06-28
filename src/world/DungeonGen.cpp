//
// BSP dungeon generation.
//
// The map starts as solid rock. We recursively split it into a binary tree of
// sub-regions (owned through std::unique_ptr, so the whole tree frees itself),
// carve one room into each leaf, then walk back up the tree connecting sibling
// sub-trees with L-shaped corridors. Because every internal node joins its two
// children, the finished dungeon is guaranteed fully connected.
//
#include "DungeonGen.hpp"

#include <algorithm>
#include <memory>
#include <optional>

namespace sh {
namespace {

struct BspNode {
    int x{0};
    int y{0};
    int w{0};
    int h{0};
    std::unique_ptr<BspNode> left;
    std::unique_ptr<BspNode> right;
    std::optional<Room> room;
};

constexpr int kMinLeaf = 11; // smallest region we are willing to split into

std::unique_ptr<BspNode> build(int x, int y, int w, int h, Rng& rng, int budget) {
    auto node = std::make_unique<BspNode>();
    node->x = x;
    node->y = y;
    node->w = w;
    node->h = h;

    const bool can_split_x = w >= kMinLeaf * 2;
    const bool can_split_y = h >= kMinLeaf * 2;

    if (budget > 0 && (can_split_x || can_split_y)) {
        bool split_x = can_split_x;
        if (can_split_x && can_split_y) split_x = rng.chance(0.5);

        if (split_x) {
            const int cut = rng.range(kMinLeaf, w - kMinLeaf);
            node->left = build(x, y, cut, h, rng, budget - 1);
            node->right = build(x + cut, y, w - cut, h, rng, budget - 1);
        } else {
            const int cut = rng.range(kMinLeaf, h - kMinLeaf);
            node->left = build(x, y, w, cut, rng, budget - 1);
            node->right = build(x, y + cut, w, h - cut, rng, budget - 1);
        }
        return node;
    }

    // Leaf: carve a randomly sized room with a 1-tile margin inside the region
    // so neighbouring rooms never merge and the map border stays solid.
    const int rw = std::clamp(rng.range(5, w - 2), 4, std::max(4, w - 2));
    const int rh = std::clamp(rng.range(4, h - 2), 3, std::max(3, h - 2));
    const int rx = x + rng.range(1, std::max(1, w - rw - 1));
    const int ry = y + rng.range(1, std::max(1, h - rh - 1));
    node->room = Room{rx, ry, rw, rh};
    return node;
}

void carve_room(Map& map, const Room& r) {
    for (int yy = r.y; yy < r.y + r.h; ++yy) {
        for (int xx = r.x; xx < r.x + r.w; ++xx) {
            const Vec2 p{xx, yy};
            if (map.in_bounds(p)) map.at(p).type = TileType::Floor;
        }
    }
}

void carve_h(Map& map, int x1, int x2, int y) {
    for (int x = std::min(x1, x2); x <= std::max(x1, x2); ++x) {
        const Vec2 p{x, y};
        if (map.in_bounds(p)) map.at(p).type = TileType::Floor;
    }
}

void carve_v(Map& map, int y1, int y2, int x) {
    for (int y = std::min(y1, y2); y <= std::max(y1, y2); ++y) {
        const Vec2 p{x, y};
        if (map.in_bounds(p)) map.at(p).type = TileType::Floor;
    }
}

void carve_corridor(Map& map, Vec2 a, Vec2 b, Rng& rng) {
    if (rng.chance(0.5)) {
        carve_h(map, a.x, b.x, a.y);
        carve_v(map, a.y, b.y, b.x);
    } else {
        carve_v(map, a.y, b.y, a.x);
        carve_h(map, a.x, b.x, b.y);
    }
}

void collect_rooms(const BspNode* n, Map& map, std::vector<Room>& rooms) {
    if (n == nullptr) return;
    if (n->room) {
        carve_room(map, *n->room);
        rooms.push_back(*n->room);
    }
    collect_rooms(n->left.get(), map, rooms);
    collect_rooms(n->right.get(), map, rooms);
}

std::optional<Room> any_room(const BspNode* n) {
    if (n == nullptr) return std::nullopt;
    if (n->room) return n->room;
    if (auto r = any_room(n->left.get())) return r;
    return any_room(n->right.get());
}

void connect(const BspNode* n, Map& map, Rng& rng) {
    if (n == nullptr || !n->left || !n->right) return;
    connect(n->left.get(), map, rng);
    connect(n->right.get(), map, rng);
    const auto a = any_room(n->left.get());
    const auto b = any_room(n->right.get());
    if (a && b) carve_corridor(map, a->center(), b->center(), rng);
}

} // namespace

DungeonResult generate_dungeon(int w, int h, Rng& rng, int depth) {
    (void)depth;

    DungeonResult result;
    result.map = Map(w, h); // all walls

    const auto root = build(0, 0, w, h, rng, 5);
    collect_rooms(root.get(), result.map, result.rooms);
    connect(root.get(), result.map, rng);

    if (result.rooms.empty()) {
        const Room fallback{w / 2 - 5, h / 2 - 3, 10, 6};
        carve_room(result.map, fallback);
        result.rooms.push_back(fallback);
    }

    result.player_start = result.rooms.front().center();

    // Stairs go in the room furthest from the entrance, so each floor is a trek.
    const Room* farthest = &result.rooms.front();
    int best = -1;
    for (const auto& r : result.rooms) {
        const int d = r.center().manhattan(result.player_start);
        if (d > best) {
            best = d;
            farthest = &r;
        }
    }
    result.stairs = farthest->center();
    result.map.at(result.stairs).type = TileType::StairsDown;

    return result;
}

} // namespace sh

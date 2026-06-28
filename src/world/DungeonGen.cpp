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

// 8-directional flood fill over walkable tiles (pits/walls block).
bool reachable(const Map& m, Vec2 from, Vec2 to) {
    if (!m.walkable(from) || !m.walkable(to)) return false;
    std::vector<char> seen(static_cast<std::size_t>(m.width()) * m.height(), 0);
    auto idx = [&](Vec2 p) { return static_cast<std::size_t>(p.y) * m.width() + p.x; };
    std::vector<Vec2> q{from};
    seen[idx(from)] = 1;
    constexpr Vec2 dirs[8] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    for (std::size_t qi = 0; qi < q.size(); ++qi) {
        const Vec2 c = q[qi];
        if (c == to) return true;
        for (const Vec2 d : dirs) {
            const Vec2 nb = c + d;
            if (m.walkable(nb) && !seen[idx(nb)]) { seen[idx(nb)] = 1; q.push_back(nb); }
        }
    }
    return false;
}

// Scatter pits, spikes and explosive barrels through the (non-entrance) rooms,
// then guarantee the stairs are still reachable (reverting pits if not).
void place_hazards(DungeonResult& res, Rng& rng, int depth) {
    Map& m = res.map;
    auto is_floor = [&](Vec2 p) { return m.in_bounds(p) && m.at(p).type == TileType::Floor; };

    for (std::size_t ri = 1; ri < res.rooms.size(); ++ri) {
        const Room& r = res.rooms[ri];
        if (r.w < 5 || r.h < 5) continue;
        const Vec2 ctr = r.center();
        // Interior, away from the room centre (corridors connect centres).
        auto interior = [&](Vec2 p) {
            return p.x > r.x && p.x < r.x + r.w - 1 && p.y > r.y && p.y < r.y + r.h - 1 &&
                   p.chebyshev(ctr) >= 2 && is_floor(p);
        };
        auto pick = [&]() -> Vec2 {
            for (int t = 0; t < 14; ++t) {
                const Vec2 p{rng.range(r.x + 1, r.x + r.w - 2), rng.range(r.y + 1, r.y + r.h - 2)};
                if (interior(p)) return p;
            }
            return Vec2{-1, -1};
        };
        auto blob = [&](TileType type) {
            const Vec2 p = pick();
            if (!m.in_bounds(p)) return;
            m.at(p).type = type;
            if (rng.chance(0.5)) {
                const Vec2 q = p + Vec2{rng.range(-1, 1), rng.range(-1, 1)};
                if (interior(q)) m.at(q).type = type;
            }
        };

        if (rng.chance(0.30 + depth * 0.02)) blob(TileType::Pit);
        if (rng.chance(0.28)) blob(TileType::Spikes);
        const int barrels = std::min(rng.range(0, 1 + depth / 3), 2);
        for (int b = 0; b < barrels; ++b) {
            const Vec2 p = pick();
            if (m.in_bounds(p) &&
                std::find(res.barrels.begin(), res.barrels.end(), p) == res.barrels.end()) {
                res.barrels.push_back(p);
            }
        }
        const int crates = std::min(rng.range(0, 2), 2);
        for (int c = 0; c < crates; ++c) {
            const Vec2 p = pick();
            if (m.in_bounds(p) &&
                std::find(res.barrels.begin(), res.barrels.end(), p) == res.barrels.end() &&
                std::find(res.crates.begin(), res.crates.end(), p) == res.crates.end()) {
                res.crates.push_back(p);
            }
        }
    }

    if (!reachable(m, res.player_start, res.stairs)) {
        for (int y = 0; y < m.height(); ++y)
            for (int x = 0; x < m.width(); ++x) {
                const Vec2 p{x, y};
                if (m.at(p).type == TileType::Pit) m.at(p).type = TileType::Floor;
            }
    }
}

} // namespace

DungeonResult generate_dungeon(int w, int h, Rng& rng, int depth) {
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

    place_hazards(result, rng, depth);

    return result;
}

} // namespace sh

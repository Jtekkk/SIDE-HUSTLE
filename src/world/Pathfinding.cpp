//
// A* search. Uses Chebyshev distance as an admissible heuristic for the
// 8-directional grid with unit step cost, a std::priority_queue as the open
// set, and unordered_map keyed on Vec2 (via our std::hash specialization) for
// came-from links and best-known g-scores.
//
#include "Pathfinding.hpp"

#include <array>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace sh {
namespace {

struct OpenNode {
    Vec2 pos;
    int f; // g + heuristic
};

struct ByF {
    bool operator()(const OpenNode& a, const OpenNode& b) const { return a.f > b.f; }
};

constexpr std::array<Vec2, 8> kDirs = {
    Vec2{1, 0}, Vec2{-1, 0}, Vec2{0, 1}, Vec2{0, -1},
    Vec2{1, 1}, Vec2{1, -1}, Vec2{-1, 1}, Vec2{-1, -1},
};

constexpr int kMaxExpansions = 4000; // safety cap so a sealed target can't hang

} // namespace

std::optional<Vec2> next_step_towards(const Map& map, Vec2 from, Vec2 to,
                                      const std::vector<Vec2>& blocked) {
    if (from == to) return std::nullopt;

    std::unordered_set<Vec2> closed_to_movement(blocked.begin(), blocked.end());
    closed_to_movement.erase(to);

    std::priority_queue<OpenNode, std::vector<OpenNode>, ByF> open;
    std::unordered_map<Vec2, Vec2> came_from;
    std::unordered_map<Vec2, int> g_score;

    open.push({from, from.chebyshev(to)});
    g_score[from] = 0;

    int expansions = 0;
    while (!open.empty() && expansions++ < kMaxExpansions) {
        const OpenNode current = open.top();
        open.pop();
        if (current.pos == to) break;

        const int g_current = g_score[current.pos];
        for (const Vec2 d : kDirs) {
            const Vec2 next = current.pos + d;
            if (next != to) {
                if (!map.walkable(next)) continue;
                if (closed_to_movement.contains(next)) continue;
            } else if (!map.in_bounds(next)) {
                continue;
            }

            const int tentative = g_current + 1;
            const auto it = g_score.find(next);
            if (it == g_score.end() || tentative < it->second) {
                g_score[next] = tentative;
                came_from[next] = current.pos;
                open.push({next, tentative + next.chebyshev(to)});
            }
        }
    }

    if (!came_from.contains(to)) return std::nullopt;

    // Walk the came-from chain back to the tile adjacent to `from`.
    Vec2 step = to;
    while (came_from.at(step) != from) {
        step = came_from.at(step);
        if (!came_from.contains(step)) return std::nullopt;
    }
    return step;
}

} // namespace sh

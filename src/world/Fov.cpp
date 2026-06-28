//
// Recursive shadow casting — the classic 8-octant symmetric FOV algorithm.
//
// Each octant is visited via a small transform matrix (xx,xy,yx,yy) that maps
// the canonical octant onto the real map, so the core recursion only ever has
// to reason about one wedge. Walls cast shadows that narrow the visible slope
// range; the recursion explores the lit sub-wedges around each shadow.
//
#include "Fov.hpp"

namespace sh {
namespace {

struct Mult {
    int xx, xy, yx, yy;
};

constexpr Mult kOctants[8] = {
    {1, 0, 0, 1},  {0, 1, 1, 0},  {0, -1, 1, 0},  {-1, 0, 0, 1},
    {-1, 0, 0, -1}, {0, -1, -1, 0}, {0, 1, -1, 0}, {1, 0, 0, -1},
};

void cast_light(Map& map, Vec2 origin, int radius, int row,
                double start, double end, const Mult& m) {
    if (start < end) return;

    double new_start = 0.0;
    for (int i = row; i <= radius; ++i) {
        int dx = -i - 1;
        const int dy = -i;
        bool blocked = false;

        while (dx <= 0) {
            ++dx;
            const int X = origin.x + dx * m.xx + dy * m.xy;
            const int Y = origin.y + dx * m.yx + dy * m.yy;
            const double l_slope = (dx - 0.5) / (dy + 0.5);
            const double r_slope = (dx + 0.5) / (dy - 0.5);

            if (start < r_slope) {
                continue;
            }
            if (end > l_slope) {
                break;
            }

            const Vec2 p{X, Y};
            if (dx * dx + dy * dy < radius * radius && map.in_bounds(p)) {
                map.at(p).visible = true;
                map.at(p).explored = true;
            }

            const bool wall = !map.in_bounds(p) || map.blocks_sight(p);
            if (blocked) {
                if (wall) {
                    new_start = r_slope;
                    continue;
                }
                blocked = false;
                start = new_start;
            } else if (wall && i < radius) {
                blocked = true;
                cast_light(map, origin, radius, i + 1, start, l_slope, m);
                new_start = r_slope;
            }
        }

        if (blocked) break;
    }
}

} // namespace

void compute_fov(Map& map, Vec2 origin, int radius) {
    for (auto& tile : map.grid()) tile.visible = false;

    if (map.in_bounds(origin)) {
        map.at(origin).visible = true;
        map.at(origin).explored = true;
    }

    for (const auto& octant : kOctants) {
        cast_light(map, origin, radius, 1, 1.0, 0.0, octant);
    }
}

} // namespace sh

#pragma once
//
// Vec2 — a tiny integer 2D vector used as the universal coordinate type.
//
// Showcases: defaulted three-way comparison (operator<=>), constexpr math,
// [[nodiscard]], and a std::hash specialization so Vec2 can be a key in
// unordered_map / unordered_set (used by the A* pathfinder).
//
#include <cstddef>
#include <cstdint>
#include <functional>

namespace sh {

struct Vec2 {
    int x{0};
    int y{0};

    constexpr Vec2() = default;
    constexpr Vec2(int x_, int y_) : x(x_), y(y_) {}

    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }

    // Defaulting <=> also synthesizes ==/!= for us.
    constexpr auto operator<=>(const Vec2&) const = default;

    // Chebyshev distance = number of king-moves (8-directional grid).
    [[nodiscard]] constexpr int chebyshev(Vec2 o) const {
        const int dx = x > o.x ? x - o.x : o.x - x;
        const int dy = y > o.y ? y - o.y : o.y - y;
        return dx > dy ? dx : dy;
    }

    [[nodiscard]] constexpr int manhattan(Vec2 o) const {
        const int dx = x > o.x ? x - o.x : o.x - x;
        const int dy = y > o.y ? y - o.y : o.y - y;
        return dx + dy;
    }
};

} // namespace sh

template <>
struct std::hash<sh::Vec2> {
    std::size_t operator()(sh::Vec2 v) const noexcept {
        // Pack the two 32-bit coordinates into one 64-bit key, then hash.
        const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(v.x)) << 32)
                         | static_cast<std::uint32_t>(v.y);
        return std::hash<std::uint64_t>{}(key);
    }
};

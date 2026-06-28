#pragma once
//
// Grid<T> — a generic, bounds-checked 2D container.
//
// Showcases: a C++20 concept constraining the cell type, contiguous storage,
// range-based iteration, and Vec2-indexed access used throughout the world.
//
#include <algorithm>
#include <concepts>
#include <cstddef>
#include <vector>

#include "Vec2.hpp"

namespace sh {

// A grid cell must be default-constructible (so we can size the grid up front)
// and copyable (so we can fill / move it around cheaply).
template <typename T>
concept GridCell = std::default_initializable<T> && std::copyable<T>;

template <GridCell T>
class Grid {
public:
    Grid() = default;

    Grid(int w, int h, T fill = T{})
        : w_(w), h_(h), cells_(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), fill) {}

    [[nodiscard]] int width() const noexcept { return w_; }
    [[nodiscard]] int height() const noexcept { return h_; }

    [[nodiscard]] bool in_bounds(Vec2 p) const noexcept {
        return p.x >= 0 && p.y >= 0 && p.x < w_ && p.y < h_;
    }

    T& at(Vec2 p) { return cells_[index(p)]; }
    const T& at(Vec2 p) const { return cells_[index(p)]; }

    void fill(const T& value) { std::fill(cells_.begin(), cells_.end(), value); }

    // Expose iteration so callers can sweep every cell (e.g. clearing FOV).
    auto begin() { return cells_.begin(); }
    auto end() { return cells_.end(); }
    auto begin() const { return cells_.begin(); }
    auto end() const { return cells_.end(); }

private:
    [[nodiscard]] std::size_t index(Vec2 p) const noexcept {
        return static_cast<std::size_t>(p.y) * static_cast<std::size_t>(w_)
               + static_cast<std::size_t>(p.x);
    }

    int w_{0};
    int h_{0};
    std::vector<T> cells_;
};

} // namespace sh

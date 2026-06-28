#pragma once
//
// Ansi — tiny helpers for 24-bit ("truecolor") terminal output: an RGB type,
// colour math (lerp / scale for lighting), and SGR escape builders.
//
#include <algorithm>
#include <format>
#include <string>
#include <string_view>

namespace sh::ansi {

struct Rgb {
    int r{0};
    int g{0};
    int b{0};
};

constexpr inline const char* reset = "\x1b[0m";
constexpr inline const char* bold = "\x1b[1m";

[[nodiscard]] inline int clamp8(int v) { return std::clamp(v, 0, 255); }

// Linear interpolation between two colours (t in [0,1]).
[[nodiscard]] inline Rgb lerp(Rgb a, Rgb b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return {clamp8(static_cast<int>(a.r + (b.r - a.r) * t)),
            clamp8(static_cast<int>(a.g + (b.g - a.g) * t)),
            clamp8(static_cast<int>(a.b + (b.b - a.b) * t))};
}

// Multiply brightness by a factor (used for FOV light falloff).
[[nodiscard]] inline Rgb scale(Rgb c, double f) {
    return {clamp8(static_cast<int>(c.r * f)),
            clamp8(static_cast<int>(c.g * f)),
            clamp8(static_cast<int>(c.b * f))};
}

// Foreground / background SGR sequences.
[[nodiscard]] inline std::string fg(Rgb c) {
    return std::format("\x1b[38;2;{};{};{}m", c.r, c.g, c.b);
}
[[nodiscard]] inline std::string bg(Rgb c) {
    return std::format("\x1b[48;2;{};{};{}m", c.r, c.g, c.b);
}

// Concatenate a (possibly multibyte) glyph n times.
[[nodiscard]] inline std::string repeat(std::string_view glyph, int n) {
    std::string s;
    if (n <= 0) return s;
    s.reserve(glyph.size() * static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) s += glyph;
    return s;
}

} // namespace sh::ansi

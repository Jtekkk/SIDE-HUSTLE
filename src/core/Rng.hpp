#pragma once
//
// Rng — a thin, seedable wrapper around the standard Mersenne-Twister engine.
//
// Showcases: <random> distributions done right (engine owned, distributions
// created per-call), and a constrained generic pick() over any iterator range.
//
#include <iterator>
#include <random>

namespace sh {

class Rng {
public:
    explicit Rng(std::uint64_t seed) : eng_(seed) {}

    // Inclusive range [lo, hi].
    int range(int lo, int hi) {
        if (lo > hi) std::swap(lo, hi);
        std::uniform_int_distribution<int> dist(lo, hi);
        return dist(eng_);
    }

    // Returns true with probability p.
    bool chance(double p) {
        std::bernoulli_distribution dist(p);
        return dist(eng_);
    }

    // Pick a random element (by reference) from a [begin, end) range.
    template <std::forward_iterator It>
    decltype(auto) pick(It begin, It end) {
        const auto n = std::distance(begin, end);
        std::advance(begin, range(0, static_cast<int>(n) - 1));
        return *begin;
    }

private:
    std::mt19937_64 eng_;
};

} // namespace sh

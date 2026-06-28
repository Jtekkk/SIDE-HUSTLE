#pragma once
//
// Field of view via recursive shadow casting.
//
#include "Map.hpp"

namespace sh {

// Recomputes visibility for the whole map: clears every tile's `visible` flag,
// then lights tiles within `radius` of `origin` that are not occluded by walls,
// marking them visible and explored.
void compute_fov(Map& map, Vec2 origin, int radius);

} // namespace sh

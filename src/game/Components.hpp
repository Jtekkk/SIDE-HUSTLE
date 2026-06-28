#pragma once
//
// Entity / Item data. A lightweight, data-oriented "component" struct rather
// than a deep class hierarchy — the kind of plain-old-data design that keeps a
// roguelike fast and easy to reason about.
//
#include <string>

#include "../core/Vec2.hpp"

namespace sh {

enum class Faction : unsigned char { Player, Monster };

enum class MonsterKind : unsigned char { Bug, Client, Recruiter, Manager };

struct Combat {
    int hp{1};
    int max_hp{1};
    int attack{1};
    int defense{0};
};

struct Entity {
    Vec2 pos{};
    char glyph{'?'};
    int color{37};        // ANSI foreground code
    std::string name{};
    Faction faction{Faction::Monster};
    Combat combat{};
    MonsterKind kind{MonsterKind::Bug};
    int xp_reward{0};
    int weight{1};        // knockback resistance
    int force{1};         // knockback power dealt on a hit
    bool alive{true};
};

// A physical object in the world (currently: an explosive barrel). Blocks
// movement, can be pushed, and detonates when something slams into it.
enum class PropKind : unsigned char { Barrel };

struct Prop {
    Vec2 pos{};
    PropKind kind{PropKind::Barrel};
    bool alive{true};
};

enum class ItemKind : unsigned char { Cash, Coffee, Upgrade };

struct Item {
    Vec2 pos{};
    char glyph{'?'};
    int color{37};
    std::string name{};
    ItemKind kind{ItemKind::Cash};
    int amount{0};
    bool taken{false};
};

} // namespace sh

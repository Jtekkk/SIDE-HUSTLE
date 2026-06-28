#pragma once
//
// Game — owns the whole simulation: the current floor, the player, monsters,
// items, progression, and the message log. Runs either interactively (run())
// or as a self-playing headless smoke test (selftest()).
//
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "../core/Rng.hpp"
#include "../world/DungeonGen.hpp"
#include "../world/Map.hpp"
#include "Components.hpp"

namespace sh {

class Game {
public:
    explicit Game(std::uint64_t seed);

    int run();       // interactive play in a raw terminal
    int selftest();  // headless: auto-plays floors and prints a report

private:
    enum class State { Playing, Dead, Won };

    // --- world setup ---------------------------------------------------------
    void new_floor(int depth);
    void spawn_monsters(const std::vector<Room>& rooms);
    void spawn_items(const std::vector<Room>& rooms);

    // --- turn logic ----------------------------------------------------------
    bool handle_key(int key);             // true if the action consumed a turn
    bool try_move_player(Vec2 dir);
    void pickup(Item& item);
    void monsters_turn();
    void attack(Entity& attacker, Entity& defender);
    void player_gain_xp(int xp);
    void descend();

    // --- queries -------------------------------------------------------------
    Entity* monster_at(Vec2 p);
    Item* item_at(Vec2 p);
    void log(std::string message);

    // --- rendering -----------------------------------------------------------
    [[nodiscard]] std::string render_frame() const;
    [[nodiscard]] std::string render_end() const;
    [[nodiscard]] std::string cell_str(Vec2 world) const;
    [[nodiscard]] std::string hud_str() const;
    [[nodiscard]] std::string ascii_snapshot() const;

    Rng rng_;
    Map map_;
    Entity player_;
    std::vector<Entity> monsters_;
    std::vector<Item> items_;
    Vec2 stairs_{};

    int depth_{1};
    int level_{1};
    int xp_{0};
    int xp_next_{20};
    int cash_{0};
    int fov_radius_{8};
    State state_{State::Playing};
    std::deque<std::string> log_;

    static constexpr int kMapW = 96;
    static constexpr int kMapH = 46;
    static constexpr int kViewW = 64;
    static constexpr int kViewH = 28;
    static constexpr int kMaxDepth = 8;
};

} // namespace sh

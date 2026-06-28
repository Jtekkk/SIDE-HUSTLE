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
#include "Scores.hpp"

namespace sh {

enum class Difficulty { Easy, Normal, Hard };

struct Config {
    std::uint64_t seed{0};
    Difficulty difficulty{Difficulty::Normal};
    bool persist_scores{true};
    std::string scores_path{"side-hustle-scores.txt"};
    std::string player_name{"founder"};
};

class Game {
public:
    explicit Game(Config config);

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
    bool use_coffee();                    // consume a carried coffee to heal
    void monsters_turn();
    void attack(Entity& attacker, Entity& defender);
    void player_gain_xp(int xp);
    void descend();
    void record_score();

    // --- queries -------------------------------------------------------------
    Entity* monster_at(Vec2 p);
    Item* item_at(Vec2 p);
    void log(std::string message);
    [[nodiscard]] int score() const;

    // --- rendering -----------------------------------------------------------
    [[nodiscard]] std::string render_title() const;
    [[nodiscard]] std::string render_frame() const;
    [[nodiscard]] std::string render_end() const;
    [[nodiscard]] std::string cell_str(Vec2 world) const;
    [[nodiscard]] std::string hud_str() const;
    [[nodiscard]] std::string scoreboard_str() const;
    [[nodiscard]] std::string ascii_snapshot() const;

    Config config_;
    Rng rng_;
    HighScores high_scores_;
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
    int coffees_{0};
    int fov_radius_{8};
    bool score_recorded_{false};
    State state_{State::Playing};
    std::deque<std::string> log_;

    static constexpr int kMapW = 96;
    static constexpr int kMapH = 46;
    static constexpr int kViewW = 64;
    static constexpr int kViewH = 26;
    static constexpr int kMaxDepth = 8;
};

} // namespace sh

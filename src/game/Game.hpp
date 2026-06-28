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
#include "Story.hpp"

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
    // A single player intent. Front-ends (terminal or graphical) translate
    // their input into a Command and call advance().
    enum class Command {
        None, Wait, Descend, UseCoffee,
        MoveW, MoveE, MoveN, MoveS, MoveNW, MoveNE, MoveSW, MoveSE,
        ShoveW, ShoveE, ShoveN, ShoveS, ShoveNW, ShoveNE, ShoveSW, ShoveSE,
        DashW, DashE, DashN, DashS, DashNW, DashNE, DashSW, DashSE,
        Slam,
    };

    explicit Game(Config config);

    int run();       // interactive play in a raw terminal
    int selftest();  // headless: auto-plays floors and prints a report

    // Smoke/testing only: regenerate the world at an arbitrary depth (e.g. to
    // drop straight onto the CEO floor for a headless render check).
    void debug_warp(int depth);

    // Apply one player command and (if it consumed a turn) resolve the monster
    // turn, refresh field-of-view, and record the score on death/win. Returns
    // true if a turn was consumed. This is the entry point for the GUI.
    bool advance(Command cmd);

    // ---- read-only view of the simulation, for front-ends -------------------
    [[nodiscard]] const Map& map() const { return map_; }
    [[nodiscard]] const Entity& player() const { return player_; }
    [[nodiscard]] const std::vector<Entity>& monsters() const { return monsters_; }
    [[nodiscard]] const std::vector<Item>& items() const { return items_; }
    [[nodiscard]] const std::vector<Prop>& props() const { return props_; }
    [[nodiscard]] const std::vector<Projectile>& projectiles() const { return projectiles_; }
    [[nodiscard]] Vec2 stairs() const { return stairs_; }
    [[nodiscard]] int depth() const { return depth_; }
    [[nodiscard]] int max_depth() const { return kMaxDepth; }
    [[nodiscard]] const char* zone_name() const { return story::act_for(depth_).name; }
    [[nodiscard]] int act() const { return story::act_for(depth_).number; }
    [[nodiscard]] int level() const { return level_; }
    [[nodiscard]] int xp() const { return xp_; }
    [[nodiscard]] int xp_next() const { return xp_next_; }
    [[nodiscard]] int cash() const { return cash_; }
    [[nodiscard]] int coffees() const { return coffees_; }
    [[nodiscard]] int dash_cd() const { return dash_cd_; }
    [[nodiscard]] int slam_cd() const { return slam_cd_; }
    static constexpr int kDashCd = 4;
    static constexpr int kSlamCd = 6;
    [[nodiscard]] int fov_radius() const { return fov_radius_; }
    [[nodiscard]] int score() const;
    [[nodiscard]] bool is_playing() const { return state_ == State::Playing; }
    [[nodiscard]] bool is_dead() const { return state_ == State::Dead; }
    [[nodiscard]] bool is_won() const { return state_ == State::Won; }
    [[nodiscard]] const std::deque<std::string>& messages() const { return log_; }
    [[nodiscard]] const HighScores& high_scores() const { return high_scores_; }
    [[nodiscard]] const char* difficulty_name() const;

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
    // --- physics / interactions ---------------------------------------------
    void damage(Entity& e, int amount, std::string cause = "");
    void on_death(Entity& e);
    void apply_knockback(Entity& target, Vec2 dir, int power);
    void explode_barrel(Prop& barrel);
    void enter_tile(Entity& e);           // spike damage / pit death on entry
    bool shove(Vec2 dir);                 // kick: knock a foe / push a prop
    bool dash(Vec2 dir);                  // ability: quick multi-tile reposition
    bool slam();                          // ability: ground-pound (AoE knockback)
    void push_prop(Prop& pr, Vec2 dir, bool strong);
    void fire_projectile(const Entity& shooter, Vec2 dir);
    void advance_projectiles();
    void boss_turn(Entity& boss);         // CEO moveset (summon / shockwave / chase)
    void boss_shockwave(Entity& boss);
    void boss_summon(Entity& boss);
    bool tick_status(Entity& e);          // burn damage + stun; true if stunned
    Entity* entity_at(Vec2 p, const Entity* exclude = nullptr);
    Prop* prop_at(Vec2 p);
    void player_gain_xp(int xp);
    void descend();
    void record_score();
    bool apply_command(Command cmd);      // player half of a turn

    // --- queries -------------------------------------------------------------
    Entity* monster_at(Vec2 p);
    Item* item_at(Vec2 p);
    void log(std::string message);

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
    std::vector<Prop> props_;
    std::vector<Projectile> projectiles_;
    std::vector<Entity> pending_spawns_; // boss summons, merged after the turn
    Vec2 stairs_{};

    int dash_cd_{0};
    int slam_cd_{0};
    bool dash_armed_{false}; // terminal: 'z' pressed, waiting for a direction

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
    static constexpr int kMaxDepth = story::kFloors;
};

} // namespace sh

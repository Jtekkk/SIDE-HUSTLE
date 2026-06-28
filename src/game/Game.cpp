//
// Game implementation — generation hookup, turn resolution, combat, rendering.
//
#include "Game.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <system_error>

#include "../core/Ansi.hpp"
#include "../core/Terminal.hpp"
#include "../world/Fov.hpp"
#include "../world/Pathfinding.hpp"

namespace sh {
namespace {

// Map a keypress to a movement delta. Supports arrows, WASD, vi-keys (hjkl)
// and vi-diagonals (yubn). Returns nullopt for non-movement keys.
std::optional<Vec2> key_to_dir(int k) {
    switch (k) {
        case 'h': case 'a': case KeyLeft:  return Vec2{-1, 0};
        case 'l': case 'd': case KeyRight: return Vec2{1, 0};
        case 'k': case 'w': case KeyUp:    return Vec2{0, -1};
        case 'j': case 's': case KeyDown:  return Vec2{0, 1};
        case 'y': return Vec2{-1, -1};
        case 'u': return Vec2{1, -1};
        case 'b': return Vec2{-1, 1};
        case 'n': return Vec2{1, 1};
        default:  return std::nullopt;
    }
}

// Inverse of key_to_dir, used by the headless self-test to drive the player.
int dir_to_key(Vec2 d) {
    if (d.x < 0 && d.y < 0) return 'y';
    if (d.x > 0 && d.y < 0) return 'u';
    if (d.x < 0 && d.y > 0) return 'b';
    if (d.x > 0 && d.y > 0) return 'n';
    if (d.x < 0) return 'h';
    if (d.x > 0) return 'l';
    if (d.y < 0) return 'k';
    if (d.y > 0) return 'j';
    return '.';
}

Vec2 unit_dir(Vec2 d) { return {(d.x > 0) - (d.x < 0), (d.y > 0) - (d.y < 0)}; }

// Difficulty tunes how hard monsters hit and how much HP they bring.
double monster_scale(Difficulty d) {
    switch (d) {
        case Difficulty::Easy:   return 0.75;
        case Difficulty::Hard:   return 1.35;
        case Difficulty::Normal: break;
    }
    return 1.0;
}

Entity make_monster(Vec2 pos, int depth, Rng& rng, Difficulty diff) {
    Entity e;
    e.pos = pos;
    e.faction = Faction::Monster;

    if (depth >= 2 && rng.chance(0.22)) {
        // a ranged spammer that kites and fires projectiles
        e.kind = MonsterKind::Phisher;
        e.glyph = 'p'; e.color = 94; e.name = "a Phisher";
        e.combat = {8, 8, 3, 0}; e.xp_reward = 18;
    } else {
        const int roll = rng.range(0, 9) + depth;
        if (roll < 4) {
            e.kind = MonsterKind::Bug;
            e.glyph = 'b'; e.color = 32; e.name = "a Bug";
            e.combat = {6, 6, 3, 0}; e.xp_reward = 6;
        } else if (roll < 8) {
            e.kind = MonsterKind::Client;
            e.glyph = 'c'; e.color = 31; e.name = "a Needy Client";
            e.combat = {12, 12, 4, 1}; e.xp_reward = 12;
        } else if (roll < 12) {
            e.kind = MonsterKind::Recruiter;
            e.glyph = 'r'; e.color = 35; e.name = "a Recruiter";
            e.combat = {18, 18, 6, 1}; e.xp_reward = 20;
        } else {
            e.kind = MonsterKind::Manager;
            e.glyph = 'M'; e.color = 91; e.name = "a Middle Manager";
            e.combat = {28, 28, 8, 2}; e.xp_reward = 35;
        }
    }

    // Physics: heavier foes resist knockback and shove harder.
    switch (e.kind) {
        case MonsterKind::Bug:       e.weight = 0; e.force = 1; break;
        case MonsterKind::Phisher:   e.weight = 1; e.force = 1; break;
        case MonsterKind::Client:    e.weight = 1; e.force = 2; break;
        case MonsterKind::Recruiter: e.weight = 2; e.force = 2; break;
        case MonsterKind::Manager:   e.weight = 3; e.force = 3; break;
    }

    // Scale with depth so deeper floors bite, then by difficulty.
    const double s = monster_scale(diff);
    e.combat.max_hp = static_cast<int>((e.combat.max_hp + depth * 2) * s);
    e.combat.max_hp = std::max(1, e.combat.max_hp);
    e.combat.hp = e.combat.max_hp;
    e.combat.attack = std::max(1, static_cast<int>((e.combat.attack + depth / 2) * s));
    return e;
}

// A named act boss (or the CEO) guarding a floor's exit. Stats come from the
// story canon and are scaled by difficulty; every boss shares the '&' glyph so
// the telegraphed boss moveset (summon / shockwave) keys off it.
Entity make_boss(Vec2 pos, Difficulty diff, const story::BossSpec& spec) {
    const double s = monster_scale(diff);
    Entity e;
    e.pos = pos;
    e.faction = Faction::Monster;
    e.kind = MonsterKind::Manager;
    e.glyph = '&';
    e.color = spec.color;
    e.name = spec.name;
    e.combat.max_hp = std::max(1, static_cast<int>(spec.hp * s));
    e.combat.hp = e.combat.max_hp;
    e.combat.attack = std::max(1, static_cast<int>(spec.attack * s));
    e.combat.defense = spec.defense;
    e.xp_reward = spec.xp;
    e.weight = spec.weight;
    e.force = spec.force;
    return e;
}

// Map the legacy ANSI colour codes stored on entities/items to RGB so the
// renderer can light and blend them uniformly.
ansi::Rgb code_to_rgb(int code) {
    switch (code) {
        case 31: return {220, 70, 70};    // client
        case 32: return {90, 200, 90};    // bug
        case 33: return {235, 200, 80};   // cash
        case 35: return {200, 110, 200};  // recruiter
        case 94: return {150, 140, 255};  // phisher
        case 36: return {90, 205, 215};   // coffee
        case 91: return {255, 95, 95};    // manager
        case 92: return {130, 215, 140};  // boss: the Scrum Lord
        case 96: return {240, 160, 70};   // boss: the Regional VP
        case 90: return {130, 165, 245};  // boss: the Board Chair
        case 95: return {240, 120, 240};  // CEO
        case 97: return {245, 245, 245};  // upgrade / stairs
        default: return {220, 220, 220};
    }
}

// Unicode glyphs for the terrain (all single terminal columns).
constexpr const char* kWallGlyph = "▒";   // ▒ medium shade
constexpr const char* kFloorGlyph = "·";  // · middle dot

// Box-drawing pieces for the UI frame.
constexpr const char* kTL = "╭"; // ╭
constexpr const char* kTR = "╮"; // ╮
constexpr const char* kBL = "╰"; // ╰
constexpr const char* kBR = "╯"; // ╯
constexpr const char* kHbar = "─"; // ─
constexpr const char* kVbar = "│"; // │

// Palette.
constexpr ansi::Rgb kWallLit{150, 134, 110};
constexpr ansi::Rgb kFloorLit{96, 88, 78};
constexpr ansi::Rgb kMemory{52, 58, 82};   // explored-but-unseen "memory"
constexpr ansi::Rgb kStairs{255, 226, 120};
constexpr ansi::Rgb kFrame{96, 110, 140};
constexpr ansi::Rgb kPlayer{255, 232, 120};

} // namespace

Game::Game(Config config)
    : config_(std::move(config)),
      rng_(config_.seed),
      high_scores_(config_.scores_path) {
    if (config_.persist_scores) high_scores_.load();

    const int start_hp = config_.difficulty == Difficulty::Easy   ? 40
                         : config_.difficulty == Difficulty::Hard ? 24
                                                                  : 30;
    player_.glyph = '@';
    player_.color = 93;
    player_.name = "You";
    player_.faction = Faction::Player;
    player_.combat = {start_hp, start_hp, 5, 1};
    player_.weight = 1;
    player_.force = 2;
    new_floor(1);
    log("Find the stairs (>). Tip: hits knock foes back - into walls, pits and barrels.");
}

// ---------------------------------------------------------------------------
// World setup
// ---------------------------------------------------------------------------

void Game::new_floor(int depth) {
    depth_ = depth;
    DungeonResult dungeon = generate_dungeon(kMapW, kMapH, rng_, depth);
    map_ = std::move(dungeon.map);
    stairs_ = dungeon.stairs;
    player_.pos = dungeon.player_start;

    monsters_.clear();
    items_.clear();
    props_.clear();
    projectiles_.clear();
    pending_spawns_.clear();
    for (const Vec2 b : dungeon.barrels) props_.push_back(Prop{b, PropKind::Barrel, true});
    for (const Vec2 cr : dungeon.crates) props_.push_back(Prop{cr, PropKind::Crate, true});
    // Announce the floor and play its story beats before the boss taunt (which
    // spawn_monsters logs), so the log reads in narrative order.
    log(std::format("Floor {}/{} - {}", depth_, kMaxDepth, story::act_for(depth_).name));
    for (const auto& line : story::beats(depth_)) log(line);

    spawn_monsters(dungeon.rooms);
    spawn_items(dungeon.rooms);

    compute_fov(map_, player_.pos, fov_radius_);
}

void Game::debug_warp(int depth) {
    new_floor(depth);
    // Drop the player next to the stairs so a headless render frames whatever
    // guards the exit (the CEO on the final floor).
    static const Vec2 around[8] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                   {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    for (const Vec2 d : around) {
        const Vec2 p{stairs_.x + d.x, stairs_.y + d.y};
        if (map_.in_bounds(p) && map_.walkable(p) && entity_at(p) == nullptr) {
            player_.pos = p;
            break;
        }
    }
    compute_fov(map_, player_.pos, fov_radius_);
}

void Game::spawn_monsters(const std::vector<Room>& rooms) {
    const int extra = config_.difficulty == Difficulty::Hard ? 1
                     : config_.difficulty == Difficulty::Easy ? -1
                                                              : 0;

    // Skip rooms[0]: that is where the player materialises.
    for (std::size_t i = 1; i < rooms.size(); ++i) {
        const Room& r = rooms[i];
        const int count = std::clamp(rng_.range(0, 1 + depth_ / 2) + extra, 0, 3);
        for (int c = 0; c < count; ++c) {
            const Vec2 p{rng_.range(r.x, r.x + r.w - 1), rng_.range(r.y, r.y + r.h - 1)};
            if (!map_.walkable(p) || p == player_.pos || monster_at(p) || prop_at(p)) continue;
            monsters_.push_back(make_monster(p, depth_, rng_, config_.difficulty));
        }
    }

    // An act boss (or the CEO) guards the exit on certain floors.
    const story::BossSpec boss = story::boss_for(depth_);
    if (boss.exists && monster_at(stairs_) == nullptr) {
        monsters_.push_back(make_boss(stairs_, config_.difficulty, boss));
        log(boss.taunt);
    }
}

void Game::spawn_items(const std::vector<Room>& rooms) {
    for (std::size_t i = 1; i < rooms.size(); ++i) {
        const Room& r = rooms[i];
        if (!rng_.chance(0.7)) continue;

        const Vec2 p{rng_.range(r.x, r.x + r.w - 1), rng_.range(r.y, r.y + r.h - 1)};
        if (!map_.walkable(p) || map_.is_spikes(p) || p == player_.pos || prop_at(p)) continue;

        Item it;
        it.pos = p;
        const int roll = rng_.range(0, 99);
        if (roll < 60) {
            it.kind = ItemKind::Cash; it.glyph = '$'; it.color = 33;
            it.name = "cash"; it.amount = rng_.range(5, 20) + depth_ * 3;
        } else if (roll < 85) {
            it.kind = ItemKind::Coffee; it.glyph = '!'; it.color = 36;
            it.name = "coffee"; it.amount = rng_.range(8, 16);
        } else {
            it.kind = ItemKind::Upgrade; it.glyph = '/'; it.color = 97;
            it.name = "a better laptop"; it.amount = 1;
        }
        items_.push_back(it);
    }
}

// ---------------------------------------------------------------------------
// Turn logic
// ---------------------------------------------------------------------------

bool Game::handle_key(int key) {
    // Dash is a two-step: press 'z' to arm, then a direction. The next key is
    // interpreted as the dash heading (a movement key) — anything else cancels.
    if (dash_armed_) {
        dash_armed_ = false;
        if (const auto dir = key_to_dir(key)) return dash(*dir);
        log("Dash cancelled.");
        return false;
    }

    if (const auto dir = key_to_dir(key)) return try_move_player(*dir);

    switch (key) {
        case 'H': return shove({-1, 0});
        case 'L': return shove({1, 0});
        case 'K': return shove({0, -1});
        case 'J': return shove({0, 1});
        case 'Y': return shove({-1, -1});
        case 'U': return shove({1, -1});
        case 'B': return shove({-1, 1});
        case 'N': return shove({1, 1});
        case 'z':
        case 'Z':
            dash_armed_ = true;
            log("Dash: press a direction (or any key to cancel).");
            return false;
        case 'x':
        case 'X':
            return slam();
        case '.':
        case ' ':
            return true; // wait a turn
        case 'e':
            return use_coffee();
        case '>':
            if (player_.pos == stairs_) {
                descend();
            } else {
                log("There are no stairs here.");
            }
            return false; // descending starts a fresh floor; no monster turn
        default:
            return false;
    }
}

bool Game::try_move_player(Vec2 dir) {
    const Vec2 dest = player_.pos + dir;

    if (Entity* target = monster_at(dest)) {
        attack(player_, *target);
        return true;
    }
    // Shove a barrel if there's clear floor behind it (Sokoban-style).
    if (Prop* barrel = prop_at(dest)) {
        const Vec2 behind = dest + dir;
        if (map_.walkable(behind) && !monster_at(behind) && !prop_at(behind) && behind != player_.pos) {
            barrel->pos = behind;
            player_.pos = dest;
            log("You shove a barrel.");
            enter_tile(player_);
            if (player_.alive && state_ == State::Playing) {
                if (Item* it = item_at(dest)) pickup(*it);
            }
            return true;
        }
        return false; // wedged — can't push
    }
    if (map_.walkable(dest)) {
        player_.pos = dest;
        enter_tile(player_);
        if (player_.alive && state_ == State::Playing) {
            if (Item* it = item_at(dest)) pickup(*it);
        }
        return true;
    }
    return false; // walked into a wall — no turn consumed
}

void Game::pickup(Item& item) {
    item.taken = true;
    switch (item.kind) {
        case ItemKind::Cash:
            cash_ += item.amount;
            log(std::format("You pocket ${} of side-hustle cash.", item.amount));
            break;
        case ItemKind::Coffee:
            ++coffees_;
            log("You stash a coffee. Press 'e' to drink one (heals).");
            break;
        case ItemKind::Upgrade:
            player_.combat.attack += item.amount;
            log("New laptop - your output (attack) permanently improves!");
            break;
    }
}

bool Game::use_coffee() {
    if (coffees_ <= 0) {
        log("You have no coffee left.");
        return false;
    }
    if (player_.combat.hp >= player_.combat.max_hp) {
        log("You're already at full energy.");
        return false;
    }
    --coffees_;
    constexpr int heal = 15;
    const int before = player_.combat.hp;
    player_.combat.hp = std::min(player_.combat.max_hp, player_.combat.hp + heal);
    log(std::format("Coffee break! You recover {} HP.", player_.combat.hp - before));
    return true; // drinking takes a turn
}

void Game::monsters_turn() {
    // Move shots already in flight first, so a shot fired this turn waits a turn
    // before travelling — giving the player a chance to see and dodge it.
    advance_projectiles();
    if (state_ != State::Playing) return;

    std::vector<Vec2> occupied;
    occupied.reserve(monsters_.size() + props_.size());
    for (const auto& m : monsters_) if (m.alive) occupied.push_back(m.pos);
    for (const auto& pr : props_) if (pr.alive) occupied.push_back(pr.pos); // route around barrels

    for (auto& m : monsters_) {
        if (!m.alive || state_ != State::Playing) continue;
        if (tick_status(m)) continue;                     // burning / stunned -> skip
        if (!m.alive || state_ != State::Playing) continue; // burn may have finished it

        if (m.glyph == '&') { boss_turn(m); continue; }   // the CEO has a moveset

        const int dist = m.pos.chebyshev(player_.pos);
        const bool aware = map_.in_bounds(m.pos) && map_.at(m.pos).visible && dist <= 12;

        // Phisher: a ranged kiter that lines up shots and backs away up close.
        if (m.kind == MonsterKind::Phisher) {
            if (dist <= 1) {
                const Vec2 away = unit_dir(m.pos - player_.pos);
                const Vec2 dest = m.pos + away;
                if (map_.walkable(dest) && dest != player_.pos && !monster_at(dest) && !prop_at(dest)) {
                    m.pos = dest;
                    enter_tile(m);
                } else {
                    attack(m, player_);
                }
                continue;
            }
            const Vec2 d = player_.pos - m.pos;
            const bool aligned = (d.x == 0 || d.y == 0 || std::abs(d.x) == std::abs(d.y));
            if (aware && aligned && dist <= 7) {
                fire_projectile(m, unit_dir(d));
            } else if (aware) {
                if (const auto step = next_step_towards(map_, m.pos, player_.pos, occupied)) {
                    if (*step != player_.pos && !monster_at(*step) && !prop_at(*step)) {
                        m.pos = *step;
                        enter_tile(m);
                    }
                }
            }
            continue;
        }

        // Melee monsters.
        if (dist <= 1) {
            attack(m, player_);
            continue;
        }
        if (aware) {
            if (const auto step = next_step_towards(map_, m.pos, player_.pos, occupied)) {
                if (*step != player_.pos && !monster_at(*step) && !prop_at(*step)) {
                    m.pos = *step;
                    enter_tile(m);
                }
            }
        } else if (rng_.chance(0.3)) {
            constexpr std::array<Vec2, 8> dirs = {
                Vec2{1, 0}, Vec2{-1, 0}, Vec2{0, 1}, Vec2{0, -1},
                Vec2{1, 1}, Vec2{1, -1}, Vec2{-1, 1}, Vec2{-1, -1},
            };
            const Vec2 d = rng_.pick(dirs.begin(), dirs.end());
            const Vec2 dest = m.pos + d;
            if (map_.walkable(dest) && dest != player_.pos && !monster_at(dest) && !prop_at(dest)) {
                m.pos = dest;
                enter_tile(m);
            }
        }
    }

    // Merge any boss summons now that we're done iterating monsters_.
    for (auto& s : pending_spawns_) monsters_.push_back(std::move(s));
    pending_spawns_.clear();
}

void Game::attack(Entity& attacker, Entity& defender) {
    const int dmg = std::max(1, attacker.combat.attack - defender.combat.defense + rng_.range(-1, 1));
    const char* verb = (&attacker == &player_) ? "hit" : "hits";
    log(std::format("{} {} {} for {} damage.", attacker.name, verb, defender.name, dmg));
    damage(defender, dmg);

    // Knock the survivor back along the line of the blow.
    if (defender.alive && (&defender != &player_ || state_ == State::Playing)) {
        apply_knockback(defender, unit_dir(defender.pos - attacker.pos),
                        attacker.force - defender.weight);
    }
}

void Game::damage(Entity& e, int amount, std::string cause) {
    if (&e != &player_ && !e.alive) return;
    e.combat.hp -= std::max(0, amount);
    if (!cause.empty()) log(std::move(cause));
    if (e.combat.hp <= 0) on_death(e);
}

void Game::on_death(Entity& e) {
    if (&e == &player_) {
        if (state_ == State::Playing) {
            state_ = State::Dead;
            log("You have been laid off. Game over.");
        }
        return;
    }
    if (!e.alive) return;
    e.alive = false;
    log(std::format("{} is defeated!", e.name));
    cash_ += e.xp_reward / 2;
    player_gain_xp(e.xp_reward);
}

Entity* Game::entity_at(Vec2 p, const Entity* exclude) {
    if (&player_ != exclude && player_.pos == p && state_ == State::Playing) return &player_;
    for (auto& m : monsters_) {
        if (&m != exclude && m.alive && m.pos == p) return &m;
    }
    return nullptr;
}

Prop* Game::prop_at(Vec2 p) {
    for (auto& pr : props_) {
        if (pr.alive && pr.pos == p) return &pr;
    }
    return nullptr;
}

// Slide `target` up to `power` tiles, resolving whatever it meets: a wall (slam),
// another body (collide + chain-knock), a pit (fall to death), spikes (bleed and
// keep going) or an explosive barrel (detonate).
void Game::apply_knockback(Entity& target, Vec2 dir, int power) {
    if ((dir.x == 0 && dir.y == 0) || power <= 0) return;
    int remaining = std::min(power, 4);
    while (remaining > 0 && target.alive && (&target != &player_ || state_ == State::Playing)) {
        const Vec2 next = target.pos + dir;
        if (!map_.in_bounds(next) || map_.is_wall(next)) {
            damage(target, remaining * 3, std::format("{} slams into the wall.", target.name));
            if (remaining >= 2 && &target != &player_ && target.alive) target.stun = std::max(target.stun, 1);
            return;
        }
        if (Prop* pr = prop_at(next)) {
            if (pr->kind == PropKind::Barrel) {
                explode_barrel(*pr);
            } else { // crate: a hard, dazing stop
                damage(target, remaining * 2, std::format("{} slams into a crate.", target.name));
                if (remaining >= 2 && &target != &player_ && target.alive) target.stun = std::max(target.stun, 1);
            }
            return;
        }
        if (Entity* other = entity_at(next, &target)) {
            damage(target, 3, std::format("{} crashes into {}.", target.name, other->name));
            damage(*other, 4);
            apply_knockback(*other, dir, remaining - 1);
            return;
        }
        if (map_.is_pit(next)) {
            target.pos = next;
            damage(target, 9999, std::format("{} falls into the pit!", target.name));
            return;
        }
        if (map_.is_spikes(next)) {
            target.pos = next;
            damage(target, 5, std::format("{} is shoved onto the spikes.", target.name));
            --remaining;
            continue;
        }
        target.pos = next;
        --remaining;
    }
}

void Game::explode_barrel(Prop& barrel) {
    if (!barrel.alive) return;
    barrel.alive = false;
    const Vec2 c = barrel.pos;
    log("A barrel explodes!");

    auto blast = [&](Entity& e) {
        if ((&e != &player_ && !e.alive) || e.pos.chebyshev(c) > 2) return;
        damage(e, 12);
        if (e.alive) {
            e.burn = std::max(e.burn, 2); // the blast sets them alight
            const Vec2 d = unit_dir(e.pos - c);
            apply_knockback(e, (d.x == 0 && d.y == 0) ? Vec2{1, 0} : d, 2);
        }
    };
    blast(player_);
    for (auto& m : monsters_) blast(m);

    // Chain to nearby barrels (and shatter nearby crates).
    for (auto& pr : props_) {
        if (!pr.alive || pr.pos.chebyshev(c) > 2) continue;
        if (pr.kind == PropKind::Barrel) explode_barrel(pr);
        else pr.alive = false; // crate splinters in the blast
    }
}

void Game::enter_tile(Entity& e) {
    if (map_.is_spikes(e.pos)) {
        const char* verb = (&e == &player_) ? "step" : "steps";
        damage(e, 4, std::format("{} {} on the spikes.", e.name, verb));
    }
}

// Burn ticks (fire damage) then stun; returns true if the entity is stunned and
// must skip its action this turn.
bool Game::tick_status(Entity& e) {
    if (&e != &player_ && !e.alive) return false;
    if (e.burn > 0) {
        --e.burn;
        const char* verb = (&e == &player_) ? "are" : "is";
        damage(e, 3, std::format("{} {} burning.", e.name, verb));
    }
    if (e.stun > 0) { --e.stun; return e.alive; }
    return false;
}

void Game::fire_projectile(const Entity& shooter, Vec2 dir) {
    if (dir.x == 0 && dir.y == 0) return;
    projectiles_.push_back(Projectile{shooter.pos, dir, 5, 2, Faction::Monster, true});
    log(std::format("{} fires a spam blast!", shooter.name));
}

// Move every projectile up to its speed, resolving the first thing it meets.
void Game::advance_projectiles() {
    for (auto& pj : projectiles_) {
        for (int s = 0; s < pj.speed && pj.alive && state_ == State::Playing; ++s) {
            const Vec2 next = pj.pos + pj.dir;
            if (!map_.in_bounds(next) || map_.is_wall(next)) { pj.alive = false; break; }
            if (Prop* pr = prop_at(next)) {
                if (pr->kind == PropKind::Barrel) explode_barrel(*pr);
                pj.alive = false; // a crate just soaks it (cover)
                break;
            }
            if (Entity* e = entity_at(next, nullptr)) {
                damage(*e, pj.damage, std::format("{} is hit by spam for {} damage.", e->name, pj.damage));
                pj.alive = false;
                break;
            }
            pj.pos = next;
        }
    }
    std::erase_if(projectiles_, [](const Projectile& p) { return !p.alive; });
}

// Slide a prop along `dir`, resolving pits (crate fills, barrel falls), bodies
// (crate crushes, barrel detonates), walls and other props.
void Game::push_prop(Prop& pr, Vec2 dir, bool strong) {
    int dist = strong ? 3 : 1;
    while (dist-- > 0 && pr.alive) {
        const Vec2 next = pr.pos + dir;
        if (!map_.in_bounds(next) || map_.is_wall(next)) {
            if (pr.kind == PropKind::Barrel) explode_barrel(pr);
            break;
        }
        if (map_.is_pit(next)) {
            if (pr.kind == PropKind::Crate) {
                map_.at(next).type = TileType::Floor;
                pr.alive = false;
                log("The crate drops in and fills the pit.");
            } else {
                pr.alive = false;
                log("The barrel tumbles into the pit.");
            }
            break;
        }
        if (Entity* e = entity_at(next, nullptr)) {
            if (pr.kind == PropKind::Barrel) {
                explode_barrel(pr);
            } else {
                damage(*e, 4, std::format("The crate slams into {}.", e->name));
                if (e->alive) apply_knockback(*e, dir, 2);
            }
            break;
        }
        if (prop_at(next) != nullptr) {
            if (pr.kind == PropKind::Barrel) explode_barrel(pr);
            break;
        }
        pr.pos = next;
    }
}

bool Game::shove(Vec2 dir) {
    const Vec2 dest = player_.pos + dir;
    if (Entity* e = monster_at(dest)) {
        damage(*e, 2, std::format("You shove {}!", e->name));
        if (e->alive) apply_knockback(*e, dir, std::max(1, player_.force + 3 - e->weight));
        return true;
    }
    if (Prop* pr = prop_at(dest)) {
        push_prop(*pr, dir, true);
        return true;
    }
    if (map_.walkable(dest)) { // nothing to shove: a short lunge
        player_.pos = dest;
        enter_tile(player_);
        if (player_.alive && state_ == State::Playing) {
            if (Item* it = item_at(dest)) pickup(*it);
        }
        return true;
    }
    return false;
}

// Ability: dash up to 3 tiles, stopping before the first obstacle (great for
// slipping out of a projectile's path).
bool Game::dash(Vec2 dir) {
    if (dir.x == 0 && dir.y == 0) return false;
    if (dash_cd_ > 0) { log("Dash isn't ready yet."); return false; }
    Vec2 p = player_.pos;
    int moved = 0;
    for (int s = 0; s < 3; ++s) {
        const Vec2 next = p + dir;
        if (!map_.walkable(next) || monster_at(next) || prop_at(next)) break;
        p = next;
        ++moved;
    }
    if (moved == 0) { log("No room to dash."); return false; }
    player_.pos = p;
    dash_cd_ = kDashCd;
    log("You dash!");
    enter_tile(player_);
    if (player_.alive && state_ == State::Playing) {
        if (Item* it = item_at(p)) pickup(*it);
    }
    return true;
}

// Ability: ground-pound. Damage + stun + knock every adjacent foe outward, and
// shove adjacent props away (barrels may detonate).
bool Game::slam() {
    if (slam_cd_ > 0) { log("Slam isn't ready yet."); return false; }
    slam_cd_ = kSlamCd;
    log("You ground-pound the floor!");
    static const Vec2 dirs[8] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    for (const Vec2 d : dirs) {
        const Vec2 c = player_.pos + d;
        if (Entity* e = monster_at(c)) {
            damage(*e, 4);
            if (e->alive) { e->stun = std::max(e->stun, 1); apply_knockback(*e, d, 3); }
        } else if (Prop* pr = prop_at(c)) {
            push_prop(*pr, d, false);
        }
    }
    return true;
}

// The CEO's moveset: a telegraphed shockwave, periodic summons, else a chase.
void Game::boss_turn(Entity& m) {
    ++m.ability_timer;
    const int dist = m.pos.chebyshev(player_.pos);

    if (m.windup > 0) { m.windup = 0; boss_shockwave(m); return; }
    if (m.ability_timer % 6 == 0) {
        int minions = 0;
        for (const auto& o : monsters_) if (o.alive && &o != &m) ++minions;
        if (minions < 6) { boss_summon(m); return; } // don't swarm endlessly
    }
    if (dist <= 4 && m.ability_timer % 5 == 0) {
        m.windup = 1;
        log("The CEO winds up a shockwave - get clear!");
        return;
    }
    if (dist <= 1) { attack(m, player_); return; }

    std::vector<Vec2> blocked;
    for (const auto& o : monsters_) if (o.alive && &o != &m) blocked.push_back(o.pos);
    for (const auto& pr : props_) if (pr.alive) blocked.push_back(pr.pos);
    if (const auto step = next_step_towards(map_, m.pos, player_.pos, blocked)) {
        if (*step != player_.pos && !monster_at(*step) && !prop_at(*step)) { m.pos = *step; enter_tile(m); }
    }
}

void Game::boss_shockwave(Entity& m) {
    log("The CEO unleashes a shockwave!");
    const Vec2 c = m.pos;
    auto hit = [&](Entity& e) {
        if (&e == &m) return;
        if ((&e != &player_ && !e.alive) || e.pos.chebyshev(c) > 3) return;
        damage(e, 8);
        if (e.alive) {
            const Vec2 d = unit_dir(e.pos - c);
            apply_knockback(e, (d.x == 0 && d.y == 0) ? Vec2{0, 1} : d, 3);
        }
    };
    hit(player_);
    for (auto& o : monsters_) hit(o);
}

void Game::boss_summon(Entity& m) {
    log("The CEO posts urgent job listings!");
    static const Vec2 dirs[8] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    int spawned = 0;
    for (const Vec2 d : dirs) {
        if (spawned >= 2) break;
        const Vec2 c = m.pos + d;
        if (!map_.walkable(c) || c == player_.pos || monster_at(c) || prop_at(c)) continue;
        Entity e;
        e.pos = c;
        e.faction = Faction::Monster;
        if (rng_.chance(0.4)) {
            e.kind = MonsterKind::Phisher; e.glyph = 'p'; e.color = 94; e.name = "a summoned Phisher";
            e.combat = {8, 8, 3, 0}; e.xp_reward = 12; e.weight = 1; e.force = 1;
        } else {
            e.kind = MonsterKind::Bug; e.glyph = 'b'; e.color = 32; e.name = "a summoned Bug";
            e.combat = {6, 6, 3, 0}; e.xp_reward = 4; e.weight = 0; e.force = 1;
        }
        e.combat.max_hp += depth_;
        e.combat.hp = e.combat.max_hp;
        pending_spawns_.push_back(e);
        ++spawned;
    }
}

void Game::player_gain_xp(int xp) {
    xp_ += xp;
    while (xp_ >= xp_next_) {
        xp_ -= xp_next_;
        ++level_;
        xp_next_ += level_ * 15;
        player_.combat.max_hp += 8;
        player_.combat.hp = player_.combat.max_hp;
        player_.combat.attack += 1;
        if (level_ % 2 == 0) player_.combat.defense += 1;
        log(std::format("You level up! You are now level {}.", level_));
    }
}

void Game::descend() {
    if (depth_ >= kMaxDepth) {
        state_ = State::Won;
        log(story::ending());
        return;
    }
    new_floor(depth_ + 1);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool Game::apply_command(Command cmd) {
    switch (cmd) {
        case Command::MoveW:  return try_move_player({-1, 0});
        case Command::MoveE:  return try_move_player({1, 0});
        case Command::MoveN:  return try_move_player({0, -1});
        case Command::MoveS:  return try_move_player({0, 1});
        case Command::MoveNW: return try_move_player({-1, -1});
        case Command::MoveNE: return try_move_player({1, -1});
        case Command::MoveSW: return try_move_player({-1, 1});
        case Command::MoveSE: return try_move_player({1, 1});
        case Command::ShoveW:  return shove({-1, 0});
        case Command::ShoveE:  return shove({1, 0});
        case Command::ShoveN:  return shove({0, -1});
        case Command::ShoveS:  return shove({0, 1});
        case Command::ShoveNW: return shove({-1, -1});
        case Command::ShoveNE: return shove({1, -1});
        case Command::ShoveSW: return shove({-1, 1});
        case Command::ShoveSE: return shove({1, 1});
        case Command::DashW:  return dash({-1, 0});
        case Command::DashE:  return dash({1, 0});
        case Command::DashN:  return dash({0, -1});
        case Command::DashS:  return dash({0, 1});
        case Command::DashNW: return dash({-1, -1});
        case Command::DashNE: return dash({1, -1});
        case Command::DashSW: return dash({-1, 1});
        case Command::DashSE: return dash({1, 1});
        case Command::Slam:   return slam();
        case Command::Wait:      return true;
        case Command::UseCoffee: return use_coffee();
        case Command::Descend:
            if (player_.pos == stairs_) {
                descend();
            } else {
                log("There are no stairs here.");
            }
            return false; // descending starts a fresh floor; no monster turn
        case Command::None:
        default:
            return false;
    }
}

bool Game::advance(Command cmd) {
    if (state_ != State::Playing) return false;

    const bool acted = apply_command(cmd);
    if (acted && state_ == State::Playing) {
        if (dash_cd_ > 0) --dash_cd_;
        if (slam_cd_ > 0) --slam_cd_;
        monsters_turn();
        if (state_ == State::Playing) tick_status(player_); // your burn ticks each turn
        compute_fov(map_, player_.pos, fov_radius_);
    }
    if (state_ != State::Playing) record_score();
    return acted;
}

const char* Game::difficulty_name() const {
    switch (config_.difficulty) {
        case Difficulty::Easy: return "Easy";
        case Difficulty::Hard: return "Hard";
        case Difficulty::Normal: break;
    }
    return "Normal";
}

Entity* Game::monster_at(Vec2 p) {
    for (auto& m : monsters_) {
        if (m.alive && m.pos == p) return &m;
    }
    return nullptr;
}

Item* Game::item_at(Vec2 p) {
    for (auto& it : items_) {
        if (!it.taken && it.pos == p) return &it;
    }
    return nullptr;
}

void Game::log(std::string message) {
    log_.push_back(std::move(message));
    while (log_.size() > 6) log_.pop_front();
}

int Game::score() const {
    return HighScores::compute(depth_, level_, cash_, state_ == State::Won);
}

void Game::record_score() {
    if (score_recorded_ || !config_.persist_scores) return;
    score_recorded_ = true;
    high_scores_.add(ScoreEntry{
        .name = config_.player_name,
        .depth = depth_,
        .level = level_,
        .cash = cash_,
        .score = score(),
        .won = state_ == State::Won,
    });
    high_scores_.save();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

std::string Game::cell_str(Vec2 world) const {
    if (!map_.in_bounds(world)) return " ";
    const Tile& t = map_.at(world);
    if (!t.explored) return " ";

    // Out-of-sight but remembered: a flat, cool "memory" tint.
    if (!t.visible) {
        const char* g = t.type == TileType::Wall ? kWallGlyph
                       : t.type == TileType::StairsDown ? ">"
                                                        : kFloorGlyph;
        return ansi::fg(kMemory) + g;
    }

    // Smooth light falloff with distance from the player.
    const double dist = static_cast<double>(world.chebyshev(player_.pos));
    const double t01 = fov_radius_ > 0 ? std::clamp(dist / fov_radius_, 0.0, 1.0) : 0.0;
    const double light = 1.0 - 0.55 * t01;

    // Player.
    if (world == player_.pos) {
        return ansi::bold + ansi::fg(kPlayer) + std::string{player_.glyph};
    }
    // Monster (drawn bright so it pops against the lit floor).
    for (const auto& m : monsters_) {
        if (m.alive && m.pos == world) {
            const ansi::Rgb c = ansi::scale(code_to_rgb(m.color), std::max(light, 0.8));
            return ansi::bold + ansi::fg(c) + std::string{m.glyph};
        }
    }
    // Item.
    for (const auto& it : items_) {
        if (!it.taken && it.pos == world) {
            const ansi::Rgb c = ansi::scale(code_to_rgb(it.color), std::max(light, 0.85));
            return ansi::bold + ansi::fg(c) + std::string{it.glyph};
        }
    }
    // Projectile.
    for (const auto& pj : projectiles_) {
        if (pj.alive && pj.pos == world) {
            return ansi::bold + ansi::fg(ansi::Rgb{180, 170, 255}) + "*";
        }
    }
    // Prop (barrel / crate).
    for (const auto& pr : props_) {
        if (pr.alive && pr.pos == world) {
            const ansi::Rgb col = pr.kind == sh::PropKind::Barrel ? ansi::Rgb{198, 132, 70}
                                                                  : ansi::Rgb{150, 120, 84};
            const char* g = pr.kind == sh::PropKind::Barrel ? "0" : "▣";
            return ansi::bold + ansi::fg(ansi::scale(col, std::max(light, 0.8))) + g;
        }
    }

    // Terrain.
    switch (t.type) {
        case TileType::StairsDown:
            return ansi::bold + ansi::fg(ansi::scale(kStairs, light)) + ">";
        case TileType::Wall:
            return ansi::fg(ansi::scale(kWallLit, light)) + kWallGlyph;
        case TileType::Pit:
            return ansi::fg(ansi::scale(ansi::Rgb{20, 22, 34}, light)) + "O";
        case TileType::Spikes:
            return ansi::fg(ansi::scale(ansi::Rgb{210, 90, 90}, light)) + "^";
        case TileType::Floor:
            break;
    }
    return ansi::fg(ansi::scale(kFloorLit, light)) + kFloorGlyph;
}

std::string Game::hud_str() const {
    using ansi::Rgb;
    const int hp = std::max(0, player_.combat.hp);
    const int mhp = std::max(1, player_.combat.max_hp);
    const double ratio = std::clamp(static_cast<double>(hp) / mhp, 0.0, 1.0);

    const Rgb green{90, 210, 90};
    const Rgb yellow{235, 205, 80};
    const Rgb red{230, 70, 70};
    const Rgb hpcol = ratio >= 0.5 ? ansi::lerp(yellow, green, (ratio - 0.5) * 2.0)
                                   : ansi::lerp(red, yellow, ratio * 2.0);

    const Rgb label{150, 162, 190};
    const Rgb value{226, 231, 242};
    const Rgb dim{74, 80, 98};

    constexpr int bar_len = 24;
    const int filled = std::clamp(static_cast<int>(ratio * bar_len + 0.5), 0, bar_len);

    std::string s = std::string{ansi::reset} + "\r\n";

    // HP bar.
    s += " " + ansi::fg(label) + "HP " + ansi::fg(hpcol) + ansi::repeat("█", filled);
    s += ansi::fg(dim) + ansi::repeat("░", bar_len - filled);
    s += ansi::fg(value) + std::format(" {}/{}", hp, mhp) + ansi::reset + "\r\n";

    // Stats line.
    s += " ";
    s += ansi::fg(label) + "Floor " + ansi::fg(value) + std::format("{}/{}", depth_, kMaxDepth)
       + ansi::fg(dim) + std::format(" {}", story::act_for(depth_).name) + "  ";
    s += ansi::fg(label) + "Lvl " + ansi::fg(value) + std::format("{}", level_)
       + ansi::fg(dim) + std::format(" ({}/{} xp)", xp_, xp_next_) + "  ";
    s += ansi::fg(yellow) + std::format("${}", cash_) + "  ";
    s += ansi::fg(Rgb{90, 205, 215}) + std::format("coffee x{}", coffees_) + "  ";
    const Rgb ready{120, 220, 160};
    s += ansi::fg(label) + "dash " +
         ansi::fg(dash_cd_ == 0 ? ready : dim) + (dash_cd_ == 0 ? std::string{"rdy"} : std::format("{}", dash_cd_)) + "  ";
    s += ansi::fg(label) + "slam " +
         ansi::fg(slam_cd_ == 0 ? ready : dim) + (slam_cd_ == 0 ? std::string{"rdy"} : std::format("{}", slam_cd_));
    s += std::string{ansi::reset} + "\r\n";

    // Message log (latest brightest).
    s += "\r\n";
    const int n = static_cast<int>(log_.size());
    int idx = 0;
    for (const auto& m : log_) {
        const bool latest = (++idx == n);
        const Rgb mc = latest ? Rgb{216, 223, 236} : Rgb{138, 146, 166};
        s += " " + ansi::fg(mc) + "› " + m + ansi::reset + "\r\n";
    }

    s += "\r\n " + ansi::fg(dim) +
         "move wasd/hjkl · diag yubn · SHOVE-shift · dash z+dir · slam x · coffee e · wait . · descend > · quit q" +
         ansi::reset + "\r\n";
    return s;
}

std::string Game::scoreboard_str() const {
    using ansi::Rgb;
    const auto& entries = high_scores_.entries();
    std::string s = "  " + ansi::fg(Rgb{150, 162, 190}) + "Top side hustles" + ansi::reset + "\r\n";
    if (entries.empty()) {
        s += "  " + ansi::fg(Rgb{100, 108, 130}) + "(none yet — be the first)" + ansi::reset + "\r\n";
        return s;
    }
    const Rgb medal[3] = {{255, 215, 90}, {198, 204, 216}, {200, 140, 80}};
    int rank = 0;
    for (const auto& e : entries) {
        const Rgb rc = rank < 3 ? medal[rank] : Rgb{120, 128, 150};
        s += "  " + ansi::fg(rc) + std::format("{:>2}. ", rank + 1)
           + ansi::fg(Rgb{235, 235, 245}) + std::format("{:>6}", e.score)
           + ansi::fg(Rgb{170, 178, 198}) + std::format("  {:<12}", e.name)
           + ansi::fg(Rgb{120, 128, 150}) + std::format("floor {} · lvl {} · ${}", e.depth, e.level, e.cash)
           + (e.won ? ansi::fg(Rgb{110, 225, 120}) + "  WON" : std::string{})
           + ansi::reset + "\r\n";
        ++rank;
    }
    return s;
}

std::string Game::render_title() const {
    using ansi::Rgb;
    static const char* const banner[5] = {
        "   ____  _     _        _   _           _   _      ",
        "  / ___|(_) __| | ___  | | | |_   _ ___| |_| | ___ ",
        "  \\___ \\| |/ _` |/ _ \\ | |_| | | | / __| __| |/ _ \\",
        "   ___) | | (_| |  __/ |  _  | |_| \\__ \\ |_| |  __/",
        "  |____/|_|\\__,_|\\___| |_| |_|\\__,_|___/\\__|_|\\___|",
    };

    std::string s = "\x1b[2J\x1b[H";
    s += std::string{ansi::reset} + "\r\n";
    for (int i = 0; i < 5; ++i) {
        const Rgb c = ansi::lerp(Rgb{255, 214, 92}, Rgb{255, 150, 46}, i / 4.0);
        s += std::string{ansi::bold} + ansi::fg(c) + banner[i] + ansi::reset + "\r\n";
    }
    s += "\r\n  " + ansi::fg(Rgb{150, 160, 185}) +
         "A corporate dungeon crawl in modern C++." + ansi::reset + "\r\n";
    s += "  " + ansi::fg(Rgb{120, 130, 155}) +
         "Descend 12 floors of corporate hell, past four act bosses, to the CEO." +
         ansi::reset + "\r\n";
    const char* diff = config_.difficulty == Difficulty::Easy   ? "Easy"
                      : config_.difficulty == Difficulty::Hard  ? "Hard"
                                                                : "Normal";
    s += "  " + ansi::fg(Rgb{110, 120, 145}) +
         std::format("Difficulty: {}   Floors: {}", diff, kMaxDepth) + ansi::reset + "\r\n\r\n";
    s += scoreboard_str();
    s += "\r\n  " + std::string{ansi::bold} + ansi::fg(Rgb{230, 235, 245}) + "Press any key to start" +
         ansi::reset + ansi::fg(Rgb{110, 120, 145}) + "   (q to quit)" + ansi::reset + "\r\n";
    return s;
}

std::string Game::render_frame() const {
    using ansi::Rgb;
    const int cam_x = std::clamp(player_.pos.x - kViewW / 2, 0, std::max(0, map_.width() - kViewW));
    const int cam_y = std::clamp(player_.pos.y - kViewH / 2, 0, std::max(0, map_.height() - kViewH));

    constexpr int W = kViewW;
    const std::string frame = ansi::fg(kFrame);
    const std::string title = std::string{ansi::bold} + ansi::fg(Rgb{255, 232, 120}) + " SIDE HUSTLE ";

    std::string out;
    out.reserve(static_cast<std::size_t>(W) * kViewH * 24);
    out += ansi::reset;

    // Top border with an inset title (" SIDE HUSTLE " spans 13 columns).
    out += frame + kTL + ansi::repeat(kHbar, 2) + title + frame + ansi::repeat(kHbar, W - 2 - 13) +
           kTR + ansi::reset + "\r\n";

    for (int sy = 0; sy < kViewH; ++sy) {
        out += frame + kVbar + ansi::reset;
        for (int sx = 0; sx < W; ++sx) out += cell_str({cam_x + sx, cam_y + sy});
        out += std::string{ansi::reset} + frame + kVbar + ansi::reset + "\r\n";
    }

    out += frame + kBL + ansi::repeat(kHbar, W) + kBR + ansi::reset + "\r\n";

    out += hud_str();
    out += std::string{ansi::reset} + "\x1b[J"; // erase anything left from a taller frame
    return out;
}

std::string Game::render_end() const {
    using ansi::Rgb;
    std::string s = "\x1b[2J\x1b[H";
    s += std::string{ansi::reset} + "\r\n";
    if (state_ == State::Dead) {
        s += "  " + std::string{ansi::bold} + ansi::fg(Rgb{235, 80, 80}) +
             "GAME OVER — the grind got you." + ansi::reset + "\r\n";
    } else {
        s += "  " + std::string{ansi::bold} + ansi::fg(Rgb{110, 225, 120}) +
             "YOU WIN — you went full-time on your side hustle!" + ansi::reset + "\r\n";
    }
    s += "\r\n  " + ansi::fg(Rgb{200, 206, 220}) +
         std::format("Reached floor {}, level {}, with ${} banked.", depth_, level_, cash_) +
         ansi::reset + "\r\n";
    s += "  " + std::string{ansi::bold} + ansi::fg(Rgb{235, 205, 80}) +
         std::format("Final score: {}", score()) + ansi::reset + "\r\n";
    if (!high_scores_.entries().empty() && high_scores_.entries().front().score == score()) {
        s += "  " + ansi::fg(Rgb{110, 225, 120}) + "★ A new top score!" + ansi::reset + "\r\n";
    }
    s += "\r\n" + scoreboard_str();
    s += "\r\n  " + ansi::fg(Rgb{150, 160, 185}) + "Press any key to exit." + ansi::reset + "\r\n";
    return s;
}

std::string Game::ascii_snapshot() const {
    constexpr int win_w = 60;
    constexpr int win_h = 22;
    const int cam_x = std::clamp(player_.pos.x - win_w / 2, 0, std::max(0, map_.width() - win_w));
    const int cam_y = std::clamp(player_.pos.y - win_h / 2, 0, std::max(0, map_.height() - win_h));

    std::string s;
    for (int y = 0; y < win_h; ++y) {
        for (int x = 0; x < win_w; ++x) {
            const Vec2 w{cam_x + x, cam_y + y};
            char ch = ' ';
            if (map_.in_bounds(w)) {
                const Tile& t = map_.at(w);
                if (t.explored || t.visible) {
                    switch (t.type) {
                        case TileType::Wall:       ch = '#'; break;
                        case TileType::Floor:      ch = '.'; break;
                        case TileType::StairsDown: ch = '>'; break;
                        case TileType::Pit:        ch = 'O'; break;
                        case TileType::Spikes:     ch = '^'; break;
                    }
                }
                bool drew = false;
                for (const auto& pj : projectiles_)
                    if (pj.alive && pj.pos == w) { ch = '*'; drew = true; break; }
                if (w == player_.pos) {
                    ch = '@';
                } else if (drew) {
                    // projectile already chosen
                } else if (const Prop* pr = [&]() -> const Prop* {
                               for (const auto& p : props_)
                                   if (p.alive && p.pos == w) return &p;
                               return nullptr;
                           }()) {
                    ch = pr->kind == PropKind::Barrel ? '0' : '=';
                } else {
                    for (const auto& m : monsters_) {
                        if (m.alive && m.pos == w) { ch = m.glyph; break; }
                    }
                }
            }
            s += ch;
        }
        s += '\n';
    }
    return s;
}

// ---------------------------------------------------------------------------
// Drivers
// ---------------------------------------------------------------------------

int Game::run() {
    Terminal term;

    Terminal::present(render_title());
    if (term.read_key() == 'q') return 0;

    Terminal::present(render_frame());
    while (state_ == State::Playing) {
        const int key = term.read_key();
        if (key == 'q' || key == 3 /* Ctrl-C */) break;

        const bool acted = handle_key(key);
        if (acted && state_ == State::Playing) {
            monsters_turn();
            compute_fov(map_, player_.pos, fov_radius_);
        }
        Terminal::present(render_frame());

        if (state_ != State::Playing) {
            record_score();
            Terminal::present(render_end());
            (void)term.read_key();
            break;
        }
    }
    return 0;
}

int Game::selftest() {
    std::printf("[selftest] SIDE HUSTLE — auto-playing toward the stairs each floor\n");

    for (int floor = 1; floor <= 5 && state_ == State::Playing; ++floor) {
        int turns = 0;
        bool cleared = false;
        while (turns++ < 1500 && state_ == State::Playing) {
            int key = '.';
            std::vector<Vec2> blocked;
            for (const auto& pr : props_) if (pr.alive) blocked.push_back(pr.pos);
            if (player_.pos == stairs_) {
                key = '>';
            } else if (const auto step = next_step_towards(map_, player_.pos, stairs_, blocked)) {
                key = dir_to_key(*step - player_.pos);
            }

            const bool acted = handle_key(key);
            if (acted && state_ == State::Playing) {
                monsters_turn();
                compute_fov(map_, player_.pos, fov_radius_);
            }
            if (depth_ > floor || state_ == State::Won) {
                cleared = true;
                break;
            }
        }

        const auto alive = std::ranges::count_if(monsters_, [](const Entity& m) { return m.alive; });
        std::printf("[selftest] floor %d: depth=%d hp=%d/%d lvl=%d cash=%d monsters_left=%lld turns=%d -> %s\n",
                    floor, depth_, player_.combat.hp, player_.combat.max_hp, level_, cash_,
                    static_cast<long long>(alive), turns,
                    state_ == State::Dead ? "DIED" : (cleared ? "cleared" : "stuck"));

        if (state_ != State::Playing) break;
    }

    std::printf("\n%s\n", ascii_snapshot().c_str());

    // Boss smoke: jump to the final floor and brawl the CEO (exercise the
    // moveset, summons, shockwave, abilities) without crashing.
    {
        state_ = State::Playing;
        level_ = 8;
        player_.combat = {90, 90, 14, 2};
        player_.burn = player_.stun = 0;
        new_floor(kMaxDepth);
        int bturns = 0, slams = 0, dashes = 0;
        while (bturns++ < 600 && state_ == State::Playing) {
            const Vec2 pp = player_.pos;
            std::vector<Vec2> blk;
            for (const auto& pr : props_) if (pr.alive) blk.push_back(pr.pos);
            int key = '.';
            if (pp == stairs_) {
                key = '>';
            } else if (const auto step = next_step_towards(map_, pp, stairs_, blk)) {
                const Vec2 d = *step - pp;
                // occasionally use abilities to exercise them
                if (slam_cd_ == 0 && bturns % 11 == 0) { slam(); ++slams; }
                else if (dash_cd_ == 0 && bturns % 7 == 0) { dash(d); ++dashes; }
                else key = dir_to_key(d);
            }
            const bool acted = (key != '.') ? handle_key(key) : true;
            if (acted && state_ == State::Playing) {
                if (dash_cd_ > 0) --dash_cd_;
                if (slam_cd_ > 0) --slam_cd_;
                monsters_turn();
                if (state_ == State::Playing) tick_status(player_);
                compute_fov(map_, player_.pos, fov_radius_);
            }
            if (state_ == State::Won) break;
        }
        const auto alive = std::ranges::count_if(monsters_, [](const Entity& m) { return m.alive; });
        std::printf("[selftest] boss floor: turns=%d state=%s monsters_alive=%lld slams=%d dashes=%d\n",
                    bturns, state_ == State::Won ? "WON" : state_ == State::Dead ? "DEAD" : "TIMEOUT",
                    static_cast<long long>(alive), slams, dashes);
    }

    // Exercise the persistent scoreboard end-to-end (save -> reload).
    {
        const auto tmp = std::filesystem::temp_directory_path() / "side-hustle-selftest-scores.txt";
        std::error_code ec;
        std::filesystem::remove(tmp, ec);

        HighScores hs(tmp);
        hs.load();
        hs.add(ScoreEntry{.name = "alice", .depth = 3, .level = 4, .cash = 120,
                          .score = HighScores::compute(3, 4, 120, false), .won = false});
        hs.add(ScoreEntry{.name = "bob", .depth = 8, .level = 9, .cash = 300,
                          .score = HighScores::compute(8, 9, 300, true), .won = true});
        const bool saved = hs.save();

        HighScores reloaded(tmp);
        reloaded.load();
        const bool ok = saved && reloaded.entries().size() == 2 &&
                        reloaded.entries().front().name == "bob";
        std::printf("[selftest] scores round-trip: %s (reloaded %zu entries, top=%s)\n",
                    ok ? "OK" : "FAIL", reloaded.entries().size(),
                    reloaded.entries().empty() ? "-" : reloaded.entries().front().name.c_str());
        std::filesystem::remove(tmp, ec);
    }

    std::printf("[selftest] final state = %s, score = %d\n",
                state_ == State::Won ? "WON" : state_ == State::Dead ? "DEAD" : "PLAYING",
                score());
    return 0;
}

} // namespace sh

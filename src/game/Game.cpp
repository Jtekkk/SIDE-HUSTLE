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

    // Scale with depth so deeper floors bite, then by difficulty.
    const double s = monster_scale(diff);
    e.combat.max_hp = static_cast<int>((e.combat.max_hp + depth * 2) * s);
    e.combat.max_hp = std::max(1, e.combat.max_hp);
    e.combat.hp = e.combat.max_hp;
    e.combat.attack = std::max(1, static_cast<int>((e.combat.attack + depth / 2) * s));
    return e;
}

// The CEO — a unique boss that guards the exit on the final floor.
Entity make_boss(Vec2 pos, Difficulty diff) {
    const double s = monster_scale(diff);
    Entity e;
    e.pos = pos;
    e.faction = Faction::Monster;
    e.kind = MonsterKind::Manager;
    e.glyph = '&';
    e.color = 95; // bright magenta
    e.name = "the CEO";
    e.combat.max_hp = std::max(1, static_cast<int>(120 * s));
    e.combat.hp = e.combat.max_hp;
    e.combat.attack = std::max(1, static_cast<int>(12 * s));
    e.combat.defense = 3;
    e.xp_reward = 200;
    return e;
}

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
    new_floor(1);
    log("You start your side hustle on floor 1. Find the stairs (>).");
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
    spawn_monsters(dungeon.rooms);
    spawn_items(dungeon.rooms);

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
            if (!map_.walkable(p) || p == player_.pos || monster_at(p)) continue;
            monsters_.push_back(make_monster(p, depth_, rng_, config_.difficulty));
        }
    }

    // The CEO waits by the exit on the deepest floor.
    if (depth_ >= kMaxDepth && monster_at(stairs_) == nullptr) {
        monsters_.push_back(make_boss(stairs_, config_.difficulty));
        log("\x1b[95mYou sense the CEO guarding the way out (&).\x1b[0m");
    }
}

void Game::spawn_items(const std::vector<Room>& rooms) {
    for (std::size_t i = 1; i < rooms.size(); ++i) {
        const Room& r = rooms[i];
        if (!rng_.chance(0.7)) continue;

        const Vec2 p{rng_.range(r.x, r.x + r.w - 1), rng_.range(r.y, r.y + r.h - 1)};
        if (!map_.walkable(p) || p == player_.pos) continue;

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
    if (const auto dir = key_to_dir(key)) return try_move_player(*dir);

    switch (key) {
        case '.':
        case ' ':
            return true; // wait a turn
        case 'e':
        case 'E':
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
    if (map_.walkable(dest)) {
        player_.pos = dest;
        if (Item* it = item_at(dest)) pickup(*it);
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
            log("New laptop — your output (attack) permanently improves!");
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
    std::vector<Vec2> occupied;
    occupied.reserve(monsters_.size());
    for (const auto& m : monsters_) {
        if (m.alive) occupied.push_back(m.pos);
    }

    for (auto& m : monsters_) {
        if (!m.alive || state_ != State::Playing) continue;

        const int dist = m.pos.chebyshev(player_.pos);
        if (dist <= 1) {
            attack(m, player_);
            continue;
        }

        // A monster reacts only while it stands on a tile the player can see
        // (line-of-sight is symmetric), otherwise it idly mills about.
        const bool aware = map_.in_bounds(m.pos) && map_.at(m.pos).visible && dist <= 12;
        if (aware) {
            if (const auto step = next_step_towards(map_, m.pos, player_.pos, occupied)) {
                if (*step != player_.pos && !monster_at(*step)) m.pos = *step;
            }
        } else if (rng_.chance(0.3)) {
            constexpr std::array<Vec2, 8> dirs = {
                Vec2{1, 0}, Vec2{-1, 0}, Vec2{0, 1}, Vec2{0, -1},
                Vec2{1, 1}, Vec2{1, -1}, Vec2{-1, 1}, Vec2{-1, -1},
            };
            const Vec2 d = rng_.pick(dirs.begin(), dirs.end());
            const Vec2 dest = m.pos + d;
            if (map_.walkable(dest) && dest != player_.pos && !monster_at(dest)) m.pos = dest;
        }
    }
}

void Game::attack(Entity& attacker, Entity& defender) {
    const int dmg = std::max(1, attacker.combat.attack - defender.combat.defense + rng_.range(-1, 1));
    defender.combat.hp -= dmg;
    log(std::format("{} hits {} for {} damage.", attacker.name, defender.name, dmg));

    if (defender.combat.hp > 0) return;

    defender.alive = false;
    if (&defender == &player_) {
        state_ = State::Dead;
        log("You have been laid off. Game over.");
    } else {
        log(std::format("{} is defeated!", defender.name));
        cash_ += defender.xp_reward / 2;
        player_gain_xp(defender.xp_reward);
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
        log("You climb the final stairs and walk out a free founder!");
        return;
    }
    new_floor(depth_ + 1);
    log(std::format("You descend to floor {}.", depth_));
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

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

    char ch = ' ';
    int color = 90;
    switch (t.type) {
        case TileType::Wall:       ch = '#'; color = t.visible ? 37 : 90; break;
        case TileType::Floor:      ch = '.'; color = t.visible ? 37 : 90; break;
        case TileType::StairsDown: ch = '>'; color = t.visible ? 97 : 90; break;
    }

    if (t.visible) {
        if (world == player_.pos) {
            ch = player_.glyph;
            color = player_.color;
        } else {
            const Entity* mm = nullptr;
            for (const auto& m : monsters_) {
                if (m.alive && m.pos == world) { mm = &m; break; }
            }
            if (mm != nullptr) {
                ch = mm->glyph;
                color = mm->color;
            } else {
                const Item* ii = nullptr;
                for (const auto& it : items_) {
                    if (!it.taken && it.pos == world) { ii = &it; break; }
                }
                if (ii != nullptr) {
                    ch = ii->glyph;
                    color = ii->color;
                }
            }
        }
    }

    return std::format("\x1b[{}m{}", color, ch);
}

std::string Game::hud_str() const {
    const int hp = std::max(0, player_.combat.hp);
    const int mhp = player_.combat.max_hp;
    constexpr int bar_len = 20;
    const int filled = std::clamp(mhp > 0 ? hp * bar_len / mhp : 0, 0, bar_len);
    std::string bar(static_cast<std::size_t>(filled), '#');
    bar.append(static_cast<std::size_t>(bar_len - filled), '-');

    std::string s = "\x1b[0m\r\n";
    s += std::format("\x1b[91mHP\x1b[0m [\x1b[92m{}\x1b[0m] {}/{}   ", bar, hp, mhp);
    s += std::format("\x1b[93mFloor\x1b[0m {}   \x1b[93mLvl\x1b[0m {} (xp {}/{})   "
                     "\x1b[93m$\x1b[0m{}   \x1b[36mcoffee\x1b[0m x{}\r\n",
                     depth_, level_, xp_, xp_next_, cash_, coffees_);
    s += "\r\n";
    for (const auto& m : log_) s += "  " + m + "\r\n";
    s += "\r\n\x1b[90mmove wasd/hjkl/arrows  diag yubn  coffee e  wait .  descend >  quit q\x1b[0m\r\n";
    return s;
}

std::string Game::scoreboard_str() const {
    std::string s = "\x1b[97m   Top side hustles\x1b[0m\r\n";
    const auto& entries = high_scores_.entries();
    if (entries.empty()) {
        s += "\x1b[90m   (none yet — be the first)\x1b[0m\r\n";
        return s;
    }
    int rank = 1;
    for (const auto& e : entries) {
        s += std::format("\x1b[90m{:>2}.\x1b[0m \x1b[93m{:>6}\x1b[0m  {:<10} "
                         "\x1b[90mfloor {} lvl {} ${}{}\x1b[0m\r\n",
                         rank++, e.score, e.name, e.depth, e.level, e.cash,
                         e.won ? " WON" : "");
    }
    return s;
}

std::string Game::render_title() const {
    std::string s = "\x1b[2J\x1b[H\x1b[0m\r\n";
    s += "\x1b[93m   ____  _     _        _   _           _   _      \x1b[0m\r\n";
    s += "\x1b[93m  / ___|(_) __| | ___  | | | |_   _ ___| |_| | ___ \x1b[0m\r\n";
    s += "\x1b[93m  \\___ \\| |/ _` |/ _ \\ | |_| | | | / __| __| |/ _ \\\x1b[0m\r\n";
    s += "\x1b[93m   ___) | | (_| |  __/ |  _  | |_| \\__ \\ |_| |  __/\x1b[0m\r\n";
    s += "\x1b[93m  |____/|_|\\__,_|\\___| |_| |_|\\__,_|___/\\__|_|\\___|\x1b[0m\r\n";
    s += "\r\n\x1b[90m   A corporate dungeon crawl in modern C++.\x1b[0m\r\n";
    const char* diff = config_.difficulty == Difficulty::Easy   ? "Easy"
                      : config_.difficulty == Difficulty::Hard  ? "Hard"
                                                                : "Normal";
    s += std::format("\x1b[90m   Difficulty: {}   Floors: {}\x1b[0m\r\n\r\n", diff, kMaxDepth);
    s += scoreboard_str();
    s += "\r\n\x1b[97m   Press any key to start";
    s += "\x1b[90m  (q to quit)\x1b[0m\r\n";
    return s;
}

std::string Game::render_frame() const {
    const int cam_x = std::clamp(player_.pos.x - kViewW / 2, 0, std::max(0, map_.width() - kViewW));
    const int cam_y = std::clamp(player_.pos.y - kViewH / 2, 0, std::max(0, map_.height() - kViewH));

    std::string out;
    out.reserve(static_cast<std::size_t>(kViewW) * kViewH * 6);
    out += "\x1b[0m\x1b[97m  SIDE HUSTLE \x1b[90m— a corporate dungeon crawl\x1b[0m\r\n";

    for (int sy = 0; sy < kViewH; ++sy) {
        for (int sx = 0; sx < kViewW; ++sx) {
            out += cell_str({cam_x + sx, cam_y + sy});
        }
        out += "\x1b[0m\r\n";
    }

    out += hud_str();
    out += "\x1b[0m\x1b[J"; // reset + erase anything left from a taller frame
    return out;
}

std::string Game::render_end() const {
    std::string s = "\x1b[2J\x1b[H\x1b[0m\r\n";
    if (state_ == State::Dead) {
        s += "\x1b[91m   GAME OVER — the grind got you.\x1b[0m\r\n";
    } else {
        s += "\x1b[92m   YOU WIN — you escaped and went full-time on your side hustle!\x1b[0m\r\n";
    }
    s += std::format("\r\n   Reached floor {}, level {}, with ${} banked.\r\n", depth_, level_, cash_);
    s += std::format("\x1b[93m   Final score: {}\x1b[0m\r\n", score());
    if (high_scores_.entries().size() == 1 ||
        (!high_scores_.entries().empty() && high_scores_.entries().front().score == score())) {
        s += "\x1b[92m   A new top score!\x1b[0m\r\n";
    }
    s += "\r\n" + scoreboard_str();
    s += "\r\n   Press any key to exit.\r\n";
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
                    }
                }
                if (w == player_.pos) {
                    ch = '@';
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
            if (player_.pos == stairs_) {
                key = '>';
            } else if (const auto step = next_step_towards(map_, player_.pos, stairs_, {})) {
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

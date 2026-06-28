//
// SIDE HUSTLE — graphical front-end (raylib).
//
// This is a pure *view + input* layer. All world simulation lives in sh::Game;
// here we translate key presses into Game::Command, call game.advance(), and
// draw the resulting state with GPU-rendered shapes. No game logic is
// duplicated. raylib links statically, so the Windows build is a single .exe.
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "raylib.h"

#include "game/Game.hpp"
#include "game/Scores.hpp"

using sh::Game;
using sh::Vec2;

namespace {

constexpr int kTile = 36;     // pixels per map tile
constexpr int kHudW = 340;    // right sidebar width
constexpr float kMoveRepeat = 0.11f;

Color C(int r, int g, int b, int a = 255) {
    return Color{static_cast<unsigned char>(r), static_cast<unsigned char>(g),
                 static_cast<unsigned char>(b), static_cast<unsigned char>(a)};
}

Color shade(Color c, double f) {
    return C(static_cast<int>(c.r * f), static_cast<int>(c.g * f), static_cast<int>(c.b * f), c.a);
}

// Mirror of the entity/item colour codes used by the terminal renderer.
Color code_color(int code) {
    switch (code) {
        case 31: return C(220, 70, 70);
        case 32: return C(96, 205, 96);
        case 33: return C(235, 200, 80);
        case 35: return C(205, 115, 205);
        case 36: return C(96, 205, 215);
        case 91: return C(255, 95, 95);
        case 93: return C(255, 226, 120);
        case 95: return C(240, 120, 240);
        case 97: return C(245, 245, 245);
        default: return C(220, 220, 220);
    }
}

// Light multiplier for a tile: 1.0 near the player fading to ~0.45 at the FOV
// edge; returns -1 for an explored-but-not-currently-visible "memory" tile.
double light_at(const Game& g, Vec2 w) {
    const auto& t = g.map().at(w);
    if (!t.visible) return -1.0;
    const double d = static_cast<double>(w.chebyshev(g.player().pos));
    const double t01 = g.fov_radius() > 0 ? std::clamp(d / g.fov_radius(), 0.0, 1.0) : 0.0;
    return 1.0 - 0.55 * t01;
}

void draw_terrain(const Game& g, Vec2 w, int px, int py) {
    const auto& t = g.map().at(w);
    const double light = light_at(g, w);
    const bool visible = light >= 0.0;
    const double L = visible ? light : 1.0;

    using sh::TileType;
    if (t.type == TileType::Wall) {
        const Color base = visible ? C(124, 110, 92) : C(46, 50, 72);
        DrawRectangle(px, py, kTile, kTile, shade(base, L));
        if (visible) {
            DrawRectangle(px, py, kTile, 4, shade(C(156, 142, 120), L));        // top bevel
            DrawRectangle(px, py + kTile - 4, kTile, 4, shade(C(78, 68, 56), L)); // bottom bevel
        }
        return;
    }

    // Floor / stairs.
    const Color base = visible ? C(58, 55, 67) : C(30, 34, 50);
    DrawRectangle(px, py, kTile, kTile, shade(base, L));
    DrawRectangleLinesEx(Rectangle{(float)px, (float)py, (float)kTile, (float)kTile}, 1.0f,
                         Fade(BLACK, 0.20f));
    if (t.type == TileType::StairsDown) {
        const Color c = shade(C(255, 226, 120), L);
        for (int i = 0; i < 3; ++i) {
            const int yy = py + 8 + i * 7;
            DrawTriangle(Vector2{(float)(px + 10), (float)yy},
                         Vector2{(float)(px + kTile - 10), (float)yy},
                         Vector2{(float)(px + kTile / 2), (float)(yy + 6)}, c);
        }
    } else if (visible) {
        DrawCircle(px + kTile / 2, py + kTile / 2, 1.6f, Fade(BLACK, 0.35f));
    }
}

void draw_hp_pip(const sh::Entity& m, int px, int py) {
    if (m.combat.hp >= m.combat.max_hp) return;
    const double frac = std::clamp(static_cast<double>(m.combat.hp) / std::max(1, m.combat.max_hp), 0.0, 1.0);
    const int bw = kTile - 10;
    DrawRectangle(px + 5, py + 2, bw, 4, C(35, 12, 12));
    DrawRectangle(px + 5, py + 2, static_cast<int>(bw * frac), 4, C(225, 70, 70));
}

void draw_monster(const sh::Entity& m, int px, int py) {
    const Vector2 ctr{(float)(px + kTile / 2), (float)(py + kTile / 2)};
    const Color col = code_color(m.color);
    const Color dark = shade(col, 0.45);

    if (m.glyph == '&') { // CEO boss
        DrawPoly(ctr, 6, kTile * 0.46f, 0.0f, col);
        DrawPolyLines(ctr, 6, kTile * 0.46f, 0.0f, dark);
        DrawRectangle((int)ctr.x - 8, py + 3, 16, 5, C(255, 215, 90)); // little crown bar
        return;
    }
    switch (m.kind) {
        case sh::MonsterKind::Bug:
            DrawCircleV(ctr, kTile * 0.22f, col);
            for (int i = -1; i <= 1; ++i) {
                DrawLineEx(Vector2{ctr.x - 10, ctr.y + i * 5.0f}, Vector2{ctr.x - 15, ctr.y + i * 5.0f}, 2, dark);
                DrawLineEx(Vector2{ctr.x + 10, ctr.y + i * 5.0f}, Vector2{ctr.x + 15, ctr.y + i * 5.0f}, 2, dark);
            }
            break;
        case sh::MonsterKind::Client:
            DrawPoly(ctr, 4, kTile * 0.30f, 45.0f, col);
            DrawPolyLines(ctr, 4, kTile * 0.30f, 45.0f, dark);
            break;
        case sh::MonsterKind::Recruiter:
            DrawRectangleRounded(Rectangle{ctr.x - 11, ctr.y - 11, 22, 22}, 0.3f, 6, col);
            DrawRectangleRoundedLines(Rectangle{ctr.x - 11, ctr.y - 11, 22, 22}, 0.3f, 6, dark);
            break;
        case sh::MonsterKind::Manager:
            DrawRectangleRounded(Rectangle{ctr.x - 13, ctr.y - 13, 26, 26}, 0.25f, 6, col);
            DrawRectangleRoundedLines(Rectangle{ctr.x - 13, ctr.y - 13, 26, 26}, 0.25f, 6, dark);
            break;
    }
}

void draw_item(const sh::Item& it, int px, int py) {
    const Vector2 ctr{(float)(px + kTile / 2), (float)(py + kTile / 2)};
    switch (it.kind) {
        case sh::ItemKind::Cash:
            DrawCircleV(ctr, kTile * 0.26f, C(235, 200, 80));
            DrawCircleLines((int)ctr.x, (int)ctr.y, kTile * 0.26f, C(150, 120, 40));
            DrawText("$", (int)ctr.x - 5, (int)ctr.y - 9, 18, C(90, 70, 20));
            break;
        case sh::ItemKind::Coffee:
            DrawRectangleRounded(Rectangle{ctr.x - 9, ctr.y - 7, 16, 15}, 0.25f, 5, C(96, 205, 215));
            DrawRing(Vector2{ctr.x + 9, ctr.y - 1}, 3, 6, 270, 90, 12, C(96, 205, 215)); // handle
            DrawLineEx(Vector2{ctr.x - 4, ctr.y - 12}, Vector2{ctr.x - 4, ctr.y - 16}, 2, C(200, 220, 225));
            DrawLineEx(Vector2{ctr.x + 2, ctr.y - 12}, Vector2{ctr.x + 2, ctr.y - 16}, 2, C(200, 220, 225));
            break;
        case sh::ItemKind::Upgrade:
            DrawRectangleRounded(Rectangle{ctr.x - 11, ctr.y - 8, 22, 14}, 0.15f, 4, C(210, 215, 225));
            DrawRectangle((int)ctr.x - 13, (int)ctr.y + 6, 26, 3, C(150, 155, 165));
            DrawRectangle((int)ctr.x - 8, (int)ctr.y - 5, 16, 8, C(70, 130, 180));
            break;
    }
}

void draw_player(const Game& g, int px, int py) {
    const Vector2 ctr{(float)(px + kTile / 2), (float)(py + kTile / 2)};
    DrawCircleV(ctr, kTile * 0.36f, C(255, 210, 92));
    DrawCircleLines((int)ctr.x, (int)ctr.y, kTile * 0.36f, C(120, 88, 30));
    DrawCircleV(ctr, kTile * 0.16f, C(120, 88, 30));
    (void)g;
}

// ---- text helpers ----------------------------------------------------------

void text(const char* s, int x, int y, int size, Color c) { DrawText(s, x, y, size, c); }

void text_center(const char* s, int cx, int y, int size, Color c) {
    DrawText(s, cx - MeasureText(s, size) / 2, y, size, c);
}

// Greedy word-wrap to a pixel width for the default font.
std::vector<std::string> wrap_text(const std::string& s, int max_w, int size) {
    std::vector<std::string> out;
    std::string line, word;
    auto flush_word = [&]() {
        if (word.empty()) return;
        const std::string trial = line.empty() ? word : line + " " + word;
        if (MeasureText(trial.c_str(), size) <= max_w) {
            line = trial;
        } else {
            if (!line.empty()) out.push_back(line);
            line = word;
        }
        word.clear();
    };
    for (char c : s) {
        if (c == ' ') flush_word();
        else word += c;
    }
    flush_word();
    if (!line.empty()) out.push_back(line);
    if (out.empty()) out.push_back("");
    return out;
}

void draw_bar(int x, int y, int w, int h, double frac, Color fill, Color back) {
    frac = std::clamp(frac, 0.0, 1.0);
    DrawRectangleRounded(Rectangle{(float)x, (float)y, (float)w, (float)h}, 0.5f, 6, back);
    if (frac > 0.0) {
        const int fw = std::max(h, static_cast<int>(w * frac));
        DrawRectangleRounded(Rectangle{(float)x, (float)y, (float)fw, (float)h}, 0.5f, 6, fill);
    }
}

Color hp_color(double ratio) {
    // green -> yellow -> red
    if (ratio >= 0.5) {
        const double t = (ratio - 0.5) * 2.0;
        return C((int)(235 + (90 - 235) * t), (int)(205 + (210 - 205) * t), (int)(80 + (90 - 80) * t));
    }
    const double t = ratio * 2.0;
    return C((int)(230 + (235 - 230) * t), (int)(70 + (205 - 70) * t), (int)(70 + (80 - 70) * t));
}

void draw_hud(const Game& g, int x, int w, int h) {
    BeginScissorMode(x, 0, w, h); // keep all sidebar text inside the panel
    DrawRectangle(x, 0, w, h, C(22, 24, 34));
    DrawLine(x, 0, x, h, C(60, 64, 84));
    const int ix = x + 20;
    int y = 22;

    text("SIDE HUSTLE", ix, y, 26, C(255, 226, 120));
    y += 38;

    const auto& p = g.player();
    const double hpRatio = std::clamp((double)p.combat.hp / std::max(1, p.combat.max_hp), 0.0, 1.0);
    text("HEALTH", ix, y, 14, C(150, 162, 190));
    y += 18;
    draw_bar(ix, y, w - 40, 16, hpRatio, hp_color(hpRatio), C(40, 44, 60));
    text(TextFormat("%d / %d", std::max(0, p.combat.hp), p.combat.max_hp), ix + 6, y + 1, 14, C(20, 24, 30));
    y += 30;

    const double xpRatio = g.xp_next() > 0 ? (double)g.xp() / g.xp_next() : 0.0;
    text(TextFormat("LEVEL %d", g.level()), ix, y, 14, C(150, 162, 190));
    y += 18;
    draw_bar(ix, y, w - 40, 10, xpRatio, C(120, 170, 235), C(40, 44, 60));
    y += 26;

    text(TextFormat("Floor  %d / 8", g.depth()), ix, y, 18, C(226, 231, 242));
    y += 24;
    text(TextFormat("Cash   $%d", g.cash()), ix, y, 18, C(235, 205, 80));
    y += 24;
    text(TextFormat("Coffee x%d   (press E)", g.coffees()), ix, y, 18, C(96, 205, 215));
    y += 24;
    text(TextFormat("Attack %d   Defense %d", p.combat.attack, p.combat.defense), ix, y, 16,
         C(170, 178, 198));
    y += 34;

    DrawLine(ix, y, x + w - 20, y, C(50, 54, 72));
    y += 12;
    text("LOG", ix, y, 14, C(150, 162, 190));
    y += 20;
    const auto& msgs = g.messages();
    const int total = static_cast<int>(msgs.size());
    const int max_w = (x + w - 20) - ix;
    int lines_drawn = 0;
    constexpr int kMaxLines = 13;
    for (int i = std::max(0, total - 8); i < total && lines_drawn < kMaxLines; ++i) {
        const bool latest = (i == total - 1);
        const Color col = latest ? C(216, 223, 236) : C(135, 143, 163);
        for (const auto& ln : wrap_text(msgs[(std::size_t)i], max_w, 13)) {
            if (lines_drawn >= kMaxLines) break;
            DrawText(ln.c_str(), ix, y, 13, col);
            y += 17;
            ++lines_drawn;
        }
    }

    // Controls hint pinned near the bottom.
    int cy = h - 78;
    DrawLine(ix, cy - 10, x + w - 20, cy - 10, C(50, 54, 72));
    text("Move  WASD / Arrows / HJKL", ix, cy, 13, C(120, 128, 150));
    text("Diagonals  Y U B N", ix, cy + 17, 13, C(120, 128, 150));
    text("Coffee E   Wait .   Descend Enter", ix, cy + 34, 13, C(120, 128, 150));
    text("Quit  Q", ix, cy + 51, 13, C(120, 128, 150));
    EndScissorMode();
}

void draw_scoreboard(const sh::HighScores& hs, int cx, int y) {
    text_center("Top side hustles", cx, y, 22, C(150, 162, 190));
    y += 32;
    const auto& e = hs.entries();
    if (e.empty()) {
        text_center("(none yet - be the first)", cx, y, 18, C(100, 108, 130));
        return;
    }
    const Color medal[3] = {C(255, 215, 90), C(198, 204, 216), C(200, 140, 80)};
    for (std::size_t i = 0; i < e.size(); ++i) {
        const Color rc = i < 3 ? medal[i] : C(120, 128, 150);
        const char* line = TextFormat("%2d.  %6d   %-12s floor %d  lvl %d  $%d%s", (int)i + 1,
                                       e[i].score, e[i].name.c_str(), e[i].depth, e[i].level,
                                       e[i].cash, e[i].won ? "  WON" : "");
        text_center(line, cx, y, 18, rc);
        y += 24;
    }
}

// Map currently-held keys to a movement delta (arrows + WASD + HJKL + YUBN).
Vec2 movement_dir() {
    Vec2 d{0, 0};
    if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A) || IsKeyDown(KEY_H)) d.x = -1;
    if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D) || IsKeyDown(KEY_L)) d.x = 1;
    if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W) || IsKeyDown(KEY_K)) d.y = -1;
    if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S) || IsKeyDown(KEY_J)) d.y = 1;
    if (IsKeyDown(KEY_Y)) { d = {-1, -1}; }
    if (IsKeyDown(KEY_U)) { d = {1, -1}; }
    if (IsKeyDown(KEY_B)) { d = {-1, 1}; }
    if (IsKeyDown(KEY_N)) { d = {1, 1}; }
    return d;
}

Game::Command dir_to_cmd(Vec2 d) {
    if (d.x < 0 && d.y < 0) return Game::Command::MoveNW;
    if (d.x > 0 && d.y < 0) return Game::Command::MoveNE;
    if (d.x < 0 && d.y > 0) return Game::Command::MoveSW;
    if (d.x > 0 && d.y > 0) return Game::Command::MoveSE;
    if (d.x < 0) return Game::Command::MoveW;
    if (d.x > 0) return Game::Command::MoveE;
    if (d.y < 0) return Game::Command::MoveN;
    if (d.y > 0) return Game::Command::MoveS;
    return Game::Command::None;
}

} // namespace

int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(1280, 768, "SIDE HUSTLE");
    SetExitKey(KEY_NULL); // we handle quitting ourselves
    SetTargetFPS(60);

    enum class Phase { Title, Playing, End };
    Phase phase = Phase::Title;

    int diff_index = 1; // 0=Easy 1=Normal 2=Hard
    const sh::Difficulty diffs[3] = {sh::Difficulty::Easy, sh::Difficulty::Normal, sh::Difficulty::Hard};
    const char* diff_names[3] = {"Easy", "Normal", "Hard"};
    const std::string scores_path = "side-hustle-scores.txt";

    sh::HighScores board(scores_path);
    board.load();

    std::unique_ptr<Game> game;
    float move_timer = 0.0f;
    float flash = 0.0f;       // red damage-flash intensity
    int last_hp = 0;

    auto start_game = [&]() {
        sh::Config cfg;
        cfg.seed = static_cast<std::uint64_t>(std::time(nullptr));
        cfg.difficulty = diffs[diff_index];
        cfg.scores_path = scores_path;
        const char* user = std::getenv("USERNAME");
        if (user == nullptr || *user == '\0') user = std::getenv("USER");
        if (user != nullptr && *user != '\0') cfg.player_name = user;
        game = std::make_unique<Game>(std::move(cfg));
        last_hp = game->player().combat.hp;
        flash = 0.0f;
        move_timer = 0.0f;
        phase = Phase::Playing;
    };

    // Headless smoke test: when SH_SMOKE=N is set, auto-start, drive a few
    // turns, and exit after N frames. Used to validate the render loop in CI /
    // a virtual framebuffer where there is no display to interact with.
    int smoke_frames = 0;
    if (const char* s = std::getenv("SH_SMOKE")) smoke_frames = std::atoi(s);
    int frame = 0;

    bool quit = false;
    while (!WindowShouldClose() && !quit) {
        ++frame;
        if (smoke_frames > 0) {
            if (frame == 2 && phase == Phase::Title) start_game();
            else if (phase == Phase::Playing && game) {
                const Game::Command demo[4] = {Game::Command::MoveE, Game::Command::MoveS,
                                               Game::Command::Wait, Game::Command::MoveW};
                game->advance(demo[frame % 4]);
                if (game->player().combat.hp < last_hp) flash = 1.0f;
                last_hp = game->player().combat.hp;
                if (!game->is_playing()) { board.load(); phase = Phase::End; }
            }
            if (frame >= smoke_frames) quit = true;
        }
        const float dt = GetFrameTime();
        const int W = GetScreenWidth();
        const int H = GetScreenHeight();

        // ---- input / update ------------------------------------------------
        if (phase == Phase::Title) {
            if (IsKeyPressed(KEY_ONE)) diff_index = 0;
            if (IsKeyPressed(KEY_TWO)) diff_index = 1;
            if (IsKeyPressed(KEY_THREE)) diff_index = 2;
            if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) diff_index = (diff_index + 2) % 3;
            if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) diff_index = (diff_index + 1) % 3;
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) start_game();
            if (IsKeyPressed(KEY_Q)) quit = true;
        } else if (phase == Phase::Playing && game) {
            Game::Command cmd = Game::Command::None;
            if (IsKeyPressed(KEY_E)) cmd = Game::Command::UseCoffee;
            else if (IsKeyPressed(KEY_ENTER)) cmd = Game::Command::Descend;
            else if (IsKeyPressed(KEY_PERIOD) || IsKeyPressed(KEY_SPACE)) cmd = Game::Command::Wait;
            else {
                const Vec2 d = movement_dir();
                move_timer -= dt;
                if (d.x == 0 && d.y == 0) {
                    move_timer = 0.0f; // next press steps immediately
                } else if (move_timer <= 0.0f) {
                    cmd = dir_to_cmd(d);
                    move_timer = kMoveRepeat;
                }
            }
            if (IsKeyPressed(KEY_Q)) quit = true;

            if (cmd != Game::Command::None) game->advance(cmd);

            const int hp = game->player().combat.hp;
            if (hp < last_hp) flash = std::min(1.0f, flash + 0.6f);
            last_hp = hp;

            if (!game->is_playing()) {
                board.load(); // refresh with the score just recorded
                phase = Phase::End;
            }
        } else if (phase == Phase::End) {
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) phase = Phase::Title;
            if (IsKeyPressed(KEY_Q)) quit = true;
        }
        flash = std::max(0.0f, flash - dt * 2.2f);

        // ---- draw ----------------------------------------------------------
        BeginDrawing();
        ClearBackground(C(14, 15, 22));

        if (phase == Phase::Title) {
            const int cx = W / 2;
            text_center("SIDE  HUSTLE", cx, H / 2 - 210, 84, C(255, 214, 92));
            text_center("A corporate dungeon crawl - built in modern C++ with raylib", cx, H / 2 - 130,
                        20, C(150, 160, 185));

            const int by = H / 2 - 70;
            int bx = cx - 240;
            for (int i = 0; i < 3; ++i) {
                const bool sel = i == diff_index;
                const Rectangle r{(float)bx, (float)by, 150, 46};
                DrawRectangleRounded(r, 0.3f, 8, sel ? C(60, 70, 96) : C(32, 36, 50));
                DrawRectangleRoundedLines(r, 0.3f, 8, sel ? C(255, 214, 92) : C(70, 76, 96));
                text_center(diff_names[i], bx + 75, by + 13, 22, sel ? C(255, 226, 120) : C(170, 178, 198));
                bx += 165;
            }
            text_center("1 / 2 / 3  or  arrows to choose difficulty", cx, by + 64, 16, C(110, 120, 145));

            draw_scoreboard(board, cx, H / 2 + 30);
            text_center("Press  ENTER  to start          Q to quit", cx, H - 70, 22, C(230, 235, 245));
        } else if (game) {
            // World view.
            const int mapAreaW = W - kHudW;
            const int cols = std::max(1, mapAreaW / kTile);
            const int rows = std::max(1, H / kTile);
            const int camX = std::clamp(game->player().pos.x - cols / 2, 0,
                                        std::max(0, game->map().width() - cols));
            const int camY = std::clamp(game->player().pos.y - rows / 2, 0,
                                        std::max(0, game->map().height() - rows));

            for (int ry = 0; ry < rows; ++ry) {
                for (int rx = 0; rx < cols; ++rx) {
                    const Vec2 w{camX + rx, camY + ry};
                    if (!game->map().in_bounds(w) || !game->map().at(w).explored) continue;
                    const int px = rx * kTile;
                    const int py = ry * kTile;
                    draw_terrain(*game, w, px, py);
                }
            }
            // Items (visible only).
            for (const auto& it : game->items()) {
                if (it.taken) continue;
                const Vec2 w = it.pos;
                if (!game->map().in_bounds(w) || !game->map().at(w).visible) continue;
                const int px = (w.x - camX) * kTile;
                const int py = (w.y - camY) * kTile;
                if (px >= 0 && py >= 0 && px < mapAreaW && py < H) draw_item(it, px, py);
            }
            // Monsters (visible only) + health pips.
            for (const auto& m : game->monsters()) {
                if (!m.alive) continue;
                const Vec2 w = m.pos;
                if (!game->map().in_bounds(w) || !game->map().at(w).visible) continue;
                const int px = (w.x - camX) * kTile;
                const int py = (w.y - camY) * kTile;
                if (px >= 0 && py >= 0 && px < mapAreaW && py < H) {
                    draw_monster(m, px, py);
                    draw_hp_pip(m, px, py);
                }
            }
            // Player.
            {
                const int px = (game->player().pos.x - camX) * kTile;
                const int py = (game->player().pos.y - camY) * kTile;
                draw_player(*game, px, py);
            }

            // Damage flash overlay.
            if (flash > 0.0f) DrawRectangle(0, 0, mapAreaW, H, Fade(C(200, 30, 30), 0.35f * flash));

            draw_hud(*game, mapAreaW, kHudW, H);

            // End overlay.
            if (phase == Phase::End) {
                DrawRectangle(0, 0, W, H, Fade(BLACK, 0.72f));
                const int cx = W / 2;
                if (game->is_won()) {
                    text_center("YOU WIN", cx, H / 2 - 150, 80, C(110, 225, 120));
                    text_center("You escaped and went full-time on your side hustle!", cx, H / 2 - 70, 22,
                                C(200, 206, 220));
                } else {
                    text_center("GAME OVER", cx, H / 2 - 150, 80, C(235, 80, 80));
                    text_center("The grind got you.", cx, H / 2 - 70, 22, C(200, 206, 220));
                }
                text_center(TextFormat("Floor %d   Level %d   $%d   Score %d", game->depth(),
                                       game->level(), game->cash(), game->score()),
                            cx, H / 2 - 30, 24, C(235, 205, 80));
                draw_scoreboard(board, cx, H / 2 + 20);
                text_center("Press  ENTER  for the menu          Q to quit", cx, H - 70, 22,
                            C(230, 235, 245));
            }
        }

        EndDrawing();

        if (smoke_frames > 0) {
            if (frame == 14) TakeScreenshot("sh_play.png");
            if (frame == 1) TakeScreenshot("sh_title.png");
            if (frame == smoke_frames - 1) TakeScreenshot("sh_final.png");
        }
    }

    CloseWindow();
    return 0;
}

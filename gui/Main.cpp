//
// SIDE HUSTLE — graphical front-end (raylib), high-detail renderer.
//
// Pure view + input over sh::Game: input -> Game::Command -> game.advance(),
// then the resulting state is drawn with GPU shapes. No game logic lives here.
//
// Visual features: embedded TrueType text, 2.5D wall blocks with bevels and
// drop shadows, per-tile floor variation + ambient occlusion, smooth
// fractional-scroll camera, tweened entity movement, idle bob, soft dynamic
// lighting + vignette, a particle system (hit sparks, coin/level bursts,
// ambient dust), and floating damage numbers.
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

#include "AssetFont.h"
#include "game/Game.hpp"
#include "game/Scores.hpp"
#include "world/Pathfinding.hpp"

using sh::Game;
using sh::Vec2;

namespace {

constexpr int kTile = 40;
constexpr int kHudW = 360;
constexpr float kMoveRepeat = 0.11f;

// ---- globals (render resources) -------------------------------------------
Font gFont{};
float a_time_rot_v = 0.0f;          // shared spin angle for the boss polygon
float a_time_rot() { return a_time_rot_v; }

// ---- tiny utilities --------------------------------------------------------
Color C(int r, int g, int b, int a = 255) {
    return Color{(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a};
}
Color shade(Color c, double f) {
    return C((int)(c.r * f), (int)(c.g * f), (int)(c.b * f), c.a);
}
Color mix(Color a, Color b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return C((int)(a.r + (b.r - a.r) * t), (int)(a.g + (b.g - a.g) * t),
             (int)(a.b + (b.b - a.b) * t), (int)(a.a + (b.a - a.a) * t));
}

uint32_t gRng = 0x9e3779b9u;
float frand() {
    gRng ^= gRng << 13; gRng ^= gRng >> 17; gRng ^= gRng << 5;
    return (gRng & 0xffffff) / float(0xffffff);
}
float frand(float a, float b) { return a + (b - a) * frand(); }

float hash2(int x, int y) {
    uint32_t h = ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return (h & 0xffff) / 65535.0f;
}

Color code_color(int code) {
    switch (code) {
        case 31: return C(224, 78, 78);
        case 32: return C(104, 210, 104);
        case 33: return C(238, 202, 84);
        case 35: return C(208, 120, 208);
        case 36: return C(98, 208, 218);
        case 91: return C(255, 100, 100);
        case 93: return C(255, 226, 122);
        case 95: return C(242, 122, 242);
        case 97: return C(245, 245, 245);
        default: return C(220, 220, 220);
    }
}

// ---- text helpers (embedded TTF) ------------------------------------------
float sp_for(float size) { return size * 0.05f; }
float tw(const char* s, float size) { return MeasureTextEx(gFont, s, size, sp_for(size)).x; }
void dt(const char* s, float x, float y, float size, Color c) {
    DrawTextEx(gFont, s, {x, y}, size, sp_for(size), c);
}
void dtsh(const char* s, float x, float y, float size, Color c, Color sh = C(0, 0, 0, 150)) {
    const float o = std::max(1.0f, size * 0.045f);
    DrawTextEx(gFont, s, {x + o, y + o}, size, sp_for(size), sh);
    DrawTextEx(gFont, s, {x, y}, size, sp_for(size), c);
}
void dtc(const char* s, float cx, float y, float size, Color c) { dt(s, cx - tw(s, size) / 2, y, size, c); }
void dtcsh(const char* s, float cx, float y, float size, Color c, Color sh = C(0, 0, 0, 160)) {
    dtsh(s, cx - tw(s, size) / 2, y, size, c, sh);
}

// ---- particles & floating text --------------------------------------------
struct Particle {
    Vector2 pos, vel;
    float life, max, size;
    Color col;
    bool gravity;
};
struct FloatTxt {
    Vector2 pos;
    std::string text;
    float life, max, size;
    Color col;
};
std::vector<Particle> gParticles;
std::vector<FloatTxt> gFloats;

void add_particle(Vector2 p, Vector2 v, float life, float size, Color c, bool grav) {
    gParticles.push_back({p, v, life, life, size, c, grav});
}
void burst(Vector2 p, Color c, int n, float speed, float life, float size, bool grav) {
    for (int i = 0; i < n; ++i) {
        const float a = frand(0.0f, 6.2831853f);
        const float s = speed * frand(0.3f, 1.0f);
        add_particle(p, {std::cos(a) * s, std::sin(a) * s - (grav ? speed * 0.4f : 0.0f)},
                     life * frand(0.6f, 1.0f), size * frand(0.7f, 1.2f), c, grav);
    }
}
void add_float(Vector2 p, const std::string& s, Color c, float size) {
    gFloats.push_back({p, s, 0.9f, 0.9f, size, c});
}

void update_particles(float dt) {
    for (auto& p : gParticles) {
        p.pos.x += p.vel.x * dt;
        p.pos.y += p.vel.y * dt;
        if (p.gravity) p.vel.y += 320.0f * dt;
        p.vel.x *= (1.0f - 2.2f * dt);
        p.life -= dt;
    }
    std::erase_if(gParticles, [](const Particle& p) { return p.life <= 0.0f; });
    for (auto& f : gFloats) {
        f.pos.y -= 30.0f * dt;
        f.life -= dt;
    }
    std::erase_if(gFloats, [](const FloatTxt& f) { return f.life <= 0.0f; });
}
void draw_particles() {
    BeginBlendMode(BLEND_ADDITIVE);
    for (const auto& p : gParticles) {
        const float a = std::clamp(p.life / p.max, 0.0f, 1.0f);
        DrawCircleV(p.pos, std::max(0.5f, p.size * a), Fade(p.col, a));
    }
    EndBlendMode();
}
void draw_floats() {
    for (const auto& f : gFloats) {
        const float a = std::clamp(f.life / f.max, 0.0f, 1.0f);
        const float y = f.pos.y - (1.0f - a) * 6.0f;
        dtcsh(f.text.c_str(), f.pos.x, y, f.size, Fade(f.col, a), Fade(C(0, 0, 0), a * 0.6f));
    }
}

// ---- animation / FX state --------------------------------------------------
struct Anim {
    Vector2 playerR{};
    std::vector<Vector2> monR;
    std::vector<int> monHp;
    int prevHp = 0, prevCash = 0, prevLevel = 1;
    int lastDepth = -1;
    float hpShown = 1.0f, xpShown = 0.0f, time = 0.0f;
};

Vector2 tile_center(float wx, float wy, float camx, float camy) {
    return {(wx + 0.5f - camx) * kTile, (wy + 0.5f - camy) * kTile};
}

void sync_fx(const Game& g, Anim& a, float dt, float camx, float camy) {
    a.time += dt;
    const auto& mons = g.monsters();

    const bool floor_changed = g.depth() != a.lastDepth || a.monR.size() != mons.size();
    if (floor_changed) {
        a.lastDepth = g.depth();
        a.monR.assign(mons.size(), {});
        a.monHp.assign(mons.size(), 0);
        for (std::size_t i = 0; i < mons.size(); ++i) {
            a.monR[i] = {(float)mons[i].pos.x, (float)mons[i].pos.y};
            a.monHp[i] = mons[i].combat.hp;
        }
        a.playerR = {(float)g.player().pos.x, (float)g.player().pos.y};
        a.prevHp = g.player().combat.hp;
        a.prevCash = g.cash();
        a.prevLevel = g.level();
        gParticles.clear();
        gFloats.clear();
    } else {
        for (std::size_t i = 0; i < mons.size(); ++i) {
            const auto& m = mons[i];
            const Vector2 sc = tile_center(a.monR[i].x, a.monR[i].y, camx, camy);
            if (m.combat.hp < a.monHp[i]) {
                const int dmg = a.monHp[i] - m.combat.hp;
                burst(sc, C(255, 170, 90), 8, 150.0f, 0.45f, 3.0f, false);
                add_float(sc, TextFormat("%d", dmg), C(255, 210, 120), 22);
                if (m.combat.hp <= 0) burst(sc, code_color(m.color), 22, 230.0f, 0.7f, 3.5f, true);
            }
            a.monHp[i] = m.combat.hp;
        }
        const Vector2  psc = tile_center(a.playerR.x, a.playerR.y, camx, camy);
        if (g.player().combat.hp < a.prevHp) {
            const int dmg = a.prevHp - g.player().combat.hp;
            burst(psc, C(255, 90, 90), 12, 170.0f, 0.5f, 3.5f, false);
            add_float({psc.x, psc.y - 6}, TextFormat("-%d", dmg), C(255, 120, 120), 24);
        }
        if (g.cash() > a.prevCash) {
            add_float({psc.x, psc.y - 6}, TextFormat("+$%d", g.cash() - a.prevCash), C(245, 210, 90), 22);
            burst(psc, C(245, 210, 90), 10, 120.0f, 0.6f, 3.0f, true);
        }
        if (g.level() > a.prevLevel) {
            add_float({psc.x, psc.y - 26}, "LEVEL UP!", C(140, 230, 150), 26);
            burst(psc, C(150, 240, 160), 36, 240.0f, 0.9f, 4.0f, false);
        }
        a.prevHp = g.player().combat.hp;
        a.prevCash = g.cash();
        a.prevLevel = g.level();
    }

    // Ease render positions / bars toward their targets.
    const float k = std::min(1.0f, dt * 16.0f);
    a.playerR.x += ((float)g.player().pos.x - a.playerR.x) * k;
    a.playerR.y += ((float)g.player().pos.y - a.playerR.y) * k;
    for (std::size_t i = 0; i < mons.size() && i < a.monR.size(); ++i) {
        a.monR[i].x += ((float)mons[i].pos.x - a.monR[i].x) * k;
        a.monR[i].y += ((float)mons[i].pos.y - a.monR[i].y) * k;
    }
    const float hpR = std::clamp((float)g.player().combat.hp / std::max(1, g.player().combat.max_hp), 0.0f, 1.0f);
    const float xpR = g.xp_next() > 0 ? (float)g.xp() / g.xp_next() : 0.0f;
    a.hpShown += (hpR - a.hpShown) * std::min(1.0f, dt * 6.0f);
    a.xpShown += (xpR - a.xpShown) * std::min(1.0f, dt * 6.0f);

    update_particles(dt);
}

// ---- terrain ---------------------------------------------------------------
double light_at(const Game& g, Vec2 w) {
    const auto& t = g.map().at(w);
    if (!t.visible) return -1.0;
    const double d = (double)w.chebyshev(g.player().pos);
    const double t01 = g.fov_radius() > 0 ? std::clamp(d / g.fov_radius(), 0.0, 1.0) : 0.0;
    return 0.52 + 0.48 * (1.0 - t01 * t01); // gentle, slightly brighter near player
}

void draw_terrain(const Game& g, Vec2 w, int px, int py) {
    using sh::TileType;
    const auto& t = g.map().at(w);
    const double light = light_at(g, w);
    const bool vis = light >= 0.0;
    const double L = vis ? light : 1.0;

    if (t.type == TileType::Wall) {
        if (!vis) {
            DrawRectangle(px, py, kTile, kTile, C(44, 48, 70));
            return;
        }
        const float jitter = 0.92f + hash2(w.x, w.y) * 0.16f;
        const Color top = shade(C((int)(132 * jitter), (int)(118 * jitter), (int)(98 * jitter)), L);
        const Color bot = shade(C(86, 76, 62), L);
        DrawRectangleGradientV(px, py, kTile, kTile, top, bot);
        DrawRectangle(px, py, kTile, 3, shade(C(182, 166, 142), L)); // lit top edge

        // Exposed front face when the tile below is open: gives the block height.
        const Vec2 below{w.x, w.y + 1};
        const bool open_below = g.map().in_bounds(below) && g.map().at(below).type != TileType::Wall &&
                                g.map().at(below).explored;
        if (open_below) {
            const int fh = 11;
            DrawRectangle(px, py + kTile - fh, kTile, fh, shade(C(64, 55, 46), L));
            DrawRectangle(px, py + kTile - fh, kTile, 2, shade(C(120, 106, 88), L));
        }
        return;
    }

    // Floor / stairs.
    if (!vis) {
        DrawRectangle(px, py, kTile, kTile, C(27, 31, 47));
        return;
    }
    const int v = (int)(hash2(w.x * 2 + 1, w.y * 2 + 1) * 10.0f) - 5;
    const Color base = shade(C(58 + v, 55 + v, 66 + v), L);
    DrawRectangle(px, py, kTile, kTile, base);
    DrawRectangleLinesEx(Rectangle{(float)px, (float)py, (float)kTile, (float)kTile}, 1.0f, Fade(BLACK, 0.16f));
    if (hash2(w.x * 5, w.y * 7) > 0.86f)
        DrawRectangle(px + 6 + (int)(hash2(w.x, w.y * 3) * (kTile - 12)), py + 6, 2, 2, Fade(BLACK, 0.4f));

    // Ambient occlusion / drop shadow from adjacent walls.
    auto is_wall = [&](int dx, int dy) {
        const Vec2 n{w.x + dx, w.y + dy};
        return g.map().in_bounds(n) && g.map().at(n).type == TileType::Wall && g.map().at(n).explored;
    };
    if (is_wall(0, -1)) DrawRectangleGradientV(px, py, kTile, 10, Fade(BLACK, 0.40f), Fade(BLACK, 0.0f));
    if (is_wall(-1, 0)) DrawRectangleGradientH(px, py, 8, kTile, Fade(BLACK, 0.28f), Fade(BLACK, 0.0f));
    if (is_wall(1, 0)) DrawRectangleGradientH(px + kTile - 8, py, 8, kTile, Fade(BLACK, 0.0f), Fade(BLACK, 0.28f));

    if (t.type == TileType::StairsDown) {
        const Vector2 c{(float)(px + kTile / 2), (float)(py + kTile / 2)};
        BeginBlendMode(BLEND_ADDITIVE);
        DrawCircleGradient((int)c.x, (int)c.y, kTile * 0.7f, Fade(C(255, 220, 110), 0.20f), Fade(BLACK, 0));
        EndBlendMode();
        const Color cc = shade(C(255, 224, 120), L);
        for (int i = 0; i < 3; ++i) {
            const float yy = py + 11.0f + i * 8.0f;
            DrawTriangle({(float)px + 11, yy}, {(float)px + kTile - 11, yy}, {c.x, yy + 7}, cc);
        }
    }
}

// ---- entities --------------------------------------------------------------
void shadow(Vector2 c) { DrawEllipse((int)c.x, (int)(c.y + kTile * 0.30f), kTile * 0.30f, kTile * 0.12f, Fade(BLACK, 0.35f)); }
void glow(Vector2 c, float r, Color col, float a) {
    BeginBlendMode(BLEND_ADDITIVE);
    DrawCircleGradient((int)c.x, (int)c.y, r, Fade(col, a), Fade(BLACK, 0));
    EndBlendMode();
}
// Clean edge-darkening vignette (no radial banding).
void draw_vignette(int x, int w, int h) {
    constexpr int e = 96;
    const Color d = C(0, 0, 0, 150), t = C(0, 0, 0, 0);
    DrawRectangleGradientV(x, 0, w, e, d, t);
    DrawRectangleGradientV(x, h - e, w, e, t, d);
    DrawRectangleGradientH(x, 0, e, h, d, t);
    DrawRectangleGradientH(x + w - e, 0, e, h, t, d);
}
void eyes(Vector2 c, float dx, float dy, float r) {
    DrawCircleV({c.x - dx, c.y - dy}, r, C(255, 255, 255));
    DrawCircleV({c.x + dx, c.y - dy}, r, C(255, 255, 255));
    DrawCircleV({c.x - dx, c.y - dy}, r * 0.5f, C(20, 20, 30));
    DrawCircleV({c.x + dx, c.y - dy}, r * 0.5f, C(20, 20, 30));
}

void draw_monster(const sh::Entity& m, Vector2 c, float bob) {
    c.y += bob;
    const Color col = code_color(m.color);
    const Color dark = shade(col, 0.5);
    const Color lite = mix(col, C(255, 255, 255), 0.35);
    shadow({c.x, c.y - bob});

    if (m.glyph == '&') { // CEO boss
        glow(c, kTile * 0.7f, col, 0.25f);
        DrawPoly(c, 6, kTile * 0.42f, a_time_rot(), col);
        DrawPolyLines(c, 6, kTile * 0.42f, a_time_rot(), dark);
        DrawPoly(c, 6, kTile * 0.30f, a_time_rot(), shade(col, 0.7));
        DrawRectangle((int)c.x - 9, (int)(c.y - kTile * 0.46f), 18, 5, C(255, 215, 90));
        eyes(c, 5, 2, 3);
        return;
    }
    switch (m.kind) {
        case sh::MonsterKind::Bug: {
            const float r = kTile * 0.24f;
            DrawCircleGradient((int)c.x, (int)c.y, r, lite, col);
            DrawCircleLines((int)c.x, (int)c.y, r, dark);
            for (int i = -1; i <= 1; ++i) {
                DrawLineEx({c.x - r, c.y + i * 5.0f}, {c.x - r - 5, c.y + i * 5.0f}, 2, dark);
                DrawLineEx({c.x + r, c.y + i * 5.0f}, {c.x + r + 5, c.y + i * 5.0f}, 2, dark);
            }
            eyes(c, 4, 3, 2.5f);
            break;
        }
        case sh::MonsterKind::Client:
            DrawPoly(c, 4, kTile * 0.31f, 45.0f, col);
            DrawPoly(c, 4, kTile * 0.20f, 45.0f, lite);
            DrawPolyLines(c, 4, kTile * 0.31f, 45.0f, dark);
            eyes(c, 4, 1, 2.5f);
            break;
        case sh::MonsterKind::Recruiter: {
            Rectangle r{c.x - 12, c.y - 12, 24, 24};
            DrawRectangleRounded(r, 0.3f, 6, col);
            DrawRectangleRounded({c.x - 12, c.y - 12, 24, 11}, 0.3f, 6, lite);
            DrawRectangleRoundedLines(r, 0.3f, 6, dark);
            eyes(c, 5, 2, 2.5f);
            break;
        }
        case sh::MonsterKind::Manager: {
            Rectangle r{c.x - 14, c.y - 14, 28, 28};
            DrawRectangleRounded(r, 0.25f, 6, col);
            DrawRectangleRounded({c.x - 14, c.y - 14, 28, 12}, 0.25f, 6, lite);
            DrawRectangleRoundedLines(r, 0.25f, 6, dark);
            eyes(c, 6, 3, 3);
            break;
        }
    }
}

void draw_item(const sh::Item& it, Vector2 c, float bob) {
    c.y += bob;
    switch (it.kind) {
        case sh::ItemKind::Cash:
            glow(c, kTile * 0.45f, C(245, 210, 90), 0.22f);
            DrawCircleGradient((int)c.x, (int)c.y, kTile * 0.24f, C(255, 235, 150), C(225, 185, 70));
            DrawCircleLines((int)c.x, (int)c.y, kTile * 0.24f, C(150, 120, 40));
            dtc("$", c.x, c.y - 9, 18, C(120, 90, 20));
            break;
        case sh::ItemKind::Coffee:
            glow(c, kTile * 0.42f, C(98, 208, 218), 0.16f);
            DrawRectangleRounded({c.x - 9, c.y - 6, 16, 15}, 0.3f, 6, C(98, 208, 218));
            DrawRing({c.x + 9, c.y + 1}, 3, 6, 270, 90, 14, C(98, 208, 218));
            DrawLineEx({c.x - 4, c.y - 11}, {c.x - 4, c.y - 16}, 2, Fade(C(220, 235, 240), 0.8f));
            DrawLineEx({c.x + 2, c.y - 11}, {c.x + 2, c.y - 16}, 2, Fade(C(220, 235, 240), 0.8f));
            break;
        case sh::ItemKind::Upgrade:
            glow(c, kTile * 0.42f, C(210, 220, 235), 0.15f);
            DrawRectangleRounded({c.x - 12, c.y - 8, 24, 15}, 0.15f, 4, C(208, 214, 226));
            DrawRectangle((int)c.x - 14, (int)c.y + 7, 28, 3, C(150, 156, 168));
            DrawRectangleRounded({c.x - 9, c.y - 5, 18, 9}, 0.1f, 4, C(72, 132, 184));
            break;
    }
}

void draw_player(Vector2 c, float bob) {
    c.y += bob;
    shadow({c.x, c.y - bob});
    glow(c, kTile * 0.62f, C(255, 205, 110), 0.30f);
    const float r = kTile * 0.34f;
    DrawCircleGradient((int)c.x, (int)c.y, r, C(255, 232, 150), C(245, 196, 84));
    DrawCircleLines((int)c.x, (int)c.y, r, C(140, 100, 36));
    DrawCircleV({c.x, c.y + 1}, r * 0.42f, C(150, 108, 40));
    DrawCircleV({c.x - r * 0.32f, c.y - r * 0.36f}, r * 0.18f, Fade(C(255, 255, 255), 0.8f));
}

// ---- HUD -------------------------------------------------------------------
void bar(int x, int y, int w, int h, float frac, Color a, Color b, Color back) {
    frac = std::clamp(frac, 0.0f, 1.0f);
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.5f, 8, back);
    if (frac > 0.0f) {
        const int fw = std::max(h, (int)(w * frac));
        DrawRectangleGradientEx({(float)x, (float)y, (float)fw, (float)h}, a, a, b, b);
    }
    DrawRectangleRoundedLines({(float)x, (float)y, (float)w, (float)h}, 0.5f, 8, Fade(BLACK, 0.4f));
}
Color hp_color(float r) {
    return r >= 0.5f ? mix(C(235, 205, 80), C(96, 214, 104), (r - 0.5f) * 2.0f)
                     : mix(C(230, 70, 70), C(235, 205, 80), r * 2.0f);
}

std::vector<std::string> wrap_text(const std::string& s, int max_w, float size) {
    std::vector<std::string> out;
    std::string line, word;
    auto flush = [&]() {
        if (word.empty()) return;
        const std::string trial = line.empty() ? word : line + " " + word;
        if (tw(trial.c_str(), size) <= max_w) line = trial;
        else { if (!line.empty()) out.push_back(line); line = word; }
        word.clear();
    };
    for (char ch : s) { if (ch == ' ') flush(); else word += ch; }
    flush();
    if (!line.empty()) out.push_back(line);
    if (out.empty()) out.push_back("");
    return out;
}

void draw_hud(const Game& g, const Anim& a, int x, int w, int h) {
    BeginScissorMode(x, 0, w, h);
    DrawRectangleGradientV(x, 0, w, h, C(26, 28, 40), C(18, 19, 28));
    DrawRectangle(x, 0, 2, h, C(70, 76, 98));
    const int ix = x + 22;
    int y = 22;

    dtsh("SIDE HUSTLE", ix, y, 30, C(255, 226, 120));
    y += 46;

    const auto& p = g.player();
    dt("HEALTH", ix, y, 16, C(150, 162, 190));
    y += 22;
    bar(ix, y, w - 44, 20, a.hpShown, hp_color(a.hpShown), shade(hp_color(a.hpShown), 0.6),
        C(40, 44, 60));
    dtc(TextFormat("%d / %d", std::max(0, p.combat.hp), p.combat.max_hp), ix + (w - 44) / 2, y + 2, 15,
        C(15, 18, 24));
    y += 34;

    dt(TextFormat("LEVEL %d", g.level()), ix, y, 16, C(150, 162, 190));
    y += 22;
    bar(ix, y, w - 44, 12, a.xpShown, C(120, 170, 235), C(70, 110, 180), C(40, 44, 60));
    y += 30;

    auto stat = [&](const char* label, const char* val, Color vc) {
        dt(label, ix, y, 19, C(170, 178, 198));
        dt(val, ix + 132, y, 19, vc);
        y += 28;
    };
    stat("Floor", TextFormat("%d / 8", g.depth()), C(232, 236, 246));
    stat("Cash", TextFormat("$%d", g.cash()), C(238, 205, 84));
    stat("Coffee", TextFormat("x%d  (E)", g.coffees()), C(98, 208, 218));
    stat("Attack", TextFormat("%d", p.combat.attack), C(232, 236, 246));
    stat("Defense", TextFormat("%d", p.combat.defense), C(232, 236, 246));
    y += 8;

    DrawLine(ix, y, x + w - 22, y, C(54, 58, 78));
    y += 14;
    dt("LOG", ix, y, 16, C(150, 162, 190));
    y += 24;
    const auto& msgs = g.messages();
    const int total = (int)msgs.size();
    const int max_w = (x + w - 22) - ix;
    int drawn = 0;
    for (int i = std::max(0, total - 8); i < total && drawn < 12; ++i) {
        const Color col = (i == total - 1) ? C(220, 226, 238) : C(140, 148, 168);
        for (const auto& ln : wrap_text(msgs[(std::size_t)i], max_w, 15)) {
            if (drawn >= 12) break;
            dt(ln.c_str(), ix, y, 15, col);
            y += 19;
            ++drawn;
        }
    }

    int cy = h - 86;
    DrawLine(ix, cy - 12, x + w - 22, cy - 12, C(54, 58, 78));
    const Color hc = C(120, 128, 150);
    dt("Move  WASD / Arrows / HJKL", ix, cy, 14, hc);
    dt("Diagonals  Y U B N", ix, cy + 19, 14, hc);
    dt("Coffee E   Wait .   Descend Enter", ix, cy + 38, 14, hc);
    dt("Quit  Q", ix, cy + 57, 14, hc);
    EndScissorMode();
}

void draw_scoreboard(const sh::HighScores& hs, int cx, int y) {
    dtc("Top side hustles", cx, y, 24, C(150, 162, 190));
    y += 36;
    const auto& e = hs.entries();
    if (e.empty()) {
        dtc("(none yet - be the first)", cx, y, 19, C(100, 108, 130));
        return;
    }
    const Color medal[3] = {C(255, 215, 90), C(198, 204, 216), C(200, 140, 80)};
    for (std::size_t i = 0; i < e.size(); ++i) {
        const Color rc = i < 3 ? medal[i] : C(120, 128, 150);
        dtc(TextFormat("%d.  %d   %s   floor %d  lvl %d  $%d%s", (int)i + 1, e[i].score,
                       e[i].name.c_str(), e[i].depth, e[i].level, e[i].cash, e[i].won ? "  WON" : ""),
            cx, y, 19, rc);
        y += 26;
    }
}

// ---- input -----------------------------------------------------------------
Vec2 movement_dir() {
    Vec2 d{0, 0};
    if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A) || IsKeyDown(KEY_H)) d.x = -1;
    if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D) || IsKeyDown(KEY_L)) d.x = 1;
    if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W) || IsKeyDown(KEY_K)) d.y = -1;
    if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S) || IsKeyDown(KEY_J)) d.y = 1;
    if (IsKeyDown(KEY_Y)) d = {-1, -1};
    if (IsKeyDown(KEY_U)) d = {1, -1};
    if (IsKeyDown(KEY_B)) d = {-1, 1};
    if (IsKeyDown(KEY_N)) d = {1, 1};
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
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);

    gFont = LoadFontFromMemory(".ttf", kUiFontTTF, (int)kUiFontTTF_len, 96, nullptr, 0);
    SetTextureFilter(gFont.texture, TEXTURE_FILTER_BILINEAR);

    enum class Phase { Title, Playing, End };
    Phase phase = Phase::Title;
    int diff_index = 1;
    const sh::Difficulty diffs[3] = {sh::Difficulty::Easy, sh::Difficulty::Normal, sh::Difficulty::Hard};
    const char* diff_names[3] = {"Easy", "Normal", "Hard"};
    const std::string scores_path = "side-hustle-scores.txt";

    sh::HighScores board(scores_path);
    board.load();
    std::unique_ptr<Game> game;
    Anim anim;
    float move_timer = 0.0f;
    float flash = 0.0f;

    auto start_game = [&]() {
        sh::Config cfg;
        cfg.seed = (std::uint64_t)std::time(nullptr);
        cfg.difficulty = diffs[diff_index];
        cfg.scores_path = scores_path;
        const char* user = std::getenv("USERNAME");
        if (user == nullptr || *user == '\0') user = std::getenv("USER");
        if (user != nullptr && *user != '\0') cfg.player_name = user;
        game = std::make_unique<Game>(std::move(cfg));
        anim = Anim{};
        flash = 0.0f;
        move_timer = 0.0f;
        phase = Phase::Playing;
    };

    int smoke_frames = 0;
    if (const char* s = std::getenv("SH_SMOKE")) smoke_frames = std::atoi(s);
    int frame = 0;
    bool combat_shot = false;

    bool quit = false;
    while (!WindowShouldClose() && !quit) {
        const float dt = GetFrameTime();
        const int W = GetScreenWidth();
        const int H = GetScreenHeight();
        ++frame;
        a_time_rot_v += dt * 18.0f;

        if (smoke_frames > 0) {
            if (frame == 2 && phase == Phase::Title) start_game();
            else if (phase == Phase::Playing && game) {
                if (frame % 3 == 0) {
                    // Path toward the stairs (via the engine's A*) so the smoke
                    // run traverses floors and fights, exercising the FX layer.
                    const Vec2 pp = game->player().pos;
                    Game::Command c = Game::Command::Wait;
                    if (pp == game->stairs()) {
                        c = Game::Command::Descend;
                    } else if (auto s = sh::next_step_towards(game->map(), pp, game->stairs(), {})) {
                        const int dx = (s->x > pp.x) - (s->x < pp.x);
                        const int dy = (s->y > pp.y) - (s->y < pp.y);
                        c = dir_to_cmd({dx, dy});
                    }
                    game->advance(c);
                }
                if (!game->is_playing()) { board.load(); phase = Phase::End; }
            }
            if (frame >= smoke_frames) quit = true;
        }

        // ---- input ----
        if (phase == Phase::Title) {
            if (IsKeyPressed(KEY_ONE)) diff_index = 0;
            if (IsKeyPressed(KEY_TWO)) diff_index = 1;
            if (IsKeyPressed(KEY_THREE)) diff_index = 2;
            if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) diff_index = (diff_index + 2) % 3;
            if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) diff_index = (diff_index + 1) % 3;
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) start_game();
            if (IsKeyPressed(KEY_Q)) quit = true;
        } else if (phase == Phase::Playing && game && smoke_frames == 0) {
            Game::Command cmd = Game::Command::None;
            if (IsKeyPressed(KEY_E)) cmd = Game::Command::UseCoffee;
            else if (IsKeyPressed(KEY_ENTER)) cmd = Game::Command::Descend;
            else if (IsKeyPressed(KEY_PERIOD) || IsKeyPressed(KEY_SPACE)) cmd = Game::Command::Wait;
            else {
                const Vec2 d = movement_dir();
                move_timer -= dt;
                if (d.x == 0 && d.y == 0) move_timer = 0.0f;
                else if (move_timer <= 0.0f) { cmd = dir_to_cmd(d); move_timer = kMoveRepeat; }
            }
            if (IsKeyPressed(KEY_Q)) quit = true;
            const int before = game->player().combat.hp;
            if (cmd != Game::Command::None) game->advance(cmd);
            if (game->player().combat.hp < before) flash = std::min(1.0f, flash + 0.55f);
            if (!game->is_playing()) { board.load(); phase = Phase::End; }
        } else if (phase == Phase::End) {
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) phase = Phase::Title;
            if (IsKeyPressed(KEY_Q)) quit = true;
        }
        flash = std::max(0.0f, flash - dt * 2.4f);

        // ---- draw ----
        BeginDrawing();
        ClearBackground(C(12, 13, 19));

        if (phase == Phase::Title) {
            const int cx = W / 2;
            dtcsh("SIDE  HUSTLE", cx, H / 2 - 220, 92, C(255, 214, 92));
            dtc("A corporate dungeon crawl - modern C++ + raylib", cx, H / 2 - 128, 22, C(150, 160, 185));
            const int by = H / 2 - 64;
            int bx = cx - 252;
            for (int i = 0; i < 3; ++i) {
                const bool sel = i == diff_index;
                Rectangle r{(float)bx, (float)by, 158, 50};
                DrawRectangleRounded(r, 0.3f, 8, sel ? C(60, 70, 96) : C(32, 36, 50));
                DrawRectangleRoundedLines(r, 0.3f, 8, sel ? C(255, 214, 92) : C(70, 76, 96));
                dtc(diff_names[i], bx + 79, by + 13, 24, sel ? C(255, 226, 120) : C(170, 178, 198));
                bx += 172;
            }
            dtc("1 / 2 / 3 or arrows to choose difficulty", cx, by + 66, 17, C(110, 120, 145));
            draw_scoreboard(board, cx, H / 2 + 40);
            dtcsh("Press  ENTER  to start        Q to quit", cx, H - 64, 24, C(230, 235, 245));
        } else if (game) {
            const int mapAreaW = W - kHudW;
            const float colsF = mapAreaW / (float)kTile;
            const float rowsF = H / (float)kTile;
            const float camx = std::clamp(anim.playerR.x + 0.5f - colsF / 2.0f, 0.0f,
                                          std::max(0.0f, (float)game->map().width() - colsF));
            const float camy = std::clamp(anim.playerR.y + 0.5f - rowsF / 2.0f, 0.0f,
                                          std::max(0.0f, (float)game->map().height() - rowsF));
            sync_fx(*game, anim, dt, camx, camy);

            auto sx = [&](float wx) { return (int)lroundf((wx - camx) * kTile); };
            auto sy = [&](float wy) { return (int)lroundf((wy - camy) * kTile); };

            const int x0 = (int)camx - 1, x1 = (int)(camx + colsF) + 2;
            const int y0 = (int)camy - 1, y1 = (int)(camy + rowsF) + 2;
            for (int wy = y0; wy <= y1; ++wy)
                for (int wx = x0; wx <= x1; ++wx) {
                    const Vec2 w{wx, wy};
                    if (!game->map().in_bounds(w) || !game->map().at(w).explored) continue;
                    draw_terrain(*game, w, sx(wx), sy(wy));
                }

            for (const auto& it : game->items()) {
                if (it.taken || !game->map().in_bounds(it.pos) || !game->map().at(it.pos).visible) continue;
                const float bob = std::sin(anim.time * 3.0f + it.pos.x * 0.7f) * 2.0f;
                draw_item(it, tile_center(it.pos.x, it.pos.y, camx, camy), bob);
            }
            const auto& mons = game->monsters();
            for (std::size_t i = 0; i < mons.size(); ++i) {
                const auto& m = mons[i];
                if (!m.alive || !game->map().in_bounds(m.pos) || !game->map().at(m.pos).visible) continue;
                const Vector2 c = tile_center(anim.monR[i].x, anim.monR[i].y, camx, camy);
                const float bob = std::sin(anim.time * 3.5f + i * 1.3f) * 1.6f;
                draw_monster(m, c, bob);
                // health pip
                if (m.combat.hp < m.combat.max_hp) {
                    const float fr = std::clamp((float)m.combat.hp / std::max(1, m.combat.max_hp), 0.0f, 1.0f);
                    DrawRectangle((int)c.x - 14, (int)c.y - 22, 28, 4, C(35, 12, 12));
                    DrawRectangle((int)c.x - 14, (int)c.y - 22, (int)(28 * fr), 4, C(225, 70, 70));
                }
            }
            {
                const float bob = std::sin(anim.time * 3.0f) * 1.4f;
                draw_player(tile_center(anim.playerR.x, anim.playerR.y, camx, camy), bob);
            }

            draw_particles();
            draw_floats();

            // Lighting: edge vignette + a warm pool around the player.
            draw_vignette(0, mapAreaW, H);
            glow(tile_center(anim.playerR.x, anim.playerR.y, camx, camy), kTile * 4.0f,
                 C(255, 196, 120), 0.06f);

            if (flash > 0.0f) DrawRectangle(0, 0, mapAreaW, H, Fade(C(200, 30, 30), 0.35f * flash));

            draw_hud(*game, anim, mapAreaW, kHudW, H);

            if (phase == Phase::End) {
                DrawRectangle(0, 0, W, H, Fade(BLACK, 0.74f));
                const int cx = W / 2;
                if (game->is_won()) {
                    dtcsh("YOU WIN", cx, H / 2 - 160, 88, C(110, 225, 120));
                    dtc("You escaped and went full-time on your side hustle!", cx, H / 2 - 66, 22,
                        C(200, 206, 220));
                } else {
                    dtcsh("GAME OVER", cx, H / 2 - 160, 88, C(235, 80, 80));
                    dtc("The grind got you.", cx, H / 2 - 66, 22, C(200, 206, 220));
                }
                dtc(TextFormat("Floor %d   Level %d   $%d   Score %d", game->depth(), game->level(),
                               game->cash(), game->score()),
                    cx, H / 2 - 24, 26, C(238, 205, 84));
                draw_scoreboard(board, cx, H / 2 + 28);
                dtcsh("Press  ENTER  for the menu        Q to quit", cx, H - 64, 24, C(230, 235, 245));
            }
        }

        EndDrawing();

        if (smoke_frames > 0) {
            if (frame == 1) TakeScreenshot("sh_title.png");
            if (frame == 40) TakeScreenshot("sh_play.png");
            if (!combat_shot && phase == Phase::Playing && !gFloats.empty()) {
                TakeScreenshot("sh_combat.png");
                combat_shot = true;
            }
            if (frame == smoke_frames - 1) TakeScreenshot("sh_final.png");
        }
    }

    UnloadFont(gFont);
    CloseWindow();
    return 0;
}

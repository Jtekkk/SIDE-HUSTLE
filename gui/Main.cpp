//
// SIDE HUSTLE — graphical front-end (raylib), shader-based renderer.
//
// Pure view + input over sh::Game. Rendering pipeline (a small deferred-style
// 2D lighting setup):
//
//   1. gScene  (RT)  : procedurally-textured terrain + entities (albedo)
//   2. gLight  (RT)  : additive light buffer — one soft light per *visible*
//                      tile (so walls occlude light), plus item/stairs/boss
//                      point lights and a flickering player torch
//   3. composite shader : out = albedo * (ambient + light), then filmic
//                      tonemap + saturation + vignette  -> gLit
//   4. bloom         : downsample gLight -> separable gaussian blur (shader)
//                      -> additively composited for glow
//   5. particles / floating numbers / HUD drawn on top in screen space
//
// All textures are generated at startup (no asset files); the font is embedded;
// raylib links statically -> the Windows build is a single self-contained .exe.
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
#include "rlgl.h"

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

// ===========================================================================
// GLSL shaders (desktop GL 3.3 / GLSL 330; raylib provides the default VS).
// ===========================================================================
const char* kCompositeFS = R"GLSL(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;   // scene albedo
uniform sampler2D lightTex;   // additive (FOV-occluded) light buffer
uniform sampler2D normalTex;  // surface normals (tangent space)
uniform vec3 ambient;
uniform vec2 res;
uniform vec3  lightPos[8];     // xy = pixel position, z = height above the plane
uniform float lightRad[8];
uniform float lightInt[8];
uniform int   lightCount;
out vec4 finalColor;
void main() {
    vec3 albedo = texture(texture0, fragTexCoord).rgb;
    vec3 light  = texture(lightTex, fragTexCoord).rgb;
    vec3 N = normalize(texture(normalTex, fragTexCoord).rgb * 2.0 - 1.0);

    // Per-light directional (normal-mapped) relief term.
    vec2 fp = fragTexCoord * res;
    float relief = 0.0;
    for (int i = 0; i < lightCount; ++i) {
        vec2 dxy = lightPos[i].xy - fp;
        float dist = length(dxy);
        float at = clamp(1.0 - dist / lightRad[i], 0.0, 1.0);
        at *= at;
        vec3 L = normalize(vec3(dxy, lightPos[i].z));
        relief += at * max(0.0, dot(N, L)) * lightInt[i];
    }
    // The flat (FOV-occluded) light gates the relief so walls still cast shadow.
    vec3 lit = albedo * (ambient + light * (0.78 + 0.70 * relief));

    // filmic-ish tonemap for soft highlights
    lit = (lit * (2.2 * lit + 0.55)) / (lit * (2.0 * lit + 1.0) + 0.18);
    float l = dot(lit, vec3(0.299, 0.587, 0.114));
    lit = mix(vec3(l), lit, 1.14);
    vec2 d = fragTexCoord - 0.5;
    float vig = smoothstep(0.95, 0.35, length(d));
    lit *= mix(0.5, 1.0, vig);
    finalColor = vec4(lit, 1.0);
}
)GLSL";

const char* kBlurFS = R"GLSL(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 dir;             // texel step in blur direction
out vec4 finalColor;
void main() {
    float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec3 c = texture(texture0, fragTexCoord).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
        c += texture(texture0, fragTexCoord + dir * float(i)).rgb * w[i];
        c += texture(texture0, fragTexCoord - dir * float(i)).rgb * w[i];
    }
    finalColor = vec4(c, 1.0);
}
)GLSL";

const char* kPostFS = R"GLSL(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;   // composed (lit + bloom + hero + fx)
uniform vec2 res;
uniform float time;
out vec4 finalColor;
void main() {
    vec2 uv = fragTexCoord;
    vec2 cc = uv - 0.5;
    float d2 = dot(cc, cc);
    // gentle barrel distortion (CRT curvature)
    uv += cc * d2 * 0.028;
    // chromatic aberration, stronger toward the edges
    float ca = 0.0012 + 0.0042 * d2;
    vec3 col;
    col.r = texture(texture0, uv + cc * ca).r;
    col.g = texture(texture0, uv).g;
    col.b = texture(texture0, uv - cc * ca).b;
    // scanlines + faint rolling flicker
    float scan = 0.93 + 0.07 * sin(uv.y * res.y * 3.14159);
    col *= scan * (0.985 + 0.015 * sin(time * 8.0));
    // edge vignette + black beyond the curved frame
    float vig = smoothstep(1.05, 0.30, length(cc));
    col *= mix(0.55, 1.0, vig);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) col = vec3(0.0);
    finalColor = vec4(col, 1.0);
}
)GLSL";

// ===========================================================================
// Globals (render resources)
// ===========================================================================
Font gFont{};
Texture2D gFloorTex{}, gWallTex{}, gLightTex{}, gFloorNrm{}, gWallNrm{};
Shader gComposite{}, gBlur{}, gPost{};
int gLocLightTex = 0, gLocAmbient = 0, gLocDir = 0;
int gLocPostRes = 0, gLocPostTime = 0;
int gLocNormalTex = 0, gLocCompRes = 0, gLocLightPos = 0, gLocLightRad = 0, gLocLightInt = 0, gLocLightCount = 0;

RenderTexture2D gScene{}, gLight{}, gLit{}, gBloomA{}, gBloomB{}, gComposed{}, gNormal{};
int gRtW = 0, gRtH = 0;
constexpr int kBloomDiv = 3; // bloom buffers at 1/3 res

float a_time_rot_v = 0.0f;
float a_time_rot() { return a_time_rot_v; }

// ---- small utilities -------------------------------------------------------
Color C(int r, int g, int b, int a = 255) {
    return Color{(unsigned char)std::clamp(r, 0, 255), (unsigned char)std::clamp(g, 0, 255),
                 (unsigned char)std::clamp(b, 0, 255), (unsigned char)std::clamp(a, 0, 255)};
}
Color shade(Color c, double f) { return C((int)(c.r * f), (int)(c.g * f), (int)(c.b * f), c.a); }
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

// ---- text helpers ----------------------------------------------------------
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
struct Particle { Vector2 pos, vel; float life, max, size; Color col; bool gravity; };
struct FloatTxt { Vector2 pos; std::string text; float life, max, size; Color col; };
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
        p.pos.x += p.vel.x * dt; p.pos.y += p.vel.y * dt;
        if (p.gravity) p.vel.y += 320.0f * dt;
        p.vel.x *= (1.0f - 2.2f * dt);
        p.life -= dt;
    }
    std::erase_if(gParticles, [](const Particle& p) { return p.life <= 0.0f; });
    for (auto& f : gFloats) { f.pos.y -= 30.0f * dt; f.life -= dt; }
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
        dtcsh(f.text.c_str(), f.pos.x, f.pos.y, f.size, Fade(f.col, a), Fade(C(0, 0, 0), a * 0.9f));
    }
}

// ---- animation / FX state --------------------------------------------------
struct Anim {
    Vector2 playerR{};
    std::vector<Vector2> monR;
    std::vector<int> monHp;
    std::vector<float> monPunch;       // white hit-flash per monster
    int prevHp = 0, prevCash = 0, prevLevel = 1, lastDepth = -1;
    float hpShown = 1.0f, xpShown = 0.0f, time = 0.0f;
    float shake = 0.0f, playerPunch = 0.0f, playerLunge = 0.0f;
    Vector2 playerLungeDir{};
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
        a.monPunch.assign(mons.size(), 0.0f);
        for (std::size_t i = 0; i < mons.size(); ++i) {
            a.monR[i] = {(float)mons[i].pos.x, (float)mons[i].pos.y};
            a.monHp[i] = mons[i].combat.hp;
        }
        a.playerR = {(float)g.player().pos.x, (float)g.player().pos.y};
        a.prevHp = g.player().combat.hp; a.prevCash = g.cash(); a.prevLevel = g.level();
        a.shake = a.playerPunch = a.playerLunge = 0.0f;
        gParticles.clear(); gFloats.clear();
    } else {
        for (std::size_t i = 0; i < mons.size(); ++i) {
            const auto& m = mons[i];
            const Vector2 sc = tile_center(a.monR[i].x, a.monR[i].y, camx, camy);
            if (m.combat.hp < a.monHp[i]) {
                burst(sc, C(255, 170, 90), 8, 150.0f, 0.45f, 3.0f, false);
                add_float({sc.x, sc.y - 10}, TextFormat("%d", a.monHp[i] - m.combat.hp), C(255, 236, 150), 28);
                if (i < a.monPunch.size()) a.monPunch[i] = 1.0f;
                if (m.combat.hp <= 0) burst(sc, code_color(m.color), 22, 230.0f, 0.7f, 3.5f, true);
                // player attacked an adjacent foe -> lunge toward it
                if (m.pos.chebyshev(g.player().pos) <= 1) {
                    const float dx = (float)(m.pos.x - g.player().pos.x);
                    const float dy = (float)(m.pos.y - g.player().pos.y);
                    const float len = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
                    a.playerLungeDir = {dx / len, dy / len};
                    a.playerLunge = 1.0f;
                }
            }
            a.monHp[i] = m.combat.hp;
        }
        const Vector2 psc = tile_center(a.playerR.x, a.playerR.y, camx, camy);
        if (g.player().combat.hp < a.prevHp) {
            const int dmg = a.prevHp - g.player().combat.hp;
            burst(psc, C(255, 90, 90), 12, 170.0f, 0.5f, 3.5f, false);
            add_float({psc.x, psc.y - 12}, TextFormat("-%d", dmg), C(255, 140, 140), 30);
            a.playerPunch = 1.0f;
            a.shake = std::min(12.0f, a.shake + 3.0f + dmg * 1.4f);
        }
        if (g.cash() > a.prevCash) {
            add_float({psc.x, psc.y - 6}, TextFormat("+$%d", g.cash() - a.prevCash), C(245, 210, 90), 22);
            burst(psc, C(245, 210, 90), 10, 120.0f, 0.6f, 3.0f, true);
        }
        if (g.level() > a.prevLevel) {
            add_float({psc.x, psc.y - 26}, "LEVEL UP!", C(140, 230, 150), 26);
            burst(psc, C(150, 240, 160), 36, 240.0f, 0.9f, 4.0f, false);
        }
        a.prevHp = g.player().combat.hp; a.prevCash = g.cash(); a.prevLevel = g.level();
    }
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

    a.shake = std::max(0.0f, a.shake - dt * 26.0f);
    a.playerPunch = std::max(0.0f, a.playerPunch - dt * 5.0f);
    a.playerLunge = std::max(0.0f, a.playerLunge - dt * 6.0f);
    for (auto& mp : a.monPunch) mp = std::max(0.0f, mp - dt * 5.0f);

    update_particles(dt);
}

// ---- terrain (textured) ----------------------------------------------------
double light_falloff(const Game& g, Vec2 w) {
    const double d = (double)w.chebyshev(g.player().pos);
    const double t01 = g.fov_radius() > 0 ? std::clamp(d / g.fov_radius(), 0.0, 1.0) : 0.0;
    return 1.0 - t01 * t01;
}

void draw_terrain(const Game& g, Vec2 w, int px, int py) {
    using sh::TileType;
    const auto& t = g.map().at(w);
    const Rectangle dest{(float)px, (float)py, (float)kTile, (float)kTile};

    if (t.type == TileType::Wall) {
        const float j = 0.85f + hash2(w.x, w.y) * 0.3f;
        const Rectangle src{0, 0, (float)gWallTex.width, (float)gWallTex.height};
        DrawTexturePro(gWallTex, src, dest, {0, 0}, 0, C((int)(168 * j), (int)(140 * j), (int)(104 * j)));
        DrawRectangle(px, py, kTile, 4, C(206, 184, 150)); // lit top edge
        const Vec2 below{w.x, w.y + 1};
        if (g.map().in_bounds(below) && g.map().at(below).type != TileType::Wall &&
            g.map().at(below).explored) {
            // taller, darker, cooler front face -> reads as real block height
            const int fh = 18;
            DrawRectangleGradientV(px, py + kTile - fh, kTile, fh, C(46, 38, 34), C(28, 24, 26));
            DrawRectangle(px, py + kTile - fh, kTile, 2, C(120, 100, 80)); // bright lip
        }
        return;
    }

    const float j = 0.9f + hash2(w.x * 3 + 1, w.y * 3 + 1) * 0.2f;
    const Rectangle src{0, 0, (float)gFloorTex.width, (float)gFloorTex.height};
    DrawTexturePro(gFloorTex, src, dest, {0, 0}, 0, C((int)(96 * j), (int)(92 * j), (int)(104 * j)));
    // Ambient occlusion from neighbouring walls.
    auto is_wall = [&](int dx, int dy) {
        const Vec2 n{w.x + dx, w.y + dy};
        return g.map().in_bounds(n) && g.map().at(n).type == TileType::Wall && g.map().at(n).explored;
    };
    if (is_wall(0, -1)) DrawRectangleGradientV(px, py, kTile, 12, Fade(BLACK, 0.5f), Fade(BLACK, 0));
    if (is_wall(-1, 0)) DrawRectangleGradientH(px, py, 9, kTile, Fade(BLACK, 0.32f), Fade(BLACK, 0));
    if (is_wall(1, 0)) DrawRectangleGradientH(px + kTile - 9, py, 9, kTile, Fade(BLACK, 0), Fade(BLACK, 0.32f));

    if (t.type == TileType::StairsDown) {
        for (int i = 0; i < 3; ++i) {
            const float yy = py + 11.0f + i * 8.0f;
            DrawTriangle({(float)px + 11, yy}, {(float)px + kTile - 11, yy},
                         {(float)(px + kTile / 2), yy + 7}, C(255, 224, 120));
        }
    }
}

// ---- entities (albedo shapes; lights are added separately) -----------------
void shadow(Vector2 c) { DrawEllipse((int)c.x, (int)(c.y + kTile * 0.30f), kTile * 0.30f, kTile * 0.12f, Fade(BLACK, 0.40f)); }
void eyes(Vector2 c, float dx, float dy, float r) {
    DrawCircleV({c.x - dx, c.y - dy}, r, WHITE);
    DrawCircleV({c.x + dx, c.y - dy}, r, WHITE);
    DrawCircleV({c.x - dx, c.y - dy}, r * 0.5f, C(20, 20, 30));
    DrawCircleV({c.x + dx, c.y - dy}, r * 0.5f, C(20, 20, 30));
}
void draw_monster(const sh::Entity& m, Vector2 c, float bob, float punch) {
    c.y += bob;
    const Color col = code_color(m.color), dark = shade(col, 0.5), lite = mix(col, WHITE, 0.35);
    shadow({c.x, c.y - bob});
    if (m.glyph == '&') {
        DrawPoly(c, 6, kTile * 0.42f, a_time_rot(), col);
        DrawPolyLines(c, 6, kTile * 0.42f, a_time_rot(), dark);
        DrawPoly(c, 6, kTile * 0.30f, a_time_rot(), shade(col, 0.7));
        DrawRectangle((int)c.x - 9, (int)(c.y - kTile * 0.46f), 18, 5, C(255, 215, 90));
        eyes(c, 5, 2, 3);
        if (punch > 0.0f) DrawCircleV(c, kTile * 0.44f, Fade(WHITE, 0.6f * punch));
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
            eyes(c, 4, 3, 2.5f); break;
        }
        case sh::MonsterKind::Client:
            DrawPoly(c, 4, kTile * 0.31f, 45.0f, col);
            DrawPoly(c, 4, kTile * 0.20f, 45.0f, lite);
            DrawPolyLines(c, 4, kTile * 0.31f, 45.0f, dark);
            eyes(c, 4, 1, 2.5f); break;
        case sh::MonsterKind::Recruiter: {
            Rectangle r{c.x - 12, c.y - 12, 24, 24};
            DrawRectangleRounded(r, 0.3f, 6, col);
            DrawRectangleRounded({c.x - 12, c.y - 12, 24, 11}, 0.3f, 6, lite);
            DrawRectangleRoundedLines(r, 0.3f, 6, dark);
            eyes(c, 5, 2, 2.5f); break;
        }
        case sh::MonsterKind::Manager: {
            Rectangle r{c.x - 14, c.y - 14, 28, 28};
            DrawRectangleRounded(r, 0.25f, 6, col);
            DrawRectangleRounded({c.x - 14, c.y - 14, 28, 12}, 0.25f, 6, lite);
            DrawRectangleRoundedLines(r, 0.25f, 6, dark);
            eyes(c, 6, 3, 3); break;
        }
    }
    if (punch > 0.0f) DrawCircleV(c, kTile * 0.34f, Fade(WHITE, 0.6f * punch));
}
void draw_item(const sh::Item& it, Vector2 c, float bob) {
    c.y += bob;
    switch (it.kind) {
        case sh::ItemKind::Cash:
            DrawCircleGradient((int)c.x, (int)c.y, kTile * 0.24f, C(255, 235, 150), C(225, 185, 70));
            DrawCircleLines((int)c.x, (int)c.y, kTile * 0.24f, C(150, 120, 40));
            dtc("$", c.x, c.y - 9, 18, C(120, 90, 20)); break;
        case sh::ItemKind::Coffee:
            DrawRectangleRounded({c.x - 9, c.y - 6, 16, 15}, 0.3f, 6, C(98, 208, 218));
            DrawRing({c.x + 9, c.y + 1}, 3, 6, 270, 90, 14, C(98, 208, 218));
            DrawLineEx({c.x - 4, c.y - 11}, {c.x - 4, c.y - 16}, 2, Fade(C(220, 235, 240), 0.8f));
            DrawLineEx({c.x + 2, c.y - 11}, {c.x + 2, c.y - 16}, 2, Fade(C(220, 235, 240), 0.8f)); break;
        case sh::ItemKind::Upgrade:
            DrawRectangleRounded({c.x - 12, c.y - 8, 24, 15}, 0.15f, 4, C(208, 214, 226));
            DrawRectangle((int)c.x - 14, (int)c.y + 7, 28, 3, C(150, 156, 168));
            DrawRectangleRounded({c.x - 9, c.y - 5, 18, 9}, 0.1f, 4, C(72, 132, 184)); break;
    }
}
void draw_player(Vector2 c, float bob, float punch, float lunge, Vector2 lunge_dir) {
    c.x += lunge_dir.x * lunge * 11.0f;
    c.y += lunge_dir.y * lunge * 11.0f + bob;
    shadow({c.x, c.y - bob});
    const float r = kTile * 0.35f;
    // Cool cyan hero: hue alone separates "me" from gold loot and warm enemies.
    DrawCircleGradient((int)c.x, (int)c.y, r, C(150, 240, 235), C(46, 168, 178));
    DrawCircleLines((int)c.x, (int)c.y, r, C(18, 86, 96));
    DrawCircleV({c.x, c.y + r * 0.05f}, r * 0.34f, C(28, 120, 132));
    eyes(c, 5, 2, 3);
    DrawCircleV({c.x - r * 0.34f, c.y - r * 0.40f}, r * 0.16f, Fade(WHITE, 0.9f));
    if (punch > 0.0f) DrawCircleV(c, r * 1.05f, Fade(WHITE, 0.55f * punch));
}

// ---- light buffer ----------------------------------------------------------
void add_light(Vector2 c, float radius_px, Color col, float intensity) {
    const float d = radius_px * 2.0f;
    DrawTexturePro(gLightTex, {0, 0, (float)gLightTex.width, (float)gLightTex.height},
                   {c.x - radius_px, c.y - radius_px, d, d}, {0, 0}, 0,
                   Fade(col, std::clamp(intensity, 0.0f, 1.0f)));
}

// ---- HUD -------------------------------------------------------------------
void bar(int x, int y, int w, int h, float frac, Color a, Color b, Color back) {
    frac = std::clamp(frac, 0.0f, 1.0f);
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.5f, 8, back);
    const int fw = std::max(0, (int)(w * frac)); // empty reads empty; proportional fill
    if (fw > h / 2) {
        DrawRectangleRounded({(float)x, (float)y, (float)fw, (float)h}, 0.5f, 8, a);
        DrawRectangleGradientV(x + h / 2, y + 1, fw - h / 2, h - 2, Fade(a, 0.0f), Fade(shade(b, 0.7), 0.6f));
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
    dtsh("SIDE HUSTLE", ix, y, 30, C(255, 226, 120)); y += 46;
    const auto& p = g.player();
    dt("HEALTH", ix, y, 16, C(150, 162, 190)); y += 22;
    bar(ix, y, w - 44, 20, a.hpShown, hp_color(a.hpShown), shade(hp_color(a.hpShown), 0.6), C(40, 44, 60));
    dtcsh(TextFormat("%d / %d", std::max(0, p.combat.hp), p.combat.max_hp), ix + (w - 44) / 2, y + 3, 16,
          C(245, 248, 252), C(0, 0, 0, 205));
    y += 34;
    dt(TextFormat("LEVEL %d", g.level()), ix, y, 16, C(150, 162, 190)); y += 22;
    bar(ix, y, w - 44, 12, a.xpShown, C(120, 170, 235), C(70, 110, 180), C(40, 44, 60)); y += 30;
    auto stat = [&](const char* label, const char* val, Color vc) {
        dt(label, ix, y, 19, C(170, 178, 198));
        dt(val, ix + 132, y, 19, vc); y += 28;
    };
    stat("Floor", TextFormat("%d / 8", g.depth()), C(232, 236, 246));
    stat("Cash", TextFormat("$%d", g.cash()), C(238, 205, 84));
    stat("Coffee", TextFormat("x%d  (E)", g.coffees()), C(98, 208, 218));
    stat("Attack", TextFormat("%d", p.combat.attack), C(232, 236, 246));
    stat("Defense", TextFormat("%d", p.combat.defense), C(232, 236, 246));
    y += 8;
    DrawLine(ix, y, x + w - 22, y, C(54, 58, 78)); y += 14;
    dt("LOG", ix, y, 16, C(150, 162, 190)); y += 24;
    const auto& msgs = g.messages();
    const int total = (int)msgs.size();
    const int max_w = (x + w - 22) - ix;
    const int first = std::max(0, total - 8);
    // The log only changes when a turn is consumed; cache the wrapping instead
    // of re-wrapping (and re-measuring) every frame.
    static int cached_count = -1, cached_w = -1;
    static std::vector<std::vector<std::string>> cache;
    if (total != cached_count || max_w != cached_w) {
        cache.clear();
        for (int i = first; i < total; ++i) cache.push_back(wrap_text(msgs[(std::size_t)i], max_w, 15));
        cached_count = total; cached_w = max_w;
    }
    int drawn = 0;
    for (std::size_t mi = 0; mi < cache.size() && drawn < 12; ++mi) {
        const bool latest = (first + (int)mi == total - 1);
        const Color col = latest ? C(220, 226, 238) : C(140, 148, 168);
        for (const auto& ln : cache[mi]) {
            if (drawn >= 12) break;
            dt(ln.c_str(), ix, y, 15, col); y += 19; ++drawn;
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
    dtc("Top side hustles", cx, y, 24, C(150, 162, 190)); y += 36;
    const auto& e = hs.entries();
    if (e.empty()) { dtc("(none yet - be the first)", cx, y, 19, C(100, 108, 130)); return; }
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

// ---- procedural textures & render targets ----------------------------------
// Derive a tangent-space normal map from a grayscale height image (Sobel-ish
// central differences, wrapping at the edges so it stays tileable).
Texture2D height_to_normal(const Image& src, float strength) {
    Color* px = LoadImageColors(src);
    const int w = src.width, h = src.height;
    auto lum = [&](int x, int y) {
        x = ((x % w) + w) % w; y = ((y % h) + h) % h;
        const Color c = px[y * w + x];
        return (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) / 255.0f;
    };
    Image out = GenImageColor(w, h, C(128, 128, 255));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float dx = (lum(x - 1, y) - lum(x + 1, y)) * strength;
            const float dy = (lum(x, y - 1) - lum(x, y + 1)) * strength;
            float nx = dx, ny = dy, nz = 1.0f;
            const float il = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
            nx *= il; ny *= il; nz *= il;
            ImageDrawPixel(&out, x, y, C((int)((nx * 0.5f + 0.5f) * 255),
                                         (int)((ny * 0.5f + 0.5f) * 255),
                                         (int)((nz * 0.5f + 0.5f) * 255)));
        }
    UnloadImageColors(px);
    Texture2D t = LoadTextureFromImage(out);
    SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
    UnloadImage(out);
    return t;
}

void make_textures() {
    Image f = GenImageCellular(128, 128, 11);   // stone tiling for floors
    ImageColorBrightness(&f, 40);
    gFloorTex = LoadTextureFromImage(f);
    SetTextureFilter(gFloorTex, TEXTURE_FILTER_BILINEAR);
    gFloorNrm = height_to_normal(f, 2.4f);
    UnloadImage(f);

    Image w = GenImagePerlinNoise(128, 128, 0, 0, 5.0f); // rough rock for walls
    ImageColorBrightness(&w, 30);
    ImageColorContrast(&w, 25);
    gWallTex = LoadTextureFromImage(w);
    SetTextureFilter(gWallTex, TEXTURE_FILTER_BILINEAR);
    gWallNrm = height_to_normal(w, 3.6f);
    UnloadImage(w);

    Image l = GenImageGradientRadial(192, 192, 0.10f, WHITE, C(255, 255, 255, 0));
    gLightTex = LoadTextureFromImage(l);
    SetTextureFilter(gLightTex, TEXTURE_FILTER_BILINEAR);
    UnloadImage(l);
}
void ensure_targets(int w, int h) {
    if (w == gRtW && h == gRtH) return;
    if (gRtW != 0) {
        UnloadRenderTexture(gScene); UnloadRenderTexture(gLight); UnloadRenderTexture(gLit);
        UnloadRenderTexture(gBloomA); UnloadRenderTexture(gBloomB); UnloadRenderTexture(gComposed);
        UnloadRenderTexture(gNormal);
    }
    gScene = LoadRenderTexture(w, h);
    gLight = LoadRenderTexture(w, h);
    gLit = LoadRenderTexture(w, h);
    gComposed = LoadRenderTexture(w, h);
    gNormal = LoadRenderTexture(w, h);
    SetTextureFilter(gNormal.texture, TEXTURE_FILTER_BILINEAR);
    gBloomA = LoadRenderTexture(w / kBloomDiv, h / kBloomDiv);
    gBloomB = LoadRenderTexture(w / kBloomDiv, h / kBloomDiv);
    SetTextureFilter(gLight.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(gComposed.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(gBloomA.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(gBloomB.texture, TEXTURE_FILTER_BILINEAR);
    gRtW = w; gRtH = h;
}

} // namespace

int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(1280, 768, "SIDE HUSTLE");
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);

    gFont = LoadFontFromMemory(".ttf", kUiFontTTF, (int)kUiFontTTF_len, 64, nullptr, 0);
    GenTextureMipmaps(&gFont.texture); // mip levels so small text doesn't shimmer
    SetTextureFilter(gFont.texture, TEXTURE_FILTER_TRILINEAR);
    make_textures();
    gComposite = LoadShaderFromMemory(nullptr, kCompositeFS);
    gBlur = LoadShaderFromMemory(nullptr, kBlurFS);
    gPost = LoadShaderFromMemory(nullptr, kPostFS);
    gLocLightTex = GetShaderLocation(gComposite, "lightTex");
    gLocAmbient = GetShaderLocation(gComposite, "ambient");
    gLocNormalTex = GetShaderLocation(gComposite, "normalTex");
    gLocCompRes = GetShaderLocation(gComposite, "res");
    gLocLightPos = GetShaderLocation(gComposite, "lightPos");
    gLocLightRad = GetShaderLocation(gComposite, "lightRad");
    gLocLightInt = GetShaderLocation(gComposite, "lightInt");
    gLocLightCount = GetShaderLocation(gComposite, "lightCount");
    gLocDir = GetShaderLocation(gBlur, "dir");
    gLocPostRes = GetShaderLocation(gPost, "res");
    gLocPostTime = GetShaderLocation(gPost, "time");

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
    float move_timer = 0.0f, flash = 0.0f;

    auto start_game = [&]() {
        sh::Config cfg;
        cfg.seed = (std::uint64_t)std::time(nullptr);
        cfg.difficulty = diffs[diff_index];
        cfg.scores_path = scores_path;
        const char* user = std::getenv("USERNAME");
        if (user == nullptr || *user == '\0') user = std::getenv("USER");
        if (user != nullptr && *user != '\0') cfg.player_name = user;
        game = std::make_unique<Game>(std::move(cfg));
        anim = Anim{}; flash = 0.0f; move_timer = 0.0f; phase = Phase::Playing;
    };

    int smoke_frames = 0;
    if (const char* s = std::getenv("SH_SMOKE")) smoke_frames = std::atoi(s);
    int frame = 0;
    bool combat_shot = false;

    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();
        const int W = GetScreenWidth(), H = GetScreenHeight();
        ++frame;
        a_time_rot_v += dt * 18.0f;
        bool quit = false;

        if (smoke_frames > 0) {
            if (frame == 2 && phase == Phase::Title) start_game();
            else if (phase == Phase::Playing && game) {
                if (frame % 3 == 0) {
                    const Vec2 pp = game->player().pos;
                    Game::Command c = Game::Command::Wait;
                    if (pp == game->stairs()) c = Game::Command::Descend;
                    else if (auto s = sh::next_step_towards(game->map(), pp, game->stairs(), {})) {
                        c = dir_to_cmd({(s->x > pp.x) - (s->x < pp.x), (s->y > pp.y) - (s->y < pp.y)});
                    }
                    game->advance(c);
                }
                if (!game->is_playing()) { board.load(); phase = Phase::End; }
            }
            if (frame >= smoke_frames) quit = true;
        }

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

        const int mapAreaW = W - kHudW;
        ensure_targets(mapAreaW, H);

        // ===================================================================
        // World rendering (only when a game exists)
        // ===================================================================
        float camx = 0, camy = 0, colsF = 0, rowsF = 0;
        if (game) {
            colsF = mapAreaW / (float)kTile;
            rowsF = H / (float)kTile;
            camx = std::clamp(anim.playerR.x + 0.5f - colsF / 2.0f, 0.0f,
                              std::max(0.0f, (float)game->map().width() - colsF));
            camy = std::clamp(anim.playerR.y + 0.5f - rowsF / 2.0f, 0.0f,
                              std::max(0.0f, (float)game->map().height() - rowsF));
            sync_fx(*game, anim, dt, camx, camy);

            const int x0 = (int)camx - 1, x1 = (int)(camx + colsF) + 2;
            const int y0 = (int)camy - 1, y1 = (int)(camy + rowsF) + 2;
            auto sx = [&](float wx) { return (int)lroundf((wx - camx) * kTile); };
            auto sy = [&](float wy) { return (int)lroundf((wy - camy) * kTile); };

            // ---- Pass 1: scene albedo ----
            BeginTextureMode(gScene);
            ClearBackground(BLACK);
            for (int wy = y0; wy <= y1; ++wy)
                for (int wx = x0; wx <= x1; ++wx) {
                    const Vec2 w{wx, wy};
                    if (!game->map().in_bounds(w) || !game->map().at(w).explored) continue;
                    draw_terrain(*game, w, sx(wx), sy(wy));
                }
            for (const auto& it : game->items()) {
                if (it.taken || !game->map().in_bounds(it.pos) || !game->map().at(it.pos).visible) continue;
                draw_item(it, tile_center(it.pos.x, it.pos.y, camx, camy),
                          std::sin(anim.time * 3.0f + it.pos.x * 0.7f) * 2.0f);
            }
            const auto& mons = game->monsters();
            for (std::size_t i = 0; i < mons.size(); ++i) {
                const auto& m = mons[i];
                if (!m.alive || !game->map().in_bounds(m.pos) || !game->map().at(m.pos).visible) continue;
                const Vector2 c = tile_center(anim.monR[i].x, anim.monR[i].y, camx, camy);
                draw_monster(m, c, std::sin(anim.time * 3.5f + i * 1.3f) * 1.6f,
                             i < anim.monPunch.size() ? anim.monPunch[i] : 0.0f);
                if (m.combat.hp < m.combat.max_hp) {
                    const float fr = std::clamp((float)m.combat.hp / std::max(1, m.combat.max_hp), 0.0f, 1.0f);
                    DrawRectangle((int)c.x - 14, (int)c.y - 22, 28, 4, C(35, 12, 12));
                    DrawRectangle((int)c.x - 14, (int)c.y - 22, (int)(28 * fr), 4, C(225, 70, 70));
                }
            }
            // (player drawn on top after compositing so its hue stays crisp)
            EndTextureMode();

            // ---- Pass 1b: surface normals (for per-pixel relief lighting) ----
            BeginTextureMode(gNormal);
            ClearBackground(C(128, 128, 255)); // flat normal (+Z)
            for (int wy = y0; wy <= y1; ++wy)
                for (int wx = x0; wx <= x1; ++wx) {
                    const Vec2 w{wx, wy};
                    if (!game->map().in_bounds(w) || !game->map().at(w).explored) continue;
                    const Texture2D& nrm = game->map().at(w).type == sh::TileType::Wall ? gWallNrm : gFloorNrm;
                    DrawTexturePro(nrm, {0, 0, (float)nrm.width, (float)nrm.height},
                                   {(float)sx(wx), (float)sy(wy), (float)kTile, (float)kTile}, {0, 0}, 0, WHITE);
                }
            // flat normals under entities so they aren't lit by the floor's relief
            for (const auto& it : game->items())
                if (!it.taken && game->map().in_bounds(it.pos) && game->map().at(it.pos).visible)
                    DrawCircleV(tile_center(it.pos.x, it.pos.y, camx, camy), kTile * 0.32f, C(128, 128, 255));
            for (std::size_t i = 0; i < game->monsters().size(); ++i) {
                const auto& m = game->monsters()[i];
                if (m.alive && game->map().in_bounds(m.pos) && game->map().at(m.pos).visible)
                    DrawCircleV(tile_center(anim.monR[i].x, anim.monR[i].y, camx, camy), kTile * 0.34f, C(128, 128, 255));
            }
            EndTextureMode();

            // ---- Pass 2: light buffer (FOV-accurate: one soft light per visible tile) ----
            const float flick = 0.86f + 0.14f * std::sin(anim.time * 11.0f) * std::sin(anim.time * 6.3f);
            BeginTextureMode(gLight);
            ClearBackground(BLACK);
            BeginBlendMode(BLEND_ADDITIVE);
            for (int wy = y0; wy <= y1; ++wy)
                for (int wx = x0; wx <= x1; ++wx) {
                    const Vec2 w{wx, wy};
                    if (!game->map().in_bounds(w) || !game->map().at(w).visible) continue;
                    const float fo = (float)light_falloff(*game, w);
                    const Vector2 c = tile_center(wx, wy, camx, camy);
                    add_light(c, kTile * 0.95f, C(255, 216, 156), (0.14f + 0.26f * fo) * flick);
                }
            // point lights
            for (const auto& it : game->items()) {
                if (it.taken || !game->map().at(it.pos).visible) continue;
                const Color lc = it.kind == sh::ItemKind::Cash ? C(255, 210, 90)
                               : it.kind == sh::ItemKind::Coffee ? C(120, 220, 230) : C(200, 215, 235);
                add_light(tile_center(it.pos.x, it.pos.y, camx, camy), kTile * 0.9f, lc, 0.32f);
            }
            if (game->map().in_bounds(game->stairs()) && game->map().at(game->stairs()).visible)
                add_light(tile_center(game->stairs().x, game->stairs().y, camx, camy), kTile * 1.3f, C(255, 220, 110), 0.45f);
            for (const auto& m : mons) {
                if (m.alive && m.glyph == '&' && game->map().at(m.pos).visible)
                    add_light(tile_center(m.pos.x, m.pos.y, camx, camy), kTile * 1.5f, code_color(m.color), 0.45f);
            }
            // player torch
            add_light(tile_center(anim.playerR.x, anim.playerR.y, camx, camy), kTile * 2.2f * flick, C(255, 198, 128), 0.42f);
            // particle sparks emit light
            for (const auto& p : gParticles) {
                const float a = std::clamp(p.life / p.max, 0.0f, 1.0f);
                add_light(p.pos, kTile * 0.55f, p.col, 0.35f * a);
            }
            EndBlendMode();
            EndTextureMode();

            // ---- Pass 3: composite (albedo * (ambient + normal-mapped light) + grade) ----
            // Gather up to 8 directional lights for relief shading (torch + nearest points).
            struct Lt { Vector2 p; float rad, inten, z; };
            std::vector<Lt> lts;
            const Vector2 ptc = tile_center(anim.playerR.x, anim.playerR.y, camx, camy);
            lts.push_back({ptc, kTile * 5.5f, 1.15f, kTile * 1.25f}); // torch
            for (const auto& it : game->items())
                if (!it.taken && game->map().at(it.pos).visible)
                    lts.push_back({tile_center(it.pos.x, it.pos.y, camx, camy), kTile * 2.4f, 0.5f, kTile * 0.8f});
            if (game->map().in_bounds(game->stairs()) && game->map().at(game->stairs()).visible)
                lts.push_back({tile_center(game->stairs().x, game->stairs().y, camx, camy), kTile * 3.0f, 0.55f, kTile * 0.8f});
            for (const auto& m : mons)
                if (m.alive && m.glyph == '&' && game->map().at(m.pos).visible)
                    lts.push_back({tile_center(m.pos.x, m.pos.y, camx, camy), kTile * 3.5f, 0.6f, kTile * 0.9f});
            if (lts.size() > 8)
                std::partial_sort(lts.begin() + 1, lts.begin() + 8, lts.end(), [&](const Lt& a, const Lt& b) {
                    auto d2 = [&](const Lt& l) { return (l.p.x - ptc.x) * (l.p.x - ptc.x) + (l.p.y - ptc.y) * (l.p.y - ptc.y); };
                    return d2(a) < d2(b);
                });
            const int nL = std::min<int>(8, (int)lts.size());
            float lp[24] = {0}, lr[8] = {0}, li[8] = {0};
            for (int i = 0; i < nL; ++i) {
                lp[i * 3] = lts[i].p.x; lp[i * 3 + 1] = lts[i].p.y; lp[i * 3 + 2] = lts[i].z;
                lr[i] = lts[i].rad; li[i] = lts[i].inten;
            }
            const float ambient[3] = {0.16f, 0.19f, 0.30f};
            const float compRes[2] = {(float)mapAreaW, (float)H};
            BeginTextureMode(gLit);
            BeginShaderMode(gComposite);
            SetShaderValueTexture(gComposite, gLocLightTex, gLight.texture);
            SetShaderValueTexture(gComposite, gLocNormalTex, gNormal.texture);
            SetShaderValue(gComposite, gLocAmbient, ambient, SHADER_UNIFORM_VEC3);
            SetShaderValue(gComposite, gLocCompRes, compRes, SHADER_UNIFORM_VEC2);
            SetShaderValueV(gComposite, gLocLightPos, lp, SHADER_UNIFORM_VEC3, nL);
            SetShaderValueV(gComposite, gLocLightRad, lr, SHADER_UNIFORM_FLOAT, nL);
            SetShaderValueV(gComposite, gLocLightInt, li, SHADER_UNIFORM_FLOAT, nL);
            SetShaderValue(gComposite, gLocLightCount, &nL, SHADER_UNIFORM_INT);
            DrawTexture(gScene.texture, 0, 0, WHITE);
            EndShaderMode();
            EndTextureMode();

            // ---- Pass 4: bloom (downsample light -> H blur -> V blur) ----
            const float bw = (float)gBloomA.texture.width, bh = (float)gBloomA.texture.height;
            BeginTextureMode(gBloomA);
            ClearBackground(BLACK);
            // Bloom the lit scene (black in the void) so glow never floats in empty space.
            DrawTexturePro(gLit.texture, {0, 0, (float)mapAreaW, -(float)H}, {0, 0, bw, bh}, {0, 0}, 0, WHITE);
            EndTextureMode();
            const float dh[2] = {1.0f / bw, 0.0f}, dv[2] = {0.0f, 1.0f / bh};
            BeginTextureMode(gBloomB);
            BeginShaderMode(gBlur);
            SetShaderValue(gBlur, gLocDir, dh, SHADER_UNIFORM_VEC2);
            DrawTexture(gBloomA.texture, 0, 0, WHITE);
            EndShaderMode();
            EndTextureMode();
            BeginTextureMode(gBloomA);
            BeginShaderMode(gBlur);
            SetShaderValue(gBlur, gLocDir, dv, SHADER_UNIFORM_VEC2);
            DrawTexture(gBloomB.texture, 0, 0, WHITE);
            EndShaderMode();
            EndTextureMode();

            // ---- Pass 5: compose lit + bloom into one buffer for post-processing ----
            // (inner blits use positive height; the single outer flip at present time
            //  gives correct screen orientation.)
            BeginTextureMode(gComposed);
            ClearBackground(BLACK);
            DrawTextureRec(gLit.texture, {0, 0, (float)mapAreaW, (float)H}, {0, 0}, WHITE);
            BeginBlendMode(BLEND_ADDITIVE);
            DrawTexturePro(gBloomA.texture, {0, 0, bw, bh}, {0, 0, (float)mapAreaW, (float)H}, {0, 0}, 0, Fade(WHITE, 0.34f));
            EndBlendMode();
            EndTextureMode();
        }

        // ===================================================================
        // Present
        // ===================================================================
        BeginDrawing();
        ClearBackground(C(10, 11, 16));

        if (phase == Phase::Title) {
            const int cx = W / 2;
            const float ty = H / 2 - 220;
            const float titleW = tw("SIDE HUSTLE", 92);
            // warm additive halo behind the wordmark
            BeginBlendMode(BLEND_ADDITIVE);
            for (int gi = 0; gi < 6; ++gi) {
                const float ang = gi * 1.0472f;
                dtc("SIDE HUSTLE", cx + std::cos(ang) * 3.0f, ty + std::sin(ang) * 3.0f, 92, Fade(C(255, 150, 40), 0.10f));
            }
            EndBlendMode();
            dtcsh("SIDE HUSTLE", cx, ty, 92, C(255, 224, 130));
            DrawRectangle((int)(cx - titleW / 2), (int)(ty + 96), (int)titleW, 3, C(255, 196, 90));
            dtc("A corporate dungeon crawl - modern C++ + raylib shaders", cx, H / 2 - 112, 22, C(150, 160, 185));
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
            const float sx = (frand() * 2.0f - 1.0f) * anim.shake;
            const float sy = (frand() * 2.0f - 1.0f) * anim.shake;
            // CRT post-process pass over the lit+bloom dungeon
            const float pres[2] = {(float)mapAreaW, (float)H};
            BeginShaderMode(gPost);
            SetShaderValue(gPost, gLocPostRes, pres, SHADER_UNIFORM_VEC2);
            SetShaderValue(gPost, gLocPostTime, &anim.time, SHADER_UNIFORM_FLOAT);
            DrawTextureRec(gComposed.texture, {0, 0, (float)mapAreaW, -(float)H}, {sx, sy}, WHITE);
            EndShaderMode();
            // hero + emissive FX drawn crisp on top, shifted with the shake
            BeginScissorMode(0, 0, mapAreaW, H);
            rlPushMatrix();
            rlTranslatef(sx, sy, 0);
            draw_player(tile_center(anim.playerR.x, anim.playerR.y, camx, camy), std::sin(anim.time * 3.0f) * 1.4f,
                        anim.playerPunch, anim.playerLunge, anim.playerLungeDir);
            draw_particles();
            draw_floats();
            rlPopMatrix();
            EndScissorMode();
            if (flash > 0.0f) DrawRectangle(0, 0, mapAreaW, H, Fade(C(200, 30, 30), 0.35f * flash));

            draw_hud(*game, anim, mapAreaW, kHudW, H);

            if (phase == Phase::End) {
                DrawRectangle(0, 0, W, H, Fade(BLACK, 0.74f));
                const int cx = W / 2;
                if (game->is_won()) {
                    dtcsh("YOU WIN", cx, H / 2 - 160, 88, C(110, 225, 120));
                    dtc("You escaped and went full-time on your side hustle!", cx, H / 2 - 66, 22, C(200, 206, 220));
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
                TakeScreenshot("sh_combat.png"); combat_shot = true;
            }
            if (frame == smoke_frames - 1) TakeScreenshot("sh_final.png");
        }
        if (quit) break;
    }

    if (gRtW != 0) {
        UnloadRenderTexture(gScene); UnloadRenderTexture(gLight); UnloadRenderTexture(gLit);
        UnloadRenderTexture(gBloomA); UnloadRenderTexture(gBloomB); UnloadRenderTexture(gComposed);
        UnloadRenderTexture(gNormal);
    }
    UnloadShader(gComposite); UnloadShader(gBlur); UnloadShader(gPost);
    UnloadTexture(gFloorTex); UnloadTexture(gWallTex); UnloadTexture(gLightTex);
    UnloadTexture(gFloorNrm); UnloadTexture(gWallNrm);
    UnloadFont(gFont);
    CloseWindow();
    return 0;
}

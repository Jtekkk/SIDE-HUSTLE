#pragma once
//
// Controls — remappable input bindings for the graphical front-end. Every
// gameplay action carries one keyboard binding and one gamepad binding, either
// of which the player can rebind from the in-game Controls menu and which
// persist to a small text file. Diagonal movement is synthesised from
// simultaneous cardinal presses, and the left analog stick always drives 8-way
// movement, so those aren't separate bindings.
//
// Header-only and self-contained (depends only on raylib), so the GUI can drop
// it in without an extra translation unit.
//
#include <array>
#include <cstdio>
#include <string>

#include "raylib.h"

namespace ctrl {

// Every rebindable action. Movement is four cardinals (diagonals come from
// pressing two at once / the analog stick); Shove and Dash are held modifiers.
enum class Act {
    MoveUp, MoveDown, MoveLeft, MoveRight,
    Shove, Dash, Slam, Coffee, Wait, Descend, Mute, Quit,
    Count
};
inline constexpr int kCount = static_cast<int>(Act::Count);

struct Binding {
    int key;  // raylib KeyboardKey, 0 = unbound
    int pad;  // raylib GamepadButton, 0 (UNKNOWN) = unbound
};

struct Map {
    std::array<Binding, kCount> b{};
    Binding& operator[](Act a) { return b[static_cast<int>(a)]; }
    const Binding& operator[](Act a) const { return b[static_cast<int>(a)]; }
};

inline const char* action_name(Act a) {
    switch (a) {
        case Act::MoveUp:    return "Move Up";
        case Act::MoveDown:  return "Move Down";
        case Act::MoveLeft:  return "Move Left";
        case Act::MoveRight: return "Move Right";
        case Act::Shove:     return "Shove (hold)";
        case Act::Dash:      return "Dash (hold)";
        case Act::Slam:      return "Slam";
        case Act::Coffee:    return "Drink Coffee";
        case Act::Wait:      return "Wait";
        case Act::Descend:   return "Descend";
        case Act::Mute:      return "Mute Music";
        case Act::Quit:      return "Quit";
        default:             return "?";
    }
}

// The stock scheme: WASD + Shift/Ctrl on the keyboard, Xbox-style dpad + face
// buttons + bumpers on the gamepad.
inline Map defaults() {
    Map m{};
    m[Act::MoveUp]    = {KEY_W, GAMEPAD_BUTTON_LEFT_FACE_UP};
    m[Act::MoveDown]  = {KEY_S, GAMEPAD_BUTTON_LEFT_FACE_DOWN};
    m[Act::MoveLeft]  = {KEY_A, GAMEPAD_BUTTON_LEFT_FACE_LEFT};
    m[Act::MoveRight] = {KEY_D, GAMEPAD_BUTTON_LEFT_FACE_RIGHT};
    m[Act::Shove]     = {KEY_LEFT_SHIFT, GAMEPAD_BUTTON_LEFT_TRIGGER_1};   // LB
    m[Act::Dash]      = {KEY_LEFT_CONTROL, GAMEPAD_BUTTON_RIGHT_TRIGGER_1}; // RB
    m[Act::Slam]      = {KEY_X, GAMEPAD_BUTTON_RIGHT_FACE_UP};    // Y
    m[Act::Coffee]    = {KEY_E, GAMEPAD_BUTTON_RIGHT_FACE_LEFT};  // X
    m[Act::Wait]      = {KEY_PERIOD, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT}; // B
    m[Act::Descend]   = {KEY_ENTER, GAMEPAD_BUTTON_RIGHT_FACE_DOWN};   // A
    m[Act::Mute]      = {KEY_M, GAMEPAD_BUTTON_MIDDLE_LEFT};   // View
    m[Act::Quit]      = {KEY_Q, GAMEPAD_BUTTON_MIDDLE_RIGHT};  // Menu
    return m;
}

// ---- input queries (keyboard OR gamepad 0) ---------------------------------
inline bool key_down(int k) { return k > 0 && IsKeyDown(k); }
inline bool key_pressed(int k) { return k > 0 && IsKeyPressed(k); }
inline bool pad_down(int btn) {
    return btn > 0 && IsGamepadAvailable(0) && IsGamepadButtonDown(0, btn);
}
inline bool pad_pressed(int btn) {
    return btn > 0 && IsGamepadAvailable(0) && IsGamepadButtonPressed(0, btn);
}

inline bool down(const Map& m, Act a) { return key_down(m[a].key) || pad_down(m[a].pad); }
inline bool pressed(const Map& m, Act a) { return key_pressed(m[a].key) || pad_pressed(m[a].pad); }

// ---- human-readable labels --------------------------------------------------
inline std::string key_name(int k) {
    if (k <= 0) return "--";
    switch (k) {
        case KEY_SPACE:         return "Space";
        case KEY_ENTER:         return "Enter";
        case KEY_KP_ENTER:      return "KpEnter";
        case KEY_TAB:           return "Tab";
        case KEY_BACKSPACE:     return "Bksp";
        case KEY_ESCAPE:        return "Esc";
        case KEY_RIGHT:         return "Right";
        case KEY_LEFT:          return "Left";
        case KEY_DOWN:          return "Down";
        case KEY_UP:            return "Up";
        case KEY_LEFT_SHIFT:    return "L.Shift";
        case KEY_RIGHT_SHIFT:   return "R.Shift";
        case KEY_LEFT_CONTROL:  return "L.Ctrl";
        case KEY_RIGHT_CONTROL: return "R.Ctrl";
        case KEY_LEFT_ALT:      return "L.Alt";
        case KEY_RIGHT_ALT:     return "R.Alt";
        case KEY_PERIOD:        return ".";
        case KEY_COMMA:         return ",";
        case KEY_SEMICOLON:     return ";";
        case KEY_SLASH:         return "/";
        default: break;
    }
    if (k >= 32 && k < 127) {
        const char c = static_cast<char>(k);
        return std::string(1, c);
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "Key%d", k);
    return buf;
}

inline std::string pad_name(int btn) {
    switch (btn) {
        case GAMEPAD_BUTTON_LEFT_FACE_UP:     return "DPad Up";
        case GAMEPAD_BUTTON_LEFT_FACE_DOWN:   return "DPad Down";
        case GAMEPAD_BUTTON_LEFT_FACE_LEFT:   return "DPad Left";
        case GAMEPAD_BUTTON_LEFT_FACE_RIGHT:  return "DPad Right";
        case GAMEPAD_BUTTON_RIGHT_FACE_UP:    return "Y";
        case GAMEPAD_BUTTON_RIGHT_FACE_RIGHT: return "B";
        case GAMEPAD_BUTTON_RIGHT_FACE_DOWN:  return "A";
        case GAMEPAD_BUTTON_RIGHT_FACE_LEFT:  return "X";
        case GAMEPAD_BUTTON_LEFT_TRIGGER_1:   return "LB";
        case GAMEPAD_BUTTON_LEFT_TRIGGER_2:   return "LT";
        case GAMEPAD_BUTTON_RIGHT_TRIGGER_1:  return "RB";
        case GAMEPAD_BUTTON_RIGHT_TRIGGER_2:  return "RT";
        case GAMEPAD_BUTTON_MIDDLE_LEFT:      return "View";
        case GAMEPAD_BUTTON_MIDDLE:           return "Guide";
        case GAMEPAD_BUTTON_MIDDLE_RIGHT:     return "Menu";
        case GAMEPAD_BUTTON_LEFT_THUMB:       return "L3";
        case GAMEPAD_BUTTON_RIGHT_THUMB:      return "R3";
        default:                              return "--";
    }
}

// Poll for any just-pressed gamepad button (so the rebind menu can capture it).
inline int any_pad_pressed() {
    if (!IsGamepadAvailable(0)) return 0;
    for (int b = GAMEPAD_BUTTON_LEFT_FACE_UP; b <= GAMEPAD_BUTTON_RIGHT_THUMB; ++b)
        if (IsGamepadButtonPressed(0, b)) return b;
    return 0;
}

// ---- persistence ------------------------------------------------------------
inline void save(const Map& m, const char* path) {
    std::FILE* f = std::fopen(path, "w");
    if (!f) return;
    std::fprintf(f, "# SIDE HUSTLE controls: <action-index> <key> <pad>\n");
    for (int i = 0; i < kCount; ++i)
        std::fprintf(f, "%d %d %d\n", i, m.b[i].key, m.b[i].pad);
    std::fclose(f);
}

inline bool load(Map& m, const char* path) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char line[128];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        int i = -1, key = 0, pad = 0;
        if (std::sscanf(line, "%d %d %d", &i, &key, &pad) == 3 && i >= 0 && i < kCount) {
            m.b[i].key = key;
            m.b[i].pad = pad;
        }
    }
    std::fclose(f);
    return true;
}

} // namespace ctrl

#pragma once
//
// Terminal — RAII guard around the console in raw mode, on both POSIX and
// Windows. The constructor switches the console into raw, no-echo mode and
// enables ANSI escape handling; the destructor *always* restores the original
// state, even on an early/abnormal exit, so the user's shell is never left
// broken. Only this file is platform-specific — the rest of the game is pure
// standard C++.
//
#include <string_view>

namespace sh {

// Synthetic key codes for arrow keys, placed well above the byte range so they
// never collide with real characters returned by read_key().
enum Key : int {
    KeyUp = 1000,
    KeyDown,
    KeyLeft,
    KeyRight,
};

} // namespace sh

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Windows Console implementation
// ---------------------------------------------------------------------------
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX // keep windows.h from clobbering std::min/std::max
#endif
#include <conio.h>
#include <windows.h>

namespace sh {

class Terminal {
public:
    Terminal() {
        h_out_ = ::GetStdHandle(STD_OUTPUT_HANDLE);
        h_in_ = ::GetStdHandle(STD_INPUT_HANDLE);

        if (::GetConsoleMode(h_out_, &orig_out_)) {
            has_out_ = true;
            ::SetConsoleMode(h_out_, orig_out_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                         ENABLE_PROCESSED_OUTPUT);
        }
        if (::GetConsoleMode(h_in_, &orig_in_)) {
            has_in_ = true;
            ::SetConsoleMode(h_in_, orig_in_ & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT |
                                                 ENABLE_PROCESSED_INPUT));
        }
        write_raw("\x1b[?25l\x1b[2J\x1b[H"); // hide cursor, clear, home
    }

    ~Terminal() { restore(); }

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    Terminal(Terminal&&) = delete;
    Terminal& operator=(Terminal&&) = delete;

    [[nodiscard]] int read_key() const {
        int c = ::_getch();
        if (c == 0 || c == 224) { // extended key (arrows etc.) -> read the code
            switch (::_getch()) {
                case 72: return KeyUp;
                case 80: return KeyDown;
                case 75: return KeyLeft;
                case 77: return KeyRight;
                default: return 0x1b;
            }
        }
        return c;
    }

    static void present(std::string_view frame) {
        write_raw("\x1b[H");
        write_raw(frame);
    }

private:
    void restore() {
        if (restored_) return;
        write_raw("\x1b[0m\x1b[?25h\x1b[2J\x1b[H");
        if (has_out_) ::SetConsoleMode(h_out_, orig_out_);
        if (has_in_) ::SetConsoleMode(h_in_, orig_in_);
        restored_ = true;
    }

    static void write_raw(std::string_view s) {
        HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        // WriteConsoleA only works on a real console handle; if stdout is
        // redirected to a file or pipe, fall back to a raw byte write.
        if (!::WriteConsoleA(h, s.data(), static_cast<DWORD>(s.size()), &written, nullptr)) {
            ::WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
        }
    }

    HANDLE h_out_{};
    HANDLE h_in_{};
    DWORD orig_out_{};
    DWORD orig_in_{};
    bool has_out_{false};
    bool has_in_{false};
    bool restored_{false};
};

} // namespace sh

#else
// ---------------------------------------------------------------------------
// POSIX implementation
// ---------------------------------------------------------------------------
#include <termios.h>
#include <unistd.h>

namespace sh {

class Terminal {
public:
    Terminal() {
        if (::tcgetattr(STDIN_FILENO, &original_) == 0) {
            restored_ = false;
            termios raw = original_;
            raw.c_lflag &= ~(static_cast<tcflag_t>(ICANON | ECHO));
            raw.c_cc[VMIN] = 1;   // block until at least one byte is available
            raw.c_cc[VTIME] = 0;
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
            write_raw("\x1b[?25l");      // hide cursor
            write_raw("\x1b[2J\x1b[H");  // clear screen, home cursor
        }
    }

    ~Terminal() { restore(); }

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    Terminal(Terminal&&) = delete;
    Terminal& operator=(Terminal&&) = delete;

    // Blocking read of a single keypress. Decodes ESC [ A/B/C/D arrow sequences
    // into the synthetic Key codes above.
    [[nodiscard]] int read_key() const {
        unsigned char c{};
        if (::read(STDIN_FILENO, &c, 1) <= 0) return -1;
        if (c != 0x1b) return static_cast<int>(c);

        unsigned char seq0{};
        unsigned char seq1{};
        if (::read(STDIN_FILENO, &seq0, 1) <= 0) return 0x1b;
        if (::read(STDIN_FILENO, &seq1, 1) <= 0) return 0x1b;
        if (seq0 == '[') {
            switch (seq1) {
                case 'A': return KeyUp;
                case 'B': return KeyDown;
                case 'C': return KeyRight;
                case 'D': return KeyLeft;
                default: break;
            }
        }
        return 0x1b;
    }

    // Draw a fully composed frame. We home the cursor and overwrite in place so
    // there is no full-screen clear (and therefore no flicker) between frames.
    static void present(std::string_view frame) {
        write_raw("\x1b[H");
        write_raw(frame);
    }

private:
    void restore() {
        if (!restored_) {
            write_raw("\x1b[0m");        // reset attributes
            write_raw("\x1b[?25h");      // show cursor
            write_raw("\x1b[2J\x1b[H");  // clear + home
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
            restored_ = true;
        }
    }

    static void write_raw(std::string_view s) {
        ::ssize_t written = ::write(STDOUT_FILENO, s.data(), s.size());
        (void)written;
    }

    termios original_{};
    bool restored_{true};
};

} // namespace sh

#endif

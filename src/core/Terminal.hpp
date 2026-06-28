#pragma once
//
// Terminal — RAII guard around a POSIX terminal in raw mode.
//
// Showcases: RAII for an OS resource. The constructor switches the terminal
// into raw, no-echo mode and hides the cursor; the destructor *always*
// restores the original settings, even if the game throws or exits early.
// This is the C++ idiom that makes "leave the user's terminal broken" bugs
// impossible by construction.
//
#include <termios.h>
#include <unistd.h>

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
            write_raw("\x1b[?25l");   // hide cursor
            write_raw("\x1b[2J\x1b[H"); // clear screen, home cursor
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

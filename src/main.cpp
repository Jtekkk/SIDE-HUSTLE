//
// Entry point. By default launches the interactive game. Pass --selftest (or
// --demo) to run the headless self-play smoke test, and --seed=N for a
// reproducible dungeon.
//
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string_view>

#include "game/Game.hpp"

int main(int argc, char** argv) {
    bool selftest = false;
    auto seed = static_cast<std::uint64_t>(std::time(nullptr));

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--selftest" || arg == "--demo") {
            selftest = true;
        } else if (arg.starts_with("--seed=")) {
            seed = std::strtoull(argv[i] + 7, nullptr, 10);
        }
    }

    sh::Game game(seed);
    return selftest ? game.selftest() : game.run();
}

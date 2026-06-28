//
// Entry point. Launches the interactive game by default.
//
// Flags:
//   --selftest / --demo   run the headless self-play smoke test
//   --seed=N              reproducible dungeon
//   --easy / --hard       difficulty (default: normal)
//   --name=NAME           name recorded on the leaderboard
//   --no-scores           do not read or write the high-score file
//
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>

#include "game/Game.hpp"

int main(int argc, char** argv) {
    bool selftest = false;
    sh::Config config;
    config.seed = static_cast<std::uint64_t>(std::time(nullptr));

    if (const char* user = std::getenv("USER"); user != nullptr && *user != '\0') {
        config.player_name = user;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--selftest" || arg == "--demo") {
            selftest = true;
            config.persist_scores = false; // never touch the real file in a test
        } else if (arg == "--easy") {
            config.difficulty = sh::Difficulty::Easy;
        } else if (arg == "--hard") {
            config.difficulty = sh::Difficulty::Hard;
        } else if (arg == "--no-scores") {
            config.persist_scores = false;
        } else if (arg.starts_with("--seed=")) {
            config.seed = std::strtoull(argv[i] + 7, nullptr, 10);
        } else if (arg.starts_with("--name=")) {
            config.player_name = std::string{arg.substr(7)};
        }
    }

    sh::Game game(std::move(config));
    return selftest ? game.selftest() : game.run();
}

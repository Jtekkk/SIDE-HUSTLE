//
// HighScores implementation.
//
// On-disk format is one record per line, pipe-delimited, name last (so it may
// contain anything but a newline):
//
//     score|depth|level|cash|won|name
//
#include "Scores.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <ranges>
#include <string_view>
#include <system_error>

namespace sh {
namespace {

int to_int(std::string_view sv, int fallback = 0) {
    int value = fallback;
    std::from_chars(sv.data(), sv.data() + sv.size(), value);
    return value;
}

void sort_and_trim(std::vector<ScoreEntry>& v) {
    std::ranges::sort(v, std::ranges::greater{}, &ScoreEntry::score);
    if (v.size() > HighScores::kMaxEntries) v.resize(HighScores::kMaxEntries);
}

} // namespace

int HighScores::compute(int depth, int level, int cash, bool won) {
    return cash + level * 100 + depth * 50 + (won ? 1000 : 0);
}

void HighScores::load() {
    entries_.clear();

    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) return;

    std::ifstream in(path_);
    if (!in) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;

        // Split into at most six fields; the sixth (name) keeps any extra pipes.
        std::array<std::string, 6> fields;
        std::size_t start = 0;
        for (std::size_t f = 0; f < fields.size(); ++f) {
            if (f == fields.size() - 1) {
                fields[f] = line.substr(start);
                break;
            }
            const std::size_t sep = line.find('|', start);
            if (sep == std::string::npos) {
                fields[f] = line.substr(start);
                start = line.size();
            } else {
                fields[f] = line.substr(start, sep - start);
                start = sep + 1;
            }
        }

        ScoreEntry e;
        e.score = to_int(fields[0]);
        e.depth = to_int(fields[1], 1);
        e.level = to_int(fields[2], 1);
        e.cash = to_int(fields[3]);
        e.won = to_int(fields[4]) != 0;
        e.name = fields[5].empty() ? "founder" : fields[5];
        entries_.push_back(std::move(e));
    }

    sort_and_trim(entries_);
}

void HighScores::add(const ScoreEntry& entry) {
    entries_.push_back(entry);
    sort_and_trim(entries_);
}

bool HighScores::save() const {
    std::ofstream out(path_, std::ios::trunc);
    if (!out) return false;
    for (const auto& e : entries_) {
        out << e.score << '|' << e.depth << '|' << e.level << '|' << e.cash << '|'
            << (e.won ? 1 : 0) << '|' << e.name << '\n';
    }
    return static_cast<bool>(out);
}

bool HighScores::is_high_score(int score) const {
    if (entries_.size() < kMaxEntries) return true;
    return std::ranges::any_of(entries_, [score](const ScoreEntry& e) { return score > e.score; });
}

} // namespace sh

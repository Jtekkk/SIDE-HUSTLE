#pragma once
//
// HighScores — a small persistent leaderboard.
//
// Showcases: std::filesystem + std::fstream for durable storage, std::ranges
// for sorting/trimming, and a stable on-disk text format that round-trips
// cleanly (and tolerates a missing or corrupt file).
//
#include <filesystem>
#include <string>
#include <vector>

namespace sh {

struct ScoreEntry {
    std::string name{"founder"};
    int depth{1};
    int level{1};
    int cash{0};
    int score{0};
    bool won{false};
};

class HighScores {
public:
    explicit HighScores(std::filesystem::path path) : path_(std::move(path)) {}

    void load();                       // populate from disk (silent if absent)
    void add(const ScoreEntry& entry); // insert, re-sort, cap to kMaxEntries
    bool save() const;                 // write back to disk; false on I/O error

    [[nodiscard]] const std::vector<ScoreEntry>& entries() const { return entries_; }
    [[nodiscard]] bool is_high_score(int score) const;

    // Single source of truth for how a run is scored.
    [[nodiscard]] static int compute(int depth, int level, int cash, bool won);

    static constexpr std::size_t kMaxEntries = 10;

private:
    std::filesystem::path path_;
    std::vector<ScoreEntry> entries_;
};

} // namespace sh

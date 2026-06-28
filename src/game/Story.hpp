#pragma once
//
// Story — the narrative spine of the descent. SIDE HUSTLE is now a 12-floor run
// split into four themed "acts" of the corporate ladder, each capped by a named
// act boss guarding the stairs. This header is the single source of truth for
// the act a floor belongs to, the boss (if any) that blocks its exit, and the
// log "beats" that play when you arrive. Keeping it data-only and header-only
// means both front-ends and the engine read the same canon with no extra TU.
//
// NB: all text here is rendered by the GUI's ASCII-only embedded font, so the
// strings deliberately stay within plain ASCII (straight quotes, hyphens).
//
#include <string>
#include <vector>

namespace sh::story {

// Total floors in a full run. The CEO waits at the very bottom.
inline constexpr int kFloors = 12;

struct Act {
    int number;        // 1..4
    const char* name;  // zone title shown in the HUD / on arrival
};

// Which act (zone) a depth belongs to. Acts are three floors each.
inline Act act_for(int depth) {
    if (depth <= 3) return {1, "The Open Floor"};
    if (depth <= 6) return {2, "Middle Management"};
    if (depth <= 9) return {3, "The Executive Suite"};
    return {4, "The C-Suite"};
}

// A named boss that guards a floor's exit. Stats are pre-difficulty-scaling.
struct BossSpec {
    bool exists{false};
    const char* name{""};
    const char* taunt{""};  // logged when you first sense it on the floor
    int color{95};          // ANSI code -> RGB in code_to_rgb / code_color
    int hp{0};
    int attack{0};
    int defense{3};
    int xp{0};
    int weight{5};
    int force{4};
};

// The boss guarding the exit on this depth, if any. Act bosses sit on floors
// 3 / 6 / 9; the CEO is the finale on floor 12.
inline BossSpec boss_for(int depth) {
    switch (depth) {
        case 3:  return {true, "the Scrum Lord",
                         "the Scrum Lord blocks the stairwell (&): 'This wasn't in the sprint.'",
                         92, 46, 7, 2, 60, 4, 3};
        case 6:  return {true, "the Regional VP",
                         "the Regional VP wants to circle back (&) - with your skull.",
                         96, 72, 9, 3, 110, 5, 4};
        case 9:  return {true, "the Board Chair",
                         "the Board Chair demands a quick sync (&). There is no agenda.",
                         90, 98, 11, 4, 160, 6, 4};
        case kFloors: return {true, "the CEO",
                         "the CEO smiles at the bottom of the ladder (&): 'Let's talk equity.'",
                         95, 135, 13, 5, 240, 6, 5};
        default: return {};
    }
}

// True on the floors where an act boss stands between you and the stairs.
inline bool is_boss_floor(int depth) { return boss_for(depth).exists; }

// The narrative lines to push into the log the moment you arrive on a floor.
// The first floor of each act opens with the act title; boss floors get a
// foreboding line (the boss's own taunt is logged separately on spawn).
inline std::vector<std::string> beats(int depth) {
    switch (depth) {
        case 1: return {"ACT I - THE OPEN FLOOR",
                        "One gig, they said: debug the cursed legacy system in the basement.",
                        "You badge in. Fluorescent lights hum. A Jira ticket ages in the dark."};
        case 2: return {"Standups echo from empty desks. The bugs here have learned to swarm."};
        case 3: return {"A burndown chart bleeds red across the wall. Something blocks the stairs."};
        case 4: return {"ACT II - MIDDLE MANAGEMENT",
                        "Past the org chart the air thickens with process and PowerPoint."};
        case 5: return {"Recruiters circle, sniffing for 'culture fit'. Phishers post fake roles."};
        case 6: return {"Synergy crackles. The stairwell is 'blocked pending review'."};
        case 7: return {"ACT III - THE EXECUTIVE SUITE",
                        "Carpet swallows your steps. The minibar is locked. NDAs underfoot."};
        case 8: return {"Glass offices, confidential everything. Torchlight catches gold leaf."};
        case 9: return {"Stock options glitter like teeth in the dark."};
        case 10: return {"ACT IV - THE C-SUITE",
                         "The private elevator only goes down. Of course it does."};
        case 11: return {"Motivational posters scream from the walls: ALWAYS. BE. CLOSING."};
        case kFloors: return {"The corner office, at the very bottom of the ladder."};
        default: return {};
    }
}

// Shown on the win screen after the CEO falls and you walk out.
inline const char* ending() {
    return "You walk out past security. Contract shredded, the side hustle now your "
           "only hustle - and you are, at last, your own boss.";
}

} // namespace sh::story

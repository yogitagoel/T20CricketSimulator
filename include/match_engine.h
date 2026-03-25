#ifndef MATCH_ENGINE_H
#define MATCH_ENGINE_H

/*
 * match_engine.h
 * ------------------------------------------------------------------------------
 * Core match simulation: innings, over management, team setup.
 */

#include "cricket_types.h"
#include "player_threads.h"
#include "scheduler.h"
#include <vector>
#include <string>

// Default player rosters 
std::vector<Player> build_mi_batting();
std::vector<Player> build_mi_bowling();
std::vector<Player> build_csk_batting();
std::vector<Player> build_csk_bowling();

// Match engine 
class MatchEngine {
public:
    MatchEngine(const MatchConfig& cfg);
    ~MatchEngine();

    // Run the full match (both innings)
    void run();

    // Run a single inning
    void run_innings(int innings_num,
                     std::vector<Player>& batting_team,
                     std::vector<Player>& bowling_team,
                     int target);

    // Post-match analysis
    void print_final_scorecard() const;
    void run_sjf_vs_fcfs_analysis();

private:
    MatchConfig  config_;
    MatchState   state_;
    Scheduler    scheduler_;

    // Teams
    std::vector<Player> team1_bat_;
    std::vector<Player> team1_bowl_;
    std::vector<Player> team2_bat_;
    std::vector<Player> team2_bowl_;

    // Event log for Gantt chart
    std::vector<BallEvent> event_log_;
    FILE* gantt_file_;

    // Fielder threads 
    std::vector<Player>    fielders_;
    std::vector<FielderArgs> fielder_args_;

    // Internal helpers
    void init_state(int innings, int target);
    void run_one_over(std::vector<Player>& batsmen,
                      std::vector<Player>& bowlers,
                      int over_num);
    void handle_wicket(std::vector<Player>& batsmen,
                       Player* dismissed,
                       BallOutcome how);
    void rotate_strike(MatchState* ms);
    bool is_innings_over() const;
    void write_gantt_file();
};

#endif

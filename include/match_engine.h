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

// Pools
extern std::vector<Player> batsmen_pool;
extern std::vector<Player> wicketkeeper_pool;
extern std::vector<Player> bowlers_pool;
extern std::vector<Player> allrounders_pool;

// Default player rosters 
std::vector<Player> build_mi_batting();
std::vector<Player> build_mi_bowling();
std::vector<Player> build_csk_batting();
std::vector<Player> build_csk_bowling();

int rand_range(int l, int r);
float rand_float(float l, float r);
void init_batsmen(int &id);
void init_wicketkeepers(int &id);
void init_bowlers(int &id);
void init_allrounders(int &id);
void init_all_players();
void shuffle_pool(std::vector<Player>& pool);
Player pick_player(std::vector<Player>& pool);
std::vector<Player> select_team();
void create_teams(std::vector<Player>& teamA, std::vector<Player>& teamB);
void print_team(const std::vector<Player>& team, const std::string& name);  
std::vector<Player> get_batting_team(const std::vector<Player>& team);
std::vector<Player> get_bowling_team(const std::vector<Player>& team);
void ensure_teams_ready();

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

#ifndef SCHEDULER_H
#define SCHEDULER_H

/*
 * Custom Over Scheduler - manages bowler rotation and batting order.
 
 * OS CONCEPTS DEMONSTRATED:
   - Round Robin   : Bowlers rotated every 6 balls (Time Quantum = 1 over)
                     Context switch saves/restores bowler stats
   - SJF           : Batting order sorted by expected_balls (burst time)
                     Minimises wait time for tail-enders
   - Priority      : Death-over specialist promoted when intensity=HIGH
                     pthread_setschedparam() used for real thread priority
 
 * The scheduler exposes one unified interface: schedule_next_bowler()
  and get_next_batsman(). The algorithm used is determined by the
  MatchConfig at startup.
 */

#include "cricket_types.h"
#include <vector>
#include <climits>

// Bowler PCB equivalent
typedef struct {
    int     bowler_id;
    int     overs_bowled;
    int     runs_conceded;
    int     wickets;
    float   economy;
    int     balls_in_current_over;
    int     runs_this_spell;
    
    ThreadState saved_state;
} BowlerContext;

// Scheduler class 
class Scheduler {
public:
    Scheduler(SchedulerType type, int max_overs);
    ~Scheduler();

    // Bowler scheduling (Round Robin / Priority) 
    void    add_bowler(Player* p);
    Player* schedule_next_bowler(MatchState* ms, std::vector<Player>& bowlers);
    void    save_bowler_context(Player* p);
    void    restore_bowler_context(Player* p);
    void    notify_over_complete(Player* bowler, MatchState* ms);

    // Batting order scheduling (SJF / FCFS) 
    void    build_batting_order(std::vector<Player>& batsmen,
                                SchedulerType order_type);
    Player* get_next_batsman(std::vector<Player>& batsmen);

     // Link live match state 
    void    set_match_state(MatchState* ms) { ms_ = ms; }

    //  Stats & analysis 
    void    print_scheduler_stats() const;
    double  get_avg_wait_time() const;
    void    record_wait_time(int player_id, double wait_ms);

    // Getters 
    SchedulerType get_type() const { return type_; }
    int get_context_switches() const { return context_switches_; }
    void add_switches(int n)         { context_switches_ += n; }

private:
    SchedulerType type_;
    int max_overs_;
    int context_switches_;
    int next_batsman_idx_;
    MatchState*   ms_;

    // RR bowler queue (circular)
    std::vector<Player*> bowler_queue_;
    int rr_head_;                   

    // Saved bowler contexts
    std::vector<BowlerContext> pcb_table_;

    // SJF batting order 
    std::vector<Player*> sjf_batting_order_;
    std::vector<Player*> fcfs_batting_order_;

    // Wait time records per player (for SJF vs FCFS analysis)
    struct WaitRecord { int id; double wait_ms; };
    std::vector<WaitRecord> wait_records_;

    // Internal helpers
    Player* rr_next(MatchState* ms, std::vector<Player>& bowlers);
    Player* priority_next(MatchState* ms, std::vector<Player>& bowlers);

    
    /*
     * compute_dynamic_priority()
     * BOWLER signals:  skill + wicket_bonus − fatigue − economy_penalty
     *                  + phase_boost + overs_left_urgency
     * BATSMAN signals: strike_rate_bonus + overs_left_power_boost
     *                  + batting_avg (middle overs) − slogger_risk (tail)
     */
    int     compute_dynamic_priority(Player* p, MatchState* ms,
                                     int overs_left, bool is_batting);
    BowlerContext* find_context(int bowler_id);
    void    update_intensity(MatchState* ms);
};

// Standalone SJF sort comparison 
bool sjf_compare(const Player* a, const Player* b);

#endif 

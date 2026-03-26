#ifndef CRICKET_TYPES_H
#define CRICKET_TYPES_H

/*
 * cricket_types.h

 * OS CONCEPT MAPPING:
    - Pitch          -> Critical Section (only 1 bowler writes at a time)
    - Players        -> Threads (pthread_t)
    - Global Score   -> Shared Resource (protected by mutex)
    - Umpire         -> Kernel / Resource Scheduler
    - Crease         -> Semaphore (capacity = 2 batsmen)
    - Fielders       -> Blocked threads (cond_wait until ball_in_air)
    - Over switch    -> Context Switch (RR scheduler, quantum = 6 balls)
    - Run-out        -> Deadlock (circular wait on end resources)
 */

#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <string>
#include <vector>
#include <atomic>

//  Constants
#define MAX_PLAYERS        11
#define MAX_OVERS          20
#define BALLS_PER_OVER      6
#define MAX_FIELDERS       10   
#define CREASE_CAPACITY     2   
#define UMPIRE_POLL_US  50000   // umpire checks every 50ms
#define BALL_DELAY_MS     200   // simulated time between balls

//  Enumerations 


typedef enum {
    THREAD_READY    = 0,  // Waiting to be scheduled
    THREAD_RUNNING  = 1,  
    THREAD_WAITING  = 2,  
    THREAD_SLEEPING = 3, 
    THREAD_DONE     = 4   // Thread has exited
} ThreadState;

// Player roles
typedef enum {
    BATSMAN       = 0,
    WICKET_KEEPER = 1,
    BOWLER        = 2,
    ALL_ROUNDER   = 3,
} PlayerRole;

// Scheduler algorithms
typedef enum {
    CRICKET_SCHED_RR        = 0,  // Round Robin  (bowler rotation)
    CRICKET_SCHED_SJF       = 1,  // Shortest Job First (tail-enders first)
    CRICKET_SCHED_PRIORITY  = 2   // Priority (death over specialist)
} SchedulerType;

// Ball outcomes
typedef enum {
    BALL_DOT     = 0,
    BALL_SINGLE  = 1,
    BALL_TWO     = 2,
    BALL_THREE   = 3,
    BALL_FOUR    = 4,
    BALL_SIX     = 6,
    BALL_WICKET  = 7,
    BALL_WIDE    = 8,
    BALL_NOBALL  = 9,
    BALL_LBW     = 10,
    BALL_CAUGHT  = 11,
    BALL_RUNOUT  = 12
} BallOutcome;

// Match intensity - drives priority scheduler
typedef enum {
    INTENSITY_LOW    = 0,  // Overs 1–10
    INTENSITY_MEDIUM = 1,  // Overs 11–16
    INTENSITY_HIGH   = 2   // Overs 17–20 (death overs)
} MatchIntensity;

// Delivery (Pitch Buffer - the Critical Section payload) 
typedef struct {
    int     over_num;          // Current over 
    int     ball_num;          // Ball in over 
    float   speed_kmh;        
    int     line;              
    int     length;            
    float   swing;             
    int     bowler_id;         // Thread ID of bowler
    bool    is_valid;          // Set true when bowler writes
} Delivery;

// Player / Thread structure 
typedef struct {
    // Identity
    int         id;
    std::string name;
    PlayerRole  role;

    // Cricket stats
    int         skill_rating;     
    int         expected_balls;   
    float       batting_avg;       // For victim selection in deadlock
    float       strike_rate;       // Affects shot selection probability
    float       bowling_economy;   // Runs per over (bowlers)
    float       bowling_speed;     
    bool        is_death_specialist; 
    int         max_overs;         // Max overs this bowler can bowl

    // Thread handles 
    pthread_t   thread_id;
    ThreadState state;

    // Per-ball tracking
    int         balls_faced;
    int         runs_scored;
    int         balls_bowled;
    int         runs_conceded;
    int         wickets_taken;
    bool        is_out;
    bool        is_active;         // Currently at crease / currently bowling

    // Deadlock detection: which end this batsman holds/wants
    int         holds_end;         // -1 = none, 0 = End1, 1 = End2
    int         wants_end;        

    // Timing (for Gantt chart)
    struct timespec start_time;
    struct timespec end_time;
    double      total_wait_ms;    
    double      entry_time_ms;     


} Player;
//Function Declarations
    void init_all_players();
    void create_teams(std::vector<Player>& teamA, std::vector<Player>& teamB);
    void print_team(const std::vector<Player>& team, const std::string& name);

//  Global Match State 
typedef struct {
    // Scoreboard
    std::atomic<int> total_runs;
    std::atomic<int> wickets;
    int  extras;
    int  current_over;         // 1-based
    int  current_ball;         // 0-based within over
    int  innings;             
    int  target;              
    bool match_over;

    // Active players
    int  striker_id;           
    int  non_striker_id;       
    int  current_bowler_idx;   

    // Scheduler state
    SchedulerType scheduler;
    MatchIntensity intensity;

    // Pitch buffer (Critical Section)
    Delivery     pitch_buffer;
    bool         ball_in_air;  
    // Timing
    struct timespec match_start;

} MatchState;

// Event log entry (for Gantt chart & analysis) 
typedef struct {
    double      timestamp_ms;
    int         over_num;
    int         ball_num;
    int         bowler_id;
    int         batsman_id;
    double      pitch_acquire_ms;
    double      pitch_release_ms;
    BallOutcome outcome;
    int         runs_scored;
    std::string commentary;
    ThreadState bowler_state;
    ThreadState batsman_state;
} BallEvent;

//  Match Configuration 
typedef struct {
    std::string team1_name;
    std::string team2_name;
    int         max_overs;           
    SchedulerType scheduler_type;
    bool        enable_deadlock_sim; 
    bool        enable_gantt;       
    bool        enable_sjf_analysis; 
    int         ball_delay_ms;       
    bool        verbose;             
    std::string log_file;           
} MatchConfig;

// Scheduler Queue Node 
typedef struct SchedNode {
    Player*         player;
    int             priority;
    struct SchedNode* next;
} SchedNode;

#endif 

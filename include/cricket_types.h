#ifndef CRICKET_TYPES_H
#define CRICKET_TYPES_H

/*
 * cricket_types.h
 *
 * OS CONCEPT MAPPING:
 *   Pitch        -> Critical Section  (only 1 bowler writes at a time)
 *   Players      -> Threads           (pthread_t)
 *   Global Score -> Shared Resource   (protected by mutex)
 *   Umpire       -> Kernel / Scheduler
 *   Crease       -> Semaphore         (capacity = 2 batsmen)
 *   Fielders     -> Blocked threads   (cond_wait until ball_in_air)
 *   Over switch  -> Context Switch    (RR scheduler, quantum = 6 balls)
 *   Run-out      -> Deadlock          (circular wait on end resources)
 */

#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <string>
#include <vector>
#include <atomic>

// Constants
#define MAX_PLAYERS        11
#define MAX_OVERS          20
#define BALLS_PER_OVER      6
#define MAX_FIELDERS       10
#define CREASE_CAPACITY     2
#define UMPIRE_POLL_US  50000   // umpire checks every 50ms
#define BALL_DELAY_MS     200   // simulated time between balls

// Bowler quota: ceil(MAX_OVERS/5)
#define MAX_BOWL_OVERS  ((MAX_OVERS + 4) / 5)

// Phase defines
#define PHASE_POWERPLAY  0
#define PHASE_MIDDLE     1
#define PHASE_DEATH      2

// Phase boundary overs
#define POWERPLAY_END_OVER   ((MAX_OVERS * 30 + 99) / 100)
#define DEATH_START_OVER     (MAX_OVERS - (MAX_OVERS * 20 + 99) / 100)

// Enumerations

typedef enum {
    THREAD_READY    = 0,  // Waiting to be schedules
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

// Batting role
typedef enum {
    BAT_ATTACKING_OPENER  = 0,  
    BAT_ANCHOR_OPENER     = 1,  
    BAT_TECHNICAL_ANCHOR  = 2,  
    BAT_MIDDLE_HITTER     = 3,  
    BAT_FINISHER          = 4,  
    BAT_WK_BATSMAN        = 5, 
    BAT_ALLROUNDER        = 6,  
    BAT_NONE              = 7,  
} BatRole;

// Bowling role
typedef enum {
    BOWL_OPENING_SWING    = 0, 
    BOWL_SPINNER          = 1,  
    BOWL_DEATH_SPECIALIST = 2,
    BOWL_MEDIUM_PACE      = 3,
    BOWL_NONE             = 4,
} BowlSpec;

// Scheduler algorithms
typedef enum {
    CRICKET_SCHED_RR        = 0,
    CRICKET_SCHED_SJF       = 1,
    CRICKET_SCHED_PRIORITY  = 2
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

// Shot type emitted by batsman thread BEFORE signalling fielders
typedef enum {
    SHOT_WIDE       = 0,   
    SHOT_NOBALL     = 1,   
    SHOT_BOWLED     = 2,  
    SHOT_LBW        = 3,   
    SHOT_RUNOUT     = 4,   
    SHOT_DOT_BLOCK  = 5,  
    SHOT_BOUNDARY_6 = 6, 
    SHOT_AERIAL     = 7,  
    SHOT_GROUND     = 8, 
} ShotType;

// Shot Result
typedef struct {
    ShotType type;
    int      direction_zone;  
    float    power;        
    float    flight_time_ms; 
} ShotResult;

// ShotState
#define MAX_FIELDER_COUNT 10

typedef struct {
    int   type;               
    int   direction_zone;     
    float power;             
    float flight_time_ms;  

    float reach_ms    [MAX_FIELDER_COUNT];
    int   skill_rating[MAX_FIELDER_COUNT];
    char  fielder_name[MAX_FIELDER_COUNT][64];
    char  fielder_pos [MAX_FIELDER_COUNT][32];

    int   winner_idx;        
    float winner_reach_ms;
    bool  winner_caught; 
    char  winner_name[64];
    char  winner_pos [32];

    std::atomic<int>  fielders_remaining;  
    std::atomic<bool> fielding_done;  
    pthread_mutex_t   mutex;
    pthread_cond_t    cond;
} ShotState;

// Match intensity
typedef enum {
    INTENSITY_LOW    = 0,
    INTENSITY_MEDIUM = 1,
    INTENSITY_HIGH   = 2
} MatchIntensity;

// Delivery (Pitch Buffer — Critical Section payload)
typedef struct {
    int   over_num;
    int   ball_num;
    float speed_kmh;
    int   line;
    int   length;
    float swing;
    int   bowler_id;
    bool  is_valid;
} Delivery;

// Player / Thread structure
typedef struct {
    int         id;
    std::string name;
    PlayerRole  role;

    // Cricket stats
    int         skill_rating;
    int         expected_balls;
    float       batting_avg;    // For victim selection in deadlock
    float       strike_rate;    // Affects shot selection probability
    float       bowling_economy;// Runs per over (bowlers)
    float       bowling_speed;
    bool        is_death_specialist; 
    int         max_overs;      // Max overs this bowler can bowl

    // Role specialisation (set at player creation)
    BatRole     bat_role;    // batting position/style
    BowlSpec    bowl_spec;   // bowling phase specialisation
    bool        is_opener;   // true for openers (bat_role 0 or 1)

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
    bool        is_active;   // Currently at crease/ currently bowling

    // Dismissal details
    BallOutcome how_out;         
    char        caught_by[64];    
    char        caught_pos[32];  
    int         dismissed_by_id;

    // Deadlock detection
    int         holds_end;   // -1=none, 0=End0, 1=End1
    int         wants_end;

    // Timing (for Gantt chart)
    struct timespec start_time;
    struct timespec end_time;
    double      total_wait_ms;
    double      entry_time_ms;
} Player;

// Function declarations
void init_all_players();
void create_teams(std::vector<Player>& teamA, std::vector<Player>& teamB);
void print_team(const std::vector<Player>& team, const std::string& name);

// Global Match State
typedef struct {
    // Scoreboard
    std::atomic<int> total_runs;
    std::atomic<int> wickets;
    int  extras;
    int  current_over;      // 1-based
    int  current_ball;      // 0-based within over
    int  innings;
    int  target;
    bool match_over;

    // Active players
    int  striker_id;
    int  non_striker_id;
    int  current_bowler_idx;

    // Scheduler state
    SchedulerType  scheduler;
    MatchIntensity intensity;

    // Phase tracking 
    int  current_phase;   
    int  phase_intensity;  

    // Pitch buffer (Critical Section)
    Delivery pitch_buffer;
    bool     ball_in_air;
    int      active_fielders;   // actual fielder thread count

    // Timing
    struct timespec match_start;
} MatchState;

// Fielding positions 
// Zone 0: Wicket Keeper
// Zones 1-9: outfield positions
extern const char* FIELDING_POSITIONS[10];

// Ball direction zone 
static inline int ball_direction_zone(int line, int length) {
    if (length == 0) return (line <= 1) ? 6 : 5;   
    if (line == 0)   return 8;  
    if (line == 2)   return 2;  
    return 4;                  
}

// Event log entry 
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

// Match Configuration 
typedef struct {
    std::string   team1_name;
    std::string   team2_name;
    int           max_overs;
    SchedulerType scheduler_type;
    bool          enable_deadlock_sim;
    bool          enable_gantt;
    bool          enable_sjf_analysis;
    int           ball_delay_ms;
    bool          verbose;
    std::string   log_file;
} MatchConfig;

// Scheduler Queue Node 
typedef struct SchedNode {
    Player*           player;
    int               priority;
    struct SchedNode* next;
} SchedNode;

#endif
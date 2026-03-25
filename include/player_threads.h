#ifndef PLAYER_THREADS_H
#define PLAYER_THREADS_H

/*
  player_threads.h
---------------------------------------------------------------------------------
 * Thread entry points and argument structures for all player threads.

 * THREAD TYPES:
   - Bowler thread  : Writer — acquires pitch_mutex, writes delivery data
   - Batsman thread : Reader — reads pitch, updates score via score_mutex
   - Fielder thread : Passive — sleeps on cond_wait until ball_in_air=true
   - Umpire thread  : Daemon — monitors for deadlocks, manages over transitions
   - Scoreboard     : Monitor — read-only display via rwlock
 */

#include "cricket_types.h"
#include "scheduler.h"
#include <vector>

// Thread argument structs 

// Passed to each bowler thread
typedef struct {
    Player*      player;
    MatchState*  match;
    MatchConfig* config;
    int          balls_to_bowl;     
    std::vector<BallEvent>* event_log;
    FILE*        gantt_file;
} BowlerArgs;

// Passed to each batsman thread
typedef struct {
    Player*      player;
    MatchState*  match;
    MatchConfig* config;
    std::vector<Player>* all_batsmen;  
    std::vector<BallEvent>* event_log;
    bool         is_striker;       
    double       entry_time_ms;    
} BatsmanArgs;

// Passed to each fielder thread
typedef struct {
    Player*      player;
    MatchState*  match;
    MatchConfig* config;
    int          fielder_index;     
} FielderArgs;

// Passed to the umpire thread
typedef struct {
    MatchState*         match;
    MatchConfig*        config;
    Scheduler*          scheduler;
    std::vector<Player>* bowlers;
    std::vector<Player>* batsmen;
    std::vector<BallEvent>* event_log;
    FILE*               gantt_file;
} UmpireArgs;

// Passed to the scoreboard thread
typedef struct {
    MatchState*          match;
    MatchConfig*         config;
    std::vector<Player>* batsmen;
    std::vector<Player>* bowlers;
} ScoreboardArgs;

// Thread entry functions (passed to pthread_create) 
void* bowler_thread_func(void* arg);
void* batsman_thread_func(void* arg);
void* fielder_thread_func(void* arg);
void* umpire_thread_func(void* arg);
void* scoreboard_thread_func(void* arg);

// Thread management helpers 
void spawn_bowler(Player* p, BowlerArgs* args);
void spawn_batsman(Player* p, BatsmanArgs* args);
void spawn_all_fielders(std::vector<Player>& fielders,
                        std::vector<FielderArgs>& args,
                        MatchState* match,
                        MatchConfig* config);
void join_all_fielders(std::vector<Player>& fielders);
void terminate_batsman(Player* p, MatchState* ms); 

// Deadlock (run-out) helpers 
void  simulate_runout_attempt(Player* batsmanA, Player* batsmanB,
                               MatchState* ms);
bool  detect_runout_deadlock(Player* batsmanA, Player* batsmanB);
void  resolve_runout(Player* victim, MatchState* ms);
void  print_resource_allocation_graph(Player* a, Player* b);

#endif

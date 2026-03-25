#ifndef UTILS_H
#define UTILS_H

/*
  utils.h
 ---------------------------------------------------------------------------------
  Logging, timing, random helpers, and display utilities.
 */

#include "cricket_types.h"
#include <string>
#include <vector>
#include <cstdio>

// Colour codes for terminal output 
#define COL_RESET   "\033[0m"
#define COL_RED     "\033[1;31m"
#define COL_GREEN   "\033[1;32m"
#define COL_YELLOW  "\033[1;33m"
#define COL_BLUE    "\033[1;34m"
#define COL_MAGENTA "\033[1;35m"
#define COL_CYAN    "\033[1;36m"
#define COL_WHITE   "\033[1;37m"
#define COL_ORANGE  "\033[0;33m"
#define COL_GRAY    "\033[0;37m"

// Log levels 
typedef enum {
    LOG_DEBUG   = 0,
    LOG_INFO    = 1,
    LOG_EVENT   = 2,  
    LOG_WARN    = 3,
    LOG_THREAD  = 4,   
    LOG_SCHED   = 5,   
    LOG_DEADLOCK= 6,   
    LOG_SCORE   = 7,   
    LOG_GANTT   = 8    
} LogLevel;

// Logger init/shutdown 
void logger_init(const std::string& log_file, bool verbose);
void logger_shutdown();

// Core logging function 
void log_msg(LogLevel level, const char* fmt, ...);

// Timing utilities 
double  get_time_ms();                           
double  elapsed_ms(struct timespec* start);     
void    sleep_ms(int ms);                        
void    get_timespec_now(struct timespec* ts);   

// Random helpers 
void    rng_init(unsigned int seed);
int     rng_range(int lo, int hi);       
float   rng_float();                     
bool    rng_chance(float probability);  

// Ball outcome generation
BallOutcome generate_outcome(const Player* batsman,
                             const Delivery* delivery,
                             bool enable_runout);

std::string outcome_to_string(BallOutcome outcome);
std::string outcome_color(BallOutcome outcome);

// Display helpers 
void print_banner();
void print_scoreboard(const MatchState* ms,
                      const std::vector<Player>& batsmen,
                      const std::vector<Player>& bowlers);
void print_over_summary(int over_num, const std::vector<BallEvent>& events);
void print_match_result(const MatchState* ms,
                        const std::string& team1,
                        const std::string& team2);
void print_thread_state(const Player* p);
void print_separator(const char* label);

// Gantt data writer 
void gantt_write_event(FILE* f, const BallEvent& ev);
void gantt_write_header(FILE* f);

// Analysis helpers 
void print_sjf_vs_fcfs_analysis(const std::vector<Player>& fcfs_order,
                                 const std::vector<Player>& sjf_order);

// String helpers 
std::string thread_state_str(ThreadState s);
std::string format_score(int runs, int wickets, int over, int ball);

#endif 

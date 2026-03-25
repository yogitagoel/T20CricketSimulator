/*
 * utils.cpp
 ---------------------------------------------------------------------------------------
 * Logging, timing, random number generation, display, and Gantt helpers.
 */

#include "../include/utils.h"
#include "../include/synchronization.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <unistd.h>
#include <stdarg.h>
#include <sys/time.h>
#include <algorithm>

// Logger state 
static FILE*  g_log_file  = NULL;
static bool   g_verbose   = false;
static double g_start_ms  = 0.0;

// RNG state
static unsigned int g_seed = 42;

// Timing 

double get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

double elapsed_ms(struct timespec* start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000.0
         + (now.tv_nsec - start->tv_nsec) / 1.0e6;
}

void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

void get_timespec_now(struct timespec* ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

//  Logger 

void logger_init(const std::string& log_file, bool verbose) {
    g_verbose  = verbose;
    g_start_ms = get_time_ms();
    if (!log_file.empty()) {
        g_log_file = fopen(log_file.c_str(), "w");
        if (!g_log_file) {
            fprintf(stderr, "Warning: cannot open log file '%s'\n",
                    log_file.c_str());
        }
    }
}

void logger_shutdown() {
    if (g_log_file) { fclose(g_log_file); g_log_file = NULL; }
}

void log_msg(LogLevel level, const char* fmt, ...) {
   
    if (level == LOG_DEBUG && !g_verbose) return;

    double ts = get_time_ms() - g_start_ms;
    char   buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    
    const char* prefix;
    const char* col;
    switch (level) {
        case LOG_DEBUG:    prefix = "DEBUG  "; col = COL_GRAY;    break;
        case LOG_INFO:     prefix = "INFO   "; col = COL_WHITE;   break;
        case LOG_EVENT:    prefix = "BALL   "; col = COL_GREEN;   break;
        case LOG_WARN:     prefix = "WARN   "; col = COL_YELLOW;  break;
        case LOG_THREAD:   prefix = "THREAD "; col = COL_CYAN;    break;
        case LOG_SCHED:    prefix = "SCHED  "; col = COL_MAGENTA; break;
        case LOG_DEADLOCK: prefix = "DEADLK "; col = COL_RED;     break;
        case LOG_SCORE:    prefix = "SCORE  "; col = COL_YELLOW;  break;
        case LOG_GANTT:    prefix = "GANTT  "; col = COL_BLUE;    break;
        default:           prefix = "LOG    "; col = COL_WHITE;   break;
    }

    // Thread-safe console output
    safe_mutex_lock(&log_mutex, "log_mutex");
    printf("%s[%8.3f] [%s] %s%s\n", col, ts, prefix, buf, COL_RESET);
    if (g_log_file) {
        fprintf(g_log_file, "[%8.3f] [%s] %s\n", ts, prefix, buf);
        fflush(g_log_file);
    }
    safe_mutex_unlock(&log_mutex, "log_mutex");
}

// RNG 

void rng_init(unsigned int seed) {
    g_seed = seed;
    srand(seed);
}

int rng_range(int lo, int hi) {
    if (lo > hi) return lo;
    return lo + (rand_r(&g_seed) % (hi - lo + 1));
}

float rng_float() {
    return (float)rand_r(&g_seed) / (float)RAND_MAX;
}

bool rng_chance(float probability) {
    return rng_float() < probability;
}

// Ball outcome generation 
/*
 * Probabilities are weighted by:
 *   - batsman skill_rating  (higher = better)
 *   - delivery quality      (yorker + high speed = harder)
 *   - realistic T20 rates   (approx. 15% boundary rate for top batsmen)
 */
BallOutcome generate_outcome(const Player* bat, const Delivery* del,
                              bool enable_runout) {
    float skill = bat->skill_rating / 10.0f;  // 0.0 to 1.0
    float diff  = (del->speed_kmh - 120.0f) / 50.0f; // delivery difficulty
    float adj   = skill - diff * 0.3f;
    adj = std::max(0.05f, std::min(0.95f, adj));

    float r = rng_float();
    // Wide / no-ball 
    if (r < 0.06f) return BALL_WIDE;
    if (r < 0.08f) return BALL_NOBALL;

    // Wicket probability 
    float wicket_p = 0.15f - adj * 0.10f + diff * 0.05f;
    wicket_p = std::max(0.03f, std::min(0.25f, wicket_p));
    if (r < 0.08f + wicket_p) {
        
        float w2 = rng_float();
        if (w2 < 0.35f) return BALL_CAUGHT;
        if (w2 < 0.60f) return BALL_WICKET;   
        if (w2 < 0.80f) return BALL_LBW;
        if (enable_runout && w2 < 0.95f) return BALL_RUNOUT;
        return BALL_WICKET;
    }

    // Scoring probabilities
    float dot_p   = 0.40f - adj * 0.20f;
    float single_p= 0.30f;
    float two_p   = 0.08f;
    float three_p = 0.03f;
    float four_p  = 0.10f + adj * 0.08f;
    float six_p   = 0.05f + adj * 0.06f;

    float cumulative = 0.08f + wicket_p;
    cumulative += dot_p;   if (r < cumulative) return BALL_DOT;
    cumulative += single_p;if (r < cumulative) return BALL_SINGLE;
    cumulative += two_p;   if (r < cumulative) return BALL_TWO;
    cumulative += three_p; if (r < cumulative) return BALL_THREE;
    cumulative += four_p;  if (r < cumulative) return BALL_FOUR;
    cumulative += six_p;   if (r < cumulative) return BALL_SIX;
    return BALL_SINGLE;
}

std::string outcome_to_string(BallOutcome o) {
    switch(o) {
        case BALL_DOT:    return "•";
        case BALL_SINGLE: return "1";
        case BALL_TWO:    return "2";
        case BALL_THREE:  return "3";
        case BALL_FOUR:   return "4";
        case BALL_SIX:    return "6";
        case BALL_WICKET: return "W(B)";
        case BALL_WIDE:   return "Wd";
        case BALL_NOBALL: return "Nb";
        case BALL_LBW:    return "W(L)";
        case BALL_CAUGHT: return "W(C)";
        case BALL_RUNOUT: return "W(R)";
        default:          return "?";
    }
}

std::string outcome_color(BallOutcome o) {
    switch(o) {
        case BALL_FOUR:
        case BALL_SIX:    return COL_GREEN;
        case BALL_WICKET:
        case BALL_LBW:
        case BALL_CAUGHT:
        case BALL_RUNOUT: return COL_RED;
        case BALL_WIDE:
        case BALL_NOBALL: return COL_YELLOW;
        case BALL_DOT:    return COL_GRAY;
        default:          return COL_WHITE;
    }
}

// Display helpers 

void print_banner() {
    printf("\n");
    printf(COL_CYAN);
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║         T20 CRICKET OS SIMULATOR — CSC-204                  ║\n");
    printf("║         Pitch = CS | Players = Threads | Umpire = Kernel    ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf(COL_RESET "\n");
}

void print_separator(const char* label) {
    printf(COL_BLUE);
    printf("\n──────────────── %s ────────────────\n", label);
    printf(COL_RESET);
}

void print_scoreboard(const MatchState* ms,
                      const std::vector<Player>& batsmen,
                      const std::vector<Player>& bowlers) {
    // Use read lock - scoreboard never modifies game state
    pthread_rwlock_rdlock(&scoreboard_rwlock);

    printf(COL_YELLOW);
    printf("\n┌─ SCOREBOARD ──────────────────────────────────────────┐\n");
    printf("│  Score: %d/%d   Over: %d.%d   Extras: %d",
           ms->total_runs.load(), ms->wickets.load(),
           ms->current_over, ms->current_ball, ms->extras);
    if (ms->innings == 2 && ms->target > 0) {
        int need = ms->target - ms->total_runs.load();
        int balls_left = (ms->current_over < MAX_OVERS) ?
            ((MAX_OVERS - ms->current_over) * 6 - ms->current_ball) : 0;
        printf("   Need: %d off %d balls", need, balls_left);
    }
    printf("\n");

    // Active batsmen
    for (const auto& p : batsmen) {
        if (p.is_active && !p.is_out) {
            printf("│  %-15s %3d(%3d)  %s\n",
                   p.name.c_str(), p.runs_scored, p.balls_faced,
                   (p.id == ms->striker_id ? "* STRIKER" : "  non-striker"));
        }
    }

    // Current bowler
    /*for (const auto& b : bowlers) {
        if (b.id == (int)batsmen[0].id || b.is_active) { // rough match
            // just show last active bowler
        }
    }*/

    printf("└───────────────────────────────────────────────────────┘\n");
    printf(COL_RESET);

    pthread_rwlock_unlock(&scoreboard_rwlock);
}

void print_over_summary(int over_num, const std::vector<BallEvent>& events) {
    printf(COL_MAGENTA "\n  Over %d: [ ", over_num);
    for (const auto& ev : events) {
        if (ev.over_num == over_num) {
            printf("%s%s%s ", outcome_color(ev.outcome).c_str(),
                   outcome_to_string(ev.outcome).c_str(), COL_RESET);
        }
    }
    printf(COL_MAGENTA "]\n" COL_RESET);
}

void print_match_result(const MatchState* ms,
                        const std::string& t1,
                        const std::string& t2) {
    printf(COL_CYAN);
    printf("\n╔══════════════════════════════════════╗\n");
    printf("║          MATCH RESULT                ║\n");
    printf("╚══════════════════════════════════════╝\n");
    printf(COL_RESET);
    if (ms->innings == 2) {
        int chasing = ms->total_runs.load();
        int need    = ms->target;
        if (chasing >= need) {
            printf(COL_GREEN "  %s WON!\n" COL_RESET, t2.c_str());
        } else {
            int margin = need - chasing - 1;
            printf(COL_GREEN "  %s WON by %d runs!\n" COL_RESET,
                   t1.c_str(), margin);
        }
    }
    printf("\n");
}

void print_thread_state(const Player* p) {
    printf(COL_CYAN "  [Thread %lu] %-15s → %s\n" COL_RESET,
           p->thread_id % 10000,
           p->name.c_str(),
           thread_state_str(p->state).c_str());
}

// Gantt chart data 

void gantt_write_header(FILE* f) {
    if (!f) return;
    fprintf(f, "timestamp_ms,over,ball,bowler_id,batsman_id,"
               "pitch_acquire_ms,pitch_release_ms,outcome,runs,commentary\n");
}

void gantt_write_event(FILE* f, const BallEvent& ev) {
    if (!f) return;
    fprintf(f, "%.3f,%d,%d,%d,%d,%.3f,%.3f,%s,%d,\"%s\"\n",
            ev.timestamp_ms, ev.over_num, ev.ball_num,
            ev.bowler_id, ev.batsman_id,
            ev.pitch_acquire_ms, ev.pitch_release_ms,
            outcome_to_string(ev.outcome).c_str(),
            ev.runs_scored,
            ev.commentary.c_str());
}

// Analysis 

void print_sjf_vs_fcfs_analysis(const std::vector<Player>& fcfs,
                                  const std::vector<Player>& sjf) {
    printf(COL_MAGENTA);
    printf("\n╔══════════════════════════════════════════════════════════════╗\n");
    printf("║          SJF vs FCFS BATTING ORDER ANALYSIS                 ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  %-14s  %-4s  %-10s  %-10s  %-8s ║\n",
           "Batsman", "Pos", "FCFS Wait", "SJF Wait", "Saved");
    printf("╠══════════════════════════════════════════════════════════════╣\n");

    for (size_t i = 0; i < fcfs.size() && i < sjf.size(); i++) {
        double fcfs_w = fcfs[i].total_wait_ms;
        double sjf_w  = sjf[i].total_wait_ms;
        double saved  = fcfs_w - sjf_w;
        printf("║  %-14s  %-4zu  %8.1fms  %8.1fms  %+7.1fms ║\n",
               fcfs[i].name.c_str(), i+1, fcfs_w, sjf_w, saved);
    }
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf(COL_RESET "\n");
}

// String helpers 

std::string thread_state_str(ThreadState s) {
    switch(s) {
        case THREAD_READY:    return "READY";
        case THREAD_RUNNING:  return "RUNNING";
        case THREAD_WAITING:  return "WAITING";
        case THREAD_SLEEPING: return "SLEEPING";
        case THREAD_DONE:     return "DONE";
        default:              return "UNKNOWN";
    }
}

std::string format_score(int runs, int wickets, int over, int ball) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d/%d (%d.%d)", runs, wickets, over, ball);
    return std::string(buf);
}

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
#include <cstdint>

#define PID(t) ((unsigned long)((uintptr_t)(t) % 10000u))

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

// generate_shot() 
/*
 * Determines what kind of shot the batsman plays.
 * Pre-decided outcomes (wide, bowled, lbw, dot, six) are returned directly.
 * SHOT_AERIAL / SHOT_GROUND trigger the fielder race in player_threads.cpp:
 *   • direction_zone (0-9) tells each fielder how far it must sprint
 *   • power (0-1) controls ball speed → boundary time for ground shots
 *                              and flight time for aerial shots
 *
 * Power <-> intended outcome mapping (fielder race adds realistic variance):
 *   0.10-0.30 → single    | 0.30-0.55 → two   | 0.50-0.70 → three
 *   0.70-0.92 → four      | 0.92-1.00 → six (pre-decided as SHOT_BOUNDARY_6)
 *   0.25-0.65 → aerial (catch possible, else 2 runs)
 */
ShotResult generate_shot(const Player* bat, const Delivery* del,
                          bool enable_runout, const MatchState* ms)
{
    ShotResult result = {SHOT_DOT_BLOCK, 0, 0.0f, 0.0f};

    int phase     = ms ? ms->current_phase   : PHASE_MIDDLE;
    int intensity = ms ? ms->phase_intensity : 15;
    int balls_in  = bat->balls_faced;

    // Set-batsman acceleration (same as before)
    int set_bonus = (balls_in >= 35) ? 18
                  : (balls_in >= 20) ? 14
                  : (balls_in >=  8) ?  8 : 0;
    float eff_skill = std::min(105.0f, (float)(bat->skill_rating + set_bonus)) / 100.0f;
    float diff  = (del->speed_kmh - 120.0f) / 50.0f;
    float adj   = std::max(0.05f, std::min(0.95f, eff_skill - diff * 0.25f));

    float r = rng_float();

    // Wide / no-ball
    if (r < 0.05f) { result.type = SHOT_WIDE;   return result; }
    if (r < 0.07f) { result.type = SHOT_NOBALL; return result; }

    // Wicket probability (role-aware, phase-aware — identical to old code)
    float wk_base = ((1.0f - (bat->strike_rate / 200.0f)) * 0.10f)
                  + (diff * 0.015f)
                  + ((float)intensity / 100.0f) * 0.005f;
    if (phase == PHASE_DEATH) wk_base *= 1.10f;
    float wk_lo = (bat->strike_rate >= 130) ? 0.012f
                : (bat->strike_rate >=  90) ? 0.018f : 0.025f;
    float wk_hi = (bat->strike_rate >= 130) ? 0.032f
                : (bat->strike_rate >=  90) ? 0.048f : 0.065f;
    float wicket_p = std::max(wk_lo, std::min(wk_hi, wk_base));

    if (r < 0.07f + wicket_p) {
        float w2 = rng_float();
        result.direction_zone = rng_range(0, 9);
        if (w2 < 0.35f) {
            // Aerial chance — fielder race decides caught vs 2 runs
            result.type           = SHOT_AERIAL;
            result.power          = 0.25f + rng_float() * 0.40f;
            result.flight_time_ms = 300.0f + result.power * 500.0f;
            return result;
        }
        if (w2 < 0.60f) { result.type = SHOT_BOWLED; return result; }
        if (w2 < 0.80f) { result.type = SHOT_LBW;    return result; }
        if (enable_runout && w2 < 0.92f) { result.type = SHOT_RUNOUT; return result; }
        result.type = SHOT_BOWLED;
        return result;
    }

    // Phase aggression multiplier
    float pm = (phase == PHASE_POWERPLAY) ? 0.90f
             : (phase == PHASE_MIDDLE)    ? 0.80f : 1.20f;

    // Scoring probabilities (unchanged from generate_outcome)
    float six_p    = 0.04f + adj * 0.07f * pm;
    float four_p   = 0.09f + adj * 0.08f * pm;
    float three_p  = 0.02f;
    float two_p    = 0.10f + adj * 0.04f;
    float single_p = 0.25f + adj * 0.05f;
    float dot_p    = 1.0f - (six_p + four_p + three_p + two_p
                              + single_p + 0.07f + wicket_p);
    dot_p = std::max(0.10f, dot_p);

    float cumulative = 0.07f + wicket_p;

    cumulative += dot_p;
    if (r < cumulative) { result.type = SHOT_DOT_BLOCK; return result; }

    result.direction_zone = rng_range(0, 9);

    cumulative += single_p;
    if (r < cumulative) {
        result.type  = SHOT_GROUND;
        result.power = 0.10f + rng_float() * 0.20f;   // low power → 1 run expected
        return result;
    }

    cumulative += two_p;
    if (r < cumulative) {
        result.type  = SHOT_GROUND;
        result.power = 0.30f + rng_float() * 0.22f;   // medium power → 2 runs expected
        return result;
    }

    cumulative += three_p;
    if (r < cumulative) {
        result.type  = SHOT_GROUND;
        result.power = 0.52f + rng_float() * 0.18f;   // higher power → 3 runs expected
        return result;
    }

    cumulative += four_p;
    if (r < cumulative) {
        result.type  = SHOT_GROUND;
        result.power = 0.72f + rng_float() * 0.20f;   // high power → boundary expected
        return result;
    }

    // Six — over the rope, pre-decided (no fielder can stop it)
    result.type  = SHOT_BOUNDARY_6;
    result.power = 0.92f + rng_float() * 0.08f;
    return result;
}

// resolve_fielding_outcome() 
/*
 * Called by batsman thread AFTER g_shot_state is fully populated by fielders.
 *
 * SHOT_AERIAL physics:
 *   winner_reach_ms < flight_time_ms  → fielder got there in time → skill check → caught or dropped
 *   winner_reach_ms >= flight_time_ms → ball fell uncaught → 2 runs
 *
 * SHOT_GROUND physics:
 *   boundary_time = 800 - power*600  (ms for ball to reach rope: 200-800ms)
 *   Fielder with min reach_ms intercepts or doesn't:
 *     reach ≥ boundary_time → FOUR
 *     reach ∈ [400, boundary) → THREE
 *     reach ∈ [200, 400)     → TWO
 *     reach < 200            → SINGLE
 */
BallOutcome resolve_fielding_outcome(const ShotResult& shot)
{
    int   winner    = g_shot_state.winner_idx;
    float reach_ms  = (winner >= 0) ? g_shot_state.winner_reach_ms : 99999.0f;

    if (shot.type == SHOT_AERIAL) {
        if (winner >= 0 && reach_ms < shot.flight_time_ms && g_shot_state.winner_caught)
            return BALL_CAUGHT;
        return BALL_TWO;   // dropped or nobody reached → ball fell, 2 runs
    }

    if (shot.type == SHOT_GROUND) {
        float boundary_time = 800.0f - shot.power * 600.0f;   // 200-800ms
        if (reach_ms >= boundary_time) return BALL_FOUR;
        if (reach_ms >= 400.0f)        return BALL_THREE;
        if (reach_ms >= 200.0f)        return BALL_TWO;
        return BALL_SINGLE;
    }

    return BALL_DOT;  // shouldn't reach here for pre-decided shots
}

// Ball outcome generation 
BallOutcome generate_outcome(const Player* bat, const Delivery* del,
                              bool enable_runout, const MatchState* ms)
{
    ShotResult s = generate_shot(bat, del, enable_runout, ms);
    switch (s.type) {
        case SHOT_WIDE:       return BALL_WIDE;
        case SHOT_NOBALL:     return BALL_NOBALL;
        case SHOT_BOWLED:     return BALL_WICKET;
        case SHOT_LBW:        return BALL_LBW;
        case SHOT_RUNOUT:     return BALL_RUNOUT;
        case SHOT_DOT_BLOCK:  return BALL_DOT;
        case SHOT_BOUNDARY_6: return BALL_SIX;
        case SHOT_AERIAL:     return BALL_CAUGHT;   // placeholder
        case SHOT_GROUND:     return BALL_SINGLE;   // placeholder
        default:              return BALL_DOT;
    }
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
    printf("_______________________________________________________________\n");
    printf("|         T20 CRICKET OS SIMULATOR — CSC-204                  |\n");
    printf("|         Pitch = CS | Players = Threads | Umpire = Kernel    |\n");
    printf("|_____________________________________________________________|\n");
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
   const char* phase_str = (ms->current_phase == PHASE_POWERPLAY) ? "POWERPLAY"
                          : (ms->current_phase == PHASE_DEATH)     ? "DEATH"
                          :                                           "MIDDLE";
    printf(COL_YELLOW);
    printf("\n┌─ SCOREBOARD ──────────────────────────────────────────┐\n");
   printf("│  Score: %d/%d   Over: %d.%d   Phase: %s   Extras: %d",
           ms->total_runs.load(), ms->wickets.load(),
           ms->current_over, ms->current_ball,
           phase_str, ms->extras);
    if (ms->innings == 2 && ms->target > 0) {
        int need      = ms->target - ms->total_runs.load();
        int overs_rem = MAX_OVERS - ms->current_over;
        printf("   Need: %d off %d overs", need, overs_rem);
    }
    printf("\n│  Intensity: %d/50\n", ms->phase_intensity);
    for (const auto& p : batsmen) {
        if (p.is_active && !p.is_out)
            printf("│  %-15s %3d(%3d)  %s\n",
                   p.name.c_str(), p.runs_scored, p.balls_faced,
                   (p.id == ms->striker_id ? "* STRIKER" : "  non-striker"));
    }
    printf("───────────────────────────────────────────────────────────────\n");
    printf(COL_RESET);
    pthread_rwlock_unlock(&scoreboard_rwlock);
    (void)bowlers;
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
    printf("\n______________________________________\n");
    printf("|          MATCH RESULT                |\n");
    printf("|______________________________________|\n");
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
    printf(COL_CYAN "  [Thread %lu] %-15s -> %s\n" COL_RESET,
           PID(p->thread_id),
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
    printf("\n_______________________________________________________________\n");
    printf("|          SJF vs FCFS BATTING ORDER ANALYSIS                   |\n");
    printf("|_______________________________________________________________|\n");
    printf("|  %-14s  %-4s  %-10s  %-10s  %-8s |\n",
           "Batsman", "Pos", "FCFS Wait", "SJF Wait", "Saved");
    printf("|_______________________________________________________________|\n");

    for (size_t i = 0; i < fcfs.size() && i < sjf.size(); i++) {
        double fcfs_w = fcfs[i].total_wait_ms;
        double sjf_w  = sjf[i].total_wait_ms;
        double saved  = fcfs_w - sjf_w;
        printf("|  %-14s  %-4zu  %8.1fms  %8.1fms  %+7.1fms |\n",
               fcfs[i].name.c_str(), i+1, fcfs_w, sjf_w, saved);
    }
    printf("|_______________________________________________________________|\n");
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

/*
  player_threads.cpp — T20 Cricket OS Simulator
 --------------------------------------------------------------------------------------
 
  THREAD OVERVIEW:
 *   bowler_thread_func  — Writer: acquires pitch_mutex, delivers ball, signals batsman
 *   batsman_thread_func — Reader: waits for delivery signal, plays shot, updates score
 *   fielder_thread_func — Blocked: cond_wait until ball_in_air broadcast
 *   umpire_thread_func  — Daemon: monitors for deadlock and match-end conditions

  SYNCHRONISATION:
 *   pitch_mutex          — Critical Section: 1 delivery at a time
 *   score_mutex          — Atomic score updates (Wide+Single same cycle safe)
 *   crease_semaphore(2)  — Max 2 batsmen at crease simultaneously
 *   delivery_ready_cond  — Bowler -> Striker signal (ball in pitch buffer)
 *   stroke_done_cond     — Striker -> Bowler signal (shot played, pitch free)
 *   ball_hit_cond        — Striker -> ALL Fielders broadcast (ball in air) 
 */

#include "../include/player_threads.h"
#include "../include/synchronization.h"
#include "../include/utils.h"
#include "../include/globals.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <unistd.h>

// Per-ball result shared between bowler and batsman 
// Protected by score_mutex — set by batsman, read by bowler for stats

static BallOutcome g_last_outcome  = BALL_DOT;
static int         g_last_runs     = 0;
static pthread_mutex_t g_res_mutex = PTHREAD_MUTEX_INITIALIZER;
int g_striker_id_local=-1;

/* Striker flag:
* which player ID is currently the striker
* Prevents non-striker from consuming delivery signals
* Written under state_mutex, read under delivery_mutex
* synced from ms->striker_id at delivery time
*/
//-----------------------------------------------------------------------------
//  BOWLER THREAD
//  Role: Writer - owns the pitch (Critical Section) for each delivery

void* bowler_thread_func(void* arg) {
    BowlerArgs* a  = (BowlerArgs*)arg;
    Player*     p  = a->player;
    MatchState* ms = a->match;

    p->state = THREAD_RUNNING;
    log_msg(LOG_THREAD, "[T-%04lu] BOWLER '%s' START — over %d",
            pthread_self() % 10000, p->name.c_str(), ms->current_over);

    int legal_balls = 0;
    while (legal_balls < a->balls_to_bowl && !ms->match_over) {

        // ENTER Critical Section (Pitch) 
        // Only one bowler delivers at a time - pitch_mutex enforces this
        double t_acquire = get_time_ms();
        safe_mutex_lock(&pitch_mutex, "pitch_mutex");

        // Build delivery and write to shared pitch buffer (Producer)
        Delivery del;
        del.over_num  = ms->current_over;
        del.ball_num  = legal_balls + 1;
        del.speed_kmh = 120.0f + (float)rng_range(0, 35);
        del.line      = rng_range(0, 2);
        del.length    = rng_range(0, 3);
        del.swing     = rng_float() * 0.4f;
        del.bowler_id = p->id;
        del.is_valid  = true;

        // Write delivery under delivery_mutex so batsman sees consistent data
        safe_mutex_lock(&delivery_mutex, "delivery_mutex");


        


memcpy(&ms->pitch_buffer, &del, sizeof(Delivery));
g_striker_id_local = ms->striker_id;
delivery_ready = true;

// Signal striker
pthread_cond_signal(&delivery_ready_cond);

safe_mutex_unlock(&delivery_mutex, "delivery_mutex");

        log_msg(LOG_DEBUG, "[T-%04lu] BOWLER '%s' → %.0f km/h (over %d.%d)",
                pthread_self() % 10000, p->name.c_str(),
                del.speed_kmh, del.over_num, del.ball_num);

        // Wait for batsman to complete stroke (Consumer acknowledges)
        safe_mutex_lock(&delivery_mutex, "delivery_mutex");
        while (!stroke_done && !ms->match_over) {
   
    if (ms->striker_id < 0) {
        stroke_done = true;
        break;
    }

    pthread_cond_wait(&stroke_done_cond, &delivery_mutex);

    
}
        stroke_done = false;
        safe_mutex_unlock(&delivery_mutex, "delivery_mutex");

        double t_release = get_time_ms();
        safe_mutex_unlock(&pitch_mutex, "pitch_mutex");
        // EXIT Critical Section 

        // Read ball result for bowler stats (set by batsman under g_res_mutex)
        pthread_mutex_lock(&g_res_mutex);
        BallOutcome outcome = g_last_outcome;
        int         runs_g  = g_last_runs;
        // 🔥 FIX 1: If striker got out → invalidate striker immediately
    pthread_mutex_unlock(&g_res_mutex);

        if (outcome != BALL_WIDE && outcome != BALL_NOBALL) {
            legal_balls++;
        }
        p->balls_bowled++;
        p->runs_conceded += runs_g;
        if (outcome==BALL_WICKET || outcome==BALL_LBW || outcome==BALL_CAUGHT)
            p->wickets_taken++;

        // Record Gantt data point
        if (a->gantt_file && a->event_log && !a->event_log->empty()) {
            BallEvent& ev       = a->event_log->back();
            ev.pitch_acquire_ms = t_acquire;
            ev.pitch_release_ms = t_release;
            ev.bowler_id        = p->id;
            gantt_write_event(a->gantt_file, ev);
        }

        sleep_ms(a->config->ball_delay_ms / 4);
    }

    p->state = THREAD_DONE;
    log_msg(LOG_THREAD, "[T-%04lu] BOWLER '%s' DONE — %dW %dR econ=%.1f",
            pthread_self() % 10000, p->name.c_str(),
            p->wickets_taken, p->runs_conceded,
            (p->balls_bowled > 0) ? p->runs_conceded * 6.0f / p->balls_bowled : 0.0f);
    return NULL;
}

// -------------------------------------------------------------------------------
//  BATSMAN THREAD
//  Role: Reader - reads pitch, updates score, signals fielders

void* batsman_thread_func(void* arg) {
    BatsmanArgs* a  = (BatsmanArgs*)arg;
    Player*      p  = a->player;
    MatchState*  ms = a->match;

    // ENTER Crease: sem_wait(2) 
    // Blocks here if 2 batsmen already at crease (3rd batsman enters WAIT state)
    p->state = THREAD_WAITING;
    log_msg(LOG_THREAD, "[T-%04lu] BATSMAN '%s' → sem_wait(crease) ...",
            pthread_self() % 10000, p->name.c_str());

    double t_wait = get_time_ms();
    safe_sem_wait(&crease_semaphore, "crease");
    p->total_wait_ms = get_time_ms() - t_wait;
    p->entry_time_ms = get_time_ms();
    p->is_active     = true;
    p->state         = THREAD_RUNNING;

    int sv = 0; sem_getvalue(&crease_semaphore, &sv);
    log_msg(LOG_THREAD,
            "[T-%04lu] BATSMAN '%s' AT CREASE | wait=%.1fms | crease_sem=%d",
            pthread_self() % 10000, p->name.c_str(), p->total_wait_ms, sv);

    // Play until out
    while (!p->is_out && !ms->match_over) {

          /* Non-striker: 
           * spin-wait 
           * The non-striker has no OS work to do between balls — it waits passively.
           * This models a thread that is READY but not RUNNING (ready queue).
           */
        if (p->id != ms->striker_id) {
            p->state = THREAD_WAITING;
            sleep_ms(20);
            continue;
        }

        /* Striker:
        * wait for bowler's delivery_ready signal
        * pthread_cond_wait atomically: releases delivery_mutex and sleeps.
        * Wakes when bowler calls pthread_cond_signal(delivery_ready_cond).
        */
        safe_mutex_lock(&delivery_mutex, "delivery_mutex");
        p->state = THREAD_WAITING;

        while ((!delivery_ready && !ms->match_over && !p->is_out)
               && !ms->match_over && !p->is_out) {
            pthread_cond_wait(&delivery_ready_cond, &delivery_mutex);
        }

        if (ms->match_over || p->is_out) {
            // Ensure bowler doesn't deadlock waiting for stroke_done
            stroke_done = true;
            pthread_cond_signal(&stroke_done_cond);
            safe_mutex_unlock(&delivery_mutex, "delivery_mutex");
            break;
        }

        delivery_ready     = false;
        g_striker_id_local = -1; 

        // Read pitch buffer (Consumer side of Producer-Consumer)
        Delivery del;
        memcpy(&del, &ms->pitch_buffer, sizeof(Delivery));
        safe_mutex_unlock(&delivery_mutex, "delivery_mutex");

        p->state = THREAD_RUNNING;

        // Generate shot outcome (weighted by batsman skill vs delivery)
        BallOutcome outcome = generate_outcome(p, &del,
                                               a->config->enable_deadlock_sim);
        int runs = 0;

        // Atomic score update (score_mutex)
        // If Wide and Single happen in same clock cycle, update atomically.
        // Both operations must complete together or not at all.
        safe_mutex_lock(&score_mutex, "score_mutex");
        switch (outcome) {
            case BALL_SINGLE: runs=1; ms->total_runs+=1; p->runs_scored+=1; break;
            case BALL_TWO:    runs=2; ms->total_runs+=2; p->runs_scored+=2; break;
            case BALL_THREE:  runs=3; ms->total_runs+=3; p->runs_scored+=3; break;
            case BALL_FOUR:   runs=4; ms->total_runs+=4; p->runs_scored+=4; break;
            case BALL_SIX:    runs=6; ms->total_runs+=6; p->runs_scored+=6; break;
            case BALL_WIDE:
            case BALL_NOBALL: runs=1; ms->total_runs+=1; ms->extras+=1;    break;
            case BALL_WICKET:
            case BALL_LBW:
            case BALL_CAUGHT:
           case BALL_RUNOUT:
   { runs = 0;
    p->is_out = true;
    ms->wickets++;

    // 🔥 STEP 1: find next batsman
    Player* next_bat = nullptr;
    for (auto& bp : *a->all_batsmen) {
        if (!bp.is_out && !bp.is_active) {
            next_bat = &bp;
            break;
        }
    }

    if (next_bat) {
        log_msg(LOG_EVENT, "NEW BATSMAN: '%s' coming to crease",
                next_bat->name.c_str());

        // 🔥 STEP 2: update striker + non-striker
        safe_mutex_lock(&state_mutex, "state_mutex");

        for (auto& bp : *a->all_batsmen) {
            if (bp.is_active && !bp.is_out && bp.id != p->id) {
                ms->non_striker_id = bp.id;
                break;
            }
        }

        ms->striker_id = next_bat->id;

        safe_mutex_unlock(&state_mutex, "state_mutex");

        // 🔥 STEP 3: spawn new batsman thread
        BatsmanArgs* nba = new BatsmanArgs{
            next_bat, a->match, a->config, a->all_batsmen, a->event_log,
            true, get_time_ms()
        };

        spawn_batsman(next_bat, nba);
    }

    // 🔥 STEP 4: wake bowler
    safe_mutex_lock(&delivery_mutex, "delivery_mutex");
    stroke_done = true;
    pthread_cond_signal(&stroke_done_cond);
    safe_mutex_unlock(&delivery_mutex, "delivery_mutex");

    break;}
            default:          runs=0;                                        break;
        }
        if (outcome != BALL_WIDE && outcome != BALL_NOBALL) {
            p->balls_faced++;
            ms->current_ball++;
        }
        safe_mutex_unlock(&score_mutex, "score_mutex");
        // End atomic update 

        // Share result with bowler thread 
        pthread_mutex_lock(&g_res_mutex);
        g_last_outcome = outcome;
        g_last_runs    = runs;
        pthread_mutex_unlock(&g_res_mutex);

        // Build and log ball event
        BallEvent ev;
        ev.timestamp_ms = get_time_ms();
        ev.over_num     = del.over_num;
        ev.ball_num     = ms->current_ball;
        ev.bowler_id    = del.bowler_id;
        ev.batsman_id   = p->id;
        ev.outcome      = outcome;
        ev.runs_scored  = runs;
        char cbuf[256];
        snprintf(cbuf, sizeof(cbuf), "Over %d.%d  %-15s  %s",
                 del.over_num, ms->current_ball, p->name.c_str(),
                 outcome_to_string(outcome).c_str());
        ev.commentary   = std::string(cbuf);

        if (a->event_log) {
            safe_mutex_lock(&state_mutex, "state_mutex");
            a->event_log->push_back(ev);
            safe_mutex_unlock(&state_mutex, "state_mutex");
        }

        // Ball-by-ball commentary
        log_msg(LOG_EVENT, "  %2d.%d  %-15s  %s%4s%s   %d/%d",
                del.over_num, ms->current_ball, p->name.c_str(),
                outcome_color(outcome).c_str(),
                outcome_to_string(outcome).c_str(), COL_RESET,
                ms->total_runs.load(), ms->wickets.load());

        // Wake fielders: broadcast ball_in_air
        // All 10 fielder threads wake up and compete to field the ball.
        // Only one succeeds (field_mutex trylock). Others go back to sleep.
        if (outcome==BALL_FOUR  || outcome==BALL_SIX    ||
            outcome==BALL_SINGLE|| outcome==BALL_TWO    ||
            outcome==BALL_THREE || outcome==BALL_CAUGHT) {
            safe_mutex_lock(&ball_hit_mutex, "ball_hit_mutex");
            ms->ball_in_air = true;
            pthread_cond_broadcast(&ball_hit_cond);  // wake ALL fielders
            safe_mutex_unlock(&ball_hit_mutex, "ball_hit_mutex");
        }

        // Deadlock simulation (run-out)
        if (outcome == BALL_RUNOUT && a->config->enable_deadlock_sim) {
            Player* partner = nullptr;
            for (auto& bp : *a->all_batsmen)
                if (bp.id == ms->non_striker_id && !bp.is_out)
                    { partner = &bp; break; }
            if (partner) simulate_runout_attempt(p, partner, ms);
        }

        // Strike rotation 
        if (!p->is_out && !ms->match_over) {
            safe_mutex_lock(&state_mutex, "state_mutex");
            if (runs % 2 == 1 || ms->current_ball >= BALLS_PER_OVER)
                std::swap(ms->striker_id, ms->non_striker_id);
            safe_mutex_unlock(&state_mutex, "state_mutex");
        }

        // Signal bowler: stroke complete, pitch is free 
        safe_mutex_lock(&delivery_mutex, "delivery_mutex");
        stroke_done = true;
        pthread_cond_signal(&stroke_done_cond);
        safe_mutex_unlock(&delivery_mutex, "delivery_mutex");

        sleep_ms(a->config->ball_delay_ms / 3);
    }

    // Exit Crease: sem_post
    // Release crease slot - allows next batsman's sem_wait to proceed
    p->is_active = false;
    p->state     = THREAD_DONE;

    safe_mutex_lock(&delivery_mutex, "delivery_mutex");
    if (!stroke_done) {
        stroke_done = true;
        pthread_cond_signal(&stroke_done_cond);
    }
    safe_mutex_unlock(&delivery_mutex, "delivery_mutex");

    safe_sem_post(&crease_semaphore, "crease");
    sv = 0; sem_getvalue(&crease_semaphore, &sv);
    log_msg(LOG_THREAD,
            "[T-%04lu] BATSMAN '%s' LEFT crease → %d(%d balls) | sem=%d",
            pthread_self() % 10000, p->name.c_str(),
            p->runs_scored, p->balls_faced, sv);
    return NULL;
}

// -------------------------------------------------------------------------------
//  FIELDER THREAD
//  Role: Passive - sleeps on cond_wait, wakes on ball_in_air broadcast

/*
 * Demonstrates: condition variables, spurious wakeup protection (while loop),
  and zero-CPU passive blocking (compared to busy-waiting).
 
 * OS Analogy: Fielders = I/O-blocked processes. They consume no CPU while
  sleeping. The OS (pthread) scheduler completely ignores them until the
  wakeup event (ball_in_air broadcast) occurs.
 */
void* fielder_thread_func(void* arg) {
    FielderArgs* a  = (FielderArgs*)arg;
    Player*      p  = a->player;
    MatchState*  ms = a->match;
    bool         wk = (a->fielder_index == 0);

    p->state = THREAD_SLEEPING;
    log_msg(LOG_THREAD, "[T-%04lu] FIELDER '%s'(%s) → cond_wait",
            pthread_self() % 10000, p->name.c_str(), wk ? "WK" : "F");

    while (!ms->match_over) {
        // BLOCK: sleep until ball_in_air 
        safe_mutex_lock(&ball_hit_mutex, "ball_hit_mutex");
        while (!ms->ball_in_air && !ms->match_over) {
            p->state = THREAD_SLEEPING;
            pthread_cond_wait(&ball_hit_cond, &ball_hit_mutex);
        }
        safe_mutex_unlock(&ball_hit_mutex, "ball_hit_mutex");
        if (ms->match_over) break;

        // WAKE: compete to field the ball 
        // Only One fielder acts per ball - field_mutex trylock ensures this.
        p->state = THREAD_RUNNING;
        if (pthread_mutex_trylock(&field_mutex) == 0) {
            if (ms->ball_in_air) {
                if      (wk && rng_chance(0.15f))
                    log_msg(LOG_DEBUG, "  WK '%s' takes catch!", p->name.c_str());
                else if (rng_chance(0.35f))
                    log_msg(LOG_DEBUG, "  FIELDER '%s' saves boundary!", p->name.c_str());
                ms->ball_in_air = false;
            }
            pthread_mutex_unlock(&field_mutex);
        }
        // Other fielders: trylock failed -> another fielder handled it -> back to sleep
    p->state = THREAD_DONE;
    log_msg(LOG_THREAD, "[T-%04lu] FIELDER '%s' exit",
            pthread_self() % 10000, p->name.c_str());
    return NULL;
}
}

// -------------------------------------------------------------------------------
//  UMPIRE THREAD (Kernel / Resource Scheduler)
//  Role: Daemon - monitors game state, detects deadlock, signals match end

void* umpire_thread_func(void* arg) {
    UmpireArgs* a  = (UmpireArgs*)arg;
    MatchState* ms = a->match;

    log_msg(LOG_THREAD, "[UMPIRE T-%04lu] Kernel thread STARTED — polling every %dms",
            pthread_self() % 10000, UMPIRE_POLL_US / 1000);

    while (!ms->match_over) {
        usleep(UMPIRE_POLL_US);

        int sv = 0;
        sem_getvalue(&crease_semaphore, &sv);
        if (sv < 0)
            log_msg(LOG_WARN, "[UMPIRE] crease_sem=%d: 3rd batsman BLOCKED", sv);

        // Deadlock detection (Resource Allocation Graph) 
        if (a->config->enable_deadlock_sim) {
            Player *bA = nullptr, *bB = nullptr;
            for (auto& p : *a->batsmen) {
                if (p.is_active && !p.is_out) {
                    if (!bA) bA = &p; else bB = &p;
                }
            }
            if (bA && bB && detect_runout_deadlock(bA, bB)) {
                log_msg(LOG_DEADLOCK, "[UMPIRE] DEADLOCK in RAG! Resolving...");
                print_resource_allocation_graph(bA, bB);
                Player* victim = (bA->batting_avg <= bB->batting_avg) ? bA : bB;
                resolve_runout(victim, ms);
                safe_mutex_lock(&delivery_mutex, "delivery_mutex");
                stroke_done = true;
                pthread_cond_signal(&stroke_done_cond);
                safe_mutex_unlock(&delivery_mutex, "delivery_mutex");
            }
        }

        // Match-end conditions 
        bool all_out = (ms->wickets.load() >= MAX_PLAYERS - 1);
        bool chased  = (ms->innings == 2 &&
                        ms->total_runs.load() >= ms->target &&
                        ms->target > 0);
        if (all_out || chased) {
            log_msg(LOG_THREAD, "[UMPIRE] Match-end: all_out=%d chased=%d",
                    all_out, chased);
            safe_mutex_lock(&state_mutex, "state_mutex");
            ms->match_over = true;
            safe_mutex_unlock(&state_mutex, "state_mutex");
            // Wake all waiting threads
            safe_mutex_lock(&ball_hit_mutex, "ball_hit_mutex");
            ms->ball_in_air = true;
            pthread_cond_broadcast(&ball_hit_cond);
            safe_mutex_unlock(&ball_hit_mutex, "ball_hit_mutex");
            safe_mutex_lock(&delivery_mutex, "delivery_mutex");
            delivery_ready = true; stroke_done = true;
            pthread_cond_broadcast(&delivery_ready_cond);
            pthread_cond_broadcast(&stroke_done_cond);
            safe_mutex_unlock(&delivery_mutex, "delivery_mutex");
        }
    }

    log_msg(LOG_THREAD, "[UMPIRE] Exiting");
    return NULL;
}

// -------------------------------------------------------------------------------
// SCOREBOARD THREAD

void* scoreboard_thread_func(void* arg) {
    ScoreboardArgs* a = (ScoreboardArgs*)arg;
    while (!a->match->match_over) {
        sleep_ms(6000);
        if (!a->match->match_over)
            print_scoreboard(a->match, *a->batsmen, *a->bowlers);
    }
    return NULL;
}

// -------------------------------------------------------------------------------
//  THREAD MANAGEMENT HELPERS

void spawn_bowler(Player* p, BowlerArgs* args) {
    p->state = THREAD_READY;
    int r = pthread_create(&p->thread_id, NULL, bowler_thread_func, args);
    if (r != 0) log_msg(LOG_WARN, "pthread_create(bowler) failed: %d", r);
    else {
        get_timespec_now(&p->start_time);
        log_msg(LOG_THREAD, "Spawned BOWLER '%s' [T-%04lu]",
                p->name.c_str(), p->thread_id % 10000);
    }
}

void spawn_batsman(Player* p, BatsmanArgs* args) {
    p->state = THREAD_READY;
    int r = pthread_create(&p->thread_id, NULL, batsman_thread_func, args);
    if (r != 0) log_msg(LOG_WARN, "pthread_create(batsman) failed: %d", r);
    else {
        get_timespec_now(&p->start_time);
        log_msg(LOG_THREAD, "Spawned BATSMAN '%s' [T-%04lu]",
                p->name.c_str(), p->thread_id % 10000);
    }
}

void spawn_all_fielders(std::vector<Player>& fielders,
                        std::vector<FielderArgs>& args,
                        MatchState* ms, MatchConfig* cfg) {
    for (size_t i = 0; i < fielders.size(); i++) {
        args[i] = {&fielders[i], ms, cfg, (int)i};
        fielders[i].state = THREAD_READY;
        pthread_create(&fielders[i].thread_id, NULL, fielder_thread_func, &args[i]);
    }
    log_msg(LOG_THREAD, "Spawned %zu fielder threads (all sleeping on cond_wait)",
            fielders.size());
}

void join_all_fielders(std::vector<Player>& fielders) {
    for (auto& f : fielders)
        if (f.thread_id) { pthread_join(f.thread_id, NULL); f.thread_id = 0; }
    log_msg(LOG_THREAD, "All fielder threads joined");
}

void terminate_batsman(Player* p, MatchState*) {
    p->is_active = false;
    p->state     = THREAD_DONE;
}

// -------------------------------------------------------------------------------
//  DEADLOCK SIMULATION: Run-Out (Circular Wait)

/*
  Four necessary conditions for deadlock - all satisfied in a run-out:
   1. Mutual Exclusion : only 1 batsman per end at a time
   2. Hold and Wait    : holds current end while wanting other end
   3. No Preemption    : cannot force batsman backward
   4. Circular Wait    : A waits for B's end; B waits for A's end → CYCLE
 
 * Prevention strategy: Resource Ordering
    Always acquire End0 before End1. Circular wait becomes impossible.
 
 * Detection strategy: Resource Allocation Graph (RAG) cycle detection
    Umpire thread scans holds_end/wants_end fields each poll cycle.
 */
void simulate_runout_attempt(Player* a, Player* b, MatchState*) {
    log_msg(LOG_DEADLOCK,
            " RUN-OUT SCENARIO: %s ↔ %s (circular wait on crease ends) ",
            a->name.c_str(), b->name.c_str());
    // Set up the circular wait state in RAG
    a->holds_end = 0;  b->holds_end = 1;
    a->wants_end = 1;  b->wants_end = 0;
    print_resource_allocation_graph(a, b);
    log_msg(LOG_DEADLOCK,
            "PREVENTION: Resource ordering (always acquire End0→End1) breaks cycle");
    log_msg(LOG_DEADLOCK,
            "DETECTION:  Umpire RAG scan detects cycle → declares victim RUN OUT");
}

bool detect_runout_deadlock(Player* a, Player* b) {
    return (a->holds_end != -1 && b->holds_end != -1 &&
            a->holds_end == b->wants_end &&
            b->holds_end == a->wants_end);
}

void resolve_runout(Player* victim, MatchState* ms) {
    log_msg(LOG_DEADLOCK, "RESOLUTION: '%s' declared RUN OUT (avg=%.1f)",
            victim->name.c_str(), victim->batting_avg);
    log_msg(LOG_DEADLOCK,
            "  OS analogy: kernel kills deadlocked thread, releases all held resources");
    victim->holds_end = -1;
    victim->wants_end = -1;
    victim->is_out    = true;
    safe_mutex_lock(&score_mutex, "score_mutex");
    ms->wickets++;
    safe_mutex_unlock(&score_mutex, "score_mutex");
    log_msg(LOG_SCORE, "RUN OUT — Score: %d/%d",
            ms->total_runs.load(), ms->wickets.load());
}

void print_resource_allocation_graph(Player* a, Player* b) {
    log_msg(LOG_DEADLOCK, "");
    log_msg(LOG_DEADLOCK, "  Resource Allocation Graph (RAG):");
    log_msg(LOG_DEADLOCK, "  ┌──────────────────────────────────────────────┐");
    log_msg(LOG_DEADLOCK, "  │  %-12s ──holds──► [End %d]              │",
            a->name.c_str(), a->holds_end);
    log_msg(LOG_DEADLOCK, "  │  %-12s ◄─wants─── [End %d]              │",
            a->name.c_str(), a->wants_end);
    log_msg(LOG_DEADLOCK, "  │                                              │");
    log_msg(LOG_DEADLOCK, "  │  %-12s ──holds──► [End %d]              │",
            b->name.c_str(), b->holds_end);
    log_msg(LOG_DEADLOCK, "  │  %-12s ◄─wants─── [End %d]              │",
            b->name.c_str(), b->wants_end);
    log_msg(LOG_DEADLOCK, "  │                                              │");
    log_msg(LOG_DEADLOCK, "  │  CYCLE: %s→[E%d]→%s→[E%d]→%s         │",
            a->name.c_str(), a->wants_end,
            b->name.c_str(), b->wants_end,
            a->name.c_str());
    log_msg(LOG_DEADLOCK, "  └──────────────────────────────────────────────┘");
    log_msg(LOG_DEADLOCK, "");
}


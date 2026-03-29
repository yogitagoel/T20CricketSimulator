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
#include <cstdint>
#define TID()  ((unsigned long)((uintptr_t)pthread_self() % 10000u))
#define PID(t) ((unsigned long)((uintptr_t)(t)            % 10000u))
 // Making compatible for MacOS
 #ifdef __APPLE__
 #  pragma clang diagnostic push
 #  pragma clang diagnostic ignored "-Wdeprecated-declarations"
 static inline void sem_val(sem_t* s, int* v){ sem_getvalue(s, v); }
 #  pragma clang diagnostic pop
 #else
 static inline void sem_val(sem_t* s, int* v){ sem_getvalue(s, v); }
 #endif


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

//  BOWLER THREAD
//  Role: Writer - owns the pitch (Critical Section) for each delivery

void* bowler_thread_func(void* arg) {
    BowlerArgs* a  = (BowlerArgs*)arg;
    Player*     p  = a->player;
    MatchState* ms = a->match;

    p->state = THREAD_RUNNING;
    log_msg(LOG_THREAD, "[T-%04lu] BOWLER '%s' START — over %d",
             TID(), p->name.c_str(), ms->current_over);
  
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
pthread_cond_broadcast(&delivery_ready_cond);

safe_mutex_unlock(&delivery_mutex, "delivery_mutex");

        log_msg(LOG_DEBUG, "[T-%04lu] BOWLER '%s' → %.0f km/h (over %d.%d)",
                TID(), p->name.c_str(),
                del.speed_kmh, del.over_num - 1, del.ball_num);

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
        // Scenario: If striker got out → invalidate striker immediately
    pthread_mutex_unlock(&g_res_mutex);

        if (outcome != BALL_WIDE && outcome != BALL_NOBALL) {
            legal_balls++;
        }
       if (outcome != BALL_WIDE && outcome != BALL_NOBALL) p->balls_bowled++;
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
            TID(), p->name.c_str(),
            p->wickets_taken, p->runs_conceded,
            (p->balls_bowled > 0) ? p->runs_conceded * 6.0f / p->balls_bowled : 0.0f);
    return NULL;
}


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
            TID(), p->name.c_str());

    double t_wait = get_time_ms();
    safe_sem_wait(crease_semaphore, "crease");
    p->total_wait_ms = get_time_ms() - t_wait;
    p->entry_time_ms = get_time_ms();
    p->is_active     = true;
    p->state         = THREAD_RUNNING;

    int sv = 0; sem_val(crease_semaphore, &sv);
    log_msg(LOG_THREAD,
            "[T-%04lu] BATSMAN '%s' AT CREASE | wait=%.1fms | crease_sem=%d",
            TID(), p->name.c_str(), p->total_wait_ms, sv);
 
    // Play until out
    while (!p->is_out && !ms->match_over) {

         // Block until THIS batsman becomes the striker -
         // OS CONCEPT: Condition Variable — non-striker yields the CPU entirely
         // (pthread_cond_wait releases the mutex and sleeps the thread).
         // It wakes only when striker_id changes, eliminating the busy-spin.
       safe_mutex_lock(&striker_changed_mutex, "striker_changed_mutex");
       while (p->id != ms->striker_id && !p->is_out && !ms->match_over) {
             p->state = THREAD_WAITING;
             log_msg(LOG_DEBUG,
                     "[T-%04lu] BATSMAN '%s' NON-STRIKER — cond_wait(striker_changed)",
                     TID(), p->name.c_str());
             pthread_cond_wait(&striker_changed_cond, &striker_changed_mutex);
         }
         safe_mutex_unlock(&striker_changed_mutex, "striker_changed_mutex");
         if (p->is_out || ms->match_over) break;
         p->state = THREAD_RUNNING;

        /* Striker:
        * wait for bowler's delivery_ready signal
        * pthread_cond_wait atomically: releases delivery_mutex and sleeps.
        * Wakes when bowler calls pthread_cond_signal(delivery_ready_cond).
        */
        safe_mutex_lock(&delivery_mutex, "delivery_mutex");
        p->state = THREAD_WAITING;

        while (!delivery_ready && !ms->match_over && !p->is_out)
             pthread_cond_wait(&delivery_ready_cond, &delivery_mutex);

        if (ms->match_over || p->is_out) {
            // Ensure bowler doesn't deadlock waiting for stroke_done
            stroke_done = true;
            pthread_cond_signal(&stroke_done_cond);
            safe_mutex_unlock(&delivery_mutex, "delivery_mutex");
            break;
        }

        delivery_ready     = false;
        g_striker_id_local = -1; 

         // RACE GUARD: over-end swap may have fired while we were blocked ─
         // Scenario: this batsman passed the striker_changed_cond gate just
         // before match_engine did the end-of-over swap, so we might be the
         // NON-STRIKER who consumed delivery_ready incorrectly.  Re-check:
         // if no longer striker, put the token back and loop so the real
         // striker can pick it up.
         if (p->id != ms->striker_id) {
             delivery_ready = true;
             pthread_cond_broadcast(&delivery_ready_cond);
             safe_mutex_unlock(&delivery_mutex, "delivery_mutex");
             continue;   // re-enters outer while -> blocks at striker_changed_cond
         }
      
        // Read pitch buffer (Consumer side of Producer-Consumer)
        Delivery del;
        memcpy(&del, &ms->pitch_buffer, sizeof(Delivery));
        safe_mutex_unlock(&delivery_mutex, "delivery_mutex");

        p->state = THREAD_RUNNING;

       // ── PHASE 1: Batsman reads ball → decides shot type / direction / power ──
       ShotResult shot = generate_shot(p, &del, a->config->enable_deadlock_sim, ms);
      
        // Generate shot outcome 
       BallOutcome outcome;
         bool is_race       = (shot.type == SHOT_AERIAL || shot.type == SHOT_GROUND);
         bool needs_fielding = (shot.type != SHOT_WIDE && shot.type != SHOT_NOBALL);

         if (needs_fielding) {
             // Publish shot data so every fielder thread can read it lock-free
             shot_state_reset(ms->active_fielders);   // ← exact count, prevents hang
             g_shot_state.type           = (int)shot.type;
             g_shot_state.direction_zone = shot.direction_zone;
             g_shot_state.power          = shot.power;
             g_shot_state.flight_time_ms = shot.flight_time_ms;

             // ── PHASE 2: Broadcast — all fielder threads wake simultaneously ────
             safe_mutex_lock(&ball_hit_mutex, "ball_hit_mutex");
             ms->ball_in_air = true;
             pthread_cond_broadcast(&ball_hit_cond);
             safe_mutex_unlock(&ball_hit_mutex, "ball_hit_mutex");

             // ── PHASE 3: Wait — last fielder signals us when race is done ───────
             // Safety: also exits if match ends mid-ball (e.g. umpire kills match)
             // to prevent infinite hang if a fielder thread exited early.
             safe_mutex_lock(&g_shot_state.mutex, "g_shot_state.mutex");
             struct timespec deadline;
             clock_gettime(CLOCK_REALTIME, &deadline);
             deadline.tv_sec += 2;   // 2-second hard timeout — should never fire
             while (!g_shot_state.fielding_done.load() && !ms->match_over)
                 pthread_cond_timedwait(&g_shot_state.cond,
                                        &g_shot_state.mutex, &deadline);
             safe_mutex_unlock(&g_shot_state.mutex, "g_shot_state.mutex");
         }

         // ── PHASE 4: Resolve final outcome ──────────────────────────────────────
         if (is_race) {
             outcome = resolve_fielding_outcome(shot);
             if (outcome == BALL_CAUGHT) {
                 strncpy(p->caught_by,  g_shot_state.winner_name, 63);
                 p->caught_by[63]  = '\0';
                 strncpy(p->caught_pos, g_shot_state.winner_pos,  31);
                 p->caught_pos[31] = '\0';
                 log_msg(LOG_EVENT, "  c %-18s @ %-13s — '%s' OUT",
                         g_shot_state.winner_name, g_shot_state.winner_pos,
                         p->name.c_str());
             }
         } else {
             switch (shot.type) {
                 case SHOT_WIDE:       outcome = BALL_WIDE;   break;
                 case SHOT_NOBALL:     outcome = BALL_NOBALL; break;
                 case SHOT_BOWLED:     outcome = BALL_WICKET; break;
                 case SHOT_LBW:        outcome = BALL_LBW;    break;
                 case SHOT_RUNOUT:     outcome = BALL_RUNOUT; break;
                 case SHOT_DOT_BLOCK:  outcome = BALL_DOT;    break;
                 case SHOT_BOUNDARY_6: outcome = BALL_SIX;    break;
                 default:              outcome = BALL_DOT;    break;
             }
         }
         int runs = 0;
 
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
             case BALL_RUNOUT: {
                 runs = 0;
                 p->is_out = true;
                 p->how_out = outcome;
                 p->dismissed_by_id = del.bowler_id;
                 ms->wickets++;
 
                 Player* next_bat = nullptr;
                 for (auto& bp : *a->all_batsmen) {
                     if (!bp.is_out && !bp.is_active) { next_bat = &bp; break; }
                 }
 
                 if (next_bat) {
                     log_msg(LOG_EVENT, "NEW BATSMAN: '%s' coming to crease",
                             next_bat->name.c_str());
                     safe_mutex_lock(&state_mutex, "state_mutex");
                     for (auto& bp : *a->all_batsmen) {
                         if (bp.is_active && !bp.is_out && bp.id != p->id) {
                             ms->non_striker_id = bp.id; break;
                         }
                     }
                     ms->striker_id = next_bat->id;
                     safe_mutex_unlock(&state_mutex, "state_mutex");

                     // Wake the newly promoted striker (OS: signal on cond var)
                     safe_mutex_lock(&striker_changed_mutex, "striker_changed_mutex");
                     pthread_cond_broadcast(&striker_changed_cond);
                     safe_mutex_unlock(&striker_changed_mutex, "striker_changed_mutex");
 
                     BatsmanArgs* nba = new BatsmanArgs{
                         next_bat, a->match, a->config, a->all_batsmen,
                         a->event_log, true, get_time_ms()
                     };
                     spawn_batsman(next_bat, nba);
                 }
 
                 safe_mutex_lock(&delivery_mutex, "delivery_mutex");
                 stroke_done = true;
                 pthread_cond_signal(&stroke_done_cond);
                 safe_mutex_unlock(&delivery_mutex, "delivery_mutex");
                 break;
             }
             default: runs=0; break;
         }
 
         if (outcome != BALL_WIDE && outcome != BALL_NOBALL) {
             p->balls_faced++;
             ms->current_ball++;
         }
         safe_mutex_unlock(&score_mutex, "score_mutex");
 
         pthread_mutex_lock(&g_res_mutex);
         g_last_outcome = outcome;
         g_last_runs    = runs;
         pthread_mutex_unlock(&g_res_mutex);
 
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
                  del.over_num - 1, ms->current_ball, p->name.c_str(),
                  outcome_to_string(outcome).c_str());
         ev.commentary = std::string(cbuf);
         if (a->event_log) {
             safe_mutex_lock(&state_mutex, "state_mutex");
             a->event_log->push_back(ev);
             safe_mutex_unlock(&state_mutex, "state_mutex");
         }
 
         log_msg(LOG_EVENT, "  %2d.%d  %-15s  %s%4s%s   %d/%d",
                 del.over_num - 1, ms->current_ball, p->name.c_str(),
                 outcome_color(outcome).c_str(),
                 outcome_to_string(outcome).c_str(), COL_RESET,
                 ms->total_runs.load(), ms->wickets.load());
 
         // (ball_in_air broadcast + fielder race already done above for legal balls)
 
         if (outcome == BALL_RUNOUT && a->config->enable_deadlock_sim) {
             Player* partner = nullptr;
             for (auto& bp : *a->all_batsmen)
                 if (bp.id == ms->non_striker_id && !bp.is_out)
                     { partner = &bp; break; }
             if (partner) simulate_runout_attempt(p, partner, ms);
         }
 
         if (!p->is_out && !ms->match_over) {
             // CREASE SWAP RULES:
             //  Mid-over odd batting runs (1, 3)  -> batsmen cross -> swap here
             //  Mid-over even runs (0, 2, 4, 6)   -> no cross -> no swap
             //  Wide / No-ball                    -> no swap; re-bowl, same striker
             //  End of over                       -> match_engine.cpp handles swap
             //                                       only after bowler thread joins.
             //                                       Do not swap here too or it
             //                                       double-cancels and the wrong
             //                                       batter faces the next over.
             bool is_extra        = (outcome == BALL_WIDE || outcome == BALL_NOBALL);
             bool batting_runs_odd = !is_extra && (runs % 2 == 1);
             safe_mutex_lock(&state_mutex, "state_mutex");
             if (batting_runs_odd)
                 std::swap(ms->striker_id, ms->non_striker_id);
             safe_mutex_unlock(&state_mutex, "state_mutex");
             // Notify both batsmen that striker may have changed
             safe_mutex_lock(&striker_changed_mutex, "striker_changed_mutex");
             pthread_cond_broadcast(&striker_changed_cond);
             safe_mutex_unlock(&striker_changed_mutex, "striker_changed_mutex");
         }
 
         safe_mutex_lock(&delivery_mutex, "delivery_mutex");
         stroke_done = true;
         pthread_cond_signal(&stroke_done_cond);
         safe_mutex_unlock(&delivery_mutex, "delivery_mutex");
 
         sleep_ms(a->config->ball_delay_ms / 3);
     }
 
     p->is_active = false;
     p->state     = THREAD_DONE;
 
     safe_mutex_lock(&delivery_mutex, "delivery_mutex");
     if (!stroke_done) { stroke_done = true; pthread_cond_signal(&stroke_done_cond); }
     safe_mutex_unlock(&delivery_mutex, "delivery_mutex");
 
     safe_sem_post(crease_semaphore, "crease");
     sem_val(crease_semaphore, &sv);
     log_msg(LOG_THREAD, "[T-%04lu] BATSMAN '%s' LEFT crease -> %d(%d balls) | sem=%d",
             TID(), p->name.c_str(),
             p->runs_scored, p->balls_faced, sv);
     return NULL;
 }
//  FIELDER THREAD
//  Role: Passive - sleeps on cond_wait, wakes on ball_in_air broadcast

/*
  * Demonstrates: condition variables, spurious wakeup protection (while loop),
  and zero-CPU passive blocking (compared to busy-waiting).
 
  * OS Analogy: Fielders = I/O-blocked processes. They consume no CPU while
  sleeping. The OS (pthread) scheduler completely ignores them until the
  wakeup event (ball_in_air broadcast) occurs.
  * Each fielder occupies one of 10 named cricket positions (WK + 9 outfielders).
  * When a ball is hit, ALL fielders wake, become RUNNING, and attempt to field.
  * The fielder whose zone matches the ball direction has the highest catch
  * probability; every other fielder also attempts with a lower probability.
  *
  * Catch result is written atomically to the global catch_result struct.
  * The batsman thread reads catch_result.caught after ball_in_air clears
  * and uses that to confirm and log the dismissal with the catcher's name.
  *
  * Positions (fielder_index → FIELDING_POSITIONS[]):
  *   0=Wicket Keeper  1=Fine Leg     2=Square Leg  3=Mid Wicket
  *   4=Mid On         5=Long On      6=Long Off     7=Mid Off
  *   8=Cover Point    9=Third Man
  */
void* fielder_thread_func(void* arg) {
     FielderArgs* a    = (FielderArgs*)arg;
     Player*      p    = a->player;
     MatchState*  ms   = a->match;
     int          idx  = a->fielder_index;   // 0-9
     bool         wk   = (idx == 0);
     const char*  pos  = FIELDING_POSITIONS[idx % 10];

     p->state = THREAD_SLEEPING;
     log_msg(LOG_THREAD, "[T-%04lu] FIELDER '%s' @ %-13s -> cond_wait",
             TID(), p->name.c_str(), pos);

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
        log_msg(LOG_DEBUG, "  [FIELD] %-13s '%s' ACTIVE — ball in play",
                 pos, p->name.c_str());

         // Snapshot shot type written by batsman before broadcast
         int shot_type = g_shot_state.type;

         // ── Compute reach time: how fast can THIS fielder get to the ball? ──
         // direction_zone (0-9) was set by batsman; circular field geometry.
         int ball_zone    = g_shot_state.direction_zone;
         int fielder_zone = wk ? 0 : idx;
         int zone_dist;
         if (wk) {
             zone_dist = 0;   // WK is always stationed for edges / nicks
         } else {
             zone_dist = abs(fielder_zone - ball_zone);
             if (zone_dist > 5) zone_dist = 10 - zone_dist;   // circular wrap
         }

         // Base reach-time table (ms) by zone distance 0..5
         static const float ZONE_BASE_MS[6] = {80.f, 210.f, 360.f, 510.f, 650.f, 790.f};
         float base_reach  = ZONE_BASE_MS[zone_dist > 5 ? 5 : zone_dist];
         float skill_bonus = (p->skill_rating / 100.0f) * 80.0f; // up to 80ms faster
         float jitter      = (float)rng_range(-45, 45);
         float reach_ms    = base_reach - skill_bonus + jitter;
         if (reach_ms < 40.0f) reach_ms = 40.0f;

         // Write to own lock-free slot (index i → unique slot, no contention)
         g_shot_state.reach_ms[idx]     = reach_ms;
         g_shot_state.skill_rating[idx] = p->skill_rating;
         strncpy(g_shot_state.fielder_name[idx], p->name.c_str(), 63);
         g_shot_state.fielder_name[idx][63] = '\0';
         strncpy(g_shot_state.fielder_pos[idx],  pos, 31);
         g_shot_state.fielder_pos[idx][31]  = '\0';

         log_msg(LOG_DEBUG,
                 "  [FIELD] %-13s '%-18s' zone=%d dist=%d reach=%.0fms",
                 pos, p->name.c_str(), ball_zone, zone_dist, reach_ms);

         // ── Atomic countdown — last voter (fetch_sub returns 1) resolves race ─
         int prev = g_shot_state.fielders_remaining.fetch_sub(1);
         if (prev == 1) {
             // I am the last fielder to vote — find the fastest one
             int   best_idx = -1;
             float best_ms  =  99999.0f;
             for (int i = 0; i < MAX_FIELDER_COUNT; i++) {
                 if (g_shot_state.reach_ms[i] < best_ms) {
                     best_ms  = g_shot_state.reach_ms[i];
                     best_idx = i;
                 }
             }

             if (best_idx >= 0 && (shot_type == SHOT_AERIAL || shot_type == SHOT_GROUND)) {
                 g_shot_state.winner_idx      = best_idx;
                 g_shot_state.winner_reach_ms = best_ms;
                 strncpy(g_shot_state.winner_name, g_shot_state.fielder_name[best_idx], 63);
                 g_shot_state.winner_name[63] = '\0';
                 strncpy(g_shot_state.winner_pos,  g_shot_state.fielder_pos[best_idx],  31);
                 g_shot_state.winner_pos[31]  = '\0';

                 if (shot_type == SHOT_AERIAL) {
                     float flight  = g_shot_state.flight_time_ms;
                     bool  reached = (best_ms < flight);
                     // Skill-weighted catch probability — WK slightly higher base
                     float cskill  = g_shot_state.skill_rating[best_idx] / 100.0f;
                     float cbase   = (best_idx == 0) ? 0.72f : 0.58f;
                     bool  caught  = reached && rng_chance(cbase + cskill * 0.28f);
                     g_shot_state.winner_caught = caught;
                     log_msg(LOG_EVENT,
                             "  [RACE] AERIAL winner='%s' @ %s reach=%.0fms flight=%.0fms — %s",
                             g_shot_state.winner_name, g_shot_state.winner_pos,
                             best_ms, flight,
                             caught ? "CAUGHT!" : (reached ? "DROPPED" : "MISSED (fell short)"));
                 } else {
                     // SHOT_GROUND — log what the outcome will be
                     float bnd = 800.0f - g_shot_state.power * 600.0f;
                     const char* res = (best_ms >= bnd)   ? "FOUR" :
                                       (best_ms >= 400.0f) ? "THREE" :
                                       (best_ms >= 200.0f) ? "TWO"   : "ONE";
                     log_msg(LOG_EVENT,
                             "  [RACE] GROUND winner='%s' @ %s reach=%.0fms boundary=%.0fms -> %s",
                             g_shot_state.winner_name, g_shot_state.winner_pos,
                             best_ms, bnd, res);
                 }
             }

             // Clear ball_in_air, then signal batsman that race is complete
             safe_mutex_lock(&ball_hit_mutex, "ball_hit_mutex");
             ms->ball_in_air = false;
             safe_mutex_unlock(&ball_hit_mutex, "ball_hit_mutex");

             safe_mutex_lock(&g_shot_state.mutex, "g_shot_state.mutex");
             g_shot_state.fielding_done.store(true);
             pthread_cond_broadcast(&g_shot_state.cond);
             safe_mutex_unlock(&g_shot_state.mutex, "g_shot_state.mutex");
         }
     }

     p->state = THREAD_DONE;
     log_msg(LOG_THREAD, "[T-%04lu] FIELDER '%s' @ %s exit",
             TID(), p->name.c_str(), pos);
     return NULL;
 }

//  UMPIRE THREAD (Kernel / Resource Scheduler)
//  Role: Daemon - monitors game state, detects deadlock, signals match end

void* umpire_thread_func(void* arg) {
    UmpireArgs* a  = (UmpireArgs*)arg;
    MatchState* ms = a->match;

    log_msg(LOG_THREAD, "[UMPIRE T-%04lu] Kernel thread STARTED — polling every %dms",
            TID(), UMPIRE_POLL_US / 1000);

    while (!ms->match_over) {
        usleep(UMPIRE_POLL_US);

        int sv = 0;
        sem_val(crease_semaphore, &sv);
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

            // Guarantee one live deadlock demonstration:-
             // OS CONCEPT: The Umpire is the kernel daemon. It uses a Resource
             // Allocation Graph (RAG) scan to detect circular waits.
             // If no natural run-out has set up the deadlock by over 2, we force
             // it here so the full detect → RAG print → resolve cycle is always
             // visible when -deadlock is passed.  Detection + resolution happen
             // in the SAME poll cycle — no second usleep() needed.
             static bool deadlock_forced = false;
             if (!deadlock_forced && bA && bB
                 && bA->holds_end == -1 && bB->holds_end == -1
                 && ms->current_over >= 2 && !ms->match_over) {
                 deadlock_forced = true;
                 log_msg(LOG_DEADLOCK,
                         "[UMPIRE] ── No natural deadlock yet — injecting scenario "
                         "(over %d) to demonstrate detection ──",
                         ms->current_over);
                 // Batsman A holds End-0, wants End-1
                 // Batsman B holds End-1, wants End-0  →  circular wait
                 bA->holds_end = 0;  bA->wants_end = 1;
                 bB->holds_end = 1;  bB->wants_end = 0;
                 log_msg(LOG_DEADLOCK,
                         "  %s holds End-0, wants End-1", bA->name.c_str());
                 log_msg(LOG_DEADLOCK,
                         "  %s holds End-1, wants End-0  →  CIRCULAR WAIT",
                         bB->name.c_str());
             }
          if (bA && bB && detect_runout_deadlock(bA, bB)) {
                 log_msg(LOG_DEADLOCK,
                         "[UMPIRE] ══ DEADLOCK DETECTED ══ RAG contains a cycle:");
                 print_resource_allocation_graph(bA, bB);
                 log_msg(LOG_DEADLOCK,
                         "  OS RESPONSE: Umpire (kernel) selects victim by lowest "
                         "batting_avg (= lowest process priority)");
                 Player* victim = (bA->batting_avg <= bB->batting_avg) ? bA : bB;
                 log_msg(LOG_DEADLOCK,
                         "  PREVENTION : Resource-ordering (End-0 before End-1) "
                         "breaks future cycles");
                 resolve_runout(victim, ms);

                 // After killing the victim thread, the match must continue
                 // 1. Make the survivor the new striker (if victim was striker)
                 // 2. Promote the next waiting batsman to non-striker
                 // 3. Spawn that batsman's thread
                 Player* survivor = (victim == bA) ? bB : bA;
                 safe_mutex_lock(&state_mutex, "state_mutex");
                 ms->striker_id     = survivor->id;
                 ms->non_striker_id = -1;
                 safe_mutex_unlock(&state_mutex, "state_mutex");

                 // Find and spawn the next batsman from the waiting list
                 Player* next_bat = nullptr;
                 for (auto& bp : *a->batsmen)
                     if (!bp.is_out && !bp.is_active) { next_bat = &bp; break; }
                 if (next_bat) {
                     log_msg(LOG_EVENT, "NEW BATSMAN: '%s' coming to crease (after run-out)",
                             next_bat->name.c_str());
                     safe_mutex_lock(&state_mutex, "state_mutex");
                     ms->non_striker_id = next_bat->id;
                     safe_mutex_unlock(&state_mutex, "state_mutex");
                     BatsmanArgs* nba = new BatsmanArgs{
                         next_bat, ms, a->config, a->batsmen,
                         a->event_log, true, get_time_ms()
                     };
                     spawn_batsman(next_bat, nba);
                 }

                 // Signal stroke_done so bowler thread doesn't hang
                 safe_mutex_lock(&delivery_mutex, "delivery_mutex");
                 stroke_done = true;
                 pthread_cond_signal(&stroke_done_cond);
                 safe_mutex_unlock(&delivery_mutex, "delivery_mutex");
                 // Wake all batsmen — survivor is now striker
                 safe_mutex_lock(&striker_changed_mutex, "striker_changed_mutex");
                 pthread_cond_broadcast(&striker_changed_cond);
                 safe_mutex_unlock(&striker_changed_mutex, "striker_changed_mutex");
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
             // Wake any non-striker threads blocked on striker_changed_cond
             safe_mutex_lock(&striker_changed_mutex, "striker_changed_mutex");
             pthread_cond_broadcast(&striker_changed_cond);
             safe_mutex_unlock(&striker_changed_mutex, "striker_changed_mutex");
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

//  THREAD MANAGEMENT HELPERS

void spawn_bowler(Player* p, BowlerArgs* args) {
    p->state = THREAD_READY;
    int r = pthread_create(&p->thread_id, NULL, bowler_thread_func, args);
    if (r != 0) log_msg(LOG_WARN, "pthread_create(bowler) failed: %d", r);
    else {
        get_timespec_now(&p->start_time);
        log_msg(LOG_THREAD, "Spawned BOWLER '%s' [T-%04lu]",
                p->name.c_str(),PID(p->thread_id));
    }
}

void spawn_batsman(Player* p, BatsmanArgs* args) {
    p->state = THREAD_READY;
    int r = pthread_create(&p->thread_id, NULL, batsman_thread_func, args);
    if (r != 0) log_msg(LOG_WARN, "pthread_create(batsman) failed: %d", r);
    else {
        get_timespec_now(&p->start_time);
        log_msg(LOG_THREAD, "Spawned BATSMAN '%s' [T-%04lu]",
                p->name.c_str(), PID(p->thread_id));
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
    victim->holds_end = -1;
    victim->wants_end = -1;
    victim->is_out    = true;
    victim->how_out   = BALL_RUNOUT; 
    safe_mutex_lock(&score_mutex, "score_mutex");
    ms->wickets++;
    safe_mutex_unlock(&score_mutex, "score_mutex");
    log_msg(LOG_SCORE, "RUN OUT — Score: %d/%d",
            ms->total_runs.load(), ms->wickets.load());
}

void print_resource_allocation_graph(Player* a, Player* b) {
     log_msg(LOG_DEADLOCK, "");
     log_msg(LOG_DEADLOCK, "  ___________________________________________________");
     log_msg(LOG_DEADLOCK, "  |        RESOURCE ALLOCATION GRAPH (RAG)           |");
     log_msg(LOG_DEADLOCK, "  |  (OS: circular-wait condition for deadlock)      |");
     log_msg(LOG_DEADLOCK, "  |__________________________________________________|");
     log_msg(LOG_DEADLOCK, "  |  Thread %-12s  ──holds──► [End %d]         |",
             a->name.c_str(), a->holds_end);
     log_msg(LOG_DEADLOCK, "  |  Thread %-12s  ◄──wants── [End %d]         |",
             a->name.c_str(), a->wants_end);
     log_msg(LOG_DEADLOCK, "  |                                                  |");
     log_msg(LOG_DEADLOCK, "  |  Thread %-12s  ──holds──► [End %d]         |",
             b->name.c_str(), b->holds_end);
     log_msg(LOG_DEADLOCK, "  |  Thread %-12s  ◄──wants── [End %d]         |",
             b->name.c_str(), b->wants_end);
     log_msg(LOG_DEADLOCK, "  |__________________________________________________|");
     log_msg(LOG_DEADLOCK, "  |  CYCLE: %s→[E%d]→%s→[E%d]→%s",
             a->name.c_str(), a->wants_end,
             b->name.c_str(), b->wants_end, a->name.c_str());
     log_msg(LOG_DEADLOCK, "  |  DETECTION  : RAG cycle scan by Umpire (kernel)  |");
     log_msg(LOG_DEADLOCK, "  |  PREVENTION : Acquire End-0 before End-1 always  |");
     log_msg(LOG_DEADLOCK, "  |  RESOLUTION : Kill lowest-priority thread (RUN OUT)|");
     log_msg(LOG_DEADLOCK, "  |__________________________________________________|");
     log_msg(LOG_DEADLOCK, "");
 }


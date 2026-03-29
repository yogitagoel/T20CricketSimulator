#ifndef SYNCHRONIZATION_H
#define SYNCHRONIZATION_H

/*
  Declares all synchronization primitives and their init/destroy functions.
 
 * OS CONCEPTS DEMONSTRATED:
 *   Mutex         : score_mutex, pitch_mutex, state_mutex, log_mutex — mutual
 *                   exclusion for score updates, pitch access, state changes.
 *   Semaphore     : crease_semaphore(2) — counting semaphore; a 3rd batsman
 *                   thread blocks in sem_wait until a crease slot is freed.
 *   Cond Var #1   : ball_hit_cond — all 10 fielder threads sleep here; batsman
 *                   broadcasts when ball is in air (pthread_cond_broadcast).
 *   Cond Var #2   : delivery_ready_cond / stroke_done_cond — producer-consumer
 *                   pipeline between bowler thread and batsman thread.
 *   Cond Var #3   : striker_changed_cond — non-striker batsman blocks here
 *                   instead of busy-spinning; signalled on every end-change.
 *   RW Lock       : scoreboard_rwlock — multiple threads read live score
 *                   concurrently; writer holds exclusive lock.
 *   Lock-free     : g_shot_state per-fielder slots + atomic fetch_sub
 *                   last-voter pattern — zero contention fielder race.
 */

#include "cricket_types.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>

// All global synchronization primitives 

// Mutex: Protects Global_Score from concurrent modification
extern pthread_mutex_t score_mutex;

// Mutex: Protects the Pitch (Critical Section) buffer
extern pthread_mutex_t pitch_mutex;

// Mutex: Protects match state transitions (wicket, over change, etc.)
extern pthread_mutex_t state_mutex;

// Mutex: Protects the fielding action 
extern pthread_mutex_t field_mutex;

// Mutex: Protects deadlock detection data structures (RAG)
extern pthread_mutex_t rag_mutex;

// Mutex: Protects log file writes
extern pthread_mutex_t log_mutex;

// Semaphore: Crease capacity = 2 
extern sem_t* crease_semaphore;

// Condition variable: Fielders sleep here until ball_in_air == true
// Batsman broadcasts after hitting; all fielders wake up and compete
extern pthread_cond_t  ball_hit_cond;
extern pthread_mutex_t ball_hit_mutex;

// Condition variable: Batsman waits for bowler to deliver
extern pthread_cond_t  delivery_ready_cond;
extern pthread_mutex_t delivery_mutex;
extern bool            delivery_ready;

// Condition variable: Bowler waits for batsman to complete stroke
extern pthread_cond_t  stroke_done_cond;
extern bool            stroke_done;

// Gives notification for striker changes replacing busy-wait for non-striker
// Non-striker batsman blocks on this cond instead of spinning with sleep_ms(20).
// Broadcast whenever striker_id changes: odd runs, wicket, end-of-over swap.
extern pthread_cond_t  striker_changed_cond;
extern pthread_mutex_t striker_changed_mutex;

// RW Lock: Scoreboard can read while game writes (non-blocking display)
extern pthread_rwlock_t scoreboard_rwlock;

// Fielder race board (batsman → fielders → batsman) ─────────────
// Batsman writes shot params before ball_in_air broadcast.
// Each fielder votes its reach_ms into its own slot.
// Last voter (fetch_sub → 0) determines winner, signals fielding_done.
extern ShotState g_shot_state;
void shot_state_reset(int actual_fielder_count = MAX_FIELDER_COUNT);  // pass real count to avoid hang

// Lifecycle functions 
void sync_init_all();    
void sync_destroy_all(); 

// Helper wrappers (add error checking) 
void safe_mutex_lock(pthread_mutex_t* m, const char* name);
void safe_mutex_unlock(pthread_mutex_t* m, const char* name);
void safe_sem_wait(sem_t* s, const char* name);
void safe_sem_post(sem_t* s, const char* name);
int  safe_sem_trywait(sem_t* s);  // Returns 0 on success, -1 if would block

#endif 

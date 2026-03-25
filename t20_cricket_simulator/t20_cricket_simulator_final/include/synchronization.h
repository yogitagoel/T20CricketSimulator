#ifndef SYNCHRONIZATION_H
#define SYNCHRONIZATION_H

/*
  synchronization.h
  --------------------------------------------------------------------------------
  Declares all synchronization primitives and their init/destroy functions.
 
 * OS CONCEPTS DEMONSTRATED:
   - Mutex      : Mutual exclusion for score updates & pitch access
   - Semaphore  : Counting semaphore for crease capacity (max 2 batsmen)
   - Cond Var   : Block fielders until ball_in_air flag is set
   - RW Lock    : Scoreboard reads vs game-state writes
 */

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

// Mutex: Protects the fielding action (only one fielder acts per ball)
extern pthread_mutex_t field_mutex;

// Mutex: Protects deadlock detection data structures (RAG)
extern pthread_mutex_t rag_mutex;

// Mutex: Protects log file writes
extern pthread_mutex_t log_mutex;

// Semaphore: Crease capacity = 2 
extern sem_t crease_semaphore;

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

// RW Lock: Scoreboard can read while game writes (non-blocking display)
extern pthread_rwlock_t scoreboard_rwlock;

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

#include "../include/cricket_types.h"
/*
  synchronization.cpp

 * Implements all synchronization primitives for the T20 Cricket Simulator.
 
 *   mutex       - Binary lock. Only one thread holds it at a time.
 *   semaphore   - Counting lock. crease_semaphore(2) = max 2 batsmen.
 *   cond var    - Suspend/resume based on a condition (ball_in_air).
 *   rwlock      - Multiple concurrent readers, exclusive writer.
 */

#include "../include/synchronization.h"
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>

// [MUTEX] Score: prevents "Wide + Single same cycle" race condition
pthread_mutex_t score_mutex        = PTHREAD_MUTEX_INITIALIZER;

// [MUTEX] Pitch: enforces Critical Section - 1 bowler delivers at a time
pthread_mutex_t pitch_mutex        = PTHREAD_MUTEX_INITIALIZER;

// [MUTEX] State: serialises over-boundary transitions
pthread_mutex_t state_mutex        = PTHREAD_MUTEX_INITIALIZER;

// [MUTEX] Field: ensures only one fielder acts per ball (prevents double-catch)
pthread_mutex_t field_mutex        = PTHREAD_MUTEX_INITIALIZER;

// [MUTEX] RAG: protects Resource Allocation Graph for deadlock detection
pthread_mutex_t rag_mutex          = PTHREAD_MUTEX_INITIALIZER;

// [MUTEX] Log: thread-safe file/console writes
pthread_mutex_t log_mutex          = PTHREAD_MUTEX_INITIALIZER;

// [SEMAPHORE] Crease: at most 2 batsmen threads active simultaneously
// If a 3rd calls sem_wait, it blocks -> simulates real cricket rule
sem_t crease_semaphore;

// [COND VAR] Ball hit: fielders sleep here; batsman broadcasts on shot
pthread_cond_t  ball_hit_cond      = PTHREAD_COND_INITIALIZER;
pthread_mutex_t ball_hit_mutex     = PTHREAD_MUTEX_INITIALIZER;

// [COND VAR] Delivery ready: batsman waits for bowler to write pitch buffer
pthread_cond_t  delivery_ready_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t delivery_mutex      = PTHREAD_MUTEX_INITIALIZER;
bool            delivery_ready      = false;

// [COND VAR] Stroke done: bowler waits for batsman to complete shot
pthread_cond_t  stroke_done_cond   = PTHREAD_COND_INITIALIZER;
bool            stroke_done        = false;

// [RWLOCK] Scoreboard: display thread reads; game threads write
pthread_rwlock_t scoreboard_rwlock = PTHREAD_RWLOCK_INITIALIZER;

// Lifecycle 

void sync_init_all() {
    // Semaphore: capacity = 2 (crease allows only 2 batsmen)
    // pshared=0 , shared between threads of the same process
    if (sem_init(&crease_semaphore, 0, CREASE_CAPACITY) != 0) {
        perror("sem_init(crease_semaphore) failed");
        exit(EXIT_FAILURE);
    }

    // Re-init mutexes with error-checking attribute for better debugging
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);

    pthread_mutex_init(&score_mutex,   &attr);
    pthread_mutex_init(&pitch_mutex,   &attr);
    pthread_mutex_init(&state_mutex,   &attr);
    pthread_mutex_init(&field_mutex,   &attr);
    pthread_mutex_init(&rag_mutex,     &attr);
    pthread_mutex_init(&log_mutex,     &attr);
    pthread_mutex_init(&ball_hit_mutex,&attr);
    pthread_mutex_init(&delivery_mutex,&attr);

    pthread_mutexattr_destroy(&attr);

    pthread_cond_init(&ball_hit_cond,       NULL);
    pthread_cond_init(&delivery_ready_cond, NULL);
    pthread_cond_init(&stroke_done_cond,    NULL);

    pthread_rwlock_init(&scoreboard_rwlock, NULL);
}

void sync_destroy_all() {
    sem_destroy(&crease_semaphore);

    pthread_mutex_destroy(&score_mutex);
    pthread_mutex_destroy(&pitch_mutex);
    pthread_mutex_destroy(&state_mutex);
    pthread_mutex_destroy(&field_mutex);
    pthread_mutex_destroy(&rag_mutex);
    pthread_mutex_destroy(&log_mutex);
    pthread_mutex_destroy(&ball_hit_mutex);
    pthread_mutex_destroy(&delivery_mutex);

    pthread_cond_destroy(&ball_hit_cond);
    pthread_cond_destroy(&delivery_ready_cond);
    pthread_cond_destroy(&stroke_done_cond);

    pthread_rwlock_destroy(&scoreboard_rwlock);
}



void safe_mutex_lock(pthread_mutex_t* m, const char* name) {
    int ret = pthread_mutex_lock(m);
    if (ret != 0) {
        fprintf(stderr, "[SYNC ERROR] pthread_mutex_lock(%s): %s\n",
                name, strerror(ret));
        
        exit(EXIT_FAILURE);
    }
}

void safe_mutex_unlock(pthread_mutex_t* m, const char* name) {
    int ret = pthread_mutex_unlock(m);
    if (ret != 0) {
        fprintf(stderr, "[SYNC ERROR] pthread_mutex_unlock(%s): %s\n",
                name, strerror(ret));
        exit(EXIT_FAILURE);
    }
}

void safe_sem_wait(sem_t* s, const char* name) {
    int ret = sem_wait(s);
    if (ret != 0 && errno != EINTR) {
        fprintf(stderr, "[SYNC ERROR] sem_wait(%s): %s\n",
                name, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

void safe_sem_post(sem_t* s, const char* name) {
    int ret = sem_post(s);
    if (ret != 0) {
        fprintf(stderr, "[SYNC ERROR] sem_post(%s): %s\n",
                name, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

int safe_sem_trywait(sem_t* s) {
    return sem_trywait(s); // Returns 0 on success, -1 (EAGAIN) if block
}

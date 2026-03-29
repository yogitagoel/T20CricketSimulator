
/*
  synchronization.cpp

 * Implements all synchronization primitives for the T20 Cricket Simulator.
 
 *   mutex       - Binary lock. Only one thread holds it at a time.
 *   semaphore   - Counting lock. crease_semaphore(2) = max 2 batsmen.
 *   cond var    - Suspend/resume based on a condition (ball_in_air).
 *   rwlock      - Multiple concurrent readers, exclusive writer.
 */

#include "../include/cricket_types.h"
#include "../include/synchronization.h"
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fcntl.h>

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
sem_t* crease_semaphore;
#define CREASE_SEM_NAME "/cricket_crease_sem"

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

pthread_cond_t  striker_changed_cond  = PTHREAD_COND_INITIALIZER;
pthread_mutex_t striker_changed_mutex = PTHREAD_MUTEX_INITIALIZER;

// [RWLOCK] Scoreboard: display thread reads; game threads write
pthread_rwlock_t scoreboard_rwlock = PTHREAD_RWLOCK_INITIALIZER;

// Fielding positions
const char* FIELDING_POSITIONS[10] = {
    "Wicket Keeper",  // 0 — behind stumps
    "Fine Leg",       // 1 — deep, leg side
    "Square Leg",     // 2 — square on leg side
    "Mid Wicket",     // 3 — mid, leg side
    "Mid On",         // 4 — straight, leg side
    "Long On",        // 5 — deep straight, leg side
    "Long Off",       // 6 — deep straight, off side
    "Mid Off",        // 7 — straight, off side
    "Cover Point",    // 8 — off side, mid-range
    "Third Man",      // 9 — fine, off side
};

//  Shot-state / fielder race board
ShotState g_shot_state = {
    /* type              */ 0,
    /* direction_zone    */ 0,
    /* power             */ 0.0f,
    /* flight_time_ms    */ 0.0f,
    /* reach_ms          */ {99999.0f,99999.0f,99999.0f,99999.0f,99999.0f,
                              99999.0f,99999.0f,99999.0f,99999.0f,99999.0f},
    /* skill_rating      */ {75,75,75,75,75,75,75,75,75,75},
    /* fielder_name      */ {},
    /* fielder_pos       */ {},
    /* winner_idx        */ -1,
    /* winner_reach_ms   */ 99999.0f,
    /* winner_caught     */ false,
    /* winner_name       */ "",
    /* winner_pos        */ "",
    /* fielders_remaining*/ ATOMIC_VAR_INIT(MAX_FIELDER_COUNT),
    /* fielding_done     */ ATOMIC_VAR_INIT(false),
    /* mutex             */ PTHREAD_MUTEX_INITIALIZER,
    /* cond              */ PTHREAD_COND_INITIALIZER,
};

void shot_state_reset(int actual_fielder_count) {
    g_shot_state.type             = 0;
    g_shot_state.direction_zone   = 0;
    g_shot_state.power            = 0.0f;
    g_shot_state.flight_time_ms   = 0.0f;
    for (int i = 0; i < MAX_FIELDER_COUNT; i++) {
        g_shot_state.reach_ms[i]     = 99999.0f;
        g_shot_state.skill_rating[i] = 75;
        g_shot_state.fielder_name[i][0] = '\0';
        g_shot_state.fielder_pos[i][0]  = '\0';
    }
    g_shot_state.winner_idx        = -1;
    g_shot_state.winner_reach_ms   = 99999.0f;
    g_shot_state.winner_caught     = false;
    g_shot_state.winner_name[0]    = '\0';
    g_shot_state.winner_pos[0]     = '\0';
    // KEY FIX: use the ACTUAL number of fielder threads spawned, not the
    // hardcoded constant.  If fewer than MAX_FIELDER_COUNT threads exist,
    // the countdown never reaches 0 and the batsman hangs forever.
    g_shot_state.fielders_remaining.store(
        actual_fielder_count > 0 ? actual_fielder_count : MAX_FIELDER_COUNT);
    g_shot_state.fielding_done.store(false);
}

// Lifecycle 

void sync_init_all() {
    // Semaphore: capacity = 2 (crease allows only 2 batsmen)
    // pshared=0 , shared between threads of the same process
    sem_unlink(CREASE_SEM_NAME);  // clean up any stale semaphore from a previous crash
    crease_semaphore = sem_open(CREASE_SEM_NAME, O_CREAT | O_EXCL, 0600, CREASE_CAPACITY);
    if (crease_semaphore == SEM_FAILED) {
        perror("sem_open(crease_semaphore) failed"); exit(EXIT_FAILURE);
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
    pthread_mutex_init(&striker_changed_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    pthread_cond_init(&ball_hit_cond,       NULL);
    pthread_cond_init(&delivery_ready_cond, NULL);
    pthread_cond_init(&stroke_done_cond,    NULL);
    pthread_cond_init(&striker_changed_cond, NULL);
    pthread_rwlock_init(&scoreboard_rwlock, NULL);
    pthread_mutex_init(&g_shot_state.mutex, &attr);
    pthread_cond_init (&g_shot_state.cond,  NULL);
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
    pthread_mutex_destroy(&striker_changed_mutex);
    pthread_cond_destroy(&ball_hit_cond);
    pthread_cond_destroy(&delivery_ready_cond);
    pthread_cond_destroy(&stroke_done_cond);
    pthread_cond_destroy(&striker_changed_cond);
    pthread_rwlock_destroy(&scoreboard_rwlock);
    pthread_mutex_destroy(&g_shot_state.mutex);
    pthread_cond_destroy (&g_shot_state.cond);
}



void safe_mutex_lock(pthread_mutex_t* m, const char* name) {
    int ret = pthread_mutex_lock(m);
    if (ret != 0) { fprintf(stderr,"[SYNC] lock(%s): %s\n",name,strerror(ret)); exit(1); }
}

void safe_mutex_unlock(pthread_mutex_t* m, const char* name) {
int ret = pthread_mutex_unlock(m);
    if (ret != 0) { fprintf(stderr,"[SYNC] unlock(%s): %s\n",name,strerror(ret)); exit(1); }}

void safe_sem_wait(sem_t* s, const char* name) {
   int ret = sem_wait(s);
    if (ret != 0 && errno != EINTR)
        { fprintf(stderr,"[SYNC] sem_wait(%s): %s\n",name,strerror(errno)); exit(1); }
}

void safe_sem_post(sem_t* s, const char* name) {
    int ret = sem_post(s);
    if (ret != 0) { fprintf(stderr,"[SYNC] sem_post(%s): %s\n",name,strerror(errno)); exit(1); }
}

int safe_sem_trywait(sem_t* s) {
    return sem_trywait(s); // Returns 0 on success, -1 (EAGAIN) if block
}

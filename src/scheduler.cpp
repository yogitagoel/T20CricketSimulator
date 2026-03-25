/* scheduler.cpp
 
 * Custom over scheduler implementing Round Robin, SJF, and Priority.
 
  ROUND ROBIN (bowlers):
    - Time Quantum = 6 balls (1 over)
    - On over completion: save bowler context 
    - Load next bowler's context 
    - No bowler bowls consecutive overs 
 
  SHORTEST JOB FIRST (batsmen):
    - Burst time = expected_balls
    - Sort batting order ascending by burst time
    - Tail-enders are promoted ahead of middle order
    - Minimises average waiting time 
 
  PRIORITY SCHEDULING (death overs):
    - Match intensity computed per over
    - When INTENSITY_HIGH: death specialist gets highest priority
    - Uses pthread_setschedparam() to set real OS thread priority
 */

#include "../include/scheduler.h"
#include "../include/utils.h"
#include "../include/synchronization.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

//SJF comparator
bool sjf_compare(const Player* a, const Player* b) {
    return a->expected_balls < b->expected_balls; // shorter burst = higher priority
}

//Constructor 
Scheduler::Scheduler(SchedulerType type, int max_overs)
    : type_(type), max_overs_(max_overs),
      context_switches_(0), next_batsman_idx_(0), rr_head_(0) {}

Scheduler::~Scheduler() {}

//Bowler management 

void Scheduler::add_bowler(Player* p) {
    bowler_queue_.push_back(p);
    // Create an empty PCB entry for this bowler
    BowlerContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.bowler_id   = p->id;
    ctx.saved_state = THREAD_READY;
    pcb_table_.push_back(ctx);
    log_msg(LOG_SCHED, "Scheduler: Bowler '%s' added to RR queue (pos %zu)",
            p->name.c_str(), bowler_queue_.size() - 1);
}

/*
  schedule_next_bowler()
  
 * The main scheduling decision point. 
 
 */
Player* Scheduler::schedule_next_bowler(MatchState* ms,
                                         std::vector<Player>& bowlers) {
    update_intensity(ms);

    Player* next = nullptr;
    if (type_ == CRICKET_SCHED_PRIORITY && ms->intensity == INTENSITY_HIGH) {
        next = priority_next(ms, bowlers);
        log_msg(LOG_SCHED, "PRIORITY SCHEDULER: Death over %d — selecting specialist",
                ms->current_over);
    } else {
        next = rr_next(ms, bowlers);
    }

    if (next) {
        context_switches_++;
        log_msg(LOG_SCHED,
                "CONTEXT SWITCH #%d: Loading bowler '%s' (overs left: %d)",
                context_switches_, next->name.c_str(),
                next->max_overs - next->balls_bowled / BALLS_PER_OVER);
    }
    return next;
}

/*
  rr_next()
 
  Round Robin bowler selection.
  Constraint: cannot pick the bowler who just completed an over.
  Each bowler has a max_overs limit (usually 4 in T20).
 */
Player* Scheduler::rr_next(MatchState* ms, std::vector<Player>& bowlers) {
    int   n        = (int)bowler_queue_.size();
    int   last_idx = ms->current_bowler_idx;

    for (int attempts = 0; attempts < n; attempts++) {
        rr_head_ = (rr_head_ + 1) % n;
        Player* candidate = bowler_queue_[rr_head_];
        // Skip if same as last bowler, or exceeded max overs
        if (candidate->id == last_idx)    continue;
        if (candidate->is_out)            continue;
        int overs_done = candidate->balls_bowled / BALLS_PER_OVER;
        if (overs_done >= candidate->max_overs) continue;
        return candidate;
    }
    // Fallback: pick anyone still eligible (handles edge case)
    for (auto* b : bowler_queue_) {
        if (!b->is_out && (b->balls_bowled / BALLS_PER_OVER) < b->max_overs)
            return b;
    }
    return bowler_queue_[0]; 
}

/*
  priority_next()
 
  Priority scheduling for death overs.
  Death specialist gets highest priority.
  Falls back to RR if specialist has bowled max overs.
 */
Player* Scheduler::priority_next(MatchState* ms, std::vector<Player>& bowlers) {
    Player* specialist = nullptr;
    Player* best_rr    = nullptr;

    for (auto* b : bowler_queue_) {
        if (b->is_out) continue;
        int overs_done = b->balls_bowled / BALLS_PER_OVER;
        if (overs_done >= b->max_overs) continue;
        if (b->id == ms->current_bowler_idx) continue;

        if (b->is_death_specialist && !specialist) {
            specialist = b;
        }
        if (!best_rr) best_rr = b; 
    }

    if (specialist) {
       
        struct sched_param param;
        param.sched_priority = 20; 
        if (specialist->thread_id) {
            pthread_setschedparam(specialist->thread_id,
                                  SCHED_OTHER, &param);
        }
        log_msg(LOG_SCHED, "PRIORITY BOOST: '%s' promoted for death over %d",
                specialist->name.c_str(), ms->current_over);
        return specialist;
    }
    return best_rr ? best_rr : rr_next(ms, bowlers);
}

/*
  save_bowler_context()
 
  Analogous to the OS saving a process's CPU context to its PCB
  when it is preempted. Here we save bowling statistics.
 */
void Scheduler::save_bowler_context(Player* p) {
    BowlerContext* ctx = find_context(p->id);
    if (!ctx) return;
    ctx->overs_bowled          = p->balls_bowled / BALLS_PER_OVER;
    ctx->runs_conceded         = p->runs_conceded;
    ctx->wickets               = p->wickets_taken;
    ctx->economy               = (ctx->overs_bowled > 0) ?
                                 (float)ctx->runs_conceded / ctx->overs_bowled : 0.0f;
    ctx->balls_in_current_over = p->balls_bowled % BALLS_PER_OVER;
    ctx->saved_state           = p->state;
    p->state                   = THREAD_WAITING; 

    log_msg(LOG_SCHED,
            "CTX SAVE  → '%s': %d overs, %d runs, %d wkts, econ=%.2f",
            p->name.c_str(), ctx->overs_bowled,
            ctx->runs_conceded, ctx->wickets, ctx->economy);
}

/*
  restore_bowler_context()
 
  Analogous to restoring a process's saved context when it is
  scheduled again. Here we restore bowling statistics.
 */
void Scheduler::restore_bowler_context(Player* p) {
    BowlerContext* ctx = find_context(p->id);
    if (!ctx) return;
    
    p->state = THREAD_RUNNING;
    log_msg(LOG_SCHED,
            "CTX RESTORE ← '%s': %d overs, %d runs, %d wkts",
            p->name.c_str(), ctx->overs_bowled,
            ctx->runs_conceded, ctx->wickets);
}

void Scheduler::notify_over_complete(Player* bowler, MatchState* ms) {
    save_bowler_context(bowler);
    log_msg(LOG_SCHED, "OVER %d COMPLETE — '%s' finished over (%d/%d overs)",
            ms->current_over, bowler->name.c_str(),
            bowler->balls_bowled / BALLS_PER_OVER,
            bowler->max_overs);
}

// Batting order scheduling 

/*
  build_batting_order()
  
  Constructs batting queues for FCFS and SJF.
  FCFS: natural order (1->11), as in traditional cricket
  SJF:  sorted by expected_balls (burst time) ascending
         - promotes tail-enders, reduces average wait time
 */
void Scheduler::build_batting_order(std::vector<Player>& batsmen,
                                     SchedulerType order_type) {
    fcfs_batting_order_.clear();
    sjf_batting_order_.clear();

    for (auto& p : batsmen) {
        fcfs_batting_order_.push_back(&p);
        sjf_batting_order_.push_back(&p);
    }

    std::sort(sjf_batting_order_.begin(), sjf_batting_order_.end(),
              sjf_compare);

    log_msg(LOG_SCHED, "BATTING ORDER built:");
    log_msg(LOG_SCHED, "  FCFS: %s, %s, %s ...",
            fcfs_batting_order_[0]->name.c_str(),
            fcfs_batting_order_.size()>1 ? fcfs_batting_order_[1]->name.c_str():"",
            fcfs_batting_order_.size()>2 ? fcfs_batting_order_[2]->name.c_str():"");
    log_msg(LOG_SCHED, "  SJF:  %s(%d), %s(%d), %s(%d) ...",
            sjf_batting_order_[0]->name.c_str(),
            sjf_batting_order_[0]->expected_balls,
            sjf_batting_order_.size()>1 ? sjf_batting_order_[1]->name.c_str():"",
            sjf_batting_order_.size()>1 ? sjf_batting_order_[1]->expected_balls:0,
            sjf_batting_order_.size()>2 ? sjf_batting_order_[2]->name.c_str():"",
            sjf_batting_order_.size()>2 ? sjf_batting_order_[2]->expected_balls:0);
}

Player* Scheduler::get_next_batsman(std::vector<Player>& batsmen) {
    std::vector<Player*>& order = (type_ == CRICKET_SCHED_SJF) ?
                                   sjf_batting_order_ : fcfs_batting_order_;

    for (size_t i = next_batsman_idx_; i < order.size(); i++) {
        if (!order[i]->is_active && !order[i]->is_out) {
            next_batsman_idx_ = (int)i + 1;
            return order[i];
        }
    }
    return nullptr; // All out
}

//Stats & Analysis

void Scheduler::record_wait_time(int player_id, double wait_ms) {
    WaitRecord r; r.id = player_id; r.wait_ms = wait_ms;
    wait_records_.push_back(r);
}

double Scheduler::get_avg_wait_time() const {
    if (wait_records_.empty()) return 0.0;
    double sum = 0.0;
    for (const auto& r : wait_records_) sum += r.wait_ms;
    return sum / wait_records_.size();
}

void Scheduler::print_scheduler_stats() const {
    log_msg(LOG_SCHED,
            "=== Scheduler Stats === Type: %s | Context Switches: %d | Avg Wait: %.1fms",
            (type_==CRICKET_SCHED_RR ? "Round Robin" :
             type_==CRICKET_SCHED_SJF ? "SJF" : "Priority"),
            context_switches_,
            get_avg_wait_time());
}

//Internal helpers

void Scheduler::update_intensity(MatchState* ms) {
    int over = ms->current_over;
    if      (over >= 17) ms->intensity = INTENSITY_HIGH;
    else if (over >= 11) ms->intensity = INTENSITY_MEDIUM;
    else                 ms->intensity = INTENSITY_LOW;
}

BowlerContext* Scheduler::find_context(int bowler_id) {
    for (auto& ctx : pcb_table_)
        if (ctx.bowler_id == bowler_id) return &ctx;
    return nullptr;
}

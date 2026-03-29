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
      context_switches_(0), next_batsman_idx_(0), rr_head_(0), ms_(nullptr){}

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
 // compute_dynamic_priority()
 int Scheduler::compute_dynamic_priority(Player* p, MatchState* ms,
                                          int overs_left, bool is_batting)
 {
     int score = p->skill_rating;
 
     if (!is_batting) {
         // BOWLER PRIORITY 
         int overs_done = p->balls_bowled / BALLS_PER_OVER;
         score -= overs_done * 5;   // fatigue
 
         // Economy signal
         float econ = (p->balls_bowled > 0)
                    ? (p->runs_conceded * 6.0f / p->balls_bowled)
                    : 7.5f;
 
         if      (econ < 6.0f)  score += 20;
         else if (econ < 7.5f)  score += 10;
         else if (econ < 9.0f)  score +=  0;
         else if (econ < 11.0f) score -= 14;
         else                   score -= 28;
 
         // Wicket-taker bonus
         score += p->wickets_taken * 9;
 
         // Phase-specialisation bonuses
         int phase = ms->current_phase;
 
         if (phase == PHASE_POWERPLAY) {
             // Powerplay
             if (p->bowl_spec == BOWL_OPENING_SWING)  score += 25;
             else if (p->bowl_spec == BOWL_MEDIUM_PACE) score += 8;
             else if (p->bowl_spec == BOWL_SPINNER)    score -= 18;
             else if (p->bowl_spec == BOWL_DEATH_SPECIALIST) score -= 8;
             // Reward genuine pace
             score += (int)((p->bowling_speed - 125.0f) * 0.3f);
 
         } else if (phase == PHASE_MIDDLE) {
             // Middle overs
             if (p->bowl_spec == BOWL_SPINNER)         score += 20;
             else if (p->bowl_spec == BOWL_MEDIUM_PACE) score += 8;
             else if (p->bowl_spec == BOWL_OPENING_SWING) score -= 5;
             else if (p->bowl_spec == BOWL_DEATH_SPECIALIST) score -= 5;
             // Save death specialist
             if (p->bowl_spec == BOWL_DEATH_SPECIALIST && overs_left > 4)
                 score -= 12;
 
         } else { // PHASE_DEATH
             // Death overs: death specialists are gold
             if (p->bowl_spec == BOWL_DEATH_SPECIALIST) score += 28;
             else if (p->bowl_spec == BOWL_MEDIUM_PACE)  score += 5;
             else if (p->bowl_spec == BOWL_SPINNER)       score -= 10;
             else if (p->bowl_spec == BOWL_OPENING_SWING) score -= 8;
             // Urgency: few overs left: push death specialists hard
             if (overs_left <= 2 && p->bowl_spec == BOWL_DEATH_SPECIALIST)
                 score += 15;
         }
 
         // Save specialist's last over for a critical moment
         int overs_remaining = p->max_overs - overs_done;
         if (overs_remaining == 1 && overs_left > 3) score -= 8;
 
     } else {
         // BATSMAN PRIORITY
         score += (int)(p->strike_rate / 10.0f);
 
         if (overs_left <= 4) {
             if      (p->strike_rate >= 155.0f) score += 32;
             else if (p->strike_rate >= 135.0f) score += 16;
             else                               score -= 12;
         }
 
         int wickets = ms->wickets.load();
         if (wickets >= 7) {
             if (p->batting_avg >= 25.0f)  score += 20;
             if (p->strike_rate >= 165.0f) score -= 15;
         }
 
         if (ms->intensity == INTENSITY_MEDIUM)
             score += (int)(p->batting_avg * 0.5f);
     }
 
     return score;
 }

/*
  schedule_next_bowler()
  
 * The main scheduling decision point. 
 
 */
Player* Scheduler::schedule_next_bowler(MatchState* ms,
                                         std::vector<Player>& bowlers) {
    update_intensity(ms);

    Player* next = nullptr;
    if (type_ == CRICKET_SCHED_RR) {
        // Pure round-robin
        next = rr_next(ms, bowlers);
    } else {
        next = priority_next(ms, bowlers);
    }

    if (next) {
        context_switches_++;
        restore_bowler_context(next);
        log_msg(LOG_SCHED,
                 "CONTEXT SWITCH #%d -> '%s'  overs=%d/%d  economy=%.1f",
                 context_switches_, next->name.c_str(),
                 next->balls_bowled / BALLS_PER_OVER, next->max_overs,
                 next->balls_bowled > 0
                     ? next->runs_conceded * 6.0f / next->balls_bowled
                     : 0.0f);
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
 */
Player* Scheduler::priority_next(MatchState* ms, std::vector<Player>& bowlers) {
     int overs_left = max_overs_ - ms->current_over + 1;
 
     // Over 1: mandate the best swing/pace opener
     if (ms->current_over == 1) {
         Player* opener = nullptr;
         int     best_s = INT_MIN;
         for (auto* b : bowler_queue_) {
             if (b->is_out || b->id == ms->current_bowler_idx) continue;
             if (b->balls_bowled > 0) continue;
             if (b->bowl_spec != BOWL_OPENING_SWING &&
                 b->bowl_spec != BOWL_MEDIUM_PACE) continue;
             int s = b->skill_rating + (int)((b->bowling_speed - 125.0f) * 0.4f);
             if (b->bowl_spec == BOWL_OPENING_SWING) s += 20;
             if (s > best_s) { best_s = s; opener = b; }
         }
         if (opener) {
             log_msg(LOG_SCHED,
                 "OPENER RULE: '%s' (swing/pace) mandated for over 1  score=%d",
                 opener->name.c_str(), best_s);
             log_msg(LOG_SCHED,
                 "DYNAMIC PRIORITY: '%s' selected  score=%d  overs_left=%d  phase=POWERPLAY",
                 opener->name.c_str(), best_s, overs_left);
             return opener;
         }
     }
 
     // All other overs: full phase-aware priority scoring
     Player* best     = nullptr;
     int     best_pri = INT_MIN;
 
     for (auto* b : bowler_queue_) {
         if (b->is_out)                              continue;
         if (b->id == ms->current_bowler_idx)        continue;
         int od = b->balls_bowled / BALLS_PER_OVER;
         if (od >= b->max_overs)                     continue;
 
         int pri = compute_dynamic_priority(b, ms, overs_left, false);
 
         const char* spec_str =
             b->bowl_spec == BOWL_OPENING_SWING   ? "swing"  :
             b->bowl_spec == BOWL_SPINNER          ? "spin"   :
             b->bowl_spec == BOWL_DEATH_SPECIALIST ? "death"  :
             b->bowl_spec == BOWL_MEDIUM_PACE      ? "medium" : "?";
 
         log_msg(LOG_SCHED,
                 "  Candidate: %-20s  skill=%d  econ=%.1f  wkts=%d  spec=%-6s  pri=%d",
                 b->name.c_str(), b->skill_rating,
                 b->balls_bowled > 0
                     ? b->runs_conceded * 6.0f / b->balls_bowled : 0.0f,
                 b->wickets_taken, spec_str, pri);
 
         if (pri > best_pri) { best_pri = pri; best = b; }
     }
 
     if (best) {
         const char* phase_str =
             ms->current_phase == PHASE_DEATH   ? "DEATH"
           : ms->current_phase == PHASE_MIDDLE  ? "MIDDLE" : "POWERPLAY";
         log_msg(LOG_SCHED,
                 "DYNAMIC PRIORITY: '%s' selected  score=%d  overs_left=%d  phase=%s",
                 best->name.c_str(), best_pri, overs_left, phase_str);
         return best;
     }
     return rr_next(ms, bowlers);
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
    // Time Quantum = 6 balls.  After each quantum the current bowler thread is preempted,
    // its PCB (BowlerContext) is saved, and the scheduler picks the next thread - identical to RR CPU scheduling.
     if (type_ == CRICKET_SCHED_RR)
         log_msg(LOG_SCHED,
                 "RR QUANTUM EXPIRED (quantum=6 balls) → CTX SAVE '%s':",
                 bowler->name.c_str());
     log_msg(LOG_SCHED, "OVER %d COMPLETE: '%s' (%d/%d overs)",
             ms->current_over, bowler->name.c_str(),
             bowler->balls_bowled / BALLS_PER_OVER, bowler->max_overs);
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

    log_msg(LOG_SCHED, "BATTING ORDER built (FCFS: %s | SJF: %s ...)",
             fcfs_batting_order_.empty() ? "?" : fcfs_batting_order_[0]->name.c_str(),
             sjf_batting_order_.empty()  ? "?" : sjf_batting_order_[0]->name.c_str());
     (void)order_type;
}

// Get next batsman
Player* Scheduler::get_next_batsman(std::vector<Player>& batsmen) {
 
     if (type_ == CRICKET_SCHED_RR) {
         for (size_t i = next_batsman_idx_; i < fcfs_batting_order_.size(); i++) {
             if (!fcfs_batting_order_[i]->is_active &&
                 !fcfs_batting_order_[i]->is_out) {
                 next_batsman_idx_ = (int)i + 1;
                 return fcfs_batting_order_[i];
             }
         }
         return nullptr;
     }
 
     if (type_ == CRICKET_SCHED_SJF) {
         for (size_t i = next_batsman_idx_; i < sjf_batting_order_.size(); i++) {
             if (!sjf_batting_order_[i]->is_active &&
                 !sjf_batting_order_[i]->is_out) {
                 next_batsman_idx_ = (int)i + 1;
                 return sjf_batting_order_[i];
             }
         }
         return nullptr;
     }
 
     if (!ms_) {
         for (auto& p : batsmen)
             if (!p.is_active && !p.is_out) return &p;
         return nullptr;
     }
 
     int     overs_left = max_overs_ - ms_->current_over + 1;
     Player* best       = nullptr;
     int     best_pri   = INT_MIN;
 
     for (auto& p : batsmen) {
         if (p.is_active || p.is_out) continue;
         int pri = compute_dynamic_priority(&p, ms_, overs_left, true);
         log_msg(LOG_SCHED,
                 "  Next-bat candidate: %-18s  SR=%.0f  avg=%.1f  pri=%d",
                 p.name.c_str(), p.strike_rate, p.batting_avg, pri);
         if (pri > best_pri) { best_pri = pri; best = &p; }
     }
 
     if (best)
         log_msg(LOG_SCHED,
                 "DYNAMIC BAT SELECT: '%s' (score=%d, overs_left=%d)",
                 best->name.c_str(), best_pri, overs_left);
 
     return best;
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
    int pp_end      = (max_overs_ * 30 + 99) / 100;
     int death_start = max_overs_ - (max_overs_ * 20 + 99) / 100;
     int over        = ms->current_over;
 
     if      (over >= death_start) ms->intensity = INTENSITY_HIGH;
     else if (over >= pp_end)      ms->intensity = INTENSITY_MEDIUM;
     else                          ms->intensity = INTENSITY_LOW;
}

BowlerContext* Scheduler::find_context(int bowler_id) {
    for (auto& ctx : pcb_table_)
        if (ctx.bowler_id == bowler_id) return &ctx;
    return nullptr;
}

/*
 * match_engine.cpp
 * --------------------------------------------------------------------------------------
 * Drives the full T20 match: team setup, over-by-over simulation,
 * Gantt chart generation, and post-match analysis.
 */

#include "../include/cricket_types.h"
#include "../include/match_engine.h"
#include "../include/player_threads.h"
#include "../include/synchronization.h"
#include "../include/globals.h"
#include "../include/utils.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <cassert>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <random>
#include <iostream>
#include <vector>
#include <string>

// Helper: build a Player struct 
static Player make_player(int id, const char* name, PlayerRole role,
                           int skill, int exp_balls, float avg, float sr,
                           float economy = 0.0f, float speed = 0.0f,
                           bool death_spec = false, int max_ov = 4) {
    Player p;
    p = Player{};
    p.id                  = id;
    p.name                = name;
    p.role                = role;
    p.skill_rating        = skill;
    p.expected_balls      = exp_balls;
    p.batting_avg         = avg;
    p.strike_rate         = sr;
    p.bowling_economy     = economy;
    p.bowling_speed       = speed;
    p.is_death_specialist = death_spec;
    p.max_overs           = max_ov;
    p.holds_end           = -1;
    p.wants_end           = -1;
    p.state               = THREAD_READY;
    p.is_out              = false;
    p.is_active           = false;
    p.total_wait_ms       = 0.0;
    return p;
}

// Team Rosters 

// Players pool generation

int rand_range(int l, int r) {
    return l + rand() % (r - l + 1);
}
float rand_float(float l, float r) {
    return l + static_cast<float>(rand()) / RAND_MAX * (r - l);
}
std::vector<Player> batsmen_pool;
std::vector<Player> wicketkeeper_pool;
std::vector<Player> bowlers_pool;
std::vector<Player> allrounders_pool;

// Batsman pool hving 20 batsman
void init_batsmen(int &id) {
    std::vector<std::string> names = {
        "Virat Kohli","Joe Root","Steve Smith","Kane Williamson","Rohit Sharma","Shubhman Gill","David Warner",
        "KL Rahul","Travis Head","Jos Buttler","Aiden Markram","Devon Conway","Harry Brook","Shahi Hope",
        "Glenn Phillips","Daryl Mitchell","Nicholas Pooran","Rahmanullah Gurbaz","Shikhar Dhawan","Suresh Raina"
    };

    for (auto &name : names) {
        batsmen_pool.push_back(
            make_player(id++, name.c_str(), BATSMAN,
                rand_range(70,95),
                rand_range(20,40),
                rand_float(35,55),
                rand_float(120,180)
            )
        );
    }
}

// Wicket-keeper pool having 6 wicket-keepers
void init_wicketkeepers(int &id) {
    std::vector<std::string> names = {
        "M S Dhoni","Dinesh Karthik","Quinton de Kock","Rishabh Pant","Sanju Samson","Alex Carey"
    };

    for (auto &name : names) {
        wicketkeeper_pool.push_back(
            make_player(id++, name.c_str(), WICKET_KEEPER,
                rand_range(65,90),
                rand_range(20,35),
                rand_float(30,45),
                rand_float(120,160)
            )
        );
    }
}

// All-rounder pool having 15 all-rounders
void init_allrounders(int &id) {
    std::vector<std::string> names = {
        "Benn Stokes","Hardik Pandya","Shakib al Hasan","Ravindra Jadeja","Glenn Maxwell",
        "Mitchell Marsh","Andre Russell","Marcus Stoinis","Sam Curran","Chris Woakes",
        "Auqib Nabi","Wanindu Hasaranga","Axar Patel","Cameron Green","Jason Holder"
    };

    for (auto &name : names) {
        allrounders_pool.push_back(
            make_player(id++, name.c_str(), ALL_ROUNDER,
                rand_range(60,85),
                rand_range(10,30),
                rand_float(25,45),
                rand_float(110,160),
                rand_float(6.5,9.0),
                rand_float(110,140),
                rand()%2,
                4
            )
        );
    }
}

// Bowler pool having 15 bowlers
void init_bowlers(int &id) {
    std::vector<std::string> names = {
        "Japreet Bumrah","Mitchell Starc","Shoaib Akhtar","Kagiso Rabada","Trent Boult",
        "Mohammad Shami","Anrich Nortje","Mark Wood","Lockie Ferguson","Adam Zampa",
        "Adil Rashid","Kuldeep Yadav","Josh Hazlewood","Mustafizur Rehman","Deepak Chahal"
    };

    for (auto &name : names) {
        bowlers_pool.push_back(
            make_player(id++, name.c_str(), BOWLER,
                rand_range(70,95),
                0,
                rand_float(10,25),
                rand_float(80,120),
                rand_float(6.0,8.5),
                rand_float(120,150),
                rand()%2,
                4
            )
        );
    }
}



void init_all_players() {
    srand(time(NULL));
    int id = 0;

    init_batsmen(id);
    init_wicketkeepers(id);
    init_allrounders(id);
    init_bowlers(id);
}

// Shuffle pools
std::mt19937 rng(std::random_device{}());

void shuffle_pool(std::vector<Player>& pool) {
    std::shuffle(pool.begin(), pool.end(), rng);
}

Player pick_player(std::vector<Player>& pool) {
    Player p = pool.back();
    pool.pop_back();
    return p;
}

// Team selection
std::vector<Player> select_team() {
    std::vector<Player> team;

    for (int i = 0; i < 4; i++)
        team.push_back(pick_player(batsmen_pool));

    team.push_back(pick_player(wicketkeeper_pool));

    for (int i = 0; i < 3; i++)
        team.push_back(pick_player(bowlers_pool));

    for (int i = 0; i < 3; i++)
        team.push_back(pick_player(allrounders_pool));

    return team;
}

void create_teams(std::vector<Player>& teamA, std::vector<Player>& teamB) {

    shuffle_pool(batsmen_pool);
    shuffle_pool(wicketkeeper_pool);
    shuffle_pool(bowlers_pool);
    shuffle_pool(allrounders_pool);

    // Safety check
    if (batsmen_pool.size() < 8 ||
        wicketkeeper_pool.size() < 2 ||
        bowlers_pool.size() < 6 ||
        allrounders_pool.size() < 6) {
        printf("Not enough players!\n");
        exit(1);
    }

    teamA = select_team();
    teamB = select_team();
}


// Print teams
void print_team(const std::vector<Player>& team, const std::string& name) {
    printf(COL_ORANGE" \n=== %s ===\n", name.c_str(),
    COL_RESET<"\n");
    for (const auto& p : team) {
        std::string role;
        if (p.role == BATSMAN) role = "Batsman";
        else if (p.role == BOWLER) role = "Bowler";
        else if(p.role == ALL_ROUNDER) role="All Rounder";
        else role = "Wicket Keeper";

        printf(COL_ORANGE"%s (%s)\n", p.name.c_str(), role.c_str(),
               COL_RESET);
    }
}

std::vector<Player> get_batting_team(const std::vector<Player>& team) {
    std::vector<Player> batting;

    for (const auto& p : team) {
        if (p.role == BATSMAN || p.role == ALL_ROUNDER) {
            batting.push_back(p);
        }
    }

    return batting;
}

std::vector<Player> get_bowling_team(const std::vector<Player>& team) {
    std::vector<Player> bowling;

    for (const auto& p : team) {
        if (p.role == BOWLER || p.role == ALL_ROUNDER) {
            bowling.push_back(p);
        }
    }

    return bowling;
}


static std::vector<Player> MI;
static std::vector<Player> CSK;
static bool teams_initialized = false;

void ensure_teams_ready() {
    if (!teams_initialized) {
        init_all_players();
        create_teams(MI, CSK);
        teams_initialized = true;
    }
}

std::vector<Player> build_mi_batting() {
    ensure_teams_ready();
    return get_batting_team(MI);
}

std::vector<Player> build_mi_bowling() {
    ensure_teams_ready();
    return get_bowling_team(MI);
}

std::vector<Player> build_csk_batting() {
    ensure_teams_ready();
    return get_batting_team(CSK);
}

std::vector<Player> build_csk_bowling() {
    ensure_teams_ready();
    return get_bowling_team(CSK);
}

//----------------------------------------------------------------------------------------------
//  MATCH ENGINE

MatchEngine::MatchEngine(const MatchConfig& cfg)
    : config_(cfg),
      scheduler_(cfg.scheduler_type, cfg.max_overs),
      gantt_file_(nullptr)
{
    // Build teams
    if (cfg.team1_name == "Mumbai Indians") {
        team1_bat_  = build_mi_batting();
        team1_bowl_ = build_mi_bowling();
        team2_bat_  = build_csk_batting();
        team2_bowl_ = build_csk_bowling();
    } else {
        team1_bat_  = build_csk_batting();
        team1_bowl_ = build_csk_bowling();
        team2_bat_  = build_mi_batting();
        team2_bowl_ = build_mi_bowling();
    }

    // Build fielder pool from bowling team (6 fielders for efficiency)
    fielders_.clear();
    for (size_t i = 0; i < team2_bowl_.size(); i++) {
        Player fp = team2_bowl_[i];
        fp.role = (i == 0) ? WICKET_KEEPER : BOWLER;
        fp.id  += 200; 
        fielders_.push_back(fp);
    }
    fielder_args_.resize(fielders_.size());

    // Open Gantt file
    if (cfg.enable_gantt) {
        gantt_file_ = fopen("logs/gantt_data.csv", "w");
        if (gantt_file_) gantt_write_header(gantt_file_);
    }
}

MatchEngine::~MatchEngine() {
    if (gantt_file_) fclose(gantt_file_);
}

void MatchEngine::init_state(int innings, int target) {
    // Reset match state fields individually (std::atomic is not copy-assignable)
    state_.total_runs   = 0;
    state_.wickets      = 0;
    state_.extras       = 0;
    state_.current_over = 1;
    state_.current_ball = 0;
    state_.innings      = innings;
    state_.target       = target;
    state_.match_over   = false;
    state_.ball_in_air  = false;
    state_.scheduler    = config_.scheduler_type;
    state_.intensity    = INTENSITY_LOW;
    state_.striker_id   = (innings == 1) ?
                          team1_bat_[0].id : team2_bat_[0].id;
    state_.non_striker_id = (innings == 1) ?
                            team1_bat_[1].id : team2_bat_[1].id;
    get_timespec_now(&state_.match_start);

    // Reset delivery flags
    delivery_ready = false;
    stroke_done    = false;
}

// run() - orchestrates both innings 

void MatchEngine::run() {
    print_banner();
    log_msg(LOG_INFO, "Match: %s vs %s | %d overs | Scheduler: %s",
            config_.team1_name.c_str(), config_.team2_name.c_str(),
            config_.max_overs,
            config_.scheduler_type == CRICKET_SCHED_RR ? "Round Robin" :
            config_.scheduler_type == CRICKET_SCHED_SJF ? "SJF" : "Priority");

    // Innings 1 
    print_separator(("INNINGS 1 - " + config_.team1_name + " BATTING").c_str());
    run_innings(1, team1_bat_, team2_bowl_, 0);
    int team1_score = state_.total_runs.load();

    // Innings 2 
    print_separator(("INNINGS 2 - " + config_.team2_name +
                     " chasing " + std::to_string(team1_score + 1)).c_str());

    // Reset all player states for second innings
    for (auto& p : team2_bat_)  { p.is_out=false; p.is_active=false;
                                   p.balls_faced=0; p.runs_scored=0;
                                   p.total_wait_ms=0.0; }
    for (auto& p : team1_bowl_) { p.balls_bowled=0; p.runs_conceded=0;
                                   p.wickets_taken=0; }

    run_innings(2, team2_bat_, team1_bowl_, team1_score + 1);

    print_final_scorecard();
    print_match_result(&state_, config_.team1_name, config_.team2_name);
    scheduler_.print_scheduler_stats();

    if (config_.enable_sjf_analysis) {
        run_sjf_vs_fcfs_analysis();
    }
    if (gantt_file_) {
        fclose(gantt_file_);
        gantt_file_ = nullptr;
        log_msg(LOG_INFO, "Gantt data written to logs/gantt_data.csv");
        log_msg(LOG_INFO, "Run: python3 visualizer/gantt_plotter.py");
    }
}

// run_innings() - manages overs for one innings 

void MatchEngine::run_innings(int innings_num,
                               std::vector<Player>& batting,
                               std::vector<Player>& bowling,
                               int target) {
    init_state(innings_num, target);
    event_log_.clear();

    // Build batting & bowling orders for scheduler
    scheduler_ = Scheduler(config_.scheduler_type, config_.max_overs);
    scheduler_.build_batting_order(batting, config_.scheduler_type);
    for (auto& b : bowling) scheduler_.add_bowler(&b);

    // Spawn Umpire thread (daemon) 
    pthread_t umpire_tid;
    UmpireArgs umpire_args;
    umpire_args.match      = &state_;
    umpire_args.config     = &config_;
    umpire_args.scheduler  = &scheduler_;
    umpire_args.bowlers    = &bowling;
    umpire_args.batsmen    = &batting;
    umpire_args.event_log  = &event_log_;
    umpire_args.gantt_file = gantt_file_;
    pthread_create(&umpire_tid, NULL, umpire_thread_func, &umpire_args);
    log_msg(LOG_THREAD, "Umpire (Kernel) thread spawned [T-%lu]",
            umpire_tid % 10000);

    // Spawn all fielder threads
    // Fielders are from the bowling team
    for (auto& f : fielders_) { f.is_out=false; f.is_active=true; }
    spawn_all_fielders(fielders_, fielder_args_, &state_, &config_);

    // Set first two batsmen active 
    batting[0].is_active = true;
    batting[1].is_active = true;
    state_.striker_id     = batting[0].id;
    state_.non_striker_id = batting[1].id;

    // Spawn opening batsmen threads
    BatsmanArgs bat_args[2];
    bat_args[0] = {&batting[0], &state_, &config_, &batting, &event_log_,
                   true,  get_time_ms()};
    bat_args[1] = {&batting[1], &state_, &config_, &batting, &event_log_,
                   false, get_time_ms()};
    spawn_batsman(&batting[0], &bat_args[0]);
    spawn_batsman(&batting[1], &bat_args[1]);

    int next_bat_idx = 2; 

    // Over loop 
    for (int ov = 1; ov <= config_.max_overs && !is_innings_over(); ov++) {
        state_.current_over = ov;
        state_.current_ball = 0;
        safe_mutex_lock(&delivery_mutex, "delivery_mutex");
delivery_ready = false;
stroke_done    = false;
g_striker_id_local = -1;
safe_mutex_unlock(&delivery_mutex, "delivery_mutex");
        
        // FIX: Ensure striker exists before over starts
if (state_.striker_id == -1) {

    log_msg(LOG_WARN, "No striker at over start → assigning new batsman");

    // Find next available batsman
    for (auto& p : batting) {
        if (!p.is_out && !p.is_active) {

            state_.striker_id = p.id;

            BatsmanArgs* nba = new BatsmanArgs{
                &p, &state_, &config_, &batting, &event_log_,
                true, get_time_ms()
            };

            spawn_batsman(&p, nba);
            break;
        }
    }
}
        print_separator(("OVER " + std::to_string(ov)).c_str());

        // Select bowler for this over (scheduler decision)
        Player* bowler = scheduler_.schedule_next_bowler(&state_, bowling);
        if (!bowler) break;
        state_.current_bowler_idx = bowler->id;
        bowler->is_active = true;

        log_msg(LOG_SCHED, "Over %d: '%s' bowling (%.0f km/h) | "
                "Scheduler: %s | Intensity: %s",
                ov, bowler->name.c_str(), bowler->bowling_speed,
                config_.scheduler_type == CRICKET_SCHED_RR ? "RR" :
                config_.scheduler_type == CRICKET_SCHED_SJF ? "SJF" : "PRIORITY",
                ov >= 17 ? "HIGH(death)" : ov >= 11 ? "MEDIUM" : "LOW");

        // Spawn bowler thread for this over
        BowlerArgs bow_args;
        bow_args.player       = bowler;
        bow_args.match        = &state_;
        bow_args.config       = &config_;
        bow_args.balls_to_bowl= BALLS_PER_OVER;
        bow_args.event_log    = &event_log_;
        bow_args.gantt_file   = gantt_file_;
        spawn_bowler(bowler, &bow_args);

        // Wait for this over to complete
        pthread_join(bowler->thread_id, NULL);
        bowler->thread_id = 0;
        bowler->is_active = false;

        // Context switch: save this bowler's state
        scheduler_.notify_over_complete(bowler, &state_);

        // Handle wickets that fell this over - spawn new batsmen
      /*  while (next_bat_idx < (int)batting.size() &&
               state_.wickets.load() >= next_bat_idx &&
               !is_innings_over()) {
            Player& incoming = batting[next_bat_idx];
            double wait_start_time = get_time_ms();

            // Check if this is a valid position
            if (incoming.is_out) { next_bat_idx++; continue; }

            // Scheduler picks next batsman (SJF or FCFS)
            Player* next_bat = scheduler_.get_next_batsman(batting);
            if (!next_bat) break;

            log_msg(LOG_SCHED,
                    "WICKET FELL — Next batsman: '%s' (burst=%d balls)",
                    next_bat->name.c_str(), next_bat->expected_balls);

            // Update striker/non-striker
            safe_mutex_lock(&state_mutex, "state_mutex");
            // Find who is still active
            for (const auto& bp : batting) {
                if (bp.is_active && !bp.is_out) {
                    state_.non_striker_id = bp.id;
                    break;
                }
            }
            state_.striker_id = next_bat->id;
            safe_mutex_unlock(&state_mutex, "state_mutex");

            BatsmanArgs* nba = new BatsmanArgs{
                next_bat, &state_, &config_, &batting, &event_log_,
                true, wait_start_time
            };
            spawn_batsman(next_bat, nba);
            next_bat_idx++;
        }*/

        // End-of-over strike rotation
        if (!is_innings_over()) {
            safe_mutex_lock(&state_mutex, "state_mutex");
            std::swap(state_.striker_id, state_.non_striker_id);
            safe_mutex_unlock(&state_mutex, "state_mutex");
        }

        // Print over summary
        print_over_summary(ov, event_log_);

        // Show live scoreboard
        log_msg(LOG_SCORE, "End of over %d: %d/%d  (Extras: %d)",
                ov, state_.total_runs.load(), state_.wickets.load(),
                state_.extras);

        sleep_ms(config_.ball_delay_ms);
    }

    // Innings end: signal all threads 
    state_.match_over = true;
    safe_mutex_lock(&ball_hit_mutex, "ball_hit_mutex");
    state_.ball_in_air = true;
    pthread_cond_broadcast(&ball_hit_cond);
    safe_mutex_unlock(&ball_hit_mutex, "ball_hit_mutex");

    safe_mutex_lock(&delivery_mutex, "delivery_mutex");
    delivery_ready = true; stroke_done = true;
    pthread_cond_broadcast(&delivery_ready_cond);
    pthread_cond_broadcast(&stroke_done_cond);
    safe_mutex_unlock(&delivery_mutex, "delivery_mutex");

    // Join all batsmen threads
    for (auto& b : batting) {
        if (b.thread_id) {
            pthread_join(b.thread_id, NULL);
            b.thread_id = 0;
        }
    }

    // Join all fielder threads
    join_all_fielders(fielders_);

    // Join umpire
    pthread_join(umpire_tid, NULL);

    // Print innings summary
    log_msg(LOG_INFO, "=== INNINGS %d COMPLETE: %d/%d in %d overs ===",
            innings_num, state_.total_runs.load(),
            state_.wickets.load(), state_.current_over - 1);
}

bool MatchEngine::is_innings_over() const {
    return state_.match_over ||
           state_.wickets.load() >= MAX_PLAYERS - 1 ||
           state_.current_over  >  config_.max_overs ||
           (state_.innings == 2 && state_.total_runs.load() >= state_.target);
}

// Scorecard 

void MatchEngine::print_final_scorecard() const {
    printf(COL_CYAN);
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║                  FINAL SCORECARD                        ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  %-18s  %-6s  %-8s  %-8s           ║\n",
           "BATSMAN", "RUNS", "BALLS", "SR");
    printf("╠══════════════════════════════════════════════════════════╣\n");

    for (const auto& p : team2_bat_) {
        if (p.balls_faced > 0 || p.is_out) {
            float sr = p.balls_faced > 0 ?
                       (p.runs_scored * 100.0f / p.balls_faced) : 0.0f;
            printf("║  %-18s  %-6d  %-8d  %-8.1f           ║\n",
                   p.name.c_str(), p.runs_scored, p.balls_faced, sr);
        }
    }
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  Total: %d/%d  Extras: %d",
           state_.total_runs.load(), state_.wickets.load(), state_.extras);
    printf("\n╚══════════════════════════════════════════════════════════╝\n");
    printf(COL_RESET);
}

// SJF vs FCFS Analysis

void MatchEngine::run_sjf_vs_fcfs_analysis() {
    // Create a copy of batting order sorted two ways
    std::vector<Player> fcfs_copy = team1_bat_;
    std::vector<Player> sjf_copy  = team1_bat_;

    // SJF: sort by expected_balls ascending
    std::sort(sjf_copy.begin(), sjf_copy.end(),
              [](const Player& a, const Player& b) {
                  return a.expected_balls < b.expected_balls;
              });

    // Simulate wait times for each order
    // FCFS: natural order -> each batsman waits for all previous wickets
    double fcfs_wait = 0.0;
    for (size_t i = 0; i < fcfs_copy.size(); i++) {
        fcfs_copy[i].total_wait_ms = fcfs_wait;
        // Each wicket takes ~15 balls on average (simulated)
        fcfs_wait += fcfs_copy[i].expected_balls * config_.ball_delay_ms;
    }

    // SJF: shortest burst first -> tail-enders promoted, wait less
    double sjf_wait = 0.0;
    for (size_t i = 0; i < sjf_copy.size(); i++) {
        sjf_copy[i].total_wait_ms = sjf_wait;
        sjf_wait += sjf_copy[i].expected_balls * config_.ball_delay_ms;
    }

    print_sjf_vs_fcfs_analysis(fcfs_copy, sjf_copy);
}

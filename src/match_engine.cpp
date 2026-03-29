/*
 * match_engine.cpp
 * Drives the full T20 match: team setup, over-by-over simulation,
 * Gantt chart generation, and post-match analysis.
 * scheduler_.set_match_state(&state_) called after add_bowler loop
 * update_phase()             — sets ms.current_phase each over
 * compute_phase_intensity()  — sets ms.phase_intensity each over
 * init_state() initialises current_phase + phase_intensity
 *  Phase string in log uses current_phase
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
#include <random>
#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>

#define PID(t) ((unsigned long)((uintptr_t)(t) % 10000u))

//Player pool globals 
 std::vector<Player> batsmen_pool;
 std::vector<Player> wicketkeeper_pool;
 std::vector<Player> bowlers_pool;
 std::vector<Player> allrounders_pool;

// Build a Player struct 
static Player make_player(int id, const char* name, PlayerRole role,
                           int skill, int exp_balls, float avg, float sr,
                           float economy = 0.0f, float speed = 0.0f,
                           BowlSpec bspec=BOWL_NONE, BatRole brole=BAT_NONE, 
                           int max_ov = 4) {
   Player p = Player{};
     p.id                  = id;
     p.name                = name;
     p.role                = role;
     p.skill_rating        = skill;
     p.expected_balls      = exp_balls;
     p.batting_avg         = avg;
     p.strike_rate         = sr;
     p.bowling_economy     = economy;
     p.bowling_speed       = speed;
     p.bowl_spec           = bspec;
     p.bat_role            = brole;
     p.is_death_specialist = (bspec == BOWL_DEATH_SPECIALIST);
     p.is_opener           = (brole == BAT_ATTACKING_OPENER || brole == BAT_ANCHOR_OPENER);
     p.max_overs           = max_ov;
     p.holds_end           = -1;
     p.wants_end           = -1;
     p.state               = THREAD_READY;
     p.is_out              = false;
     p.is_active           = false;
     p.how_out             = BALL_DOT;
     p.caught_by[0]        = '\0';
     p.caught_pos[0]       = '\0';
     p.dismissed_by_id     = -1;
     p.total_wait_ms       = 0.0;
    return p;
}

// Players pool generation
int rand_range(int l, int r)           { return l + rand() % (r - l + 1); }
float rand_float(float l, float r)     { return l + (float)rand()/RAND_MAX*(r-l); }

void init_batsmen(int& id) {
     // Attacking openers — high SR, moderate avg
     struct { const char* name; BatRole role; } openers[] = {
         {"Rohit Sharma",    BAT_ATTACKING_OPENER},
         {"David Warner",    BAT_ATTACKING_OPENER},
         {"Jos Buttler",     BAT_ATTACKING_OPENER},
         {"Travis Head",     BAT_ATTACKING_OPENER},
         {"Aiden Markram",   BAT_ATTACKING_OPENER},
         {"Suresh Raina",    BAT_ATTACKING_OPENER},
         // Anchor openers — higher avg, balanced SR
         {"Kane Williamson", BAT_ANCHOR_OPENER},
         {"Shubhman Gill",   BAT_ANCHOR_OPENER},
         {"Devon Conway",    BAT_ANCHOR_OPENER},
         {"Shikhar Dhawan",  BAT_ANCHOR_OPENER},
     };
     for (auto& o : openers) {
         bool attacking = (o.role == BAT_ATTACKING_OPENER);
         batsmen_pool.push_back(make_player(id++, o.name, BATSMAN,
             rand_range(75, 95),
             attacking ? rand_range(20, 35) : rand_range(30, 45),
             attacking ? rand_float(30, 45) : rand_float(40, 55),
             attacking ? rand_float(140, 180) : rand_float(120, 150),
             0.0f, 0.0f, BOWL_NONE, o.role));
     }
     // Technical anchors (pos 3)
     struct { const char* name; } anchors[] = {
         {"Virat Kohli"}, {"Joe Root"}, {"Steve Smith"}, {"Babar Azam"},
         {"Shahi Hope"},  {"Harry Brook"},
     };
     for (auto& a : anchors) {
         batsmen_pool.push_back(make_player(id++, a.name, BATSMAN,
             rand_range(80, 95), rand_range(35, 55),
             rand_float(45, 60), rand_float(125, 155),
             0.0f, 0.0f, BOWL_NONE, BAT_TECHNICAL_ANCHOR));
     }
     // Middle-order hitters (pos 4-5)
     struct { const char* name; } mids[] = {
         {"Nicholas Pooran"}, {"Daryl Mitchell"}, {"Glenn Phillips"},
         {"KL Rahul"},        {"Rahmanullah Gurbaz"},
     };
     for (auto& m : mids) {
         batsmen_pool.push_back(make_player(id++, m.name, BATSMAN,
             rand_range(70, 90), rand_range(15, 30),
             rand_float(30, 45), rand_float(145, 175),
             0.0f, 0.0f, BOWL_NONE, BAT_MIDDLE_HITTER));
     }
     // Finishers (pos 5-6)
     struct { const char* name; } fins[] = {
         {"Andre Russell"}, {"Kieron Pollard"}, {"Tim David"}, {"Rinku Singh"},
     };
     for (auto& f : fins) {
         batsmen_pool.push_back(make_player(id++, f.name, BATSMAN,
             rand_range(70, 88), rand_range(10, 22),
             rand_float(25, 38), rand_float(160, 190),
             0.0f, 0.0f, BOWL_NONE, BAT_FINISHER));
     }
 }
 
 void init_wicketkeepers(int& id) {
     struct { const char* name; } wks[] = {
         {"MS Dhoni"},      {"Dinesh Karthik"}, {"Quinton de Kock"},
         {"Rishabh Pant"},  {"Sanju Samson"},   {"Alex Carey"},
         {"Jos Buttler"},   {"Heinrich Klaasen"},
     };
     for (auto& w : wks) {
         wicketkeeper_pool.push_back(make_player(id++, w.name, WICKET_KEEPER,
             rand_range(72, 92), rand_range(20, 35),
             rand_float(32, 48), rand_float(130, 165),
             0.0f, 0.0f, BOWL_NONE, BAT_WK_BATSMAN));
     }
 }
 
 void init_allrounders(int& id) {
     // Pace-bowling all-rounders (bat pos 6-8, bowl = medium/death)
     struct { const char* name; BowlSpec bs; } paceAR[] = {
         {"Ben Stokes",      BOWL_MEDIUM_PACE},
         {"Hardik Pandya",   BOWL_DEATH_SPECIALIST},
         {"Mitchell Marsh",  BOWL_MEDIUM_PACE},
         {"Jason Holder",    BOWL_MEDIUM_PACE},
         {"Sam Curran",      BOWL_DEATH_SPECIALIST},
         {"Chris Woakes",    BOWL_OPENING_SWING},
         {"Cameron Green",   BOWL_MEDIUM_PACE},
         {"Marco Jansen",    BOWL_OPENING_SWING},
     };
     for (auto& a : paceAR) {
         bool death = (a.bs == BOWL_DEATH_SPECIALIST);
         allrounders_pool.push_back(make_player(id++, a.name, ALL_ROUNDER,
             rand_range(68, 85), rand_range(15, 28),
             rand_float(28, 42), rand_float(120, 155),
             rand_float(7.0f, 9.0f),
             death ? rand_float(130, 148) : rand_float(120, 140),
             a.bs, BAT_ALLROUNDER));
     }
     // Spinning all-rounders
     struct { const char* name; } spinAR[] = {
         {"Shakib Al Hasan"}, {"Ravindra Jadeja"}, {"Glenn Maxwell"},
         {"Wanindu Hasaranga"}, {"Axar Patel"}, {"Moeen Ali"}, {"Marcus Stoinis"},
     };
     for (auto& a : spinAR) {
         allrounders_pool.push_back(make_player(id++, a.name, ALL_ROUNDER,
             rand_range(65, 83), rand_range(12, 25),
             rand_float(25, 40), rand_float(115, 148),
             rand_float(6.5f, 8.5f), rand_float(100, 125),
             BOWL_SPINNER, BAT_ALLROUNDER));
     }
 }
 
 void init_bowlers(int& id) {
     // Opening/swing bowlers
     struct { const char* name; } swingBowlers[] = {
         {"Jasprit Bumrah"},  {"Mitchell Starc"},  {"Trent Boult"},
         {"Deepak Chahar"},   {"Mohammad Shami"},  {"Josh Hazlewood"},
         {"Lockie Ferguson"}, {"Anrich Nortje"},   {"Mark Wood"},
     };
     for (auto& b : swingBowlers) {
         bowlers_pool.push_back(make_player(id++, b.name, BOWLER,
             rand_range(75, 95), 0,
             rand_float(10, 22), rand_float(80, 115),
             rand_float(6.5f, 8.5f), rand_float(132, 152),
             BOWL_OPENING_SWING, BAT_NONE));
     }
     // Death specialists
     struct { const char* name; } deathBowlers[] = {
         {"Kagiso Rabada"}, {"Mustafizur Rahman"}, {"T Natarajan"},
     };
     for (auto& b : deathBowlers) {
         bowlers_pool.push_back(make_player(id++, b.name, BOWLER,
             rand_range(72, 92), 0,
             rand_float(8, 18), rand_float(80, 110),
             rand_float(7.0f, 9.0f), rand_float(128, 148),
             BOWL_DEATH_SPECIALIST, BAT_NONE));
     }
     // Spinners
     struct { const char* name; } spinners[] = {
         {"Adam Zampa"},       {"Adil Rashid"},       {"Kuldeep Yadav"},
         {"Yuzvendra Chahal"}, {"Rashid Khan"},
     };
     for (auto& b : spinners) {
         bowlers_pool.push_back(make_player(id++, b.name, BOWLER,
             rand_range(73, 93), 0,
             rand_float(8, 18), rand_float(75, 110),
             rand_float(6.0f, 8.0f), rand_float(90, 115),
             BOWL_SPINNER, BAT_NONE));
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
    // Batting core: 1 attacking opener + 1 anchor opener + 1 anchor (pos3)
    //               + 1 middle hitter + 1 finisher  = 5 specialist batsmen
     auto pick_by_batrole = [&](BatRole br) -> bool {
         for (size_t i = 0; i < batsmen_pool.size(); i++) {
             if (batsmen_pool[i].bat_role == br) {
                 team.push_back(batsmen_pool[i]);
                 batsmen_pool.erase(batsmen_pool.begin() + (int)i);
                 return true;
             }
         }
         // fallback: any remaining batsman
         if (!batsmen_pool.empty()) { team.push_back(pick_player(batsmen_pool)); return true; }
         return false;
     };
     pick_by_batrole(BAT_ATTACKING_OPENER);
     pick_by_batrole(BAT_ANCHOR_OPENER);
     pick_by_batrole(BAT_TECHNICAL_ANCHOR);
     pick_by_batrole(BAT_MIDDLE_HITTER);
     pick_by_batrole(BAT_FINISHER);
 
     // WK-batsman (pos 5-7)
     team.push_back(pick_player(wicketkeeper_pool));
 
     // 2 all-rounders (one pace AR, one spin AR if possible)
     auto pick_by_bowlspec = [&](std::vector<Player>& pool, BowlSpec bs) -> bool {
         for (size_t i = 0; i < pool.size(); i++) {
             if (pool[i].bowl_spec == bs) {
                 team.push_back(pool[i]);
                 pool.erase(pool.begin() + (int)i);
                 return true;
             }
         }
         return false;
     };
     if (!pick_by_bowlspec(allrounders_pool, BOWL_MEDIUM_PACE) &&
         !pick_by_bowlspec(allrounders_pool, BOWL_DEATH_SPECIALIST))
         if (!allrounders_pool.empty()) team.push_back(pick_player(allrounders_pool));
     if (!pick_by_bowlspec(allrounders_pool, BOWL_SPINNER))
         if (!allrounders_pool.empty()) team.push_back(pick_player(allrounders_pool));
 
     // 1 spinner (pure bowler)
     if (!pick_by_bowlspec(bowlers_pool, BOWL_SPINNER))
         if (!bowlers_pool.empty()) team.push_back(pick_player(bowlers_pool));
 
     // 1 opening/swing bowler
     if (!pick_by_bowlspec(bowlers_pool, BOWL_OPENING_SWING))
         if (!bowlers_pool.empty()) team.push_back(pick_player(bowlers_pool));
 
     // 1 death specialist bowler
     if (!pick_by_bowlspec(bowlers_pool, BOWL_DEATH_SPECIALIST))
         if (!bowlers_pool.empty()) team.push_back(pick_player(bowlers_pool));
 
     return team;   // 11 players
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
     printf(COL_CYAN
            "\n╔══════════════════════════════════════════════════════════════════════╗\n"
            "║  %-68s║\n"
            "╠══╦═══════════════════╦══════════════╦══════╦════════╦═════════════╣\n"
            "║##║ PLAYER            ║ ROLE         ║SKILL ║BAT AVG ║ BOWL SPEC   ║\n"
            "╠══╬═══════════════════╬══════════════╬══════╬════════╬═════════════╣\n",
            name.c_str());

     int pos = 1;
     for (const auto& p : team) {
         const char* role_str;
         switch (p.role) {
             case BATSMAN:       role_str = "Batsman";       break;
             case BOWLER:        role_str = "Bowler";        break;
             case ALL_ROUNDER:   role_str = "All-Rounder";   break;
             default:            role_str = "Wicket Keeper"; break;
         }
         const char* spec_str;
         switch (p.bowl_spec) {
             case BOWL_OPENING_SWING:    spec_str = "Swing/Pace";   break;
             case BOWL_SPINNER:          spec_str = "Spinner";       break;
             case BOWL_DEATH_SPECIALIST: spec_str = "Death";         break;
             case BOWL_MEDIUM_PACE:      spec_str = "Medium Pace";   break;
             default:                    spec_str = "—";             break;
         }
         // Mark captain (highest skill batsman) and WK
         char flag[4] = "   ";
         if (p.role == WICKET_KEEPER)   flag[0] = '+';  // WK marker (ASCII safe)
         if (pos == 1)                  flag[1] = 'C';  // Captain (opener)

         printf(COL_CYAN
                "║%2d║ %-17s %s║ %-12s ║  %2d  ║  %4.1f  ║ %-11s ║\n",
                pos++,
                p.name.c_str(), flag,
                role_str,
                p.skill_rating,
                p.batting_avg,
                spec_str);
     }
     printf(COL_CYAN
            "╚══╩═══════════════════╩══════════════╩══════╩════════╩═════════════╝\n"
            "  † Wicket Keeper   C Captain\n"
            COL_RESET);
}

std::vector<Player> get_batting_team(const std::vector<Player>& team) {
    std::vector<Player> out;
    for (const auto& p : team)
         if (p.role == BATSMAN || p.role == ALL_ROUNDER || p.role == WICKET_KEEPER)
             out.push_back(p);
     return out;
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

/*
  * update_phase() — maps current_over to PHASE_POWERPLAY / MIDDLE / DEATH.
  * Boundaries scale with max_overs so 8-over and 12-over formats work correctly.
  *   20-over: powerplay <6,  death >=16
  *   12-over: powerplay <4,  death >=10
  *    8-over: powerplay <3,  death >=7
  */
 static void update_phase(MatchState& ms, int max_overs) {
     int pp_end      = (max_overs * 30 + 99) / 100;
     int death_start = max_overs - (max_overs * 20 + 99) / 100;
 
     if      (ms.current_over <  pp_end)      ms.current_phase = PHASE_POWERPLAY;
     else if (ms.current_over <  death_start) ms.current_phase = PHASE_MIDDLE;
     else                                     ms.current_phase = PHASE_DEATH;
 }
 
 /*
  * compute_phase_intensity() — 1-50 pressure score.
  * Chasing: driven by required run rate + wickets remaining.
  * First innings: rises with over progress.
  */
 static void compute_phase_intensity(MatchState& ms, int max_overs) {
     int rem = max_overs - ms.current_over + 1;
     if (rem < 1) rem = 1;
     int wl           = 10 - ms.wickets.load();
     int pressure_zone = (max_overs * 15 + 99) / 100;
     int intensity;
 
     if (ms.target > 0) {
         int need = ms.target - ms.total_runs.load();
         if (need < 0) need = 0;
         double rrr = (double)need / rem;
         intensity = (int)(rrr * 2.0) + (10 - wl)
                   + (rem <= pressure_zone ? 20 : 0);
     } else {
         int pct = (ms.current_over * 100) / max_overs;
         intensity = pct / 5 + (10 - wl) * 2;
     }
     ms.phase_intensity = std::max(1, std::min(50, intensity));
 }
 

//
// ** MATCH ENGINE **
//

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
    fielders_.clear();
    // Cricket: 11 players field = 1 bowler (active thread) + 10 fielders.
    // Fielder index 0 = WK (behind stumps), 1-9 = named outfield positions.
     // The active bowler is spawned separately each over — not in fielders.
    {
         std::vector<Player> full_team2;
         for (const auto& p : team2_bat_)  full_team2.push_back(p);
         for (const auto& p : team2_bowl_) full_team2.push_back(p);

         // WK at index 0 first
         for (auto& fp : full_team2) {
             if (fp.role == WICKET_KEEPER) {
                 Player f = fp; f.id += 200;
                 fielders_.push_back(f);
                 break;
             }
         }
         // If no WK found, use first player as WK slot
         if (fielders_.empty() && !full_team2.empty()) {
             Player f = full_team2[0]; f.id += 200;
             fielders_.push_back(f);
         }
         // Fill positions 1-9 with non-WK players
         for (auto& fp : full_team2) {
             if ((int)fielders_.size() >= MAX_FIELDERS) break;
             if (fp.role == WICKET_KEEPER) continue;
             Player f = fp; f.id += 200;
             fielders_.push_back(f);
         }
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
    // Reset match state fields individually 
     state_.total_runs  = 0;
     state_.wickets     = 0;
     state_.extras      = 0;
     state_.current_over= 1;
     state_.current_ball= 0;
     state_.innings     = innings;
     state_.target      = target;
     state_.match_over  = false;
     state_.ball_in_air     = false;
    state_.active_fielders = 0;   // set after spawn_all_fielders
     state_.scheduler   = config_.scheduler_type;
     state_.intensity   = INTENSITY_LOW;
     // NEW: initialise phase fields
     state_.current_phase   = PHASE_POWERPLAY;
     state_.phase_intensity = 10;
 
     state_.striker_id     = (innings == 1) ? team1_bat_[0].id : team2_bat_[0].id;
     state_.non_striker_id = (innings == 1) ? team1_bat_[1].id : team2_bat_[1].id;
     get_timespec_now(&state_.match_start);

    // Reset delivery flags
    delivery_ready = false;
    stroke_done    = false;
}

// run() - starts both innings 

void MatchEngine::run() {
    print_banner();
    log_msg(LOG_INFO, "Match: %s vs %s | %d overs | Scheduler: %s",
            config_.team1_name.c_str(), config_.team2_name.c_str(),
            config_.max_overs,
            config_.scheduler_type == CRICKET_SCHED_RR ? "Round Robin" :
            config_.scheduler_type == CRICKET_SCHED_SJF ? "SJF" : "Priority");

     // Print full squads before the match begins─
     std::vector<Player> team1_full, team2_full;
     for (const auto& p : team1_bat_)  team1_full.push_back(p);
     for (const auto& p : team1_bowl_) {
         bool dup = false;
         for (const auto& q : team1_full) if (q.id == p.id) { dup = true; break; }
         if (!dup) team1_full.push_back(p);
     }
     for (const auto& p : team2_bat_)  team2_full.push_back(p);
     for (const auto& p : team2_bowl_) {
         bool dup = false;
         for (const auto& q : team2_full) if (q.id == p.id) { dup = true; break; }
         if (!dup) team2_full.push_back(p);
     }
     print_team(team1_full, config_.team1_name + " — SQUAD");
     print_team(team2_full, config_.team2_name + " — SQUAD");
     printf("\n");
    // Innings 1 
    print_separator(("INNINGS 1 - " + config_.team1_name + " BATTING").c_str());
    run_innings(1, team1_bat_, team2_bowl_, 0);
    int team1_score = state_.total_runs.load();

    // Save innings-1 totals before state_ is re-initialised for innings 2
     innings1_runs_    = state_.total_runs.load();
     innings1_wickets_ = state_.wickets.load();
     innings1_extras_  = state_.extras;
     innings1_overs_   = state_.current_over;
  
    // Innings 2 
    print_separator(("INNINGS 2 - " + config_.team2_name +
                     " chasing " + std::to_string(team1_score + 1)).c_str());

    // Reset all player states for second innings
    for (auto& p : team2_bat_)  { p.is_out=false; p.is_active=false;
                                    p.balls_faced=0; p.runs_scored=0;
                                    p.how_out=BALL_DOT; p.caught_by[0]='\0'; p.caught_pos[0]='\0'; p.dismissed_by_id=-1;
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

     // Rebuild fielder pool from the FIELDING team for this innings :─
     // constructor built fielders_ from team2 only. In innings 2,
     // team1 fields — so we must rebuild with the correct side's players,
     // otherwise batsmen from the batting team appear as their own fielders.
     {
         // Fielding team = the bowling side (they are the ones in the field)
         // Collect ALL players from the fielding team (batting + bowling roles)
         std::vector<Player> field_team;
         // The fielding team is whoever is NOT batting this innings
         std::vector<Player>& fielding_bat  = (innings_num == 1) ? team2_bat_  : team1_bat_;
         std::vector<Player>& fielding_bowl = (innings_num == 1) ? team2_bowl_ : team1_bowl_;
         for (const auto& p : fielding_bat)  field_team.push_back(p);
         for (const auto& p : fielding_bowl) field_team.push_back(p);

         fielders_.clear();
         // WK always at index 0
         for (auto& fp : field_team) {
             if (fp.role == WICKET_KEEPER) {
                 Player f = fp; f.id += 200; fielders_.push_back(f); break;
             }
         }
         if (fielders_.empty() && !field_team.empty()) {
             Player f = field_team[0]; f.id += 200; fielders_.push_back(f);
         }
         for (auto& fp : field_team) {
             if ((int)fielders_.size() >= MAX_FIELDERS) break;
             if (fp.role == WICKET_KEEPER) continue;
             Player f = fp; f.id += 200; fielders_.push_back(f);
         }
         fielder_args_.resize(fielders_.size());
     }

    // Build batting & bowling orders for scheduler
     int prev_switches = scheduler_.get_context_switches();
     scheduler_ = Scheduler(config_.scheduler_type, config_.max_overs);
     scheduler_.add_switches(prev_switches);  // carry count across innings
     scheduler_.build_batting_order(batting, config_.scheduler_type);
    for (auto& b : bowling) scheduler_.add_bowler(&b);

    // link live match state so dynamic batsman selection works
     scheduler_.set_match_state(&state_);

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
    log_msg(LOG_THREAD, "Umpire thread spawned [T-%lu]", PID(umpire_tid));
  
    // Spawn all fielder threads
    // Fielders are from the bowling team
    for (auto& f : fielders_) { f.is_out=false; f.is_active=true; }
    spawn_all_fielders(fielders_, fielder_args_, &state_, &config_);
    state_.active_fielders = (int)fielders_.size();   // ← tell batsman exact count

   //  OS CONCEPT: Thread Pool accounting-
   //   1  Bowler    — writer thread (delivers to pitch critical section)
   //   2  Batsmen   — reader threads (consume delivery, update Global_Score)
   //  10  Fielders  — blocked on ball_hit_cond (pthread_cond_wait)
   //   1  Umpire    — kernel/daemon (deadlock detection + match-end)
   //   1  Scoreboard— periodic reader (scoreboard_rwlock, read-side)

   //  TOTAL = 15 concurrent threads per ball 
     log_msg(LOG_THREAD,
             "── THREAD POOL: 1 bowler + 2 batsmen + %zu fielders "
             "+ 1 umpire + 1 scoreboard = %zu total concurrent threads ──",
             fielders_.size(), fielders_.size() + 5);


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

    // Over loop
     for (int ov = 1; ov <= config_.max_overs && !is_innings_over(); ov++) {
         state_.current_over = ov;
         state_.current_ball = 0;
 
         // update phase and intensity at start of every over
         update_phase(state_, config_.max_overs);
         compute_phase_intensity(state_, config_.max_overs);
 
         safe_mutex_lock(&delivery_mutex, "delivery_mutex");
         delivery_ready      = false;
         stroke_done         = false;
         g_striker_id_local  = -1;
         safe_mutex_unlock(&delivery_mutex, "delivery_mutex");
 
         if (state_.striker_id == -1) {
             log_msg(LOG_WARN, "No striker at over start -> assigning next batsman");
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
 
       const char* phase_str =
             state_.current_phase == PHASE_DEATH     ? "HIGH(death)"
           : state_.current_phase == PHASE_MIDDLE    ? "MEDIUM"
           :                                           "LOW(powerplay)";
      
        print_separator(("OVER " + std::to_string(ov)).c_str());

        // Select bowler for this over (scheduler decision)
        Player* bowler = scheduler_.schedule_next_bowler(&state_, bowling);
        if (!bowler) break;
        state_.current_bowler_idx = bowler->id;
        bowler->is_active = true;

        log_msg(LOG_SCHED,
                 "Over %d: '%s' (%.0f km/h) | Scheduler: %s | Phase: %s | Intensity: %d",
                 ov, bowler->name.c_str(), bowler->bowling_speed,
                 config_.scheduler_type == CRICKET_SCHED_RR  ? "RR"
               : config_.scheduler_type == CRICKET_SCHED_SJF ? "SJF" : "PRIORITY",
                 phase_str, state_.phase_intensity);
      
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

        if (!is_innings_over()) {
             safe_mutex_lock(&state_mutex, "state_mutex");
             std::swap(state_.striker_id, state_.non_striker_id);
             safe_mutex_unlock(&state_mutex, "state_mutex");
             // Over-end swap: notify non-striker they are now striker
             safe_mutex_lock(&striker_changed_mutex, "striker_changed_mutex");
             pthread_cond_broadcast(&striker_changed_cond);
             safe_mutex_unlock(&striker_changed_mutex, "striker_changed_mutex");
         }
 
         print_over_summary(ov, event_log_);
         log_msg(LOG_SCORE, "End of over %d: %d/%d  (Extras: %d)  Intensity: %d",
                 ov, state_.total_runs.load(), state_.wickets.load(),
                 state_.extras, state_.phase_intensity);
 
         sleep_ms(config_.ball_delay_ms);
     }

    // Innings end: signal all threads 
    state_.match_over = true;
    // Wake non-strikers blocked on striker_changed_cond
     safe_mutex_lock(&striker_changed_mutex, "striker_changed_mutex");
     pthread_cond_broadcast(&striker_changed_cond);
     safe_mutex_unlock(&striker_changed_mutex, "striker_changed_mutex");
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
            state_.wickets.load(), state_.current_over);
}

bool MatchEngine::is_innings_over() const {
    return state_.match_over ||
           state_.wickets.load() >= MAX_PLAYERS - 1 ||
           state_.current_over  >  config_.max_overs ||
           (state_.innings == 2 && state_.total_runs.load() >= state_.target);
}

// Scorecard 

void MatchEngine::print_final_scorecard() const {
     auto print_bat = [](const std::vector<Player>& bat, int inn,
                          int runs, int wkts, int extras, int overs) {
         printf(COL_CYAN
                "\n╔══════════════════════════════════════════════════════════════╗\n"
                "║             INNINGS %d BATTING SCORECARD                     ║\n"
                "╠══════════════════════════════════════════════════════════════╣\n"
                "║  %-20s  %-5s  %-6s  %-9s  %-9s ║\n",
                inn, "BATSMAN","RUNS","BALLS","SR","STATUS");
         printf("╠══════════════════════════════════════════════════════════════╣\n");
         for (const auto& p : bat) {
             if (p.balls_faced > 0 || p.is_out) {
                 float sr = p.balls_faced > 0
                          ? (p.runs_scored * 100.0f / p.balls_faced) : 0.0f;
                 std::string ds;
                 if (!p.is_out) { ds = "not out"; }
                 else switch (p.how_out) {
                     case BALL_WICKET: ds = "b (bowled)"; break;
                     case BALL_LBW:    ds = "lbw"; break;
                     case BALL_RUNOUT: ds = "run out"; break;
                     case BALL_CAUGHT:
                         ds = std::string("c ") + (p.caught_by[0] ? p.caught_by : "fielder");
                         if (p.caught_pos[0]) { ds += " ("; ds += p.caught_pos; ds += ")"; }
                         break;
                     default: ds = "out"; break;
                 }
                 printf("║  %-20s  %-5d  %-6d  %-8.1f%%  %-28s ║\n",
                        p.name.c_str(), p.runs_scored, p.balls_faced, sr,
                        ds.c_str());
             } else {
                 printf("║  %-20s  %-5s  %-6s  %-9s  %-22s ║\n",
                        p.name.c_str(), "-", "-", "-", "dnb");
             }
         }
         printf("╠══════════════════════════════════════════════════════════════╣\n");
         printf("║  Total: %d/%d   Overs: %d   Extras: %d\n",
                runs, wkts, overs, extras);
         printf("╚══════════════════════════════════════════════════════════════╝\n"
                COL_RESET);
     };

     // Bowling scorecard 
     auto print_bowl = [](const std::vector<Player>& bowl, int inn) {
         printf(COL_MAGENTA
                "\n╔══════════════════════════════════════════════════════════════╗\n"
                "║             INNINGS %d BOWLING FIGURES                       ║\n"
                "╠══════════════════════════════════════════════════════════════╣\n"
                "║  %-20s  %-5s  %-4s  %-4s  %-7s  %-7s ║\n",
                inn, "BOWLER","O","R","W","ECON","SPEC");
         printf("╠══════════════════════════════════════════════════════════════╣\n");
         for (const auto& p : bowl) {
             if (p.balls_bowled == 0) continue;
             int   ov  = p.balls_bowled / BALLS_PER_OVER;
             int   rem = p.balls_bowled % BALLS_PER_OVER;
             float eco = p.balls_bowled > 0
                       ? p.runs_conceded * 6.0f / p.balls_bowled : 0.0f;
             const char* spec =
                 p.bowl_spec == BOWL_OPENING_SWING   ? "Swing"  :
                 p.bowl_spec == BOWL_SPINNER          ? "Spin"   :
                 p.bowl_spec == BOWL_DEATH_SPECIALIST ? "Death"  :
                 p.bowl_spec == BOWL_MEDIUM_PACE      ? "Medium" : "-";
             char ov_str[16];
             if (rem) snprintf(ov_str, sizeof(ov_str), "%d.%d", ov, rem);
             else     snprintf(ov_str, sizeof(ov_str), "%d",    ov);
             printf("║  %-20s  %-5s  %-4d  %-4d  %-7.2f  %-7s ║\n",
                    p.name.c_str(), ov_str,
                    p.runs_conceded, p.wickets_taken, eco, spec);
         }
         printf("╚══════════════════════════════════════════════════════════════╝\n"
                COL_RESET);
     };

     // Innings 1 batting (team1) + innings 1 bowling (team2)
     print_bat (team1_bat_,  1, innings1_runs_, innings1_wickets_,
                innings1_extras_, innings1_overs_);
     print_bowl(team2_bowl_, 1);

     // Innings 2 batting (team2) + innings 2 bowling (team1)
     print_bat (team2_bat_,  2, state_.total_runs.load(), state_.wickets.load(),
                state_.extras, state_.current_over);
     print_bowl(team1_bowl_, 2);
}

// SJF vs FCFS Analysis

void MatchEngine::run_sjf_vs_fcfs_analysis() {
    // sorted by FCFS (natural order) and SJF (ascending expected_balls).
     // Bowlers have expected_balls=0 → they always go FIRST in SJF.
     auto analyse_team = [&](const std::vector<Player>& bat,
                              const std::vector<Player>& bowl,
                              const std::string& team_name) {

         // Build full squad — batsmen first (natural order), then bowlers
         std::vector<Player> full;
         for (const auto& p : bat)  full.push_back(p);
         for (const auto& p : bowl) {
             bool dup = false;
             for (const auto& q : full) if (q.id == p.id) { dup = true; break; }
             if (!dup) full.push_back(p);
         }

         // FCFS order = natural squad order (openers first)
         std::vector<Player> fcfs_copy = full;

         // SJF order = ascending expected_balls
         // Bowlers have expected_balls=0 → they bat FIRST under SJF
         std::vector<Player> sjf_copy  = full;
         std::stable_sort(sjf_copy.begin(), sjf_copy.end(),
                          [](const Player& a, const Player& b) {
                              return a.expected_balls < b.expected_balls;
                          });

         // Compute cumulative wait for each batsman
         double fcfs_wait = 0.0, sjf_wait = 0.0;
         for (auto& p : fcfs_copy) {
             p.total_wait_ms  = fcfs_wait;
             fcfs_wait       += (double)p.expected_balls * config_.ball_delay_ms;
         }
         for (auto& p : sjf_copy) {
             p.total_wait_ms  = sjf_wait;
             sjf_wait        += (double)p.expected_balls * config_.ball_delay_ms;
         }

         // Build a map: player_id → SJF wait time
         // so we can look up each FCFS player's wait under SJF correctly
         std::unordered_map<int,double> sjf_wait_map;
         std::unordered_map<int,int>    sjf_pos_map;
         for (size_t i = 0; i < sjf_copy.size(); i++) {
             sjf_wait_map[sjf_copy[i].id] = sjf_copy[i].total_wait_ms;
             sjf_pos_map [sjf_copy[i].id] = (int)i + 1;
         }

         // Print team header
         printf(COL_MAGENTA
                "\n╔══════════════════════════════════════════════════════════════════════╗\n"
                "║  SJF vs FCFS ANALYSIS — %-44s║\n"
                "╠════╦══════════════════════╦═══════╦════════════╦════════════╦══════════╣\n"
                "║ ## ║ Batsman              ║ Exp.B ║ FCFS Wait  ║  SJF Wait  ║  Saved   ║\n"
                "╠════╬══════════════════════╬═══════╬════════════╬════════════╬══════════╣\n",
                team_name.c_str());

         double total_fcfs = 0.0, total_sjf = 0.0;
         for (size_t i = 0; i < fcfs_copy.size(); i++) {
             double fcfs_w = fcfs_copy[i].total_wait_ms;
             double sjf_w  = sjf_wait_map.count(fcfs_copy[i].id)
                             ? sjf_wait_map[fcfs_copy[i].id] : 0.0;
             double saved  = fcfs_w - sjf_w;
             total_fcfs   += fcfs_w;
             total_sjf    += sjf_w;

             printf(COL_MAGENTA
                    "║ %2zu ║ %-20s ║  %3d  ║ %8.1fms ║ %8.1fms ║ %+7.1fms ║\n",
                    i + 1,
                    fcfs_copy[i].name.c_str(),
                    fcfs_copy[i].expected_balls,
                    fcfs_w, sjf_w, saved);
         }

         // SJF order column
         printf(COL_MAGENTA
                "╠════╩══════════════════════╩═══════╩════════════╩════════════╩══════════╣\n"
                "║  Avg wait:  FCFS = %7.1fms     SJF = %7.1fms     Total saved = %+.1fms\n"
                "╠══════════════════════════════════════════════════════════════════════════╣\n"
                "║  SJF batting order (shortest job first):                               ║\n",
                total_fcfs / (double)fcfs_copy.size(),
                total_sjf  / (double)sjf_copy.size(),
                total_fcfs - total_sjf);

         for (size_t i = 0; i < sjf_copy.size(); i++) {
             const char* tag = (sjf_copy[i].expected_balls == 0) ? " [Bowler/tail]" : "";
             printf(COL_MAGENTA "║    %2zu. %-20s  exp_balls=%-3d%s\n",
                    i + 1,
                    sjf_copy[i].name.c_str(),
                    sjf_copy[i].expected_balls,
                    tag);
         }
         printf(COL_MAGENTA
                "╚══════════════════════════════════════════════════════════════════════════╝\n"
                COL_RESET "\n");
     };

     analyse_team(team1_bat_, team1_bowl_, config_.team1_name);
     analyse_team(team2_bat_, team2_bowl_, config_.team2_name);
 }

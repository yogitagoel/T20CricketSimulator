//  main.cpp
// ------------------------------------------------------------------------------------

#include "../include/cricket_types.h"
#include "../include/synchronization.h"
#include "../include/match_engine.h"
#include "../include/utils.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

static void print_help(const char* prog) {
    printf(
        "Usage: %s [options]\n\n"
        "Options:\n"
        "  --scheduler=RR|SJF|PRIORITY  Scheduling algorithm (default: RR)\n"
        "  --team1=<name>               First team (default: India)\n"
        "  --team2=<name>               Second team (default: Australia)\n"
        "  --overs=<n>                  Number of overs (default: 20)\n"
        "  --deadlock                   Simulate run-out deadlock scenario\n"
        "  --gantt                      Generate Gantt chart CSV\n"
        "  --analysis                   Print SJF vs FCFS wait-time analysis\n"
        "  --speed=<n>                  Sim speed 1=slow,5=fast,10=instant\n"
        "  --verbose                    Debug-level output\n"
        "  --log=<file>                 Log file path\n"
        "  --seed=<n>                   RNG seed\n"
        "  --help                       Show this help\n\n"
        "Examples:\n"
        "  %s --scheduler=RR --overs=5 --gantt\n"
        "  %s --scheduler=SJF --analysis --verbose\n"
        "  %s --scheduler=PRIORITY --deadlock --speed=5\n\n",
        prog, prog, prog, prog);
}

static SchedulerType parse_scheduler(const char* s) {
    if (strcmp(s, "SJF")      == 0) return CRICKET_SCHED_SJF;
    if (strcmp(s, "PRIORITY") == 0) return CRICKET_SCHED_PRIORITY;
    return CRICKET_SCHED_RR;
}

static int parse_int_arg(const char* arg, const char* prefix) {
    const char* val = arg + strlen(prefix);
    return atoi(val);
}

static bool starts_with(const char* s, const char* prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int main(int argc, char* argv[]) {
    // Defaults 
    MatchConfig cfg;
    cfg.team1_name          = "Mumbai Indians";
    cfg.team2_name          = "Chennai Super Kings";
    cfg.scheduler_type      = CRICKET_SCHED_RR;
    cfg.enable_deadlock_sim = false;
    cfg.enable_gantt        = false;
    cfg.enable_sjf_analysis = false;
    cfg.ball_delay_ms       = BALL_DELAY_MS;
    cfg.verbose             = false;
    cfg.log_file            = "logs/match.log";
    unsigned int seed       = (unsigned int)time(NULL);

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_help(argv[0]);
            return 0;
        }
        else if (starts_with(arg, "--scheduler=")) {
            cfg.scheduler_type = parse_scheduler(arg + strlen("--scheduler="));
        }
        else if (starts_with(arg, "--team1=")) {
            cfg.team1_name = std::string(arg + strlen("--team1="));
        }
        else if (starts_with(arg, "--team2=")) {
            cfg.team2_name = std::string(arg + strlen("--team2="));
        }
        else if (starts_with(arg, "--overs=")) {
            cfg.max_overs = parse_int_arg(arg, "--overs=");
            if (cfg.max_overs < 1 || cfg.max_overs > 20) cfg.max_overs = 5;
        }
        else if (strcmp(arg, "--deadlock") == 0) {
            cfg.enable_deadlock_sim = true;
        }
        else if (strcmp(arg, "--gantt") == 0) {
            cfg.enable_gantt = true;
        }
        else if (strcmp(arg, "--analysis") == 0) {
            cfg.enable_sjf_analysis = true;
        }
        else if (starts_with(arg, "--speed=")) {
            int speed = parse_int_arg(arg, "--speed=");
            if (speed < 1)  speed = 1;
            if (speed > 10) speed = 10;
            cfg.ball_delay_ms = BALL_DELAY_MS / speed;
        }
        else if (strcmp(arg, "--verbose") == 0) {
            cfg.verbose = true;
        }
        else if (starts_with(arg, "--log=")) {
            cfg.log_file = std::string(arg + strlen("--log="));
        }
        else if (starts_with(arg, "--seed=")) {
            seed = (unsigned int)parse_int_arg(arg, "--seed=");
        }
        else {
            fprintf(stderr, "Unknown option: %s  (use --help)\n", arg);
            return 1;
        }
    }

    // Initialise subsystems

    // Create logs directory
    mkdir("logs", 0755);

    rng_init(seed);

    // Initialise logger )
    logger_init(cfg.log_file, cfg.verbose);

    // Initialise all synchronisation primitives
    sync_init_all();

    print_banner();
    printf(COL_ORANGE
           "  Configuration:\n"
           "    Teams       : %s vs %s\n"
           "    Overs       : %d\n"
           "    Scheduler   : %s\n"
           "    Deadlock    : %s\n"
           "    Gantt       : %s\n"
           "    Analysis    : %s\n"
           "    Speed       : %dx (ball_delay=%dms)\n"
           "    RNG Seed    : %u\n"
           "    Log file    : %s\n"
           COL_RESET "\n",
           cfg.team1_name.c_str(), cfg.team2_name.c_str(),
           cfg.max_overs,
           cfg.scheduler_type == CRICKET_SCHED_RR ? "Round Robin" :
           cfg.scheduler_type == CRICKET_SCHED_SJF ? "SJF" : "Priority",
           cfg.enable_deadlock_sim ? "ON" : "OFF",
           cfg.enable_gantt ? "ON (logs/gantt_data.csv)" : "OFF",
           cfg.enable_sjf_analysis ? "ON" : "OFF",
           BALL_DELAY_MS / (cfg.ball_delay_ms > 0 ? cfg.ball_delay_ms : 1),
           cfg.ball_delay_ms,
           seed,
           cfg.log_file.c_str());

    printf(COL_CYAN
           "  OS Concept Mapping:\n"
           "    Pitch         = Critical Section  (pitch_mutex)\n"
           "    Bowler        = Writer thread      (writes delivery to pitch)\n"
           "    Batsman       = Reader thread      (reads pitch, updates score)\n"
           "    Fielders      = Blocked threads    (cond_wait on ball_in_air)\n"
           "    Crease        = Semaphore(2)        (max 2 batsmen active)\n"
           "    Score         = Shared resource    (protected by score_mutex)\n"
           "    Over switch   = Context Switch     (RR scheduler)\n"
           "    Umpire        = Kernel/Scheduler   (daemon thread)\n"
           "    Run-out       = Deadlock           (circular wait on ends)\n"
           COL_RESET "\n");

    // Run the match 
    MatchEngine engine(cfg);
    engine.run();

    // Cleanup 
    sync_destroy_all();
    logger_shutdown();

    printf(COL_GREEN "\n  Simulation complete.\n" COL_RESET);
    if (cfg.enable_gantt) {
        printf(COL_CYAN
               "  To visualise Gantt chart:\n"
               "    pip install matplotlib\n"
               "    python3 visualizer/gantt_plotter.py\n"
               COL_RESET);
    }
    return 0;
}

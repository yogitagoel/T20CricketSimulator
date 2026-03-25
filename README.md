# T20 Cricket OS Simulator
## CSC-204 Operating Systems — Assignment 1

A multi-threaded T20 cricket match simulator that models core OS concepts using
`pthreads` in C++17.

---

## OS Concept → Cricket Mapping

| OS Concept              | Cricket Equivalent                          | Implementation                        |
|-------------------------|---------------------------------------------|---------------------------------------|
| **Critical Section**    | The Pitch (only 1 delivery at a time)       | `pitch_mutex`                         |
| **Shared Resource**     | Global Score                                | `score_mutex` (atomic update)         |
| **Threads**             | Players (Bowler, Batsman, Fielder, Umpire)  | `pthread_t` for each player           |
| **Semaphore**           | Crease (max 2 batsmen allowed)              | `sem_t crease_semaphore(2)`           |
| **Condition Variable**  | Ball in air (wake fielders)                 | `pthread_cond_wait / broadcast`       |
| **Context Switch**      | Over transition (new bowler each over)      | `save_context / restore_context()`    |
| **Round Robin**         | Bowler rotation (Quantum = 6 balls)         | `Scheduler::rr_next()`               |
| **SJF**                 | Batting order by expected balls (burst)     | `Scheduler::sjf_batting_order_`      |
| **Priority Scheduling** | Death-over specialist in overs 17-20        | `pthread_setschedparam()`            |
| **Deadlock**            | Run-out (circular wait on ends)             | Resource Allocation Graph detection  |
| **Kernel**              | Third Umpire thread (monitors everything)   | `umpire_thread_func()`               |
| **RW Lock**             | Scoreboard (readers) vs. game (writers)     | `pthread_rwlock_t scoreboard_rwlock` |

---

## Project Structure

```
t20_cricket_simulator/
├── src/
│   ├── main.cpp              Entry point + CLI argument parsing
│   ├── synchronization.cpp   All mutex/semaphore/cond-var init & wrappers
│   ├── utils.cpp             Logger, timing, RNG, display, Gantt helpers
│   ├── scheduler.cpp         RR, SJF, Priority scheduling algorithms
│   ├── player_threads.cpp    Thread functions: bowler/batsman/fielder/umpire
│   └── match_engine.cpp      Over loop, innings management, team rosters
├── include/
│   ├── cricket_types.h       All shared structs, enums, constants
│   ├── synchronization.h     Extern declarations of all sync primitives
│   ├── utils.h               Logger + helper declarations
│   ├── scheduler.h           Scheduler class declaration
│   ├── player_threads.h      Thread argument structs + function prototypes
│   └── match_engine.h        MatchEngine class declaration
├── logs/                     Runtime output (auto-created)
│   ├── match.log             Full match log
│   ├── gantt_data.csv        Thread timeline data (for plotting)
│   ├── gantt_chart.png       Generated Gantt chart
│   └── wait_analysis.png     SJF vs FCFS wait-time chart
├── visualizer/
│   └── gantt_plotter.py      Python matplotlib Gantt chart generator
├── Makefile
└── README.md
```

---

## Quick Start

### Build
```bash
# Requires: g++ with C++17 and pthreads support
make

# Or manually:
g++ -std=c++17 -pthread -O2 -Wall -I include \
    src/main.cpp src/synchronization.cpp src/utils.cpp \
    src/scheduler.cpp src/player_threads.cpp src/match_engine.cpp \
    -o cricket_sim
```

### Run (Demos)
```bash
make run            # 5-over Round Robin demo
make run-sjf        # SJF batting order + wait-time table
make run-priority   # Priority scheduling (death over specialist)
make run-deadlock   # Run-out deadlock simulation
make run-full       # Full 20-over match + Gantt + Analysis
make test           # Quick 2-over sanity check
```

### CLI Options
```bash
./cricket_sim --scheduler=RR|SJF|PRIORITY
              --team1=India --team2=Australia
              --overs=5
              --deadlock          # Enable circular-wait simulation
              --gantt             # Write Gantt CSV
              --analysis          # Print SJF vs FCFS table
              --speed=5           # 5x faster simulation
              --verbose           # Debug output
              --seed=42           # Reproducible RNG
              --log=logs/out.log  # Save output
```

### Generate Gantt Chart
```bash
pip install matplotlib
make run-full   # generates logs/gantt_data.csv
make gantt      # generates logs/gantt_chart.png
```

---

## Synchronization Primitives

### Mutex (`pthread_mutex_t`)
- `score_mutex`  — atomic score update (Wide + Single same cycle = no race)
- `pitch_mutex`  — enforces critical section on pitch buffer
- `state_mutex`  — serialises over-boundary transitions
- `field_mutex`  — only one fielder acts per ball

### Semaphore (`sem_t`)
- `crease_semaphore(2)` — at most 2 batsmen at crease simultaneously
- 3rd batsman calling `sem_wait` enters WAITING state until a wicket falls

### Condition Variables (`pthread_cond_t`)
- `ball_hit_cond` — fielders sleep here; batsman broadcasts after hitting
- `delivery_ready_cond` — batsman waits for bowler to write pitch buffer
- `stroke_done_cond` — bowler waits for batsman to complete shot

### RW Lock (`pthread_rwlock_t`)
- `scoreboard_rwlock` — display thread holds read lock; game holds write lock

---

## Scheduling Algorithms

### Round Robin — Bowler Rotation
- Time Quantum = 6 balls (one over)
- Each over: save current bowler context → load next bowler context
- Constraint enforced: no bowler can bowl consecutive overs
- Analogy: OS preemptive scheduling with time-slice

### Shortest Job First — Batting Order
- Burst time = `expected_balls` (estimated innings length)
- Tail-enders (short burst) promoted ahead of middle order
- Minimises average waiting time (provably optimal for non-preemptive SJF)
- Use `--analysis` to see the wait-time comparison table

### Priority Scheduling — Death Over Specialist
- `MatchIntensity` computed per over (LOW / MEDIUM / HIGH)
- Overs 17–20 (INTENSITY_HIGH): death specialist promoted
- `pthread_setschedparam()` used to elevate real OS thread priority

---

## Deadlock Simulation (Run-Out)

```
Coffman's Four Conditions — all satisfied:
  1. Mutual Exclusion   ✓ (only one batsman per end)
  2. Hold and Wait      ✓ (holds current end, wants other end)
  3. No Preemption      ✓ (can't force batsman back)
  4. Circular Wait      ✓ (A waits for B; B waits for A)

Resource Allocation Graph (RAG):
  Batsman A  ──holds──►  [End 1]
  Batsman A  ◄─wants───  [End 2]
  Batsman B  ──holds──►  [End 2]
  Batsman B  ◄─wants───  [End 1]
  Cycle: A → [E2] → B → [E1] → A  ← DEADLOCK!

Prevention:  Resource ordering (always acquire End1 before End2)
Detection:   Umpire thread polls RAG for cycles every 50ms
Resolution:  Umpire declares lower-average batsman RUN OUT
             (thread "killed" = resources released, wicket decremented)
```

Enable with: `./cricket_sim --deadlock`

---

## Sample Output

```
╔══════════════════════════════════════════════════════════════╗
║         T20 CRICKET OS SIMULATOR — CSC-204                  ║
║         Pitch = CS | Players = Threads | Umpire = Kernel    ║
╚══════════════════════════════════════════════════════════════╝

──────────── OVER 1 ──── Bumrah bowling ────────────────────────
[SCHED  ] CONTEXT SWITCH #1: Loading bowler 'Jasprit Bumrah'
[THREAD ] [T-1234] BOWLER 'Bumrah' thread RUNNING — bowling over 1
[THREAD ] [T-5678] BATSMAN 'Rohit Sharma' ENTERED crease | wait=0.0ms
[THREAD ] Fielder threads sleeping (cond_wait)

[BALL   ]  1.1  Rohit Sharma      •   Score: 0/0
[BALL   ]  1.2  Rohit Sharma      4   Score: 4/0
[BALL   ]  1.3  KL Rahul          1   Score: 5/0
[BALL   ]  1.4  Rohit Sharma      6   Score: 11/0
[BALL   ]  1.5  Rohit Sharma     W(B) Score: 11/1
[DEADLK ] === RUN-OUT SCENARIO: Rohit ↔ KL Rahul ===
[DEADLK ]   Resource Allocation Graph (RAG):
[DEADLK ]   ┌────────────────────────────────────┐
[DEADLK ]   │  Rohit       ──holds──►  [End 0]  │
[DEADLK ]   │  Rohit       ◄─wants───  [End 1]  │
[DEADLK ]   │  KL Rahul    ──holds──►  [End 1]  │
[DEADLK ]   │  KL Rahul    ◄─wants───  [End 0]  │
[DEADLK ]   └────────────────────────────────────┘
[SCHED  ] CONTEXT SWITCH #2: Saving Bumrah: 1 over, 15 runs, 1 wkt
```

---

## Compilation Troubleshooting

| Error | Fix |
|---|---|
| `undefined reference to pthread_*` | Add `-pthread` to compile command |
| `sem_init not found` | Linux only; macOS uses `sem_open` (uncomment macOS section) |
| `'atomic' not found` | Use `-std=c++11` or higher |
| `PTHREAD_MUTEX_ERRORCHECK undeclared` | Use `_POSIX_C_SOURCE=200112L` |

---

## Deliverables Checklist

- [x] Ball-by-ball commentary log
- [x] Thread execution log with timestamps
- [x] Gantt chart data (CSV + Python plotter)
- [x] SJF vs FCFS wait-time analysis table
- [x] Deadlock scenario with RAG visualisation
- [x] 3 scheduling algorithms (RR, SJF, Priority)
- [x] Mutex / Semaphore / Condition Variable / RW Lock
- [x] Configurable via CLI flags
- [x] Modular design (6 separate .cpp/.h pairs)
- [x] Real T20 WC 2026 player names and stats

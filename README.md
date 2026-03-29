# T20 Cricket Simulator

A multi-threaded T20 cricket match simulator demonstrating core Operating System concepts.



## Table of Contents

1.  [Introduction](#-introduction)
2.  [OS Concepts Demonstrated](#-os-concepts-demonstrated)
3.  [Building the Project](#-building-the-project)
4.  [Running the Simulator](#-running-the-simulator)
5.  [Command Line Options](#-command-line-options)
6.  [Project Structure](#-project-structure)
7.  [Synchronization Primitives](#-synchronization-primitives)
8.  [Scheduling Algorithms](#-scheduling-algorithms)
9.  [Deadlock Simulation](#-deadlock-simulation)
10.  [Visualization](#-visualization)
11.  [Sample Output](#-sample-output)
12.  [Technical Details](#-technical-details)



## Introduction

This is an advanced, multi-threaded cricket match simulation that demonstrates core **Operating System concepts** through T20 cricket. The project models a complete cricket match using real threading primitives.



## OS Concepts Demonstrated

| Cricket Element | OS Concept       | Description                     |
|-----------------|------------------|---------------------------------|
| **Pitch**       | Critical Section | Only 1 bowler writes at a time  |
| **Players**     | Threads          | `pthread_t` handles             |
| **Global Score**| Shared Resource  | Protected by mutex              |
| **Umpire**      | Kernel/Scheduler | Manages match transitions       |
| **Crease**      | Semaphore        | Capacity = 2 batsmen            |
| **Fielders**    | Blocked Threads  | `cond_wait` until ball in air   |
| **Over Switch** | Context Switch   | RR scheduler, quantum = 6 balls |
| **Run-out**     | Deadlock         | Circular wait on resources      |




## Building the Project

```bash
# Compile the project
make

# Clean build artifacts
make clean

# Rebuild from scratch
make clean && make
```



## Running the Simulator

```bash
# Default: 5 overs, Round Robin scheduler
./cricket_sim

# Various configurations
./cricket_sim --scheduler=PRIORITY --overs=5 --deadlock
./cricket_sim --scheduler=SJF --analysis --verbose
./cricket_sim --gantt --speed=10

# Custom teams
./cricket_sim --team1=Royal_Challengers --team2=Delhi_Capitals --overs=20
```



## Command Line Options

| Option                          | Description                       | Default             |
|---------------------------------|-----------------------------------|---------------------|
| `--scheduler=RR\|SJF\|PRIORITY` | Scheduling algorithm              | `RR`                |
| `--team1=<name>`                | First team name                   | Mumbai Indians      |
| `--team2=<name>`                | Second team name                  | Chennai Super Kings |
| `--overs=<n>`                   | Number of overs (1-20)            | `5`                 |
| `--deadlock`                    | Enable run-out deadlock simulation| `Off`               |
| `--gantt`                       | Generate Gantt chart CSV          | `Off`               |
| `--analysis`                    | Print SJF vs FCFS analysis        | `Off`               |
| `--speed=<n>`                   | Speed (1=slow, 10=instant)        | `1`                 |
| `--verbose`                     | Debug-level output                | `Off`               |
| `--log=<file>`                  | Log file path                     | `logs/match.log`    |
| `--seed=<n>`                    | RNG seed                          | time-based          |



##  Project Structure

```
T20CricketSimulator/
│
├── include/                 # Header files
│   ├── cricket_types.h        # Core types & structures
│   ├── globals.h              # Global variables
│   ├── match_engine.h         # Match engine class
│   ├── player_threads.h       # Thread entry functions
│   ├── scheduler.h             # Scheduling algorithms
│   ├── synchronization.h         # Sync primitives
│   └── utils.h                 # Logging utilities
│
├── src/                    # Source implementation
│   ├── main.cpp              # Entry point
│   ├── match_engine.cpp
│   ├── player_threads.cpp
│   ├── scheduler.cpp
│   ├── synchronization.cpp
│   └── utils.cpp
│
├── logs/                   # Match logs
│   ├── gantt_data.csv
│   ├── match_full.log
│   └── match_rr.log
│
├── visualizer/             # Visualization tools
│   └── gantt_plotter.py
│
├── Makefile
└── README.md
```



## Synchronization Primitives

### Mutexes ( Mutual Exclusion )
- `score_mutex` - Protects global score updates
- `pitch_mutex` - Protects the pitch (critical section)
- `state_mutex` - Protects match state transitions
- `field_mutex` - Protects fielding action
- `rag_mutex` - Protects deadlock detection (RAG)
- `log_mutex` - Protects log file writes

### Semaphores ( Counting )
- `crease_semaphore(2)` - Counting semaphore for crease capacity (3rd batsman blocks until slot frees)

### Condition Variables ( Signaling )
- `ball_hit_cond` - Fielders sleep here until ball in air
- `delivery_ready_cond` - Batsman waits for bowler delivery
- `stroke_done_cond` - Bowler waits for batsman stroke
- `striker_changed_cond` - Non-striker blocks instead of busy-wait

### Reader-Writer Lock
- `scoreboard_rwlock` - Multiple readers, exclusive writer (Non-blocking live score display)



##  Scheduling Algorithms

###  Round Robin (RR)
- **Time Quantum**: 1 over (6 balls)
- Bowlers rotate after every over
- Context switches saved/restored

###  Shortest Job First (SJF)
- Batting order sorted by expected balls faced
- Minimizes wait time for tail-enders

###  Priority-Based
- Death-over specialists promoted in death overs
- Uses `pthread_setschedparam()` for thread priority



##  Deadlock Simulation

Run-out deadlock demonstration can be enabled:

```bash
./cricket_sim --deadlock
```

**How it works:**
1. Both batsmen attempt to run to the same end
2. Circular wait on crease resource
3. Detection via **Resource Allocation Graph (RAG)**
4. Resolution by selecting a victim



##  Visualization

Generate **Gantt chart** data for analysis:

```bash
# Generate CSV data
./cricket_sim --gantt

# Visualize with Python
pip install matplotlib
python3 visualizer/gantt_plotter.py
```



##  Sample Output

```
═══════════════════════════════════════════════════════════════════════════════
            T20 CRICKET MATCH SIMULATOR 
           Operating Systems Major Project
═══════════════════════════════════════════════════════════════════════════════

  OS Concept Mapping:
    Pitch         = Critical Section  (pitch_mutex)
    Bowler        = Writer thread
    Batsman       = Reader thread
    Crease        = Semaphore(2)
    Score         = Shared resource    (score_mutex)
    Over switch   = Context Switch    (RR scheduler)
    Run-out       = Deadlock         (circular wait)

  Configuration:
    Teams       : Mumbai Indians vs Chennai Super Kings
    Overs       : 5
    Scheduler   : Round Robin

═════════════════════════════════════════════════════════════════════════════
```



##  Technical Details

| Specification   | Value                  |
|-----------------|------------------------|
| **Language**    | C++ with POSIX Threads |
| **Platform**    | Linux/Unix             |
| **Max Players** | 11 per team            |
| **Max Overs**   | 20                     |
| **Balls/Over**  | 6                      |
| **Max Fielders**| 10                     |



##  Dependencies

- POSIX Threads (`pthread`)
- C++11 Standard Library
- Python 3 (optional, for visualization)
- matplotlib (optional)



##  Teams Included

### Batting Order
- Openers → Anchors → Middle Order → Finishers

### Bowling Attack
- Opening Swing → Spinners → Death Specialists



##  Game Phases

```
┌─────────────────────────────────────────────────┐
│  POWERPLAY (Overs 1-6)                          │
│  Field restrictions, aggressive batting         │
├─────────────────────────────────────────────────┤
│  MIDDLE OVERS (Overs 7-14)                      │
│  Build platform, bowling changes                │
├─────────────────────────────────────────────────┤
│  DEATH OVERS (Overs 15-20)                      │
│  Aggressive batting, tight bowling              │
└─────────────────────────────────────────────────┘
```




##  Future Improvements

- GUI scoreboard
- Live match visualization
- More scheduling algorithms
- Network multiplayer simulation




## Acknowledgments

This project was created as part of learning Operating Systems concepts and aims to demonstrate how theoretical concepts like threads, semaphores, mutexes, scheduling, and deadlocks can be applied in a real-world simulation using the exciting game of cricket.




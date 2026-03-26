# Makefile — T20 Cricket OS Simulator
#----------------------------------------------------------------------------------
# Targets:
#   make             - build the simulator (default)
#   make clean       - remove build artifacts
#   make run         - build and run with Round Robin scheduler (5 overs demo)
#   make run-sjf     - run with SJF batting order + analysis table
#   make run-priority- run with Priority scheduling (death over specialist)
#   make run-deadlock- run with deadlock simulation enabled
#   make run-full    - full 20-over match with all features
#   make gantt       - generate Gantt chart (requires matplotlib)
#   make docs        - run with verbose output, save to logs/
#   make test        - quick 2-over sanity check

CXX      := g++
CXXFLAGS := -std=c++17 -pthread -O2 -Wall -Wextra -I include
SRCS     := src/main.cpp \
            src/synchronization.cpp \
            src/utils.cpp \
            src/scheduler.cpp \
            src/player_threads.cpp \
            src/match_engine.cpp
TARGET   := cricket_sim

# Build 
.PHONY: all clean run run-sjf run-priority run-deadlock run-full gantt docs test

all: $(TARGET)

$(TARGET): $(SRCS)
	@echo "==> Compiling T20 Cricket OS Simulator..."
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET)
	@echo "==> Build successful: ./$(TARGET)"
	@echo ""
	@echo "Quick start:"
	@echo "  make run          — 5-over demo (Round Robin)"
	@echo "  make run-sjf      — SJF scheduler + analysis"
	@echo "  make run-priority — Priority scheduling (death overs)"
	@echo "  make run-deadlock — Deadlock simulation"
	@echo "  make run-full     — Full 20-over match"

clean:
	rm -f $(TARGET) logs/*.log logs/*.csv logs/*.png
	@echo "==> Cleaned"

# Run targets 

# Default: 5-over demo, Round Robin, speed 3x
run: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=RR --overs=5 --speed=3 \
	            --team1=Mumbai_Indians --team2=Chennai_Super_Kings \
	            --log=logs/match_rr.log

# SJF batting order + wait time analysis table
run-sjf: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=SJF --overs=5 --speed=3 --analysis \
	            --log=logs/match_sjf.log

# Priority scheduling for death overs
run-priority: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=PRIORITY --overs=5 --speed=3 \
	            --log=logs/match_priority.log

# Deadlock simulation (run-out scenario)
run-deadlock: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=RR --overs=5 --speed=3 --deadlock \
	            --log=logs/match_deadlock.log

# Full 20-over match, all features, Gantt enabled
run-full: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=PRIORITY --overs=20 --speed=5 \
	            --deadlock --gantt --analysis \
	            --log=logs/match_full.log
	@echo ""
	@echo "Log saved to logs/match_full.log"
	@echo "Gantt data saved to logs/gantt_data.csv"

# Verbose debug run
docs: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=RR --overs=3 --speed=1 --verbose \
	            --gantt --analysis --deadlock \
	            --log=logs/match_verbose.log

# Quick 2-over sanity test
test: $(TARGET)
	@echo "==> Running 2-over sanity test..."
	./$(TARGET) --scheduler=RR --overs=2 --speed=10 --seed=42

# Generate Gantt chart (requires logs/gantt_data.csv from run-full)
gantt:
	@echo "==> Generating Gantt chart..."
	python3 visualizer/gantt_plotter.py \
	        --input logs/gantt_data.csv \
	        --out-gantt logs/gantt_chart.png \
	        --out-analysis logs/wait_analysis.png
	@echo "==> Charts saved to logs/"

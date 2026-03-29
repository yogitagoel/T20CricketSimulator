# Makefile — T20 Cricket OS Simulator
CXX      := g++
CXXFLAGS := -std=c++17 -pthread -O2 -Wall -Wextra -I include
SRCS     := src/main.cpp src/synchronization.cpp src/utils.cpp \
            src/scheduler.cpp src/player_threads.cpp src/match_engine.cpp
TARGET   := cricket_sim

.PHONY: all clean run run-sjf run-priority run-deadlock run-full \
        view view-sjf view-priority view-deadlock view-full gantt test

all: $(TARGET)

$(TARGET): $(SRCS)
	@echo "==> Compiling..."
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET)
	@echo "==> Build OK: ./$(TARGET)"

clean:
	rm -f $(TARGET) logs/*.log logs/*.csv logs/*.png

# ── Standard runs (output saved to log) ───────────────────────────────────────
run: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=RR --overs=20 --speed=5 --log=logs/match_rr.log

run-sjf: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=SJF --overs=20 --speed=5 --analysis --log=logs/match_sjf.log

run-priority: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=PRIORITY --overs=20 --speed=5 --log=logs/match_priority.log

run-deadlock: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=RR --overs=20 --speed=5 --deadlock --log=logs/match_deadlock.log

run-full: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=PRIORITY --overs=20 --speed=5 \
	            --deadlock --gantt --analysis --log=logs/match_full.log

# ── View targets — full scrollable output in terminal (colors preserved) ───────
view: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=RR --overs=20 --speed=5 --log=logs/match_rr.log | less -R

view-sjf: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=SJF --overs=20 --speed=5 --analysis \
	            --log=logs/match_sjf.log | less -R

view-priority: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=PRIORITY --overs=20 --speed=5 \
	            --log=logs/match_priority.log | less -R

view-deadlock: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=RR --overs=20 --speed=5 --deadlock \
	            --log=logs/match_deadlock.log | less -R

view-full: $(TARGET)
	@mkdir -p logs
	./$(TARGET) --scheduler=PRIORITY --overs=20 --speed=5 \
	            --deadlock --gantt --analysis --log=logs/match_full.log | less -R

test: $(TARGET)
	./$(TARGET) --scheduler=RR --overs=2 --speed=10 --seed=42

gantt:
	python3 visualizer/gantt_plotter.py --input logs/gantt_data.csv \
	        --out-gantt logs/gantt_chart.png --out-analysis logs/wait_analysis.png

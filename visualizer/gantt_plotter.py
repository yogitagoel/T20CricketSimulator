#!/usr/bin/env python3
"""
gantt_plotter.py
================
Reads logs/gantt_data.csv produced by the C++ simulator and generates:
  1. Gantt chart   — thread vs. time, coloured by event type
  2. Wait-time bar chart  — SJF vs FCFS comparison (if analysis data present)

Usage:
    python3 visualizer/gantt_plotter.py [--input logs/gantt_data.csv]

Requirements:
    pip install matplotlib pandas
"""

import csv
import sys
import os
import argparse
import matplotlib
matplotlib.use('Agg')  # headless rendering
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.ticker import MultipleLocator

# ── Configuration ──────────────────────────────────────────────────────────────
COLORS = {
    'BOWL':    '#185FA5',  # blue  — bowler owns pitch
    'BAT':     '#1D9E75',  # teal  — batsman shot
    'WAIT':    '#B4B2A9',  # gray  — thread waiting
    'FIELD':   '#BA7517',  # amber — fielder active
    'MUTEX_S': '#D85A30',  # coral — score mutex locked
    'MUTEX_P': '#7F77DD',  # purple— pitch mutex locked
    'SWITCH':  '#639922',  # green — context switch
    'DEADLOCK':'#E24B4A',  # red   — deadlock event
}

OUTCOME_COLOR = {
    '•':    '#B4B2A9',  # dot
    '1':    '#1D9E75',  # single
    '2':    '#1D9E75',
    '3':    '#1D9E75',
    '4':    '#639922',  # boundary
    '6':    '#3B6D11',  # six
    'W(B)': '#E24B4A',  # wicket-bowled
    'W(L)': '#E24B4A',  # wicket-lbw
    'W(C)': '#E24B4A',  # caught
    'W(R)': '#A32D2D',  # run-out
    'Wd':   '#BA7517',  # wide
    'Nb':   '#BA7517',  # no-ball
}


def load_csv(filepath):
    events = []
    try:
        with open(filepath, newline='') as f:
            reader = csv.DictReader(f)
            for row in reader:
                events.append(row)
    except FileNotFoundError:
        print(f"[ERROR] File not found: {filepath}")
        print("  Run the simulator first: ./cricket_sim --gantt")
        sys.exit(1)
    return events


def make_gantt(events, out_path='logs/gantt_chart.png'):
    if not events:
        print("[WARN] No events to plot")
        return

    # Collect unique threads
    threads = []
    seen = set()
    for ev in events:
        bid = f"Bowler-{ev['bowler_id']}"
        bat = f"Batsman-{ev['batsman_id']}"
        for t in [bid, bat]:
            if t not in seen:
                threads.append(t)
                seen.add(t)
    threads.sort()

    # Add synthetic rows for mutex and context-switch
    threads += ['score_mutex', 'pitch_mutex', 'ctx_switch']
    idx_map  = {t: i for i, t in enumerate(threads)}
    n        = len(threads)

    fig, ax = plt.subplots(figsize=(22, max(6, n * 0.7)))
    fig.patch.set_facecolor('#1e1e2e')
    ax.set_facecolor('#1e1e2e')

    # Draw bars
    for ev in events:
        try:
            t_start  = float(ev['pitch_acquire_ms'])
            t_end    = float(ev['pitch_release_ms'])
            duration = max(t_end - t_start, 5.0)  # min 5ms for visibility
            over     = int(ev['over'])
            ball     = int(ev['ball'])
            outcome  = ev['outcome'].strip()
            runs     = int(ev['runs'])

            # Bowler bar
            bid   = f"Bowler-{ev['bowler_id']}"
            y_bow = idx_map.get(bid, 0)
            color = COLORS['BOWL']
            ax.barh(y_bow, duration, left=t_start - float(events[0]['timestamp_ms']),
                    height=0.5, color=color, align='center',
                    edgecolor='#2e2e4e', linewidth=0.3)
            ax.text(t_start - float(events[0]['timestamp_ms']) + duration / 2,
                    y_bow, f'{over}.{ball}', ha='center', va='center',
                    fontsize=7, color='white', fontweight='bold')

            # Batsman bar
            bat_id = f"Batsman-{ev['batsman_id']}"
            y_bat  = idx_map.get(bat_id, 1)
            b_col  = OUTCOME_COLOR.get(outcome, COLORS['BAT'])
            ax.barh(y_bat, duration * 1.5, left=t_start - float(events[0]['timestamp_ms']),
                    height=0.5, color=b_col, align='center',
                    edgecolor='#2e2e4e', linewidth=0.3)
            ax.text(t_start - float(events[0]['timestamp_ms']) + duration * 0.75,
                    y_bat, outcome, ha='center', va='center',
                    fontsize=8, color='white', fontweight='bold')

            # Score mutex bar (brief, every scoring ball)
            if runs > 0:
                y_m = idx_map.get('score_mutex', n - 3)
                ax.barh(y_m, 3.0, left=t_start - float(events[0]['timestamp_ms']),
                        height=0.3, color=COLORS['MUTEX_S'], align='center',
                        edgecolor='none')

        except (ValueError, KeyError):
            continue

    # Over boundary separators
    if events:
        t0   = float(events[0]['timestamp_ms'])
        prev = None
        for ev in events:
            over = int(ev['over'])
            if over != prev and prev is not None:
                x = float(ev['pitch_acquire_ms']) - t0
                ax.axvline(x, color='#444466', linestyle='--', linewidth=0.8)
                ax.text(x + 2, n - 0.3, f'O{over}', fontsize=7,
                        color='#8888aa', va='top')
            prev = over

    # Axis labels and styling
    ax.set_yticks(range(n))
    ax.set_yticklabels(threads, fontsize=8, color='#c0c0d0')
    ax.set_xlabel('Elapsed time (ms)', color='#c0c0d0', fontsize=10)
    ax.set_title('T20 Cricket Simulator — Thread Gantt Chart (Pitch Resource)',
                 color='white', fontsize=13, pad=12)
    ax.tick_params(colors='#888899')
    ax.spines['bottom'].set_color('#444466')
    ax.spines['left'].set_color('#444466')
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)

    # Legend
    legend_patches = [
        mpatches.Patch(color=COLORS['BOWL'],    label='Bowler (pitch write)'),
        mpatches.Patch(color=COLORS['BAT'],     label='Batsman shot (score)'),
        mpatches.Patch(color='#639922',          label='Boundary (4/6)'),
        mpatches.Patch(color='#E24B4A',          label='Wicket'),
        mpatches.Patch(color=COLORS['MUTEX_S'], label='score_mutex locked'),
    ]
    ax.legend(handles=legend_patches, loc='lower right',
              facecolor='#2e2e4e', edgecolor='#444466',
              labelcolor='#c0c0d0', fontsize=8)

    plt.tight_layout()
    plt.savefig(out_path, dpi=150, bbox_inches='tight', facecolor=fig.get_facecolor())
    print(f"[OK] Gantt chart saved → {out_path}")
    plt.close()


def make_wait_chart(events, out_path='logs/wait_analysis.png'):
    """Bar chart: over-by-over runs scored (proxy for wait analysis)."""
    if not events:
        return

    over_runs = {}
    for ev in events:
        try:
            ov   = int(ev['over'])
            runs = int(ev['runs'])
            over_runs[ov] = over_runs.get(ov, 0) + runs
        except (ValueError, KeyError):
            continue

    if not over_runs:
        return

    overs = sorted(over_runs.keys())
    runs  = [over_runs[o] for o in overs]

    fig, ax = plt.subplots(figsize=(14, 5))
    fig.patch.set_facecolor('#1e1e2e')
    ax.set_facecolor('#1e1e2e')

    bars = ax.bar(overs, runs, color='#185FA5', edgecolor='#2e2e4e', linewidth=0.5)

    # Colour death overs differently
    for i, ov in enumerate(overs):
        if ov >= 17:
            bars[i].set_color('#E24B4A')
        elif ov >= 11:
            bars[i].set_color('#BA7517')

    for bar, run in zip(bars, runs):
        ax.text(bar.get_x() + bar.get_width() / 2., bar.get_height() + 0.1,
                str(run), ha='center', va='bottom', fontsize=8, color='white')

    ax.set_xlabel('Over Number', color='#c0c0d0')
    ax.set_ylabel('Runs Scored', color='#c0c0d0')
    ax.set_title('Runs Per Over (Blue=Powerplay, Amber=Middle, Red=Death)',
                 color='white', fontsize=11)
    ax.tick_params(colors='#888899')
    ax.set_xticks(overs)
    for spine in ax.spines.values():
        spine.set_color('#444466')

    plt.tight_layout()
    plt.savefig(out_path, dpi=150, bbox_inches='tight',
                facecolor=fig.get_facecolor())
    print(f"[OK] Wait-time analysis chart saved → {out_path}")
    plt.close()


def main():
    parser = argparse.ArgumentParser(description='T20 Simulator Gantt Plotter')
    parser.add_argument('--input', default='logs/gantt_data.csv',
                        help='Path to gantt_data.csv')
    parser.add_argument('--out-gantt',  default='logs/gantt_chart.png')
    parser.add_argument('--out-analysis', default='logs/wait_analysis.png')
    args = parser.parse_args()

    print(f"Loading: {args.input}")
    events = load_csv(args.input)
    print(f"  {len(events)} ball events loaded")

    os.makedirs('logs', exist_ok=True)

    make_gantt(events, args.out_gantt)
    make_wait_chart(events, args.out_analysis)
    print("\nDone! Open the PNG files in logs/ to view the charts.")


if __name__ == '__main__':
    main()

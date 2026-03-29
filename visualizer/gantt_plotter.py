#!/usr/bin/env python3
"""gantt_plotter.py — reads logs/gantt_data.csv and produces charts."""
import csv, sys, os, argparse
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

COLORS = {
    'BOWL':'#185FA5','BAT':'#1D9E75','WAIT':'#B4B2A9',
    'MUTEX_S':'#D85A30','SWITCH':'#639922','DEADLOCK':'#E24B4A',
}
OUTCOME_COLOR = {
    '•':'#B4B2A9','1':'#1D9E75','2':'#1D9E75','3':'#1D9E75',
    '4':'#639922','6':'#3B6D11','W(B)':'#E24B4A','W(L)':'#E24B4A',
    'W(C)':'#E24B4A','W(R)':'#A32D2D','Wd':'#BA7517','Nb':'#BA7517',
}

def load_csv(path):
    try:
        with open(path) as f:
            return list(csv.DictReader(f))
    except FileNotFoundError:
        print(f"[ERROR] {path} not found. Run: ./cricket_sim --gantt"); sys.exit(1)

def make_gantt(events, out):
    if not events: return
    threads, seen = [], set()
    for ev in events:
        for t in [f"Bowler-{ev['bowler_id']}", f"Batsman-{ev['batsman_id']}"]:
            if t not in seen: threads.append(t); seen.add(t)
    threads.sort()
    threads += ['score_mutex','ctx_switch']
    idx = {t:i for i,t in enumerate(threads)}
    n   = len(threads)
    fig, ax = plt.subplots(figsize=(22, max(6, n*0.7)))
    fig.patch.set_facecolor('#1e1e2e'); ax.set_facecolor('#1e1e2e')
    t0 = float(events[0]['timestamp_ms'])
    for ev in events:
        try:
            ts = float(ev['pitch_acquire_ms']); te = float(ev['pitch_release_ms'])
            dur = max(te-ts, 5.0); ov=int(ev['over']); ball=int(ev['ball'])
            out_s = ev['outcome'].strip()
            bid = f"Bowler-{ev['bowler_id']}"; bat = f"Batsman-{ev['batsman_id']}"
            ax.barh(idx.get(bid,0), dur, left=ts-t0, height=0.5,
                    color=COLORS['BOWL'], edgecolor='#2e2e4e', linewidth=0.3)
            ax.text(ts-t0+dur/2, idx.get(bid,0), f'{ov}.{ball}',
                    ha='center', va='center', fontsize=7, color='white', fontweight='bold')
            bc = OUTCOME_COLOR.get(out_s, COLORS['BAT'])
            ax.barh(idx.get(bat,1), dur*1.5, left=ts-t0, height=0.5,
                    color=bc, edgecolor='#2e2e4e', linewidth=0.3)
            ax.text(ts-t0+dur*0.75, idx.get(bat,1), out_s,
                    ha='center', va='center', fontsize=8, color='white', fontweight='bold')
            if int(ev['runs']) > 0:
                ax.barh(idx.get('score_mutex', n-2), 3.0, left=ts-t0,
                        height=0.3, color=COLORS['MUTEX_S'], edgecolor='none')
        except (ValueError, KeyError): continue
    ax.set_yticks(range(n)); ax.set_yticklabels(threads, fontsize=8, color='#c0c0d0')
    ax.set_xlabel('Elapsed time (ms)', color='#c0c0d0')
    ax.set_title('T20 Cricket Simulator — Thread Gantt Chart', color='white', fontsize=13)
    ax.tick_params(colors='#888899')
    for sp in ax.spines.values(): sp.set_color('#444466')
    ax.spines['top'].set_visible(False); ax.spines['right'].set_visible(False)
    legend = [mpatches.Patch(color=COLORS['BOWL'], label='Bowler (pitch write)'),
              mpatches.Patch(color=COLORS['BAT'],  label='Batsman shot'),
              mpatches.Patch(color='#639922',       label='Boundary'),
              mpatches.Patch(color='#E24B4A',       label='Wicket')]
    ax.legend(handles=legend, loc='lower right', facecolor='#2e2e4e',
              edgecolor='#444466', labelcolor='#c0c0d0', fontsize=8)
    plt.tight_layout()
    plt.savefig(out, dpi=150, bbox_inches='tight', facecolor=fig.get_facecolor())
  print(f"[OK] Gantt chart -> {out}"); plt.close()

def make_run_chart(events, out):
    over_runs = {}
    for ev in events:
        try: ov=int(ev['over']); over_runs[ov]=over_runs.get(ov,0)+int(ev['runs'])
        except: continue
    if not over_runs: return
    overs = sorted(over_runs); runs = [over_runs[o] for o in overs]
    fig, ax = plt.subplots(figsize=(14,5))
    fig.patch.set_facecolor('#1e1e2e'); ax.set_facecolor('#1e1e2e')
    bars = ax.bar(overs, runs, color='#185FA5', edgecolor='#2e2e4e', linewidth=0.5)
    for i,ov in enumerate(overs):
        if ov >= 17: bars[i].set_color('#E24B4A')
        elif ov >= 11: bars[i].set_color('#BA7517')
    for bar,run in zip(bars,runs):
        ax.text(bar.get_x()+bar.get_width()/2, bar.get_height()+0.1,
                str(run), ha='center', va='bottom', fontsize=8, color='white')
    ax.set_xlabel('Over', color='#c0c0d0'); ax.set_ylabel('Runs', color='#c0c0d0')
    ax.set_title('Runs Per Over (Blue=Powerplay, Amber=Middle, Red=Death)',
                 color='white', fontsize=11)
    ax.tick_params(colors='#888899'); ax.set_xticks(overs)
    for sp in ax.spines.values(): sp.set_color('#444466')
    plt.tight_layout()
    plt.savefig(out, dpi=150, bbox_inches='tight', facecolor=fig.get_facecolor())
    print(f"[OK] Run chart -> {out}"); plt.close()

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--input',        default='logs/gantt_data.csv')
    p.add_argument('--out-gantt',    default='logs/gantt_chart.png')
    p.add_argument('--out-analysis', default='logs/wait_analysis.png')
    args = p.parse_args()
    print(f"Loading {args.input}")
    events = load_csv(args.input)
    print(f"  {len(events)} ball events")
    os.makedirs('logs', exist_ok=True)
    make_gantt(events, args.out_gantt)
    make_run_chart(events, args.out_analysis)
    print("Done.")

if __name__ == '__main__':
    main()


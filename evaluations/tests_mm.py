"""
tests over the built-in LTE/5G traces
mahimahi keeps traces in /mahimahi/traces/
each trace is tested as: mm-delay <rtt/2> mm-link <up> <down> -- sender

usage:
  # built-in mahimahi traces only
  python tests_mm.py --port 9800

  # specific subset
  python tests_mm.py --filter TMobile --port 9800
"""
import argparse, subprocess, time, tempfile
from pathlib import Path
from utils import RECEIVER_BIN, RESULTS_DIR, cleanup, parse_log, summarise, save, make_script, sender_cmd

_ROOT = Path(__file__).resolve().parent.parent.parent

# there may be multiple formats for different PCs
MAHIMAHI_TRACE_DIRS = [_ROOT/"mahimahi"/"traces"]

def find_mahimahi_traces(filter_str=""):
  """return list of (name, up_path, down_path) for paired traces."""
  pairs = []
  for d in MAHIMAHI_TRACE_DIRS:
    if not d.exists(): continue
    ups = sorted(d.glob("*.up"))
    for up in ups:
      down = up.with_suffix(".down")
      if not down.exists(): continue
      name = up.stem  # e.g. "TMobile-LTE-driving"
      if filter_str and filter_str.lower() not in name.lower(): continue
      pairs.append((name, up, down))
  return pairs

def run_trace(name, up, down, rtt, dur, port):
  """run one sender/receiver session over a given trace pair."""
  procs = []
  with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    log = tmp / "send.log"
    try:
      procs.append(subprocess.Popen([RECEIVER_BIN, str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
      time.sleep(0.3)
      script = make_script(tmp, [sender_cmd(port, dur, log)])
      mm_cmd = f"mm-delay {rtt // 2} mm-link {up} {down} -- {script}"
      procs.append(subprocess.Popen(mm_cmd, shell=True))
      time.sleep(dur + 4)
    finally:
      cleanup(procs)
    raw = log.read_text() if log.exists() else ""
  burs, bitrates, delays = parse_log(raw)
  s = summarise(burs, bitrates, delays, label=name)
  return s

def run(args):
  results = []

  # built-in mahimahi traces
  builtin = find_mahimahi_traces(args.filter)
  if not builtin:
    print("[!] no mahimahi traces found — check MAHIMAHI_TRACE_DIRS in this script")
  for name, up, down in builtin:
    print(f"running {name} ...", end=" ", flush=True)
    s = run_trace(name, up, down, args.rtt, args.dur, args.port)
    results.append(s)
    print(f"avg_br={s['avg_bitrate']} mbps  avg_delay={s['avg_delay']} ms  stall={s['stall_100ms']*100:.2f}%")

  if results:
    save({"test": "tests_mm", "results": results}, "tests_mm")
    # print summary table
    RESULTS_DIR.mkdir(exist_ok=True)
    summary_file = RESULTS_DIR / "mahimahi_tabular_summary.txt"
    with open(summary_file, "w") as f:
      f.write(f"{'trace':<25} {'avg_rate':>10} {'avg_delay':>10} {'p99_delay':>10} {'stall_100ms%':>12}\n")
      f.write("-" * 75 + "\n")
      for s in results:
        f.write(f"{s['label']:<25} {s['avg_bitrate']:>9.2f}m {s['avg_delay']:>8.1f}ms "
                f"{s['p99_delay']:>8.1f}ms {s['stall_100ms']*100:>11.3f}%\n")
        
    print(f"\nTests complete. Analysis written to: {summary_file}")

if __name__ == "__main__":
  p = argparse.ArgumentParser()
  p.add_argument("--filter",   default="", help="only run traces whose name contains this string")
  p.add_argument("--dur",  type=int, default=20)
  p.add_argument("--rtt",  type=int, default=20)
  p.add_argument("--port", type=int, default=9800)
  run(p.parse_args())
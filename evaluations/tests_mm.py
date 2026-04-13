"""
tests over the built-in LTE/5G traces
mahimahi ships traces in /usr/share/mahimahi/traces/
each trace is tested as: mm-delay <rtt/2> mm-link <up> <down> -- sender

usage:
  # built-in mahimahi traces only
  python tests_mm.py --port 9800

  # specific subset
  python tests_mm.py --filter TMobile --port 9800
"""
import argparse, csv, subprocess, time, tempfile, json
from pathlib import Path
from utils import RECEIVER_BIN, cleanup, parse_log, summarise, save, make_script, sender_cmd

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
  with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    log = tmp / "send.log"
    procs = []
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

  # custom 5g traces from manifest
  if args.manifest:
    manifest = Path(args.manifest)
    if not manifest.exists():
      print(f"[!] manifest not found: {manifest}"); 
    else:
      with open(manifest) as f:
        rows = list(csv.DictReader(f))
      for row in rows:
        name = row["tag"]
        if args.filter and args.filter.lower() not in name.lower(): continue
        up   = Path(row["ul_trace"])
        down = Path(row["dl_trace"])
        rtt  = int(float(row.get("latency_ms", args.rtt)))
        if not up.exists() or not down.exists():
          print(f"  skipping {name}: trace files missing"); continue
        print(f"running 5g/{name} ...", end=" ", flush=True)
        s = run_trace(name, up, down, rtt, args.dur, args.port)
        s["label"] = f"5g/{name}"
        results.append(s)
        print(f"avg_br={s['avg_bitrate']} mbps  avg_delay={s['avg_delay']} ms")

  if results:
    save({"test": "tests_mm", "results": results}, "tests_mm")
    # print summary table
    print(f"\n{'trace':<25} {'avg_rate':>8} {'avg_delay':>9} {'p99_delay':>9} {'stall_100ms%':>8}")
    print("-" * 80)
    for s in results:
      print(f"{s['label']:<20} {s['avg_bitrate']:>7.2f}m {s['avg_delay']:>8.1f}ms "
            f"{s['p99_delay']:>8.1f}ms {s['stall_100ms']*100:>7.3f}%")

if __name__ == "__main__":
  p = argparse.ArgumentParser()
  p.add_argument("--manifest", default=None, help="path to manifest.csv from csv_to_mahimahi.py")
  p.add_argument("--filter",   default="", help="only run traces whose name contains this string")
  p.add_argument("--dur",  type=int, default=20)
  p.add_argument("--rtt",  type=int, default=20)
  p.add_argument("--port", type=int, default=9800)
  run(p.parse_args())
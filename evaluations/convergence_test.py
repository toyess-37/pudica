# convergence test script
import argparse, subprocess, time, tempfile
import numpy as np
from pathlib import Path
from utils import (
  TRACES_DIR, RECEIVER_BIN, SENDER_BIN, TARGET_IP,
  const_trace, cleanup, parse_jsonl, save, make_script, sender_cmd
)

def measure_convergence_ms(bitrates, swap_frame, threshold=0.05, window=10):
  # how many ms after swap_frame until bitrate stabilizes
  if swap_frame >= len(bitrates) - window:
    print(f"  [!] warning: test duration too short to measure convergence (len={len(bitrates)})")
    return None
  for i in range(swap_frame, len(bitrates) - window):
    w = bitrates[i:i + window]
    mean = np.mean(w)
    if mean > 0 and (max(w) - min(w)) / mean < threshold:
      return round((i - swap_frame) * 16.666, 1)
  return None # did not converge

def run(args):
  TRACES_DIR.mkdir(exist_ok=True)
  total_dur = args.dur + args.stagger * args.flows
  trace = TRACES_DIR / f"conv_{args.bw}Mbps.up"
  const_trace(trace, args.bw, total_dur)
  print(f"[*] Convergence test: {args.flows} flows, stagger={args.stagger}s")

  procs = []
  with tempfile.TemporaryDirectory() as tmpdir:
    tmp = Path(tmpdir)
    logs = [tmp / f"flow{i}.jsonl" for i in range(args.flows)]

    try:
      for i in range(args.flows):
        procs.append(subprocess.Popen(
          [RECEIVER_BIN, str(args.port + i)],
          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        ))
      time.sleep(0.4)

      cmds = []
      for i in range(args.flows):
        delay = f"sleep {i * args.stagger} && " if i > 0 else ""
        cmds.append(
          f"({delay}{SENDER_BIN} --log {logs[i]} {TARGET_IP} {args.port+i} "
          f"{args.dur} > /dev/null 2>&1) &"
        )
      cmds.append("wait")
      script = make_script(tmp, cmds)

      mm_cmd = (f"mm-delay {args.rtt // 2} mm-link {trace} {trace} -- {script}")
      procs.append(subprocess.Popen(mm_cmd, shell=True))
      time.sleep(total_dur + 4)
    finally:
      cleanup(procs)
 
    convergence_times = []
    for i, log in enumerate(logs):
      try:
        burs, bitrates, _ = parse_jsonl(log)
      except (ValueError, FileNotFoundError):
        burs, bitrates = [], []
      entry_frame = int(i * args.stagger * 1000 / 16.666)
      conv = measure_convergence_ms(bitrates, entry_frame)
      convergence_times.append(conv)
      print(f"  flow {i}: entry_frame={entry_frame} convergence={conv} ms")
 
    save({
    "test": "convergence",
    "flows": args.flows,
    "convergence_ms_per_flow": convergence_times,
    "avg_convergence_ms": round(
      np.mean([c for c in convergence_times if c is not None]), 1)
      if any(c is not None for c in convergence_times) else None,
    }, "convergence")

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--flows", type=int, default=4)
  parser.add_argument("--bw", type=float, default=20)
  parser.add_argument("--dur", type=int, default=40)
  parser.add_argument("--stagger", type=int, default=10)
  parser.add_argument("--rtt", type=int, default=20)
  parser.add_argument("--port", type=int, default=9900)
  run(parser.parse_args())

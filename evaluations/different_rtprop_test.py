# test for different rtprops
import argparse, subprocess, time, tempfile
import numpy as np
from pathlib import Path
from utils import (
  TRACES_DIR, RECEIVER_BIN, SENDER_BIN, TARGET_IP,
  const_trace, cleanup, parse_jsonl, summarise, save, make_script, sender_cmd
)

def run(args):
  TRACES_DIR.mkdir(exist_ok=True)
  trace = TRACES_DIR / f"rtprop_{args.bw}Mbps.up"
  const_trace(trace, args.bw, args.dur)
  print(f"[*] RTprop test: bw={args.bw} rtt0={args.rtt0}ms rtt1={args.rtt1}ms")

  procs = []
  with tempfile.TemporaryDirectory() as tmpdir:
    tmp = Path(tmpdir)
    log0 = tmp / "flow0.jsonl"
    log1 = tmp / "flow1.jsonl"
    
    try:
      # Two receivers on different ports
      procs.append(subprocess.Popen(
        [RECEIVER_BIN, str(args.port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
      ))
      procs.append(subprocess.Popen(
        [RECEIVER_BIN, str(args.port + 1)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
      ))
      time.sleep(0.4)

      # Flow 0: normal RTT, Flow 1: higher RTT via nested mm-delay
      inner = (
        f"({SENDER_BIN} --log {log0} {TARGET_IP} {args.port} {args.dur} > /dev/null 2>&1) & "
        f"mm-delay {(args.rtt1 - args.rtt0) // 2} -- "
        f" {SENDER_BIN} --log {log1} {TARGET_IP} {args.port+1} {args.dur} > /dev/null 2>&1 & "
        f"wait"
      )

      mm_cmd = (
        f"mm-delay {args.rtt0 // 2} "
        f"mm-link {trace} {trace} "
        f"-- bash -c '{inner}'"
      )

      procs.append(subprocess.Popen(mm_cmd, shell=True))
      time.sleep(args.dur + 4)
    finally:
      cleanup(procs)
 
    burs0, br0, dl0 = parse_jsonl(log0) if log0.exists() else ([], [], [])
    burs1, br1, dl1 = parse_jsonl(log1) if log1.exists() else ([], [], [])
 
    avg0 = np.mean(br0) if br0 else 0.0
    avg1 = np.mean(br1) if br1 else 0.0
    if avg0 == 0 and avg1 == 0:
      ratio = 1.0
    else:
      ratio = max(avg0, avg1) / min(avg0, avg1) if min(avg0, avg1) > 0 else float('inf')
 
    print(f"flow0 avg={avg0:.2f} Mbps  flow1 avg={avg1:.2f} Mbps  ratio={ratio:.2f}x")
    save({
    "test": "different_rtprop",
    "rtt0_ms": args.rtt0,
    "rtt1_ms": args.rtt1,
    "flow0_avg_Mbps": round(avg0, 3),
    "flow1_avg_Mbps": round(avg1, 3),
    "throughput_ratio": round(ratio, 3),
    }, f"rtprop_{args.rtt0}_{args.rtt1}")

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--bw", type=float, default=20)
  parser.add_argument("--rtt0", type=int, default=20)
  parser.add_argument("--rtt1", type=int, default=60)
  parser.add_argument("--dur", type=int, default=40)
  parser.add_argument("--port", type=int, default=9900)
  run(parser.parse_args())

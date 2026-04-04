import argparse, subprocess, time, tempfile
import numpy as np
from pathlib import Path
from utils import (
  TRACES_DIR, RECEIVER_BIN, SENDER_BIN, MAHIMAHI_IP, PKT_BITS,
  step_trace, find_free_port, cleanup, parse_log, 
  summarise, save, plot_single
)

def run(args):
  TRACES_DIR.mkdir(exist_ok=True)
  trace = TRACES_DIR / f"step_{args.bw1}_{args.bw2}_{args.dur}s.up"
  
  if args.bw3 and args.swap2:
    ms1 = 1000 / ((args.bw1 * 1e6) / PKT_BITS)
    ms2 = 1000 / ((args.bw2 * 1e6) / PKT_BITS)
    ms3 = 1000 / ((args.bw3 * 1e6) / PKT_BITS)
    with open(trace, "w") as f:
      t = 0.0
      for ms, end in [(ms1, args.swap * 1000), (ms2, args.swap2 * 1000), (ms3, args.dur * 1000)]:
        while t < end:
          f.write(f"{int(t)}\n"); t += ms
  else:
    step_trace(trace, args.bw1, args.bw2, args.swap, args.dur)

  port = find_free_port(9500)
  procs = []

  with tempfile.TemporaryDirectory() as tmpdir:
    tmp = Path(tmpdir)
    send_lf = tmp / "send.log"
    
    try:
      recv_proc = subprocess.Popen(
        [RECEIVER_BIN, str(port)], 
        stdout=subprocess.DEVNULL, 
        stderr=subprocess.DEVNULL
      )
      procs.append(recv_proc)
      time.sleep(0.5)

      inner_cmd = f"{SENDER_BIN} {MAHIMAHI_IP} {port} > {send_lf} 2>&1"
      mm_cmd = f"mm-delay {args.rtt // 2} mm-link {trace} {trace} -- bash -c '{inner_cmd}'"
      procs.append(subprocess.Popen(mm_cmd, shell=True))
      
      time.sleep(args.dur + 5)
    finally: cleanup(procs)

    with open(send_lf, "r") as f:
      burs, bitrates, delays = parse_log(f.read())

  conv_t_ms = None
  swap_frame = int(args.swap * 1000 / 16.67)
  for i in range(swap_frame, len(bitrates) - 10):
    window = bitrates[i:i+10]
    if max(window) - min(window) < 0.05 * np.mean(window):
      conv_t_ms = round((i - swap_frame) * 16.67, 1)
      break

  s = summarise(burs, bitrates, delays, label="step")
  print(f"\n[STEP] avg_delay={s['avg_delay']} ms  convergence={conv_t_ms} ms")
  out = save({"test": "step", "convergence_ms": conv_t_ms, "summary": s}, "step")
  
  if args.plot:
    plot_single(burs, bitrates, delays, title=f"Step change {args.bw1}→{args.bw2} Mbps @ t={args.swap}s", out_pdf=str(out).replace(".json", ".pdf"))

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--bw1", type=float, default=20)
  parser.add_argument("--bw2", type=float, default=10)
  parser.add_argument("--bw3", type=float, default=None)
  parser.add_argument("--swap", type=int, default=10)
  parser.add_argument("--swap2", type=int, default=None)
  parser.add_argument("--dur", type=int, default=30)
  parser.add_argument("--rtt", type=int, default=20)
  parser.add_argument("--plot", action="store_true")
  run(parser.parse_args())
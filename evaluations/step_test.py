import argparse, subprocess, time, tempfile
import numpy as np
from pathlib import Path
from utils import (
  TRACES_DIR, RECEIVER_BIN, PKT_BITS,
  step_trace, cleanup, parse_log, summarise, save, plot_single,
  make_script, sender_cmd
)

def run_step(args):
  TRACES_DIR.mkdir(exist_ok=True)
  trace = TRACES_DIR / f"step_{args.bw1}_{args.bw2}.up"

  if args.bw3 and args.swap2:
    with open(trace, "w") as f:
      for bw, end_ms in [
        (args.bw1, args.swap  * 1000),
        (args.bw2, args.swap2 * 1000),
        (args.bw3, args.dur   * 1000),
      ]:
        ms = 1000 / ((bw * 1e6) / PKT_BITS)
        t  = 0.0
        while t < end_ms:
          f.write(f"{int(t)}\n"); t += ms
  else:
    step_trace(trace, args.bw1, args.bw2, args.swap, args.dur)

  procs = []
  with tempfile.TemporaryDirectory() as tmpdir:
    send_lf = Path(tmpdir) / "send.log"
    try:
      procs.append(subprocess.Popen([RECEIVER_BIN, str(args.port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
      time.sleep(0.5)
      script = make_script(tmpdir, [sender_cmd(args.port, args.dur, send_lf)])
      mm_cmd = f"mm-delay {args.rtt // 2} mm-link {trace} {trace} -- {script}"
      procs.append(subprocess.Popen(mm_cmd, shell=True))
      time.sleep(args.dur + 5)
    finally:
      cleanup(procs)

    burs, bitrates, delays = parse_log(send_lf.read_text() if send_lf.exists() else "")

  conv_ms = None
  swap_frame = int(args.swap * 1000 / 16.666)
  for i in range(swap_frame, len(bitrates) - 10):
    w = bitrates[i:i + 10]
    if max(w) - min(w) < 0.05 * np.mean(w):
      conv_ms = round((i - swap_frame) * 16.666, 1)
      break

  s = summarise(burs, bitrates, delays, label="step")
  print(f"avg_delay={s['avg_delay']} ms  convergence={conv_ms} ms after step")
  out = save({"test": "step", "convergence_ms": conv_ms, "summary": s}, "step")

  if args.plot:
    plot_single(burs, bitrates, delays,
                title=f"step {args.bw1} to {args.bw2} mbps at t={args.swap}s",
                out_pdf=str(out).replace(".json", ".pdf"))

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--bw1",  type=float, default=20)
  parser.add_argument("--bw2",  type=float, default=10)
  parser.add_argument("--bw3",  type=float, default=None)
  parser.add_argument("--swap", type=int,   default=10)
  parser.add_argument("--swap2",type=int,   default=None)
  parser.add_argument("--dur",  type=int,   default=30)
  parser.add_argument("--rtt",  type=int,   default=20)
  parser.add_argument("--port", type=int,   default=9500)
  parser.add_argument("--plot", action="store_true")
  run_step(parser.parse_args())
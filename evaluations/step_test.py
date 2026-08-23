import argparse, subprocess, time, tempfile
import numpy as np
from pathlib import Path
from utils import (
  TRACES_DIR, RECEIVER_BIN, PKT_BITS,
  step_trace, cleanup, parse_jsonl, summarise, save, plot_single,
  make_script, sender_cmd
)

def run_step(args):
  TRACES_DIR.mkdir(exist_ok=True)
  trace = TRACES_DIR / f"step_{args.bw1}_{args.bw2}.up"

  if args.bw3 and args.swap2:
    with open(trace, "w") as f:
      t = 0.0
      for bw, end_ms in [
        (args.bw1, args.swap  * 1000),
        (args.bw2, args.swap2 * 1000),
        (args.bw3, args.dur   * 1000),
      ]:
        ms = 1000 / ((bw * 1e6) / PKT_BITS)
        while t < end_ms:
          f.write(f"{int(t)}\n"); t += ms
  else:
    step_trace(trace, args.bw1, args.bw2, args.swap, args.dur)

  procs = []
  with tempfile.TemporaryDirectory() as tmpdir:
    send_lf = Path(tmpdir) / "send.jsonl"
    try:
      procs.append(subprocess.Popen([RECEIVER_BIN, str(args.port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
      time.sleep(0.5)
      script = make_script(tmpdir, [sender_cmd(args.port, args.dur, send_lf)])
      mm_cmd = f"mm-delay {args.rtt // 2} mm-link {trace} {trace} -- {script}"
      procs.append(subprocess.Popen(mm_cmd, shell=True))
      time.sleep(args.dur + 5)
    finally:
      cleanup(procs)
 
    burs, bitrates, delays = parse_jsonl(str(send_lf))
 
    conv_down_ms = None
    swap_frame = int(args.swap * 1000 / 16.666)
    for i in range(swap_frame, len(bitrates) - 10):
      w = bitrates[i:i + 10]
      if max(w) - min(w) < 0.05 * np.mean(w):
        conv_down_ms = round((i - swap_frame) * 16.666, 1)
        break
 
    conv_up_ms = None
    if args.swap2 and args.bw3:
      swap2_frame = int(args.swap2 * 1000 / 16.666)
      for i in range(swap2_frame, len(bitrates) - 10):
        w = bitrates[i:i + 10]
        if max(w) - min(w) < 0.05 * np.mean(w):
          conv_up_ms = round((i - swap2_frame) * 16.666, 1)
          break
 
    s = summarise(burs, bitrates, delays, label="step")
    print(f"avg_delay={s['avg_delay']} ms  conv_down={conv_down_ms} ms  conv_up={conv_up_ms} ms")
    out = save({"test": "step", "convergence_down_ms": conv_down_ms, "convergence_up_ms": conv_up_ms, "summary": s}, "step")
 
    if args.plot:
      title = f"step {args.bw1} -> {args.bw2}" + (f" -> {args.bw3}" if args.bw3 else "") + " Mbps"
      plot_single(burs, bitrates, delays,
            title=title,
            out_svg=str(out).replace(".json", ".svg"))

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--bw1",  type=float, default=20)
  parser.add_argument("--bw2",  type=float, default=10)
  parser.add_argument("--bw3",  type=float, default=20)
  parser.add_argument("--swap", type=int,   default=10)
  parser.add_argument("--swap2",type=int,   default=20)
  parser.add_argument("--dur",  type=int,   default=30)
  parser.add_argument("--rtt",  type=int,   default=20)
  parser.add_argument("--port", type=int,   default=9500)
  parser.add_argument("--plot", action="store_true")
  run_step(parser.parse_args())
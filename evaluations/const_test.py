import argparse, subprocess, time
import numpy as np
from pathlib import Path
import tempfile
from utils import (
  TRACES_DIR, RECEIVER_BIN,
  const_trace, cleanup, parse_jsonl, summarise, save, plot_single,
  make_script, sender_cmd
)

def run_const(args):
  TRACES_DIR.mkdir(exist_ok=True)
  trace = TRACES_DIR / f"{args.bw}Mbps.up"
  const_trace(trace, args.bw, args.dur)
  print(f"[*] bw={args.bw} Mbps  dur={args.dur}s  rtt={args.rtt}ms  port={args.port}")

  procs = []
  with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    send_lf = tmp / "send.jsonl"
    try:
      procs.append(subprocess.Popen([RECEIVER_BIN, str(args.port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
      time.sleep(0.5)
      script = make_script(tmp, [sender_cmd(args.port, args.dur, send_lf)])
      mm_cmd = f"mm-delay {args.rtt // 2} mm-link {trace} {trace} -- {script}"
      procs.append(subprocess.Popen(mm_cmd, shell=True))
      time.sleep(args.dur + 3)
    finally:
      cleanup(procs)
    
    burs, bitrates, delays = parse_jsonl(str(send_lf))
    
    if not bitrates: print("[!] empty log"); return

    br_std = float(np.std(bitrates))
    br_var = float(np.var(bitrates))
    
    s = summarise(burs, bitrates, delays, label="const")
    print(f"avg_br={s['avg_bitrate']:.3f} Mbps  std={br_std:.3f}  var={br_var:.3f}  avg_delay={s['avg_delay']:.3f} ms  stall={s['stall_100ms']*100:.3f}%")
    out = save({"test": f"const_{args.bw}", "bw_Mbps": args.bw, "summary": s}, f"const_{args.bw}")

    if args.plot:
      plot_single(burs, bitrates, delays,
            title=f"baseline convergence (bw={args.bw} Mbps)",
            out_svg=str(out).replace(".json", ".svg"))

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--bw",   type=float, default=20)
  parser.add_argument("--dur",  type=int,   default=60, help="duration in seconds")
  parser.add_argument("--rtt",  type=int,   default=20)
  parser.add_argument("--port", type=int,   default=9700)
  parser.add_argument("--plot", action="store_true")
  run_const(parser.parse_args())
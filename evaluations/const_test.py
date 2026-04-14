import argparse, subprocess, time
from pathlib import Path
import tempfile
from utils import (
  TRACES_DIR, RECEIVER_BIN,
  const_trace, cleanup, parse_log, summarise, save, plot_single,
  make_script, sender_cmd
)

def run_const(args):
  TRACES_DIR.mkdir(exist_ok=True)
  trace = TRACES_DIR / f"const_{args.bw}Mbps.up"
  const_trace(trace, args.bw, args.dur)
  print(f"[*] bw={args.bw} Mbps  dur={args.dur}s  rtt={args.rtt}ms  port={args.port}")

  procs = []
  with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    send_lf = tmp / "send.log"
    try:
      procs.append(subprocess.Popen([RECEIVER_BIN, str(args.port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
      time.sleep(0.5)
      script = make_script(tmp, [sender_cmd(args.port, args.dur, send_lf)])
      mm_cmd = f"mm-delay {args.rtt // 2} mm-link {trace} {trace} -- {script}"
      procs.append(subprocess.Popen(mm_cmd, shell=True))
      time.sleep(args.dur + 3)
    finally:
      cleanup(procs)
      
    burs, bitrates, delays = parse_log(send_lf.read_text() if send_lf.exists() else "")
    
  if not burs: print("[!] empty log"); return

  s = summarise(burs, bitrates, delays, label="const_baseline")
  print(f"avg_br={s['avg_bitrate']} Mbps  avg_delay={s['avg_delay']} ms  stall={s['stall_100ms']*100:.3f}%")
  out = save({"test": "const_baseline", "bw_mbps": args.bw, "summary": s}, "const_baseline")

  if args.plot:
    plot_single(burs, bitrates, delays,
                title=f"baseline convergence (bw={args.bw} Mbps)",
                out_pdf=str(out).replace(".json", ".pdf"))
  send_lf.unlink(missing_ok=True)

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--bw",   type=float, default=20)
  parser.add_argument("--dur",  type=int,   default=30)
  parser.add_argument("--rtt",  type=int,   default=20)
  parser.add_argument("--port", type=int,   default=9700)
  parser.add_argument("--plot", action="store_true")
  run_const(parser.parse_args())
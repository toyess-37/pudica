import argparse, subprocess, time
from pathlib import Path
from utils import (
  TRACES_DIR, RECEIVER_BIN, SENDER_BIN, MAHIMAHI_IP,
  const_trace, cleanup, parse_log, summarise, save, plot_single
)

def run_const(args):
  TRACES_DIR.mkdir(exist_ok=True)
  trace = TRACES_DIR / f"const_{args.bw}mbps.up"
  const_trace(trace, args.bw, args.dur)
  print(f"[*] bw={args.bw} mbps  dur={args.dur}s  rtt={args.rtt}ms  port={args.port}")

  send_lf = Path(f"/tmp/const_send_{args.port}.log")
  procs = []
  try:
    procs.append(subprocess.Popen(
      [RECEIVER_BIN, str(args.port)],
      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    ))
    time.sleep(0.5)
    inner = f"{SENDER_BIN} {MAHIMAHI_IP} {args.port} {args.dur} > {send_lf} 2>&1"
    mm_cmd = f"mm-delay {args.rtt // 2} mm-link {trace} {trace} -- bash -c '{inner}'"
    procs.append(subprocess.Popen(mm_cmd, shell=True))
    time.sleep(args.dur + 3)
  finally:
    cleanup(procs)

  if not send_lf.exists():
    print("[!] log file missing — did the receiver send acks?"); return

  burs, bitrates, delays = parse_log(send_lf.read_text())
  if not burs:
    print("[!] empty log"); return

  s = summarise(burs, bitrates, delays, label="const_baseline")
  print(f"avg_br={s['avg_bitrate']} mbps  avg_delay={s['avg_delay']} ms  stall={s['stall_100ms']*100:.3f}%")
  out = save({"test": "const_baseline", "bw_mbps": args.bw, "summary": s}, "const_baseline")

  if args.plot:
    plot_single(burs, bitrates, delays,
                title=f"baseline convergence (bw={args.bw} mbps)",
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
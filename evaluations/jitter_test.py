import argparse, subprocess, time, tempfile
from pathlib import Path
from utils import (
  TRACES_DIR, RECEIVER_BIN, PKT_BITS,
  cleanup, parse_log, summarise, save, plot_single,
  make_script, sender_cmd
)

def run_jitter(args):
  TRACES_DIR.mkdir(exist_ok=True)
  trace = TRACES_DIR / f"jitter_{args.bw}mbps.up"
  ms_pkt = 1000 / ((args.bw * 1e6) / PKT_BITS)
  with open(trace, "w") as f:
    t = period_ctr = 0.0
    while t < args.dur * 1000:
      f.write(f"{int(t)}\n")
      t += ms_pkt
      period_ctr += ms_pkt
      if period_ctr >= args.period:
        t += args.jitter
        period_ctr = 0.0

  procs = []
  with tempfile.TemporaryDirectory() as tmpdir:
    send_lf = Path(tmpdir) / "send.log"
    try:
      procs.append(subprocess.Popen(
        [RECEIVER_BIN, str(args.port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
      ))
      time.sleep(0.3)
      script = make_script(tmpdir, [sender_cmd(args.port, args.dur, send_lf)])
      mm_cmd = f"mm-delay {args.rtt // 2} mm-link {trace} {trace} -- {script}"
      procs.append(subprocess.Popen(mm_cmd, shell=True))
      time.sleep(args.dur + 5)
    finally:
      cleanup(procs)

    burs, bitrates, delays = parse_log(send_lf.read_text() if send_lf.exists() else "")

  s = summarise(burs, bitrates, delays, label="jitter")
  print(f"avg_delay={s['avg_delay']} ms  p99={s['p99_delay']} ms  stall={s['stall_100ms']*100:.3f}%")
  out = save({"test": "jitter", "jitter_ms": args.jitter, "summary": s}, "jitter")

  if args.plot:
    plot_single(burs, bitrates, delays,
                title=f"jitter {args.jitter}ms / {args.period}ms period",
                out_svg=str(out).replace(".json", ".svg"))

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--bw",     type=float, default=20)
  parser.add_argument("--jitter", type=int,   default=40)
  parser.add_argument("--period", type=int,   default=500)
  parser.add_argument("--dur",    type=int,   default=15)
  parser.add_argument("--rtt",    type=int,   default=20)
  parser.add_argument("--port",   type=int,   default=9400)
  parser.add_argument("--plot",   action="store_true")
  run_jitter(parser.parse_args())
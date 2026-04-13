import argparse, subprocess, time, tempfile
import numpy as np
from pathlib import Path
import matplotlib.pyplot as plt
from utils import (
  TRACES_DIR, RECEIVER_BIN,
  const_trace, cleanup, parse_log, save,
  make_script, sender_cmd
)

def run_bur(args):
  TRACES_DIR.mkdir(exist_ok=True)
  bw_list = [float(x) for x in args.bw_list.split(",")]
  results = []

  for bw in bw_list:
    trace = TRACES_DIR / f"bur_{bw}mbps.up"
    const_trace(trace, bw, args.dur)

    with tempfile.TemporaryDirectory() as tmpdir:
      tmp = Path(tmpdir)
      send_lf = tmp / "send.log"
      procs = []
      try:
        procs.append(subprocess.Popen(
          [RECEIVER_BIN, str(args.port)],
          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        ))
        time.sleep(0.3)
        script = make_script(tmp, [sender_cmd(args.port, args.dur, send_lf)])
        mm_cmd = (
          f"mm-delay {args.rtt // 2} "
          f"mm-link --uplink-queue=droptail --uplink-queue-args=packets=500 "
          f"{trace} {trace} -- {script}"
        )
        procs.append(subprocess.Popen(mm_cmd, shell=True))
        time.sleep(args.dur + 2)
      finally:
        cleanup(procs)
      burs, bitrates, _ = parse_log(send_lf.read_text() if send_lf.exists() else "")

    tail    = burs[int(len(burs) * 0.6):] if len(burs) > 10 else burs
    est_bur = float(np.mean(tail)) if tail else 0.0
    results.append({"bw": bw, "est_bur": round(est_bur, 4),
                    "avg_bitrate": round(float(np.mean(bitrates)), 3) if bitrates else 0})
    print(f"bw={bw} mbps  converged bur={est_bur:.4f}")

  out = save({"test": "bur_accuracy", "data": results}, "bur_accuracy")

  if args.plot:
    xs = [r["bw"] for r in results]
    ys = [r["est_bur"] for r in results]
    fig, ax = plt.subplots(figsize=(7, 4))
    ax.plot(xs, ys, "o-", label="estimated bur (converged)")
    ax.axhline(1.0, color="gray", ls="--", label="ideal bur=1.0")
    ax.set_xlabel("link bw (mbps)"); ax.set_ylabel("estimated bur")
    ax.set_title("bur estimation accuracy"); ax.legend(); ax.grid(True, ls="--", alpha=0.6)
    plt.tight_layout()
    plt.savefig(str(out).replace(".json", ".pdf"), format="pdf")
    plt.close(fig)

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--bw-list", type=str, default="5,10,15,20,25,30")
  parser.add_argument("--dur",  type=int,   default=20)
  parser.add_argument("--rtt",  type=int,   default=20)
  parser.add_argument("--port", type=int,   default=9600)
  parser.add_argument("--plot", action="store_true")
  run_bur(parser.parse_args())
import argparse, subprocess, time, tempfile
import numpy as np
from pathlib import Path
import matplotlib
import matplotlib.pyplot as plt
from utils import (
  TRACES_DIR, RECEIVER_BIN, SENDER_BIN, MAHIMAHI_IP, 
  const_trace, find_free_port, cleanup, parse_log, save
)

def run(args):
  TRACES_DIR.mkdir(exist_ok=True)
  bw_list = [float(x) for x in args.bw_list.split(",")]
  results = []

  for bw in bw_list:
    trace = TRACES_DIR / f"bur_{bw}mbps_{args.dur}s.up"
    const_trace(trace, bw, args.dur)
    port = find_free_port(9600)
    procs = []

    with tempfile.TemporaryDirectory() as tmpdir:
      tmp = Path(tmpdir)
      send_lf = tmp / "send.log"
      
      try:
        procs.append(subprocess.Popen([RECEIVER_BIN, str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(0.3)

        inner_cmd = f"{SENDER_BIN} {MAHIMAHI_IP} {port} > {send_lf} 2>&1"
        mm_cmd = f"mm-delay {args.rtt // 2} mm-link {trace} {trace} -- bash -c '{inner_cmd}'"
        procs.append(subprocess.Popen(mm_cmd, shell=True))
        
        time.sleep(args.dur + 2)
      finally: cleanup(procs)

      with open(send_lf, "r") as f:
        burs, bitrates, _ = parse_log(f.read())
        
    tail = burs[int(len(burs)*0.6):] if len(burs) > 10 else burs
    est_bur = float(np.mean(tail)) if tail else 0
    results.append({"bw": bw, "est_bur": round(est_bur, 4), "avg_bitrate": round(float(np.mean(bitrates)), 3) if bitrates else 0})
    print(f"BW={bw} Mbps; converged avg BUR={est_bur:.4f}")

  out = save({"test": "bur_accuracy", "data": results}, "bur_accuracy")

  if args.plot:
    xs = [r["bw"] for r in results]
    ys = [r["est_bur"] for r in results]
    fig, ax = plt.subplots(figsize=(7, 4))
    ax.plot(xs, ys, "o-", label="Estimated BUR (converged)")
    ax.axhline(1.0, color="gray", ls="--", label="Ideal BUR=1.0")
    ax.set_xlabel("Link BW (Mbps)"); ax.set_ylabel("Estimated BUR")
    ax.set_title("BUR Estimation Accuracy"); ax.legend(); ax.grid(True, ls="--", alpha=0.6)
    plt.tight_layout()
    plt.savefig(str(out).replace(".json", ".pdf"), dpi="figure", format="pdf")
    plt.close(fig)

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--bw-list", type=str, default="5,10,15,20,25,30")
  parser.add_argument("--dur", type=int, default=20)
  parser.add_argument("--rtt", type=int, default=20)
  parser.add_argument("--plot", action="store_true")
  run(parser.parse_args())
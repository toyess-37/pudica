import argparse, json
import os, subprocess, tempfile, time
import numpy as np
from pathlib import Path
from utils import (
  TRACES_DIR, RECEIVER_BIN, SENDER_BIN, MAHIMAHI_IP, 
  const_trace, find_free_port, cleanup, parse_log, 
  summarise, save, plot_single
)

# launching pudica and cubic together
def run(args):
  """
  One Pudica flow vs. one iperf3 TCP Cubic bulk flow on the same bottleneck.
  Pudica launches first; Cubic starts after `args.cubic_delay_s` seconds.
  Attempts to test Appendix H (Fig. 18.) of the paper
  """

  TRACES_DIR.mkdir(exist_ok=True)
  trace = TRACES_DIR / f"cubic_{args.bw}mbps_{args.dur}s.up"
  const_trace(trace, args.bw, args.dur)

  port_pudica = find_free_port(9200)
  port_iperf = find_free_port(9300)
  print(f"BW={args.bw}Mbps  buffer={args.buf}pkts  Cubic delay={args.cubic_delay}s  dur={args.dur}s")

  procs = []
  with tempfile.TemporaryDirectory() as tmpdir:
    tmp = Path(tmpdir)
    recv_lf = tmp / "recv.log"
    send_lf = tmp / "send.log"
    iperf_lf = tmp / "iperf.json"

    try:
      with open(recv_lf, "w") as f:
        procs.append(subprocess.Popen([RECEIVER_BIN, str(port_pudica)], stdout=f, stderr=f))
      procs.append(subprocess.Popen(["iperf3", "-s", "-p", str(port_iperf)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
      time.sleep(0.4)

      inner_script = (
        f"({SENDER_BIN} {MAHIMAHI_IP} {port_pudica} > {send_lf} 2>&1) & "
        f"sleep {args.cubic_delay} && "
        f"iperf3 -c {MAHIMAHI_IP} -p {port_iperf} -t {args.dur - args.cubic_delay} -J > {iperf_lf} 2>/dev/null; wait;"
      )

      mm_cmd = (f"mm-delay {args.rtt // 2} mm-link --uplink-queue=droptail --uplink-queue-args=packets={args.buf} "
                f"{trace} {trace} -- bash -c '{inner_script}'")
      procs.append(subprocess.Popen(mm_cmd, shell=True))
      time.sleep(args.dur + 3)
    finally: cleanup(procs)

    with open(send_lf, "r") as f:
      burs, bitrates, delays = parse_log(f.read())
      
    cubic_thput = 0.0
    try:
      with open(iperf_lf, "r") as f:
        ij = json.load(f)
        cubic_thput = ij["end"]["sum_received"]["bits_per_second"] / 1e6
    except Exception: 
      print("Warning: Could not parse iperf3 results. Did it run?")

  pudica_avg = float(np.mean(bitrates)) if bitrates else 0
  share = pudica_avg / (pudica_avg + cubic_thput) if (pudica_avg + cubic_thput) > 0 else 0
  s = summarise(burs, bitrates, delays, label="pudica_vs_cubic")

  print(f"\n[CUBIC] Pudica avg={pudica_avg:.2f} Mbps\t\tCubic={cubic_thput:.2f} Mbps\tPudica share={share*100:.1f}%")
  out = save({"test": "cubic", "pudica_avg_br": round(pudica_avg, 3), "cubic_thput": round(cubic_thput, 3), "pudica_share": round(share, 4), "summary": s}, "cubic")

  if args.plot:
    plot_single(burs, bitrates, delays, title=f"Pudica vs Cubic (BW={args.bw}Mbps)", out_pdf=str(out).replace(".json", ".pdf"))
    
if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--bw", type=float, default=20)
  parser.add_argument("--buf", type=int, default=500)
  parser.add_argument("--dur", type=int, default=30)
  parser.add_argument("--cubic-delay", type=int, default=5, dest="cubic_delay")
  parser.add_argument("--rtt", type=int, default=20)
  parser.add_argument("--plot", action="store_true")
  run(parser.parse_args())
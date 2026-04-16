import argparse, json, subprocess, tempfile, time
import numpy as np
from pathlib import Path
from utils import (
  TRACES_DIR, SENDER_BIN, RECEIVER_BIN, TARGET_IP,
  const_trace, cleanup, parse_log, summarise, save, plot_single
)

# tests pudica vs iperf3/cubic on a shared bottleneck.
# use --buf 7 for competitive result and --buf 50 for the concession scenario

def run(args):
  TRACES_DIR.mkdir(exist_ok=True)
  trace = TRACES_DIR / f"cubic_{args.bw}Mbps_{args.dur}s.up"
  const_trace(trace, args.bw, args.dur)
  print(f"bw={args.bw} Mbps  buf={args.buf} pkts;  cubic delay={args.cubic_delay}s")

  procs = []
  with tempfile.TemporaryDirectory() as tmpdir:
    tmp    = Path(tmpdir)
    send_lf  = tmp / "send.log"
    iperf_lf = tmp / "iperf.json"

    try:
      procs.append(subprocess.Popen(
        [RECEIVER_BIN, str(args.port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
      ))
      procs.append(subprocess.Popen(
        ["iperf3", "-s", "-p", str(args.iperf_port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
      ))
      time.sleep(0.4)

      # pudica runs for full duration; cubic starts after cubic_delay seconds.
      # both are inside the same mahimahi shell so they share the bottleneck.
      inner = (
        f"({SENDER_BIN} {TARGET_IP} {args.port} {args.dur} > {send_lf} 2>&1) & "
        f"sleep {args.cubic_delay} && "
        f"iperf3 -c {TARGET_IP} -p {args.iperf_port} "
        f"  -t {args.cubic_dur} -J > {iperf_lf} 2>/dev/null; "
        f"wait;"
      )
      mm_cmd = (
        f"mm-delay {args.rtt // 2} "
        f"mm-link --uplink-queue=droptail --uplink-queue-args=packets={args.buf} "
        f"{trace} {trace} "
        f"-- bash -c '{inner}'"
      )
      procs.append(subprocess.Popen(mm_cmd, shell=True))
      time.sleep(args.dur + 3)

    finally:
      cleanup(procs)

    burs, bitrates, delays = parse_log(send_lf.read_text() if send_lf.exists() else "")

    cubic_thput = 0.0
    try:
      ij = json.loads(iperf_lf.read_text())
      cubic_thput = ij["end"]["sum_received"]["bits_per_second"] / 1e6
    except Exception:
      print("[!] could not parse iperf3 output - did iperf3 run?")

  pudica_avg = float(np.mean(bitrates)) if bitrates else 0.0
  total      = pudica_avg + cubic_thput
  share      = pudica_avg / total if total > 0 else 0.0

  s = summarise(burs, bitrates, delays, label="pudica_vs_cubic")
  print(f"\n[cubic] pudica={pudica_avg:.2f} Mbps  cubic={cubic_thput:.2f} Mbps  pudica share={share*100:.1f}%")

  out = save({
    "test": f"cubic_{args.buf}",
    "buf_pkts": args.buf,
    "pudica_avg_Mbps": round(pudica_avg, 3),
    "cubic_Mbps":      round(cubic_thput, 3),
    "pudica_share":    round(share, 4),
    "summary": s,
  }, f"cubic_{args.buf}")

  if args.plot:
    plot_single(
      burs, bitrates, delays,
      title=f"pudica vs cubic  bw={args.bw} Mbps  buf={args.buf} pkts",
      out_svg=str(out).replace(".json", ".svg"),
    )

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--bw",          type=float, default=50)
  parser.add_argument("--buf",         type=int,   default=7,
    help="bottleneck queue in packets. 7 is approx. 10KB (paper fig18a), 50 is approx. 50KB (fig18b)")
  parser.add_argument("--dur",         type=int,   default=30)
  parser.add_argument("--cubic-delay", type=int,   default=5,  dest="cubic_delay")
  parser.add_argument("--cubic-dur",   type=int,   default=5,  dest="cubic_dur")
  parser.add_argument("--rtt",         type=int,   default=20)
  parser.add_argument("--port",        type=int,   default=9200, help="pudica receiver port")
  parser.add_argument("--iperf-port",  type=int,   default=9300, dest="iperf_port")
  parser.add_argument("--plot",        action="store_true")
  run(parser.parse_args())
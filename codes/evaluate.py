import argparse, json, re
import os, subprocess, sys, tempfile, time
from pathlib import Path
from datetime import datetime

import matplotlib
import matplotlib.pyplot as plt
import numpy as np

PKT_BITS      = 1400 * 8  # packet size
SENDER_BIN    = os.environ.get("SENDER",   "./sender")
RECEIVER_BIN  = os.environ.get("RECEIVER", "./receiver")
RESULTS_DIR   = Path("results")
TRACES_DIR    = Path("traces")

def const_trace(path, bw_mbps, dur_s):
  ms_pkt = 1000 / ((bw_mbps * 1e6) / PKT_BITS)
  with open(path, "w") as f:
    t = 0.0
    while t < dur_s * 1000:
      f.write(f"{int(t)}\n")
      t += ms_pkt


def step_trace(path, bw1, bw2, swap_s, dur_s):
  with open(path, "w") as f:
    ms1 = 1000 / ((bw1 * 1e6) / PKT_BITS)
    t = 0.0
    while t < swap_s * 1000:
      f.write(f"{int(t)}\n")
      t += ms1
    ms2 = 1000 / ((bw2 * 1e6) / PKT_BITS)
    while t < dur_s * 1000:
      f.write(f"{int(t)}\n")
      t += ms2


def jitter_trace(path, bw_mbps, jitter_ms, period_ms, dur_s):
  """constant bandwidth trace; mm-delay bursts are added in the shell"""
  const_trace(path, bw_mbps, dur_s)  # BW trace is constant; jitter injected via tc/netem

def parse_log(text):
  burs, bitrates, delays = [], [], []
  for line in text.splitlines():
    if "BUR:" not in line or "bitrate:" not in line or "delay:" not in line:
      continue
    try:
      parts = line.split()
      b = float(parts[parts.index("BUR:")+1])
      br = float(parts[parts.index("bitrate:")+1])
      d = float(parts[parts.index("delay:")+1].replace("ms", ""))
      burs.append(b); bitrates.append(br); delays.append(d)
    except (ValueError, IndexError):
      continue
  return burs, bitrates, delays

# flows with stall rate > 100ms
def stall_rate(delays, thresh_ms = 100.0):
  if not delays:
    return 0.0
  return sum(1 for d in delays if d > thresh_ms) / len(delays)

def percentile(vals, p):
  if not vals:
    return 0.0
  return float(np.percentile(vals, p))

def summarise(burs, bitrates, delays, label="flow"):
  return {
    "label": label,
    "n_frames": len(bitrates),
    "avg_bitrate": round(float(np.mean(bitrates)), 3) if bitrates else 0,
    "avg_delay": round(float(np.mean(delays)), 3) if delays else 0,
    "p95_delay": round(percentile(delays, 95), 3),
    "p99_delay": round(percentile(delays, 99), 3),
    "stall_100ms": round(stall_rate(delays, 100), 3),
    "stall_200ms": round(stall_rate(delays, 200),3),
    "avg_bur": round(float(np.mean(burs)), 4) if burs else 0,
  }

def run_single_flow(trace_path, dur_s, port, extra_args=None, rtt_ms=10) -> tuple[list, list, list]:
  """
  run ./sender & ./receiver inside mm-delay + mm-link for dur_s seconds.
  Returns (burs, bitrates, delays).
  """
  extra_mm_args = extra_args or []
  recv_log = tempfile.NamedTemporaryFile(delete=False, suffix=".log")
  send_log = tempfile.NamedTemporaryFile(delete=False, suffix=".log")

  # receiver (outside the shell – listens on all interfaces)
  recv_proc = subprocess.Popen([RECEIVER_BIN, str(port)], stdout=recv_log, stderr=recv_log)

  time.sleep(0.3)  # let receiver bind

  # sender inside mm-delay + mm-link
  mm_cmd = (
    f"mm-delay {rtt_ms // 2} mm-link {' '.join(extra_args)} {trace_path} {trace_path} "
    f"-- {SENDER_BIN} $MAHIMAHI_BASE {port}"
  )
  send_proc = subprocess.Popen(
    mm_cmd, shell=True, stdout=send_log, stderr=send_log
  )

  time.sleep(dur_s+5)

  send_proc.terminate()
  send_proc.wait()
  recv_proc.terminate()
  recv_proc.wait()

  with open(send_log.name) as f:
    raw = f.read()

  os.unlink(recv_log.name); os.unlink(send_log.name)
  return parse_log(raw)

def save(data: dict, prefix: str) -> Path:
  RESULTS_DIR.mkdir(exist_ok=True)
  ts  = datetime.now().strftime("%Y%m%d_%H%M%S")
  out = RESULTS_DIR / f"{prefix}_{ts}.json"
  with open(out, "w") as f:
    json.dump(data, f, indent=2)
  print(f"Results saved to {out}")
  return out

def plot_single(burs, bitrates, delays, title="", out_pdf="out.pdf"):
  if not bitrates:
    print("[error] No data to plot."); return
  t = [i * 16.67 for i in range(len(bitrates))]
  fig, (a1, a2, a3) = plt.subplots(3, 1, figsize=(10, 8), sharex=True)
  fig.suptitle(title, fontsize=13, fontweight="bold")

  a1.plot(t, bitrates, color="red", lw=1.5)
  a1.set_ylabel("Bitrate (Mbps)", fontweight="bold"); a1.grid(True, ls="--", alpha=0.6)

  a2.plot(t, delays, color="green", lw=1.5)
  a2.set_ylabel("Delay (ms)", fontweight="bold"); a2.grid(True, ls="--", alpha=0.6)

  a3.plot(t, burs, color="blue", lw=1.5, alpha=0.8)
  a3.axhline(1.0,  color="black", ls="--", lw=2,   label="BUR=1.0")
  a3.axhline(0.85, color="gray",  ls=":",  lw=1.5, label="α=0.85")
  a3.set_xlabel("Timeline (ms)", fontweight="bold")
  a3.set_ylabel("BUR", fontweight="bold")
  a3.legend(loc="lower right"); a3.grid(True, ls="--", alpha=0.6)

  plt.tight_layout()
  plt.savefig(out_pdf, dpi="figure", format="pdf")
  print(f"Plot saved in {out_pdf}")

def bar_metric(labels, values, ylabel, title, out_pdf):
  fig, ax = plt.subplots(figsize=(max(4, len(labels) * 1.5), 4))
  ax.bar(labels, values, color=plt.cm.tab10.colors[:len(labels)])
  ax.set_ylabel(ylabel); ax.set_title(title)
  plt.tight_layout()
  plt.savefig(out_pdf, dpi="figure", format="pdf")
  print(f"Plot saved in {out_pdf}")

def cmd_plot(args):
  with open(args.file) as f:
    data = json.load(f)
  print(json.dumps(data, indent=2))


def main():
  P = argparse.ArgumentParser(
    description="Pudica evaluation",
    formatter_class=argparse.RawDescriptionHelpFormatter,
    epilog=__doc__,
  )
  sub = P.add_subparsers(dest="cmd", required=True)

  # ── fairness ──
  p_f = sub.add_parser("fairness", help="Jain's fairness index across N flows")
  p_f.add_argument("--flows", type=int, default=3, help="Number of competing flows")
  p_f.add_argument("--bw", type=float, default=30, help="Bottleneck BW in Mbps")
  p_f.add_argument("--dur", type=int, default=45, help="Duration per flow (s)")
  p_f.add_argument("--stagger", type=int, default=10, dest="stagger_s", help="Seconds between flow entry")
  p_f.add_argument("--rtt", type=int, default=20, help="Base RTT in ms")
  p_f.add_argument("--plot", action="store_true")
  # p_f.set_defaults(func=cmd_fairness)

  # ── cubic ──
  p_c = sub.add_parser("cubic", help="Pudica vs TCP Cubic competition")
  p_c.add_argument("--bw", type=float, default=20, help="Link BW in Mbps")
  p_c.add_argument("--buf", type=int, default=500, help="Bottleneck queue (packets)")
  p_c.add_argument("--dur", type=int, default=30, help="Total duration (s)")
  p_c.add_argument("--cubic-delay", type=int, default=5, dest="cubic_delay", help="Seconds to wait before starting Cubic")
  p_c.add_argument("--rtt", type=int, default=20)
  p_c.add_argument("--plot", action="store_true")
  # p_c.set_defaults(func=cmd_cubic)

  # ── jitter ──
  p_j = sub.add_parser("jitter", help="Periodic delay jitter robustness test")
  p_j.add_argument("--bw", type=float, default=20, help="Link BW in Mbps")
  p_j.add_argument("--jitter", type=int, default=40, help="Jitter spike duration (ms)")
  p_j.add_argument("--period", type=int, default=500, help="Jitter period (ms)")
  p_j.add_argument("--dur", type=int, default=15, help="Test duration (s)")
  p_j.add_argument("--rtt", type=int, default=20)
  p_j.add_argument("--plot", action="store_true")
  # p_j.set_defaults(func=cmd_jitter)

  # ── step ──
  p_s = sub.add_parser("step", help="Bandwidth step change convergence test")
  p_s.add_argument("--bw1", type=float, default=20, help="Initial BW (Mbps)")
  p_s.add_argument("--bw2", type=float, default=10, help="Post-drop BW (Mbps)")
  p_s.add_argument("--bw3", type=float, default=None, help="Optional recovery BW (Mbps)")
  p_s.add_argument("--swap", type=int, default=10, help="Time of first change (s)")
  p_s.add_argument("--swap2", type=int, default=None, help="Time of second change (s)")
  p_s.add_argument("--dur", type=int, default=30, help="Total duration (s)")
  p_s.add_argument("--rtt", type=int, default=20)
  p_s.add_argument("--plot", action="store_true")
  # p_s.set_defaults(func=cmd_step)

  # ── bur_accuracy ──
  # p_b = sub.add_parser("bur_accuracy", help="BUR estimation accuracy sweep")
  # p_b.add_argument("--bw-list", type=str, default="5,10,15,20,25,30", help="list of bandwidths to test (Mbps), separated by commas")
  # p_b.add_argument("--dur", type=int, default=20, help="Duration per BW point (s)")
  # p_b.add_argument("--rtt", type=int, default=20)
  # p_b.add_argument("--plot", action="store_true")
  # p_b.set_defaults(func=cmd_bur_accuracy)

  # ── plot ──
  p_p = sub.add_parser("plot", help="Re-display a saved results JSON")
  p_p.add_argument("--file", required=True, help="Path to results/*.json")
  p_p.set_defaults(func=cmd_plot)

  args = P.parse_args()
  args.func(args)

if __name__ == "__main__":
  main()
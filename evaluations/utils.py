import json, os, subprocess, tempfile, time
from pathlib import Path
import matplotlib
import matplotlib.pyplot as plt
import numpy as np

_PROJECT_ROOT = Path(__file__).resolve().parent.parent

PKT_BITS      = 1400 * 8  # packet size
SENDER_BIN    = str(_PROJECT_ROOT / "codes" / "sender")
RECEIVER_BIN  = str(_PROJECT_ROOT / "codes" / "receiver")
RESULTS_DIR   = _PROJECT_ROOT / "results"
TRACES_DIR    = _PROJECT_ROOT / "traces"
FAST_TRACE    = TRACES_DIR / "fast_100mbps.up"
MAHIMAHI_IP   = "$MAHIMAHI_BASE"

def const_trace(path, bw, secs):
  ms_pkt = 1000 / ((bw * 1e6) / PKT_BITS)
  with open(path, "w") as f:
    t = 0.0
    while t < secs * 1000:
      f.write(f"{int(t)}\n")
      t += ms_pkt

# Fast link to be used during downlink
TRACES_DIR.mkdir(exist_ok=True)
if not FAST_TRACE.exists():
  const_trace(FAST_TRACE, 100.0, 200.0)

def step_trace(path, bw1, bw2, swap_s, secs):
  with open(path, "w") as f:
    ms1 = 1000 / ((bw1 * 1e6) / PKT_BITS)
    t = 0.0
    while t < swap_s * 1000:
      f.write(f"{int(t)}\n")
      t += ms1
    ms2 = 1000 / ((bw2 * 1e6) / PKT_BITS)
    while t < secs * 1000:
      f.write(f"{int(t)}\n")
      t += ms2


def jitter_trace(path, bw, jitter_ms, period_ms, secs):
  """constant bandwidth; jitter bursts are added later"""
  const_trace(path, bw, secs)  # jitter injected via tc/netem

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
    except (ValueError, IndexError): continue
  return burs, bitrates, delays

# helper to calculate jain's fairness for multiple competing flows
def jains_fairness(bitrates_per_flow):
  """Jain's fairness index over per-flow average bitrates."""
  avgs = [np.mean(b) for b in bitrates_per_flow if b]
  if not avgs: return 0.0
  n = len(avgs)
  return (sum(avgs) ** 2) / (n * sum(x ** 2 for x in avgs))

# flows with stall rate > 100ms
def stall_rate(delays, thresh_ms = 100.0):
  if not delays: return 0.0
  return sum(1 for d in delays if d > thresh_ms) / len(delays)

def percentile(vals, p):
  if not vals: return 0.0
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

def find_free_port(base=9000) -> int:
  import socket
  for p in range(base, base + 200):
    with socket.socket() as s:
      try:
        s.bind(("", p)); return p
      except OSError:
        continue
  raise RuntimeError("No free port found")

def cleanup(procs: list):
  for p in procs:
    if p and p.poll() is None:
      p.terminate()
      p.wait()

def run_single_flow(trace_path, secs, port, extra_args=None, rtt_ms=10) -> tuple[list, list, list]:
  """
  run ./sender & ./receiver inside mm-delay + mm-link for secs seconds.
  Returns (burs, bitrates, delays).
  """
  extra_args = extra_args or []
  recv_log = tempfile.NamedTemporaryFile(delete=False, suffix=".log")
  send_log = tempfile.NamedTemporaryFile(delete=False, suffix=".log")

  # receiver (outside the shell – listens on all interfaces)
  recv_proc = subprocess.Popen([RECEIVER_BIN, str(port)], stdout=recv_log, stderr=recv_log)

  time.sleep(0.3)  # let receiver bind

  # sender inside mahimahi
  mm_cmd = (
    f"mm-delay {rtt_ms // 2} mm-link {' '.join(extra_args)} {trace_path} {trace_path} "
    f"-- {SENDER_BIN} 100.64.0.1 {port}"
  )
  send_proc = subprocess.Popen(
    mm_cmd, shell=True, stdout=send_log, stderr=send_log
  )

  time.sleep(secs+5)

  send_proc.terminate()
  send_proc.wait()
  recv_proc.terminate()
  recv_proc.wait()

  with open(send_log.name) as f:
    raw = f.read()

  os.unlink(recv_log.name); os.unlink(send_log.name)
  return parse_log(raw)

def save(data, prefix):
  RESULTS_DIR.mkdir(exist_ok=True)
  out = RESULTS_DIR / f"{prefix}.json"
  with open(out, "w") as f:
    json.dump(data, f, indent=2)
  print(f"Results saved to {out}")
  return out

def plot_single(burs, bitrates, delays, title="", out_pdf="out.pdf", window=10):
  if not bitrates:
    print("[error] No data to plot."); return
  
  def smooth(data):
    if window <= 1 or len(data) < window: return data
    box = np.ones(window)/window
    return np.convolve(data, box, mode='same')
  
  t = [i * 16.67 for i in range(len(bitrates))]
  fig, (a1, a2, a3) = plt.subplots(3, 1, figsize=(10, 8), sharex=True)
  fig.suptitle(title, fontsize=13, fontweight="bold")

  a1.plot(t, bitrates, color="red", alpha=0.25, lw=1)
  a1.plot(t, smooth(bitrates), color="red", lw=2)
  a1.set_ylabel("Bitrate (Mbps)", fontweight="bold")
  a1.grid(True, ls="--", alpha=0.6)

  a2.plot(t, delays, color="green", alpha=0.25, lw=1)
  a2.plot(t, smooth(delays), color="green", lw=2)
  a2.set_ylabel("Delay (ms)", fontweight="bold")
  a2.grid(True, ls="--", alpha=0.6)

  a3.plot(t, burs, color="blue", alpha=0.25, lw=1)
  a3.plot(t, smooth(burs), color="blue", lw=2)
  a3.axhline(1.0,  color="black", ls="--", lw=2,   label="BUR=1.0")
  a3.axhline(0.85, color="gray",  ls=":",  lw=1.5, label="alpha=0.85")
  a3.set_xlabel("Timeline (ms)", fontweight="bold")
  a3.set_ylabel("BUR", fontweight="bold")
  a3.legend(loc="lower right")
  a3.grid(True, ls="--", alpha=0.6)

  plt.tight_layout()
  plt.savefig(out_pdf, dpi="figure", format="pdf")
  print(f"Plot saved in {out_pdf}")

def bar_metric(labels, values, ylabel, title, out_pdf="bar_out.pdf"):
  fig, ax = plt.subplots(figsize=(max(4, len(labels) * 1.5), 4))
  ax.bar(labels, values, color=plt.cm.tab10.colors[:len(labels)])
  ax.set_ylabel(ylabel)
  ax.set_title(title)
  plt.tight_layout()
  plt.savefig(out_pdf, dpi="figure", format="pdf")
  plt.close(fig)
  print(f"Bar plot saved in {out_pdf}")
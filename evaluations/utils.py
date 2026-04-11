import json, os, subprocess, time
from pathlib import Path
import matplotlib.pyplot as plt
import numpy as np

_ROOT = Path(__file__).resolve().parent.parent

PKT_BITS     = 1400 * 8
SENDER_BIN   = str(_ROOT / "codes" / "sender")
RECEIVER_BIN = str(_ROOT / "codes" / "receiver")
RESULTS_DIR  = _ROOT / "results"
TRACES_DIR   = _ROOT / "traces"
MAHIMAHI_IP  = "$MAHIMAHI_BASE"


def const_trace(path, bw, secs):
  ms_pkt = 1000 / ((bw * 1e6) / PKT_BITS)
  with open(path, "w") as f:
    t = 0.0
    while t < secs * 1000:
      f.write(f"{int(t)}\n")
      t += ms_pkt

def step_trace(path, bw1, bw2, swap_s, secs):
  with open(path, "w") as f:
    for bw, end in [(bw1, swap_s * 1000), (bw2, secs * 1000)]:
      ms = 1000 / ((bw * 1e6) / PKT_BITS)
      t = end - (secs - swap_s) * 1000 if bw == bw2 else 0.0
      # reset t properly
    t = 0.0
    for bw, end_ms in [(bw1, swap_s * 1000), (bw2, secs * 1000)]:
      ms = 1000 / ((bw * 1e6) / PKT_BITS)
      while t < end_ms:
        f.write(f"{int(t)}\n")
        t += ms

def parse_log(text):
  burs, bitrates, delays = [], [], []
  for line in text.splitlines():
    if "BUR:" not in line or "bitrate:" not in line or "delay:" not in line:
      continue
    try:
      parts = line.split()
      burs.append(float(parts[parts.index("BUR:") + 1]))
      bitrates.append(float(parts[parts.index("bitrate:") + 1]))
      delays.append(float(parts[parts.index("delay:") + 1]))
    except (ValueError, IndexError):
      continue
  return burs, bitrates, delays

def jains_fairness(bitrates_per_flow):
  avgs = [np.mean(b) for b in bitrates_per_flow if b]
  if not avgs: return 0.0
  n = len(avgs)
  return (sum(avgs) ** 2) / (n * sum(x ** 2 for x in avgs))

def stall_rate(delays, thresh_ms=100.0):
  if not delays: return 0.0
  return sum(1 for d in delays if d > thresh_ms) / len(delays)

def summarise(burs, bitrates, delays, label="flow"):
  return {
    "label": label,
    "n_frames": len(bitrates),
    "avg_bitrate": round(float(np.mean(bitrates)), 3) if bitrates else 0,
    "avg_delay":   round(float(np.mean(delays)),   3) if delays   else 0,
    "p95_delay":   round(float(np.percentile(delays, 95)), 3) if delays else 0,
    "p99_delay":   round(float(np.percentile(delays, 99)), 3) if delays else 0,
    "stall_100ms": round(stall_rate(delays, 100), 4),
    "stall_200ms": round(stall_rate(delays, 200), 4),
    "avg_bur":     round(float(np.mean(burs)), 4) if burs else 0,
  }

def cleanup(procs):
  for p in procs:
    if p and p.poll() is None:
      p.terminate()
      p.wait()

def save(data, prefix):
  RESULTS_DIR.mkdir(exist_ok=True)
  out = RESULTS_DIR / f"{prefix}.json"
  out.write_text(json.dumps(data, indent=2))
  print(f"saved to: {out}")
  return out

def plot_single(burs, bitrates, delays, title="", out_pdf="out.pdf", window=10):
  if not bitrates:
    print("[error] no data to plot"); return

  def smooth(d):
    if window <= 1 or len(d) < window: return d
    return np.convolve(d, np.ones(window) / window, mode="same")

  t = [i * 16.67 for i in range(len(bitrates))]
  fig, (a1, a2, a3) = plt.subplots(3, 1, figsize=(10, 8), sharex=True)
  fig.suptitle(title, fontsize=13, fontweight="bold")

  for ax, data, label, color in [
    (a1, bitrates, "Bitrate (Mbps)", "red"),
    (a2, delays,   "Delay (ms)",     "green"),
    (a3, burs,     "BUR",            "blue"),
  ]:
    ax.plot(t, data, color=color, alpha=0.2, lw=1)
    ax.plot(t, smooth(data), color=color, lw=2)
    ax.set_ylabel(label, fontweight="bold")
    ax.grid(True, ls="--", alpha=0.5)

  a3.axhline(1.0,  color="black", ls="--", lw=2,   label="BUR=1.0")
  a3.axhline(0.85, color="gray",  ls=":",  lw=1.5, label="alpha=0.85")
  a3.legend(loc="lower right")
  a3.set_xlabel("timeline (ms)", fontweight="bold")

  plt.tight_layout()
  plt.savefig(out_pdf, dpi="figure", format="pdf")
  plt.close(fig)
  print(f"plot: {out_pdf}")

def plot_fairness(flow_data, fairness, title="", out_pdf="fairness.pdf", window=10):
  """all flows overlaid on one page: bitrate / delay / bur subplots."""
  n = len(flow_data)
  colors = plt.cm.tab10.colors

  def smooth(d):
    if window <= 1 or len(d) < window: return d
    return np.convolve(d, np.ones(window) / window, mode="same")

  fig, axes = plt.subplots(3, 1, figsize=(11, 9), sharex=True)
  fig.suptitle(f"{title}  |  Jain's index = {fairness:.3f}", fontsize=13, fontweight="bold")
  labels_used = set()

  for i, (burs, bitrates, delays) in enumerate(flow_data):
    if not bitrates: continue
    c  = colors[i % 10]
    t  = [j * 16.67 for j in range(len(bitrates))]
    lbl = f"flow {i}"

    for ax, data in zip(axes, [bitrates, delays, burs]):
      ax.plot(t, data, color=c, alpha=0.15, lw=1)
      ax.plot(t, smooth(data), color=c, lw=2,
              label=lbl if lbl not in labels_used else "")
    labels_used.add(lbl)

  axes[0].set_ylabel("bitrate (Mbps)", fontweight="bold")
  axes[1].set_ylabel("delay (ms)",     fontweight="bold")
  axes[2].set_ylabel("BUR",            fontweight="bold")
  axes[2].axhline(1.0,  color="black", ls="--", lw=1.5, label="BUR=1.0")
  axes[2].axhline(0.85, color="gray",  ls=":",  lw=1.2, label="alpha=0.85")
  axes[2].set_xlabel("timeline (ms)", fontweight="bold")

  for ax in axes:
    ax.grid(True, ls="--", alpha=0.4)
    ax.legend(loc="upper right", fontsize=8)

  plt.tight_layout()
  plt.savefig(out_pdf, dpi="figure", format="pdf")
  plt.close(fig)
  print(f"fairness plot: {out_pdf}")
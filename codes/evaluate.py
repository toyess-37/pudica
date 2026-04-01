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
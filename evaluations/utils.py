import json
import logging
import subprocess
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

log = logging.getLogger(__name__)

_ROOT        = Path(__file__).resolve().parent.parent
SENDER_BIN   = str(_ROOT / "codes" / "sender")
RECEIVER_BIN = str(_ROOT / "codes" / "receiver")
RESULTS_DIR  = _ROOT / "results"
TRACES_DIR   = _ROOT / "traces"
ZEUS_DIR     = _ROOT / "zeus_traces"

# for cross-machine testing write the actual receiver IP here within the quotes
TARGET_IP = "$MAHIMAHI_BASE"

# Mahimahi traces encode bandwidth as packet departure timestamps in ms.
# We assume 1418-byte packets (according to protocol.h)
PKT_BITS = 1418 * 8
FRAME_MS = 1000 / 60

# Trace generation

def const_trace(path: Path, bw_mbps: float, secs: int) -> None:
  # constant mahimahi uplink (or downlink for that purpose) trace
  ms_per_pkt = 1000 / ((bw_mbps * 1e6) / PKT_BITS)
  with open(path, "w") as f:
    t = 0.0
    while t < secs * 1000:
      f.write(f"{int(t)}\n")
      t += ms_per_pkt

def step_trace(path: Path, bw1: float, bw2: float, swap_s: int, secs: int) -> None:
  # two-phase bandwidth trace: bw1 for swap_s seconds, then bw2
  with open(path, "w") as f:
    t = 0.0
    for bw, end_ms in [(bw1, swap_s * 1000), (bw2, secs * 1000)]:
      ms_per_pkt = 1000 / ((bw * 1e6) / PKT_BITS)
      while t < end_ms:
        f.write(f"{int(t)}\n")
        t += ms_per_pkt

# Log parsing

def parse_log(text: str) -> tuple[list, list, list]:
  # The sender prints one line per completed frame: BUR: <float> bitrate: <float> delay: <float> ...
  burs, bitrates, delays = [], [], []
  n_candidate = 0

  for line in text.splitlines():
    if "BUR:" not in line or "bitrate:" not in line or "delay:" not in line: continue
    n_candidate += 1
    try:
      parts = line.split()
      burs.append(float(parts[parts.index("BUR:") + 1]))
      bitrates.append(float(parts[parts.index("bitrate:") + 1]))
      delays.append(float(parts[parts.index("delay:") + 1]))
    except (ValueError, IndexError) as e:
      log.warning("unrecognized line (%s): %r", e, line)

  # Distinguish the three failure modes so callers know what actually went wrong.
  if not text.strip():
    log.warning("sender produced no output - binary may not have run")
  elif n_candidate == 0:
    log.warning(
      "sender output contains no BUR lines - format may have changed\n"
      "  first 3 lines: %s",
      "\n  ".join(text.splitlines()[:3]),
    )
  elif not burs:
    log.warning("%d candidate lines found but all failed to parse", n_candidate)

  return burs, bitrates, delays

def parse_jsonl(path: Path) -> tuple[list, list, list]:
  # parse the structured JSONL log from logger.cc
  # raises ValueError on bad input so we don't get silent failures
  path = Path(path)
  if not path.exists():
    raise FileNotFoundError(f"log not found: {path}")

  events = []
  with open(path) as f:
    for lineno, raw in enumerate(f, 1):
      raw = raw.strip()
      if not raw: continue
      try:
        events.append(json.loads(raw))
      except json.JSONDecodeError as e:
        raise ValueError(f"malformed JSON at line {lineno}: {e!r}") from e

  frames = [e for e in events if e.get("type") == "FRAME_ACKED"]
  if not frames:
    raise ValueError(f"no FRAME_ACKED events in {path} ({len(events)} total events)")

  return (
    [f["bur"]      for f in frames],
    [f["bitrate"]  for f in frames],
    [f["delay_ms"] for f in frames],
  )

# Statistics

def stall_rate(delays: list, thresh_ms: float = 100.0) -> float:
  if not delays: return 0.0
  return sum(1 for d in delays if d > thresh_ms) / len(delays)

def summarise(burs: list, bitrates: list, delays: list, label: str = "flow") -> dict:
  # per-flow summary stats over the windows
  def pct(data, p):
    return round(float(np.percentile(data, p)), 3) if data else 0

  return {
    "label":       label,
    "n_frames":    len(bitrates),
    "avg_bitrate": round(float(np.mean(bitrates)), 3) if bitrates else 0,
    "avg_delay":   round(float(np.mean(delays)),   3) if delays   else 0,
    "p95_delay":   pct(delays, 95),
    "p99_delay":   pct(delays, 99),
    "stall_100ms": round(stall_rate(delays, 100), 4),
    "stall_200ms": round(stall_rate(delays, 200), 4),
    "avg_bur":     round(float(np.mean(burs)), 4) if burs else 0,
  }

def smooth(data: list, window: int = 10) -> np.ndarray:
  # visual smoothing
  if window <= 1 or len(data) < window:
    return np.array(data)
  return np.convolve(data, np.ones(window) / window, mode="same")

# Helpers

def make_script(path, cmds: list) -> Path:
  # Write an executable shell script into path/run.sh.
  s = Path(path) / "run.sh"
  s.write_text("#!/bin/sh\n" + "\n".join(cmds) + "\n")
  s.chmod(0o755)
  return s

def sender_cmd(port: int, dur: int, log_path: Path) -> str:
  return f"{SENDER_BIN} --log {log_path} {TARGET_IP} {port} {dur} > /dev/null 2>&1"

def cleanup(procs: list) -> None:
  # SIGTERM all live subprocesses and wait for them to exit.
  for p in procs:
    if p and p.poll() is None:
      p.terminate()
      try:
        p.wait(timeout=5)
      except subprocess.TimeoutExpired:
        log.warning("process %d did not exit after SIGTERM, sending SIGKILL", p.pid)
        p.kill()
        p.wait()

def wait_for_experiment(proc: subprocess.Popen, timeout_s: int) -> bool:
  # wait up to timeout_s for the mahimahi shell to finish
  try:
    proc.wait(timeout=timeout_s)
    return True
  except subprocess.TimeoutExpired:
    log.error(
      "experiment timed out after %ds (pid %d) - "
      "sender may have deadlocked in precise_sleep",
      timeout_s, proc.pid,
    )
    return False

# save results

def save(data: dict, prefix: str) -> Path:
  RESULTS_DIR.mkdir(exist_ok=True)
  out = RESULTS_DIR / f"{prefix}.json"
  out.write_text(json.dumps(data, indent=2))
  log.info("results written to %s", out)
  return out

# Plotting

def plot_single(burs: list, bitrates: list, delays: list, title: str = "", out_svg: str = "out.svg", window: int = 10) -> None:
  if not bitrates:
    log.error("plot_single called with no data")
    return

  t = [i * FRAME_MS for i in range(len(bitrates))]

  fig, (ax_br, ax_dl, ax_bur) = plt.subplots(3, 1, figsize=(12, 9), sharex=True)
  fig.suptitle(title, fontsize=13, fontweight="bold")

  for ax, data, ylabel, color in [
    (ax_br,  bitrates, "Bitrate (Mbps)", "tab:red"),
    (ax_dl,  delays,   "Delay (ms)",     "tab:green"),
    (ax_bur, burs,     "BUR",            "tab:blue"),
  ]:
    ax.plot(t, data, color=color, alpha=0.20, lw=0.8)
    ax.plot(t, smooth(data, window), color=color, lw=1.8)
    ax.set_ylabel(ylabel)
    ax.grid(True, ls="--", alpha=0.4)

  # alpha=0.85 is the MI/AI-MD threshold from paper section 4.1.
  ax_bur.axhline(1.0,  color="black", ls="--", lw=1.5, label="BUR = 1.0")
  ax_bur.axhline(0.85, color="gray",  ls=":",  lw=1.2, label="alpha = 0.85")
  ax_bur.legend(loc="upper right", fontsize=9)
  ax_bur.set_xlabel("time (ms)")

  plt.tight_layout()
  plt.savefig(out_svg, format="svg")
  plt.close(fig)
  log.info("plot saved to %s", out_svg)
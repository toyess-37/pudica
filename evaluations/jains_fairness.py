import argparse, subprocess, tempfile, time
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path
from utils import (
  TRACES_DIR, RECEIVER_BIN,
  const_trace, cleanup, parse_log, summarise, 
  save, smooth, make_script, sender_cmd
)

# calculate Jains fairness index
def jains_fairness(bitrates_per_flow):
  avgs = [np.mean(b) for b in bitrates_per_flow if b]
  if not avgs: return 0.0
  n = len(avgs)
  return (sum(avgs) ** 2) / (n * sum(x ** 2 for x in avgs))

def plot_fairness(flow_data, fairness, stagger_s=0, dur_s=45, title="", window=10, out_svg="fairness.svg"):
  colors = plt.cm.tab10.colors

  fig, axes = plt.subplots(3, 1, figsize=(12, 9), sharex=True)
  fig.suptitle(f"{title}  |  Jain's index = {fairness:.3f}",
               fontsize=13, fontweight="bold")

  for i, (burs, bitrates, delays) in enumerate(flow_data):
    if not bitrates: continue
    c      = colors[i % 10]
    # offset this flow's frames by its entry time (ms)
    t0_ms  = i * stagger_s * 1000
    t      = [t0_ms + j * 16.666 for j in range(len(bitrates))]
    lbl    = f"flow {i}  (t={i*stagger_s}s-{i*stagger_s+dur_s}s)"

    for ax, data in zip(axes, [bitrates, delays, burs]):
      ax.plot(t, data, color=c, alpha=0.25, lw=1)
      ax.plot(t, smooth(data, window), color=c, lw=2, label=lbl)

  # vertical entry markers on all subplots
  for i in range(len(flow_data)):
    entry_ms = i * stagger_s * 1000
    for ax in axes:
      ax.axvline(entry_ms, color=colors[i % 10],
                 ls="--", lw=1.2, alpha=0.6,
                 label=f"flow {i} enters" if ax is axes[0] else "")

  axes[0].set_ylabel("Bitrate (Mbps)", fontweight="bold")
  axes[1].set_ylabel("Delay (ms)",     fontweight="bold")
  axes[2].set_ylabel("BUR",            fontweight="bold")
  axes[2].axhline(1.0,  color="black", ls="--", lw=1.5, label="BUR=1.0")
  axes[2].axhline(0.85, color="gray",  ls=":",  lw=1.2, label="alpha=0.85")
  axes[2].set_xlabel("timeline", fontweight="bold")

  # x-axis ticks in seconds for readability
  total_ms = (dur_s + stagger_s * max(0, len(flow_data) - 1)) * 1000
  tick_ms  = [i * 10000 for i in range(int(total_ms/10000) + 2)]
  axes[2].set_xticks(tick_ms)
  axes[2].set_xticklabels([f"{int(x/1000)}s" for x in tick_ms])
  axes[2].set_xlim(0, total_ms)

  for ax in axes:
    ax.grid(True, ls="--", alpha=0.4)
    ax.legend(loc="upper right", fontsize=7, ncol=2)

  plt.tight_layout()
  plt.savefig(out_svg, dpi="figure", format="svg")
  plt.close(fig)
  print(f"fairness plot: {out_svg}")

def run(args):
  TRACES_DIR.mkdir(exist_ok=True)
  total_dur = args.dur + args.stagger * args.flows
  trace = TRACES_DIR / f"{args.bw}Mbps.up"
  const_trace(trace, args.bw, total_dur)
  print(f"[*] bw={args.bw} Mbps  flows={args.flows}  dur={args.dur}s  stagger={args.stagger}s")

  procs = []
  with tempfile.TemporaryDirectory() as tmpdir:
    tmp = Path(tmpdir)
    send_logs = [tmp / f"send{i}.log" for i in range(args.flows)]

    try:
      # start all receivers on consecutive ports outside mahimahi
      for i in range(args.flows):
        port = args.port + i
        procs.append(subprocess.Popen([RECEIVER_BIN, str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
      time.sleep(0.4)

      # build one inner bash script that staggers sender launches
      cmds = []
      for i in range(args.flows):
        port = args.port + i
        delay = f"sleep {i * args.stagger} && " if i > 0 and args.stagger else ""
        cmds.append(f"({delay}{sender_cmd(port, args.dur, send_logs[i])}) &")
      cmds.append("wait")
      script = make_script(tmp, cmds)
      mm_cmd = (f"mm-delay {args.rtt // 2} mm-link {trace} {trace} -- {script}")
      procs.append(subprocess.Popen(mm_cmd, shell=True))
      time.sleep(total_dur + 3)

    finally:
      cleanup(procs)

    flow_data = []
    for i, lf in enumerate(send_logs):
      raw = lf.read_text() if lf.exists() else ""
      data = parse_log(raw)
      if not data[0]:
        print(f"[!] flow {i} produced no data")
      flow_data.append(data)

  summaries  = [summarise(*fd, label=f"flow_{i}") for i, fd in enumerate(flow_data)]
  fairness   = jains_fairness([fd[1] for fd in flow_data])

  print(f"\n[fairness] jain's index = {fairness:.3f}")
  for s in summaries:
    print(f"  {s['label']}: avg_br={s['avg_bitrate']} Mbps  avg_delay={s['avg_delay']} ms  stall={s['stall_100ms']*100:.3f}%")

  out = save({
    "test": "fairness",
    "bw_Mbps": args.bw,
    "jains_index": round(fairness, 3),
    "per_flow": summaries,
  }, "fairness")

  if args.plot:
    plot_fairness(
      flow_data, fairness,
      stagger_s=args.stagger,
      title=f"fairness - {args.flows} flows on {args.bw} Mbps",
      out_svg=str(out).replace(".json", ".svg"),
    )

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--flows",   type=int,   default=3)
  parser.add_argument("--bw",      type=float, default=30)
  parser.add_argument("--dur",     type=int,   default=45)
  parser.add_argument("--stagger", type=int,   default=10)
  parser.add_argument("--rtt",     type=int,   default=20)
  parser.add_argument("--port",    type=int,   default=9100,
    help="base port; receivers use port, port+1, ... port+N-1")
  parser.add_argument("--plot",    action="store_true")
  run(parser.parse_args())
import argparse, subprocess, tempfile, time
from pathlib import Path
from utils import (
  TRACES_DIR, RECEIVER_BIN, SENDER_BIN, MAHIMAHI_IP,
  const_trace, cleanup, parse_log, summarise,
  jains_fairness, save, plot_fairness
)

def run(args):
  TRACES_DIR.mkdir(exist_ok=True)
  total_dur = args.dur + args.stagger * args.flows
  trace = TRACES_DIR / f"fair_{args.bw}mbps.up"
  const_trace(trace, args.bw, total_dur)
  print(f"[*] bw={args.bw} mbps  flows={args.flows}  dur={args.dur}s  stagger={args.stagger}s")

  procs = []
  with tempfile.TemporaryDirectory() as tmpdir:
    tmp = Path(tmpdir)
    send_logs = [tmp / f"send{i}.log" for i in range(args.flows)]

    try:
      # start all receivers on consecutive ports outside mahimahi
      for i in range(args.flows):
        port = args.port + i
        procs.append(subprocess.Popen(
          [RECEIVER_BIN, str(port)],
          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        ))
      time.sleep(0.4)

      # build one inner bash script that staggers sender launches
      cmds = []
      for i in range(args.flows):
        port = args.port + i
        delay = f"sleep {i * args.stagger} && " if i > 0 and args.stagger else ""
        cmds.append(
          f"({delay}{SENDER_BIN} {MAHIMAHI_IP} {port} {args.dur} > {send_logs[i]} 2>&1) &"
        )
      inner = " ".join(cmds) + " wait;"

      mm_cmd = (
        f"mm-delay {args.rtt // 2} "
        f"mm-link {trace} {trace} "
        f"-- bash -c '{inner}'"
      )
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
    print(f"  {s['label']}: avg_br={s['avg_bitrate']} mbps  avg_delay={s['avg_delay']} ms  stall={s['stall_100ms']*100:.3f}%")

  out = save({
    "test": "fairness",
    "bw_mbps": args.bw,
    "jains_index": round(fairness, 3),
    "per_flow": summaries,
  }, "fairness")

  if args.plot:
    plot_fairness(
      flow_data, fairness,
      title=f"fairness — {args.flows} flows on {args.bw} mbps",
      out_pdf=str(out).replace(".json", ".pdf"),
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
import argparse
import os, subprocess, tempfile, time
from pathlib import Path
from utils import (
  TRACES_DIR, RECEIVER_BIN, SENDER_BIN, MAHIMAHI_IP, 
  const_trace, find_free_port, cleanup, parse_log, 
  summarise, jains_fairness, save, plot_single, bar_metric
)

def run(args):
  """
  Launch N Pudica senders simultaneously on the SAME bottleneck.
  Stagger flow entry by args.stagger seconds.
  Computes Jain's fairness index and per-flow summary.
  """
  TRACES_DIR.mkdir(exist_ok=True)
  trace = TRACES_DIR / f"fair_{args.bw}mbps_{args.dur}s.up"
  const_trace(trace, args.bw, args.dur + args.stagger * args.flows)
  print(f"[*] Shared Trace: {trace}  BW={args.bw}Mbps  flows={args.flows}  dur={args.dur}s")

  base_port = find_free_port(9100)
  procs = []

  with tempfile.TemporaryDirectory() as tmpdir:
    tmp = Path(tmpdir)
    recv_logs = [tmp / f"recv{i}.log" for i in range(args.flows)]
    send_logs = [tmp / f"send{i}.log" for i in range(args.flows)]
    
    try:
      # start all receivers outside
      for i in range(args.flows):
        port = base_port + i
        with open(recv_logs[i], "w") as f:
          procs.append(subprocess.Popen([RECEIVER_BIN, str(port)], stdout=f, stderr=f))
      time.sleep(0.4)

      # build the commands to run INSIDE the shared Mahimahi shell
      inner_cmds = []
      for i in range(args.flows):
        port = base_port + i
        sleep_cmd = f"sleep {i * args.stagger} && " if i > 0 and args.stagger else ""
        # run in background (&), redirect output directly to the log file inside the shell
        inner_cmds.append(f"({sleep_cmd}{SENDER_BIN} {MAHIMAHI_IP} {port} > {send_logs[i]} 2>&1) &")

      inner_script = " ".join(inner_cmds) + " wait;"

      # launch one mm-link and mm-delay container
      mm_cmd = f"mm-delay {args.rtt // 2} mm-link {trace} {trace} -- bash -c '{inner_script}'"
      
      print(f"[*] Launching Mahimahi...")
      procs.append(subprocess.Popen(mm_cmd, shell=True))
      
      time.sleep(args.dur + args.stagger * args.flows + 3)

    finally: cleanup(procs)

    # 4. Parse Data
    flow_data = []
    for idx, lf in enumerate(send_logs):
      if not lf.exists():
        print(f"\n[!] CRITICAL ERROR: Flow {idx} log file was never created!")
        flow_data.append(([], [], []))
        continue
        
      with open(lf, "r") as f:
        raw_log = f.read()
        data = parse_log(raw_log)
        if not data[0]:
          print(f"\n[!] CRITICAL ERROR: Flow {idx} failed to send data.\nRAW LOG:\n{raw_log}")
        flow_data.append(data)

  summaries = [summarise(*fd, label=f"flow_{i}") for i, fd in enumerate(flow_data)]
  fairness = jains_fairness([fd[1] for fd in flow_data])

  print(f"\n[FAIRNESS] Jain's index = {fairness:.3f}")
  for s in summaries:
    print(f"  {s['label']}: avg_br={s['avg_bitrate']} Mbps  avg_delay={s['avg_delay']} ms  stall={s['stall_100ms']*100:.3f}%")

  out = save({"test": "fairness", "bw_mbps": args.bw, "jains_index": round(fairness, 3), "per_flow": summaries}, "fairness")

  if args.plot:
    for i, (b, br, d) in enumerate(flow_data):
      plot_single(b, br, d, title=f"Fairness-Flow {i} (Jain={fairness:.3f})", out_pdf=str(out).replace(".json", f"_flow{i}.pdf"))
    
    avgs = [s["avg_bitrate"] for s in summaries]
    bar_metric(
      labels=[s["label"] for s in summaries], 
      values=avgs, 
      ylabel="Avg Bitrate (Mbps)", 
      title=f"Per-Flow Bitrate  Jain={fairness:.3f}", 
      out_pdf=str(out).replace(".json", "_bar.pdf")
    )

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--flows", type=int, default=3)
  parser.add_argument("--bw", type=float, default=30)
  parser.add_argument("--dur", type=int, default=45)
  parser.add_argument("--stagger", type=int, default=10)
  parser.add_argument("--rtt", type=int, default=20)
  parser.add_argument("--plot", action="store_true")
  run(parser.parse_args())
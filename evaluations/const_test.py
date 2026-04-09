import argparse, subprocess, time
from pathlib import Path
from utils import (
  TRACES_DIR, RECEIVER_BIN, SENDER_BIN, MAHIMAHI_IP,
  const_trace, find_free_port, cleanup, parse_log, 
  summarise, save, plot_single
)

def run(args):
  TRACES_DIR.mkdir(exist_ok=True)
  trace = TRACES_DIR / f"const_{args.bw}mbps_{args.dur}s.up"
  const_trace(trace, args.bw, args.dur)
  print(f"[*] Trace: {trace}  BW={args.bw}Mbps  dur={args.dur}s  RTT={args.rtt}ms")

  port = find_free_port(9700)
  procs = []
  send_lf = Path(f"debug_send_{port}.log")

  try:
    print(f"[*] Starting receiver on port {port}...")
    recv_proc = subprocess.Popen(
      [RECEIVER_BIN, str(port)], 
      stdout=subprocess.DEVNULL, 
      stderr=subprocess.DEVNULL
    )
    procs.append(recv_proc)
    time.sleep(0.5)

    if recv_proc.poll() is not None:
      print(f"[!] Receiver crashed immediately with code {recv_proc.returncode}")
      return

    inner_cmd = f"{SENDER_BIN} {MAHIMAHI_IP} {port} > {send_lf} 2>&1"
    mm_cmd = f"mm-delay {args.rtt // 2} mm-link {trace} {trace} -- bash -c '{inner_cmd}'"
    
    print(f"[*] Starting Mahimahi sender...")
    procs.append(subprocess.Popen(mm_cmd, shell=True))
    
    time.sleep(args.dur + 3)
    
  finally: cleanup(procs)

  if not send_lf.exists():
    print("[!] ERROR: Log file was never created by Mahimahi!")
    return

  with open(send_lf, "r") as f:
    raw_log = f.read()
    burs, bitrates, delays = parse_log(raw_log)

  # 5. THE DIAGNOSTIC DUMP
  if not burs:
    print("\n" + "="*60)
    print("CRITICAL: SENDER LOG IS EMPTY OR INVALID!")
    print("Here is the raw output from the sender/bash container:")
    print("-" * 60)
    print(raw_log if raw_log.strip() else "<File is completely empty. Did the receiver send ACKs?>")
    print("="*60 + "\n")
    # Leave the log file on disk so you can inspect it
    return

  s = summarise(burs, bitrates, delays, label="const_baseline")
  print(f"\n[BASELINE] avg_br={s['avg_bitrate']} Mbps  avg_delay={s['avg_delay']} ms  stall={s['stall_100ms']*100:.3f}%")
  
  out = save({"test": "const_baseline", "bw_mbps": args.bw, "summary": s}, "const_baseline")
  
  if args.plot:
    plot_single(
      burs, bitrates, delays, 
      title=f"Baseline Convergence (BW={args.bw}Mbps)", 
      out_pdf=str(out).replace(".json", ".pdf")
    )
    
  # Clean up the local log file if successful
  send_lf.unlink(missing_ok=True)

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--bw", type=float, default=20)
  parser.add_argument("--dur", type=int, default=30)
  parser.add_argument("--rtt", type=int, default=20)
  parser.add_argument("--plot", action="store_true")
  run(parser.parse_args())
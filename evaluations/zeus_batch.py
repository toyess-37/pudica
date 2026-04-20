import argparse, subprocess, time
from pathlib import Path
import tempfile
from utils import (
  ZEUS_DIR, RECEIVER_BIN, RESULTS_DIR, cleanup, parse_log, summarise, save,
  make_script, sender_cmd
)

def get_full_duration(trace_path):
  """Reads the last line of the trace file to find its total length in seconds."""
  with open(trace_path, "r") as f:
    lines = [line.strip() for line in f if line.strip()]
    if not lines: return 30
    last_ms = int(lines[-1])
    return (last_ms // 1000) + 2

def run_zeus_trace(trace_path, dur, rtt, port):
  print(f"[*] Running Zeus trace: {trace_path.name} | dur={dur}s")
  with tempfile.TemporaryDirectory() as tmp:
    tmp_path = Path(tmp)
    send_lf = tmp_path / "send.log"
    procs = []
    try:
      procs.append(subprocess.Popen(
        [RECEIVER_BIN, str(port)], 
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
      ))
      time.sleep(0.5)
      
      script = make_script(tmp_path, [sender_cmd(port, dur, send_lf)])
      mm_cmd = f"mm-delay {rtt // 2} mm-link {trace_path} {trace_path} -- {script}"
      procs.append(subprocess.Popen(mm_cmd, shell=True))
      
      time.sleep(dur + 3)
    finally:
      cleanup(procs)

    burs, bitrates, delays = parse_log(send_lf.read_text() if send_lf.exists() else "")

  if not burs:
    print(f"[!] Empty log for {trace_path.name}.")
    return None

  return summarise(burs, bitrates, delays, label=trace_path.stem)

def run_batch(args):
  target_path = Path(args.traces).resolve()
  if target_path.is_file():
    trace_files = [target_path]
  elif target_path.is_dir():
    trace_files = sorted(target_path.glob("*.up"))
  else:
    target_path = ZEUS_DIR / args.traces
    if target_path.is_dir():
      trace_files = sorted(target_path.glob("*.up"))
    else:
      print(f"[!] Invalid path: {args.traces}")
      return

  if not trace_files:
    print("[!] No .up trace files found.")
    return

  results = []
  test_type = "zeus_batch"
  for i, trace in enumerate(trace_files):
    test_port = args.port + i 
    
    if args.full:
      test_dur = get_full_duration(trace)
      test_type = "zeus_batch_full"
    else:
      test_dur = args.dur

    s = run_zeus_trace(trace, test_dur, args.rtt, test_port)
    if s:
      results.append(s)

  if not results:
    return

  save({"test": test_type, "results": results}, test_type)

  # Write tabular summary
  RESULTS_DIR.mkdir(exist_ok=True)
  summary_file = RESULTS_DIR / f"{test_type}_tabular_summary.txt"

  with open(summary_file, "w") as f:
    f.write(f"{'trace':<25} {'avg_rate':>10} {'avg_delay':>10} {'p99_delay':>10} {'stall_100ms%':>12}\n")
    f.write("-" * 75 + "\n")
    for s in results:
      f.write(f"{s['label']:<25} {s['avg_bitrate']:>9.2f}m {s['avg_delay']:>8.1f}ms "
              f"{s['p99_delay']:>8.1f}ms {s['stall_100ms']*100:>11.3f}%\n")

  print(f"\nTests complete. Analysis written to: {summary_file}")

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--traces", type=str, default=str(ZEUS_DIR), help="Directory containing .up files")
  parser.add_argument("--rtt",    type=int, default=20)
  parser.add_argument("--port",   type=int, default=9800)
  
  dur_group = parser.add_mutually_exclusive_group()
  dur_group.add_argument("--dur",  type=int, default=30, help="Run for a fixed duration in seconds (default 30)")
  dur_group.add_argument("--full", action="store_true", help="Auto-detect and run for the complete duration of the trace")
  
  run_batch(parser.parse_args())
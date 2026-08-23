import argparse
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed

def run_test(command):
  print(f"[START] {command}")
  start_t = time.time()
  try:
    full_cmd = [sys.executable] + command.split()
    res = subprocess.run(full_cmd, capture_output=True, text=True, check=True)
    dur = time.time() - start_t
    print(f"[PASS] ({dur:.1f}s) {command}")
    return True, command, res.stdout
  except subprocess.CalledProcessError as e:
    dur = time.time() - start_t
    print(f"[FAIL] ({dur:.1f}s) {command}\n{e.stderr}")
    return False, command, e.stderr

def main():
  parser = argparse.ArgumentParser(description="Parallel master automation script for Pudica evaluations.")
  
  # Global Duration Controls
  dur_group = parser.add_mutually_exclusive_group()
  dur_group.add_argument("--dur", type=int, default=30, help="Base duration for all tests in seconds (default: 30)")
  parser.add_argument("--full", action="store_true", help="Run trace-based tests (Zeus) for their complete duration")
  parser.add_argument("-j", "--jobs", type=int, default=4, help="Number of parallel jobs (default: 4)")
  
  args = parser.parse_args()

  print(f"[*] Starting parallel Pudica evaluation suite...")
  print(f"[*] Base Synthetic Duration: {args.dur}s")
  print(f"[*] Zeus Trace Mode: {'Full Duration' if args.full else f'{args.dur}s Fixed'}")
  print(f"[*] Parallel Jobs: {args.jobs}")

  swap_time = max(5, args.dur // 3) 
  fairness_dur = args.dur + 15

  zeus_cmd = "zeus_batch.py --full" if args.full else f"zeus_batch.py --dur {args.dur}"

  commands = [
    f"const_test.py --bw 20 --dur {args.dur} --plot",
    f"step_test.py --bw1 20 --bw2 10 --swap {swap_time} --dur {args.dur} --plot",
    f"jitter_test.py --bw 20 --jitter 40 --period 500 --dur {args.dur} --plot",
    f"bur_accuracy.py --bw-list 5,10,15,20,25 --dur {args.dur} --plot",
    f"jains_fairness.py --flows 3 --bw 30 --dur {fairness_dur} --stagger 10 --plot",
    f"tcpcubic_compete.py --bw 50 --buf 7 --dur {args.dur} --plot",
    f"tcpcubic_compete.py --bw 50 --buf 50 --dur {args.dur} --plot",
    f"tests_mm.py --dur {args.dur}",
    zeus_cmd
  ]

  success = 0
  with ProcessPoolExecutor(max_workers=args.jobs) as executor:
    futures = {executor.submit(run_test, cmd): cmd for cmd in commands}
    for future in as_completed(futures):
      passed, cmd, out = future.result()
      if passed:
        success += 1

  print("="*60)
  print(f"ALL TESTS COMPLETE! {success}/{len(commands)} passed.")
  print("="*60)
  
  if success != len(commands):
    sys.exit(1)

if __name__ == "__main__":
  main()

import argparse
import subprocess
import sys
import time

def print_header(title):
  print("="*50)
  print(f"Running test: {title}")
  print("="*50)

def run_test(command):
  try:
    full_cmd = [sys.executable] + command.split()
    subprocess.run(full_cmd, check=True)
    time.sleep(2) 
  except subprocess.CalledProcessError as e:
    print(f"\n[!] ERROR: Test failed with command: {command}")
    print(e)

def main():
  parser = argparse.ArgumentParser(description="Master automation script for Pudica evaluations.")
  
  # Global Duration Controls
  dur_group = parser.add_mutually_exclusive_group()
  dur_group.add_argument("--dur", type=int, default=30, help="Base duration for all tests in seconds (default: 30)")
  parser.add_argument("--full", action="store_true", help="Run trace-based tests (Zeus) for their complete duration")
  
  args = parser.parse_args()

  print(f"[*] Starting full Pudica evaluation suite...")
  print(f"[*] Base Synthetic Duration: {args.dur}s")
  print(f"[*] Zeus Trace Mode: {'Full Duration' if args.full else f'{args.dur}s Fixed'}")

  # test1: Baseline Convergence
  print_header(f"Constant Link ({args.dur}s)")
  run_test(f"const_test.py --bw 20 --dur {args.dur} --plot")

  # test2: step trace test
  swap_time = max(5, args.dur // 3) 
  print_header(f"Step Test: (Bandwidth Drop at {swap_time}s)")
  run_test(f"step_test.py --bw1 20 --bw2 10 --swap {swap_time} --dur {args.dur} --plot")

  # test3: jitter test
  print_header(f"Jitter Test: ({args.dur}s)")
  run_test(f"jitter_test.py --bw 20 --jitter 40 --period 500 --dur {args.dur} --plot")

  # test4: BUR Estimation Accuracy
  print_header(f"BUR Estimation Accuracy ({args.dur}s per point)")
  run_test(f"bur_accuracy.py --bw-list 5,10,15,20,25 --dur {args.dur} --plot")

  # test5: Jain's Fairness test
  fairness_dur = args.dur + 15
  print_header(f"Cross-Flow Fairness (3 Flows, {fairness_dur}s total)")
  run_test(f"jains_fairness.py --flows 3 --bw 30 --dur {fairness_dur} --stagger 10 --plot")

  # test6: TCP Cubic Competition
  print_header(f"TCP Cubic Competition (Shallow Buffer: 7 pkts)")
  run_test(f"tcpcubic_compete.py --bw 50 --buf 7 --dur {args.dur} --plot")
  
  print_header(f"TCP Cubic Competition (Deep Buffer: 50 pkts)")
  run_test(f"tcpcubic_compete.py --bw 50 --buf 50 --dur {args.dur} --plot")

  # test7: Mahimahi Built-in Traces
  print_header(f"Mahimahi LTE Traces ({args.dur}s)")
  run_test(f"tests_mm.py --dur {args.dur}")

  # test8: Zeus 5G Traces
  print_header("Zeus 5G Traces")
  zeus_cmd = "zeus_batch.py --full" if args.full else f"zeus_batch.py --dur {args.dur}"
  run_test(zeus_cmd)

  print("="*60)
  print("ALL TESTS COMPLETE! Check the 'results' folder.")
  print("="*60)

if __name__ == "__main__":
  main()
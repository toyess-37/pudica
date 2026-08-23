import argparse
import sys
import json
from pathlib import Path

def analyze_probes(log_path):
  if not Path(log_path).exists():
    print(f"Log not found: {log_path}")
    sys.exit(1)

  total_frames = 0
  total_probes_received = 0

  with open(log_path, 'r') as f:
    for line in f:
      line = line.strip()
      if not line:
        continue
      try:
        event = json.loads(line)
        if event.get("type") == "FRAME_ACKED":
          total_frames += 1
          total_probes_received += int(event.get("n_probes", 0))
      except json.JSONDecodeError:
        pass

  if total_frames == 0:
    print("No frames found.")
    return

  expected_probes = total_frames * 4
  lost_probes = expected_probes - total_probes_received
  loss_rate = (lost_probes / expected_probes) * 100 if expected_probes > 0 else 0

  print(f"Total Frames Acked: {total_frames}")
  print(f"Total Probes Expected: {expected_probes}")
  print(f"Total Probes Received: {total_probes_received}")
  print(f"Probe Loss Rate: {loss_rate:.2f}%")

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("log_path", type=str, help="Path to sender JSONL log")
  args = parser.parse_args()
  analyze_probes(args.log_path)
# sift.py - check logs for weird invariants
# TODO: maybe check if pacing is actually sane
import argparse, json, sys
from pathlib import Path

def load_events(path):
  events = []
  with open(path) as f:
    for i, line in enumerate(f, 1):
      line = line.strip()
      if not line: continue
      try:
        events.append(json.loads(line))
      except json.JSONDecodeError as e:
        print(f"[sift] bad json at line {i}: {e}")
        sys.exit(2)
  return events

def check_drain(events):
  # every DRAIN_START from congestion should have 3 preceding bur>1 frames
  bad = []
  for i, ev in enumerate(events):
    if ev["type"] != "DRAIN_START": continue
    if ev.get("fid", 0) == 0 or float(ev.get("drain_rate", 0.0)) == 0.0: continue

    prev = [e for e in events[:i] if e["type"] == "FRAME_ACKED"]
    if len(prev) < 3:
      bad.append(f"  DRAIN at fid={ev['fid']} - less than 3 preceding frames")
      continue

    last3 = prev[-3:]
    hot = [e for e in last3 if e["bur"] > 1.0]
    if len(hot) < 3:
      bad.append(
        f"  DRAIN at fid={ev['fid']} - only {len(hot)}/3 had bur>1 "
        f"(burs={[round(e['bur'],3) for e in last3]})")
  return bad

def check_fallback(events):
  # every FALLBACK_SET should have a matching FALLBACK_RESTORED
  bad = []
  for i, ev in enumerate(events):
    if ev["type"] != "FALLBACK_SET": continue

    # find next FRAME_ACKED after this
    next_acked = None
    for e in events[i+1:]:
      if e["type"] == "FRAME_ACKED":
        next_acked = e
        break

    if next_acked is None: continue  # end of log, whatever

    idx = events.index(next_acked, i+1)
    found = False
    for e in events[i+1:idx+1]:
      if e["type"] == "FALLBACK_RESTORED":
        found = True
        break

    if not found:
      bad.append(
        f"  FALLBACK_SET at fid={ev['fid']} - "
        f"no restore before next ack (fid={next_acked['fid']})")
  return bad

def check_loss(events):
  # every FRAME_LOSS should be followed by DRAIN_START soon after
  bad = []
  for i, ev in enumerate(events):
    if ev["type"] != "FRAME_LOSS": continue

    upcoming = events[i+1:i+3]
    if not any(e["type"] == "DRAIN_START" for e in upcoming):
      bad.append(
        f"  FRAME_LOSS at fid={ev.get('fid', 0)} - "
        f"no DRAIN_START within next 2 events")
  return bad

def main():
  parser = argparse.ArgumentParser()
  parser.add_argument("--log", required=True)
  args = parser.parse_args()

  if not Path(args.log).exists():
    print(f"[sift] file not found: {args.log}")
    sys.exit(2)

  events = load_events(args.log)
  print(f"[sift] loaded {len(events)} events")

  total_bad = 0

  # check drain is preceded by 3 congested frames
  r = check_drain(events)
  if r:
    print(f"\n[FAIL] drain check ({len(r)} issues):")
    for v in r: print(v)
    total_bad += len(r)
  else:
    print("[PASS] drain check")

  # check fallback is single-shot
  r = check_fallback(events)
  if r:
    print(f"\n[FAIL] fallback check ({len(r)} issues):")
    for v in r: print(v)
    total_bad += len(r)
  else:
    print("[PASS] fallback check")

  # check loss triggers drain
  r = check_loss(events)
  if r:
    print(f"\n[FAIL] loss check ({len(r)} issues):")
    for v in r: print(v)
    total_bad += len(r)
  else:
    print("[PASS] loss check")

  if total_bad:
    print(f"\n{total_bad} issue(s) found")
    sys.exit(1)
  else:
    print("\nall good!")

if __name__ == "__main__":
  main()
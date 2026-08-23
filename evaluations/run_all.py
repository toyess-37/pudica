import argparse, subprocess, sys, time

TESTS = [
  # (name, command, group) group=0 means parallel; group=1..N means sequential
  ("bur_5", "bur_accuracy.py --bw-list 5 --dur {dur} --port 9600", 0),
  ("bur_10", "bur_accuracy.py --bw-list 10 --dur {dur} --port 9601", 0),
  ("bur_15", "bur_accuracy.py --bw-list 15 --dur {dur} --port 9602", 0),
  ("bur_20", "bur_accuracy.py --bw-list 20 --dur {dur} --port 9603", 0),
  ("bur_25", "bur_accuracy.py --bw-list 25 --dur {dur} --port 9604", 0),
  ("const", "const_test.py --dur {dur} --port 9700", 1),
  ("step", "step_test.py --dur {dur} --port 9700", 1),
  ("jitter", "jitter_test.py --dur {dur} --port 9700", 1),
  ("fairness", "jains_fairness.py --dur {dur} --port 9100", 2),
  ("cubic_s", "tcpcubic_compete.py --buf 7 --dur {dur} --port 9200", 2),
  ("cubic_d", "tcpcubic_compete.py --buf 50 --dur {dur} --port 9200", 2),
]

def run_one(name, cmd):
  t0 = time.time()
  r = subprocess.run([sys.executable] + cmd.split(), capture_output=True, text=True)
  elapsed = time.time() - t0
  return name, r.returncode, elapsed, r.stdout, r.stderr

def main():
  parser = argparse.ArgumentParser()
  parser.add_argument("--dur", type=int, default=30)
  args = parser.parse_args()

  t_start = time.time()
  results = {}

  # Group 0: fully parallel BUR tests
  parallel = [(n, c.format(dur=args.dur)) for n, c, g in TESTS if g == 0]
  procs = []
  for n, c in parallel:
    t0 = time.time()
    p = subprocess.Popen([sys.executable] + c.split(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    procs.append((n, p, t0))
    
  for n, p, t0 in procs:
    out, err = p.communicate()
    code = p.returncode
    elapsed = time.time() - t0
    results[n] = code
    print(f"[{'OK' if code == 0 else 'FAIL'}] {n} ({elapsed:.1f}s)")
    if code != 0:
      print(f" stderr: {err[:200]}")

  # Groups 1+: sequential within each group
  for group_id in sorted(set(g for _, _, g in TESTS if g > 0)):
    group = [(n, c.format(dur=args.dur)) for n, c, g in TESTS if g == group_id]
    for n, c in group:
      name, code, elapsed, out, err = run_one(n, c)
      results[name] = code
      print(f"[{'OK' if code == 0 else 'FAIL'}] {name} ({elapsed:.1f}s)")
      if code != 0:
        print(f" stderr: {err[:200]}")

  total = time.time() - t_start
  passed = sum(1 for v in results.values() if v == 0)
  print(f"\n{passed}/{len(results)} tests passed in {total:.1f}s")

if __name__ == "__main__":
  main()
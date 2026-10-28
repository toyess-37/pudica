import argparse
import os
import shlex
import subprocess
import sys
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
from scenarios import find_mahimahi_traces
from utils import ZEUS_DIR

def run_test(command):
    args=shlex.split(command)
    args[0]=str(Path(__file__).resolve().parent/args[0])
    result=subprocess.run([sys.executable,*args],capture_output=True,text=True,timeout=3600)
    print(f"[{'PASS' if result.returncode==0 else 'FAIL'}] {command}")
    if result.returncode:print(result.stderr)
    return result.returncode==0

def main():
    p=argparse.ArgumentParser();p.add_argument("--dur",type=int,default=30)
    p.add_argument("--full",action="store_true");p.add_argument("-j","--jobs",type=int,default=1)
    a=p.parse_args()
    if a.dur<6 or a.jobs<1:raise ValueError("duration >= 6 seconds and jobs >= 1 required")
    commands=[f"const_test.py --dur {a.dur} --port 9700",
              f"step_test.py --dur {a.dur} --port 9750",
              f"jitter_test.py --dur {a.dur} --port 9800",
              f"bur_accuracy.py --dur {a.dur} --port 9850",
              f"jains_fairness.py --dur {a.dur} --stagger {a.dur/6} --port 9900",
              f"tcpcubic_compete.py --buf 7 --dur {a.dur} --cubic-delay 1 --port 9950 --iperf-port 9951",
              f"tcpcubic_compete.py --buf 50 --dur {a.dur} --cubic-delay 1 --port 9960 --iperf-port 9961"]
    if find_mahimahi_traces():commands.append(f"tests_mm.py --dur {a.dur} --port 9970")
    else:print("[SKIP] optional Mahimahi trace dataset unavailable")
    if any(ZEUS_DIR.glob("*.up")):commands.append("zeus_batch.py --port 9980 "+("--full" if a.full else f"--dur {a.dur}"))
    else:print("[SKIP] optional Zeus trace dataset unavailable")
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        success=list(pool.map(run_test,commands))
    return 0 if all(success) else 1

if __name__=="__main__":raise SystemExit(main())

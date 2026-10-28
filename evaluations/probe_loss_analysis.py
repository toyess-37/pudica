import argparse
import json
from metrics import load_events,probe_summary

def analyze_probes(log_path):
    result=probe_summary(load_events(log_path))
    print(json.dumps(result,indent=2))
    return result

if __name__=="__main__":
    p=argparse.ArgumentParser();p.add_argument("log_path")
    analyze_probes(p.parse_args().log_path)

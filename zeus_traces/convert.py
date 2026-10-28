import argparse
import os
import tempfile
from pathlib import Path

def convert(input_path, output_path, max_packets_per_ms=None):
    source=Path(input_path);destination=Path(output_path)
    if source.resolve()==destination.resolve():raise ValueError("input and output must differ")
    if max_packets_per_ms is not None and max_packets_per_ms<=0:raise ValueError("cap must be positive")
    clipped=0;last=-1
    descriptor,temp=tempfile.mkstemp(prefix=destination.name+".",dir=destination.parent)
    try:
        with os.fdopen(descriptor,"w") as output,open(source) as rows:
            for number,line in enumerate(rows,1):
                if not line.strip() or line.lstrip().startswith("#"):continue
                parts=line.replace(","," ").split()
                if len(parts)!=2:raise ValueError(f"line {number}: expected timestamp,count")
                timestamp,count=map(int,parts)
                if timestamp<last or timestamp<0 or count<0:
                    raise ValueError(f"line {number}: invalid timestamp/count")
                last=timestamp
                selected=count if max_packets_per_ms is None else min(count,max_packets_per_ms)
                clipped+=count-selected
                for _ in range(selected):output.write(f"{timestamp}\n")
        os.replace(temp,destination)
    finally:
        if os.path.exists(temp):os.unlink(temp)
    return clipped

if __name__=="__main__":
    parser=argparse.ArgumentParser(description="Convert Zeus timestamp,count rows to a mahimahi trace.")
    parser.add_argument("input");parser.add_argument("output")
    parser.add_argument("--max-packets-per-ms",type=int,default=None,help="optional explicit capacity cap")
    args=parser.parse_args()
    print(f"Converted; clipped packets: {convert(args.input,args.output,args.max_packets_per_ms)}")

import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def generate_readme():
    directory=ROOT/"results";files=sorted(directory.glob("current_*.json"))
    if not files:raise FileNotFoundError("no current reports found")
    lines=["# Current implementation results","Smoke tests; not paper-performance claims.",
           "Legacy reports and results/README.md are preserved.",""]
    fence=chr(96)*3
    for path in files:
        data=json.loads(path.read_text())
        lines.extend([f"## {path.name}",fence+"json",json.dumps(data,indent=2),fence,""])
    output=directory/"CURRENT_RESULTS.md";output.write_text("\n".join(lines));return output
if __name__=="__main__":print(generate_readme())

# just a quick script to generate a readme for results
import json
import os
from pathlib import Path

def generate_readme():
  results_dir = Path("results")
  if not results_dir.exists():
    print("results directory not found.")
    return

  json_files = list(results_dir.glob("*.json"))
  if not json_files:
    print("No json files found in results.")
    return

  lines = ["# Pudica Evaluation Results\n", "This file is auto-generated.\n"]
  
  for jf in sorted(json_files):
    with open(jf, "r") as f:
      try:
        data = json.load(f)
        lines.append(f"## {jf.name}\n")
        lines.append("```json")
        lines.append(json.dumps(data, indent=2))
        lines.append("```\n")
      except Exception:
        lines.append(f"couldn't load {jf.name}\n")

  readme_path = results_dir / "README.md"
  readme_path.write_text("\n".join(lines))
  print(f"Generated {readme_path}")

if __name__ == "__main__":
  generate_readme()

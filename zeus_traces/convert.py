import sys

# 300 Mbps ceiling (25 packets/ms)
MAX_PACKETS_PER_MS = 25 

def convert(input, output):
  with open(input, 'r') as inp, open(output, 'w') as out:
    for line in inp:
      parts = line.strip().split()
      if len(parts) == 2:
        timestamp = parts[0]
        num = int(parts[1])
        
        mini = min(num, MAX_PACKETS_PER_MS)
        for _ in range(mini):
          out.write(f"{timestamp}\n")

if __name__ == "__main__":
    # python3 convert.py <FILENAME>.csv <FILENAME>.up
    if len(sys.argv) != 3:
      print("Usage: python convert.py <input_zeus> <output_up>")
      sys.exit(1)
        
    convert(sys.argv[1], sys.argv[2])
    print(f"Conversion complete.")
import matplotlib.pyplot as plt
import argparse
import sys

def parse_log(log_file):
  burs = []
  bitrates = []
  delays = []

  try:
    with open(log_file, 'r') as f:
      for line in f:
        if "BUR:" in line and "bitrate:" in line and "delay:" in line:
          parts = line.split()
          
          try:
            b_idx = parts.index("BUR:") + 1
            br_idx = parts.index("bitrate:") + 1
            d_idx = parts.index("delay:") + 1
            
            b_val = float(parts[b_idx])
            br_val = float(parts[br_idx])

            d_str = parts[d_idx].replace("ms", "") 
            d_val = float(d_str)
            
            burs.append(b_val)
            bitrates.append(br_val)
            delays.append(d_val)
                
          except (ValueError, IndexError):
            continue
                      
  except FileNotFoundError:
    print(f"[!] Error: Could not find log file '{log_file}'")
    sys.exit(1)

  return burs, bitrates, delays

def plot_measurements(burs, bitrates, delays, title_suffix=""):
  if not burs:
    print("[!] No valid data found in the log file.")
    return

  time_ms = [i * 16.67 for i in range(len(burs))]

  fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(10, 8), sharex=True)
  fig.suptitle(f'Pudica Algorithm Convergence {title_suffix}', fontsize=14, fontweight='bold')

  # --- Plot 1: Frame Bitrate ---
  ax1.plot(time_ms, bitrates, color='#d62728', linewidth=1.5, label='Frame Bitrate')
  ax1.set_ylabel('Bitrate (Mbps)', fontweight='bold')
  ax1.grid(True, linestyle='--', alpha=0.6)
  ax1.legend(loc='upper right')

  # --- Plot 2: Frame Delay ---
  # The paper plots delay on a logarithmic scale to highlight tail latency spikes
  ax2.plot(time_ms, delays, color='#2ca02c', linewidth=1.5, label='One-Way Delay')
  ax2.set_ylabel('Delay (ms)', fontweight='bold')
  # ax2.set_yscale('log')
  ax2.grid(True, linestyle='--', alpha=0.6)
  ax2.legend(loc='upper right')

  # --- Plot 3: Bandwidth Utilization Ratio (BUR) ---
  ax3.plot(time_ms, burs, color='#1f77b4', alpha=0.8, linewidth=1.5, label='Estimated BUR')
  ax3.axhline(y=1.0, color='black', linestyle='--', linewidth=2, label='Bottleneck Limit (BUR=1.0)')
  ax3.axhline(y=0.85, color='gray', linestyle=':', linewidth=1.5, label='Alpha Threshold (0.85)')
  ax3.set_xlabel('Timeline (ms)', fontweight='bold')
  ax3.set_ylabel('BUR', fontweight='bold')
  ax3.grid(True, linestyle='--', alpha=0.6)
  ax3.legend(loc='upper right')

  plt.tight_layout()
  
  # Save and display
  output_filename = 'pudica_measurement.pdf'
  plt.savefig(output_filename, dpi='figure', format='pdf')
  print(f"[*] Successfully generated plot: {output_filename}")
  plt.show()

if __name__ == "__main__":
  parser = argparse.ArgumentParser(description="Plot Pudica Network Metrics")
  parser.add_argument("--log", type=str, default="test_log.txt", help="Path to the sender log file")
  parser.add_argument("--title", type=str, default="(Mahimahi Emulation)", help="Suffix for the graph title")
  args = parser.parse_args()

  print(f"[*] Parsing log file: {args.log}...")
  burs, bitrates, delays = parse_log(args.log)
  
  print(f"[*] Found {len(burs)} valid frame records.")
  plot_measurements(burs, bitrates, delays, args.title)
PKT_BITS = 1400 * 8

def const_trace(fname, bw, dur):
  """Constant bandwidth trace"""
  ms_pkt = 1000 / ((bw * 1e6) / PKT_BITS)
  
  with open(fname, 'w') as f:
    t = 0.0
    while t < dur * 1000:
      f.write(f"{int(t)}\n")
      t += ms_pkt
  print(f"Saved: {fname}")

def step_trace(fname, bw1, bw2, t_swap, dur):
  """Bandwidth drop/spike trace"""
  with open(fname, 'w') as f:
    t = 0.0
    
    ms_pkt1 = 1000 / ((bw1 * 1e6) / PKT_BITS)
    while t < t_swap * 1000:
      f.write(f"{int(t)}\n")
      t += ms_pkt1
        
    ms_pkt2 = 1000 / ((bw2 * 1e6) / PKT_BITS)
    while t < dur * 1000:
      f.write(f"{int(t)}\n")
      t += ms_pkt2
  print(f"Saved: {fname}")

if __name__ == "__main__":
  const_trace("const_20m.up", 20, 30)
  step_trace("step_30_10m.up", 30, 10, 15, 30)
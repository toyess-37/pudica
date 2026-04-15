import argparse, subprocess, time
from pathlib import Path
import tempfile
from utils import (
  ZEUS_DIR, RECEIVER_BIN, cleanup, parse_log, summarise, save, plot_single,
  make_script, sender_cmd
)

def run_zeus_trace(args):
  trace_path = Path(f"{ZEUS_DIR}/{args.trace}").resolve()
  if not trace_path.exists():
    print(f"[!] Trace file not found: {trace_path}")
    return

  print(f"[*] Running Zeus trace: {trace_path.name} | dur={args.dur}s | rtt={args.rtt}ms")

  with tempfile.TemporaryDirectory() as tmp:
    tmp_path = Path(tmp)
    send_lf = tmp_path / "send.log"
    procs = []
    try:
      procs.append(subprocess.Popen(
        [RECEIVER_BIN, str(args.port)], 
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
      ))
      time.sleep(0.5)
      
      script = make_script(tmp_path, [sender_cmd(args.port, args.dur, send_lf)])
      mm_cmd = f"mm-delay {args.rtt // 2} mm-link {trace_path} {trace_path} -- {script}"
      procs.append(subprocess.Popen(mm_cmd, shell=True))
      
      time.sleep(args.dur + 3)
    finally:
      cleanup(procs)

    burs, bitrates, delays = parse_log(send_lf.read_text() if send_lf.exists() else "")

  if not burs:
    print("[!] Empty log. Did the sender run successfully?")
    return

  test_name = trace_path.stem
  s = summarise(burs, bitrates, delays, label=f"zeus_{test_name}")
  print(f"avg_br={s['avg_bitrate']} Mbps  avg_delay={s['avg_delay']} ms  stall={s['stall_100ms']*100:.3f}%")
  
  out = save({"test": "zeus_trace", "trace": trace_path.name, "summary": s}, f"zeus_{test_name}")

  if args.plot:
    plot_single(burs, bitrates, delays, title=f"Pudica over 5G Zeus Trace: {test_name}", out_pdf=str(out).replace(".json", ".pdf"))

if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--trace", type=str, required=True, help="Path to the converted .up trace file")
  parser.add_argument("--dur",   type=int, default=30, help="Duration of the test in seconds")
  parser.add_argument("--rtt",   type=int, default=20, help="Base RTT in ms")
  parser.add_argument("--port",  type=int, default=9800, help="Receiver port")
  parser.add_argument("--plot",  action="store_true", help="Generate bandwidth/delay/BUR plots")
  run_zeus_trace(parser.parse_args())
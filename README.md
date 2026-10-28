# pudica

An unofficial, educational reproduction of Pudica, the congestion control algorithm for cloud gaming described in:

> Wang et al., [*Pudica: Toward Near-Zero Queuing Delay in Congestion Control for Cloud Gaming*](https://www.usenix.org/conference/nsdi24/presentation/wang-shibo), USENIX NSDI 2024.

The official code was never released, so the controller here is rebuilt from the paper's equations and text. Some loss handling comes from a later paper by the same group:

> Wang et al., [*From Source to Solution: Tackling Packet Losses in Large-scale Cloud Gaming Systematically and Precisely*](https://www.usenix.org/conference/nsdi26/presentation/wang-jing) (LADR), USENIX NSDI 2026.

From LADR we use the loss-based rate ceiling for shallow buffers, the reactive rate limit and the pending-data limit. Loss recovery is simpler than in LADR: fixed XOR parity on every frame and timer-based retransmission instead of opportunistic FEC and RACK-TLP.

The project streams the sender's screen as H.264 over UDP to a receiver that decodes and shows it. It does not carry game input or audio.

## Repository layout

| Path | Contents |
|---|---|
| `codes/pudica_algo.h`, `codes/pudica_algo.cc` | The controller: BUR estimation, pacing, rate updates, fallback and draining, loss ceiling |
| `codes/sender.cc` | Capture, encode, pacing, probes, FEC, retransmission, feedback processing |
| `codes/receiver.cc` | Reassembly, FEC recovery, ACKs, keyframe requests, decode and display |
| `codes/protocol.h` | Packet and ACK formats with explicit big-endian encoding |
| `codes/transport.h` | Per-frame bookkeeping on both sides (sent frames, received frames, FEC recovery) |
| `codes/logger.h`, `codes/logger.cc` | JSON-lines event log written by the sender |
| `codes/test_controller.cc`, `codes/test_transport.cc` | C++ unit tests |
| `cloud_gaming/capture/` | X11 screen capture and the libx264 encoder |
| `cloud_gaming/playback/` | H.264 decoder and SDL2 window |
| `evaluations/` | Emulated experiments under mahimahi and the metrics code |
| `realnet/` | The same experiments across two real Linux machines (see `realnet/GUIDE.md`) |
| `tests/` | Python unit tests and localhost transport integration tests |
| `traces/`, `zeus_traces/` | Link traces for mahimahi |

[TECHNICAL_DOCUMENTATION.md](TECHNICAL_DOCUMENTATION.md) explains how every part works.

## Building

On Debian or Ubuntu:

```bash
sudo apt install -y build-essential libx11-dev libxext-dev libavcodec-dev libavutil-dev libswscale-dev libsdl2-dev python3-numpy python3-matplotlib mahimahi iperf3 xvfb x11-utils ffmpeg
```

Mahimahi needs IP forwarding:

```bash
sudo sysctl -w net.ipv4.ip_forward=1
```

Then build both programs:

```bash
make -C codes
```

The encoder needs FFmpeg built with libx264, which the Debian and Ubuntu packages are.

## Tests

```bash
make -C codes test
```

This runs the controller and transport unit tests and the Python unit tests.

```bash
make -C codes integration
```

This starts a sender and receiver on localhost behind a small UDP relay that drops, delays or holds packets. It checks FEC recovery, retransmission, lost probe ACKs, ACK blackouts and dead links, and decodes real H.264 at the end. It needs Xvfb and `xdpyinfo`.

## Running

Start the receiver first:

```bash
./codes/receiver 9700
```

Then the sender, which captures the X display named in `DISPLAY`:

```bash
./codes/sender --log sender.jsonl 192.168.1.50 9700 30
```

The arguments are the receiver's IPv4 address, its port and the run length in seconds. Other sender options are `--bmin` and `--bmax` (bitrate limits in Mbps, default 0.2 and 50) and `--synthetic`, which sends fixed-size dummy frames instead of video. While it runs, typing `q` stops the sender, `s` prints its state, and `l`, `r` and `d` force a loss backoff, clear the rate history or start a drain.

Receiver modes:

- no option: decode and show the video in an SDL window
- `--decode-only`: decode without a window
- `--headless`: count frames without decoding, for synthetic runs

For a quick test without video:

```bash
./codes/receiver 9700 --headless
./codes/sender --synthetic --log sender.jsonl 127.0.0.1 9700 10
```

Sender and receiver must be built from the same version: packets carry a magic number and version, and mismatched packets are dropped.

## Experiments

Each script in `evaluations/` builds a link trace, runs sender and receiver inside mahimahi, and writes a JSON report to `results/`:

| Script | What it measures |
|---|---|
| `const_test.py` | Constant capacity |
| `step_test.py` | Capacity steps down and back up, and how long the rate takes to follow |
| `jitter_test.py` | Periodic gaps in link service |
| `bur_accuracy.py` | Estimated BUR against the known offered load |
| `jains_fairness.py`, `convergence_test.py` | Several staggered flows sharing a link |
| `different_rtprop_test.py` | Two flows with different RTTs on one bottleneck |
| `tcpcubic_compete.py` | One flow against TCP CUBIC (iperf3), with a shallow and a deep buffer |
| `zeus_batch.py`, `zeus_tests.py` | Recorded 5G traces |
| `run_all.py` | All of the above in sequence |

All reported rates are delivered video payload unless a field says otherwise. Convergence targets are converted from link capacity to the payload the link can carry, which is about 74% of capacity at 20 Mbps once parity, headers and probes are counted.

`python3 evaluations/sift.py --log sender.jsonl` checks a sender log for consistency: every sent frame has an outcome and a probe summary, and every drain follows three congested samples.

To run on real machines instead of an emulator, see [realnet/GUIDE.md](realnet/GUIDE.md).

## Known limitations

- The BUR estimate tends to sit near 1 when the link is only 50 to 65% used. Pacing feeds back into the delay it measures, which multiplies any small constant delay about five times, including the probe term. This is the main reason throughput stays below capacity in the emulator. Part of the probe term is probably caused by mahimahi's 1 ms delivery granularity.
- Every frame carries 25% XOR parity, so payload throughput is at most about 74% of the link even when nothing is lost.
- On long paths the rate climbs slowly, because each increase waits for feedback from a frame encoded at the new rate, as the paper specifies.
- The frame rate is 35 fps, where the paper used 60.
- There is no encryption or authentication. The 64-bit session number only separates runs.

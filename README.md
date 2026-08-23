# pudica
An educational (and unofficial) reproduction of Pudica, a custom UDP-based congestion control algorithm for cloud gaming presented at USENIX NSDI 2024.

## NOTE
The official source code for Pudica (NSDI '24) has not been released publicly.
This implementation follows the paper:

> [*Pudica: Toward Near-Zero Queuing Delay in Congestion Control for Cloud Gaming*](https://www.usenix.org/conference/nsdi24/presentation/wang-shibo)
> 
> Wang et al., USENIX NSDI 2024.

Loss recovery is a loose, unofficial take on ideas from a 2026 NSDI submission on LADR (Loss-Aware Delay-based Rate control), not a full implementation. Recovered packets go through simple XOR-based FEC (fixed group size, single-loss correction) plus packet-level retransmission for whatever FEC can't fix; the retrans-loss signal is fed back into Pudica's congestion detector but implementation is still not so smooth yet.

## Files
- `pudica_algo.cc` and `pudica_algo.h`: core Pudica control algorithm.
- `sender.cc`: UDP sender with pacing, retransmission tracking, and a capture/encode loop feeding the pacer.
- `receiver.cc`: UDP receiver that acks packets (echoing time and receive-rate), recovers losses via FEC, and drives playback.
- `protocol.h`: shared packet and ACK structures.
- `logger.h` / `logger.cc`: JSON-line logging used by the sender for offline analysis.
- `cloud_gaming/capture/`: X11 screen capture (`capture.h`) and H.264 encoding (`encoder.h`), with live bitrate adjustment by the controller.
- `cloud_gaming/playback/`: H.264 decode (`decoder.h`) and SDL2-based rendering (`renderer.h`) on the receiver side.

## Build
Run:
```bash
cd codes
make
```
This builds the sender and receiver executable files in the codes directory.

## Usage
Start receiver first, then sender.

Receiver:
```bash
./receiver <port>
```

Sender:
```bash
./sender <target_ip> <port> <duration_sec>
```

TODO:
1. Make separate function-wise detailed implementation document.
2. ~~Make it so that sender runs first and receiver can hop in anytime.~~ done - sender periodically broadcasts stream info so a late-joining receiver can sync.
3. ~~Integrate FFMPEG and simulate a video game from one pc to another.~~ done - capture/encode on the sender, decode/render on the receiver.
4. Packet-level retransmission and FEC-aware acking (loosely LADR-style, NSDI 2026) are in; the shallow-congestion / retrans-loss coupling in `pudica_algo.cc` is still not good enough.
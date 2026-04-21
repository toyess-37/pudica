# pudica
An educational (and unofficial) reproduction of Pudica, a custom UDP-based congestion control algorithm for cloud gaming presented at USENIX NSDI 2024.

## NOTE
The official source code for Pudica (NSDI '24) has not been released publicly.
This implementation follows the paper:

> [*Pudica: Toward Near-Zero Queuing Delay in Congestion Control for Cloud Gaming*](https://www.usenix.org/conference/nsdi24/presentation/wang-shibo)
> 
> Wang et al., USENIX NSDI 2024.

## Files
- `pudica_algo.cc` and `pudica_algo.h`: core Pudica control algorithm.
- `sender.cc`: UDP sender with pacing and congestion control logic.
- `receiver.cc`: UDP receiver with ACK echoing and receive-rate calculation.
- `protocol.h`: shared packet and ACK structures.

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
./sender <ip> <port> <duration_sec>
```

TODO:
1. Complete the report/analysis.

2. Make it so that sender runs first and receiver can hop in anytime.
3. Integrate FFMPEG and simulate a video game from one pc to another. 
# Pudica: technical documentation

This document explains how the project works, from the packet format to the controller's equations and the experiment tooling. It describes the code as it is now. Where the code differs from the Pudica paper (NSDI 2024) or the LADR paper (NSDI 2026), the difference is stated where it occurs, and section 12 lists all of them.

Equation numbers such as "Eq. 5" refer to the Pudica paper unless LADR is named.

## Contents

1. Overview
2. Programs and threads
3. Wire protocol
4. The life of one frame
5. Clocks and delay measurement
6. The congestion controller
7. The sender in detail
8. The receiver in detail
9. Video capture, encoding and playback
10. The event log
11. Experiments and metrics
12. Differences from the papers
13. Known limitations
14. Constants

## 1. Overview

Cloud gaming sends one video frame every frame interval, and each frame has to reach the player quickly. A conventional congestion controller fills the bottleneck queue to find the available bandwidth, and that queue adds delay to every frame. Pudica avoids building a queue. Instead of measuring bandwidth directly, it measures the bandwidth utilization ratio (BUR): the fraction of each frame interval during which the bottleneck link was busy sending this flow's frame. A BUR below 1 means the link had idle time; a BUR above 1 means data from this frame was still queued when the next frame should have started.

The project has two programs. `sender` captures the screen, encodes it with H.264, splits each frame into UDP packets, adds parity and probe packets, and paces them out. `receiver` reassembles frames, repairs single losses with parity, acknowledges every packet, and decodes and shows the video. The acknowledgements carry the receive times the controller needs to compute BUR.

The controller in `codes/pudica_algo.cc` has no networking code. The sender gives it one summary per delivered frame (a `FrameAck`) and asks it for the bitrate to use for the next encoded frame. Because of this split the controller can be unit tested on its own.

## 2. Programs and threads

### Sender

The sender has one UDP socket, connected to the receiver, and four threads:

| Thread | Function | Job |
|---|---|---|
| capture | `Sender::capture` | Every frame interval: decide whether to skip, ask the controller for a target bitrate, capture and encode a frame (or build a dummy frame in synthetic mode), and queue it |
| pacer | `Sender::pacer` | Take the next queued frame, send its data and parity packets at the paced rate, send four probes, and send queued retransmissions first |
| listener | `Sender::listener` | Read ACKs, update RTT and one-way delay estimates, mark packets and frames as acknowledged, run retransmission timers, and pass completed frames to the controller |
| keyboard | `Sender::keyboard` | Read single keys from standard input for manual testing |

All shared state is protected by one mutex, `Sender::mutex`. The controller is only ever called with that mutex held, so it needs no locking of its own. The condition variable `available` wakes the pacer when a frame or a retransmission is ready.

An exception in any thread is caught by `guarded`, which logs an `ERROR` event, stops all threads and makes `run` throw once everything has joined.

### Receiver

The receiver is a single loop in `main`. Each pass receives at most one datagram (with a 10 ms timeout), processes it, sends ACKs, delivers any frames that are ready in order, and polls the SDL window. Decoding and rendering happen in the same loop, which is why packet arrival times are taken from kernel timestamps rather than from the time the loop gets to them (section 5).

## 3. Wire protocol

All integers are big-endian and written field by field by `put` and read by `get` in `codes/protocol.h`. There are no packed structs, so the layout does not depend on the compiler or the CPU.

### Data, parity, probe and stream-info packets

Every packet starts with a 46-byte header:

| Bytes | Field | Meaning |
|---|---|---|
| 0-3 | magic | `0x50554432` ("PUD2") |
| 4 | version | 2 |
| 5 | flags | see below |
| 6-7 | retrans_seq | 0 for the first transmission, then 1, 2, ... for retransmissions |
| 8-15 | session | random per sender run; never 0 |
| 16-23 | send_time | sender clock in microseconds when this packet was sent |
| 24-31 | frame_time | sender clock when the frame's first packet was sent; identifies the frame together with its size |
| 32-35 | frame_id | starts at 1 and increases by one per sent frame |
| 36-39 | packet_id | index of the packet within the frame, or probe index 0-3 |
| 40-43 | frame_bytes | size of the encoded frame |
| 44-45 | fec_group | parity group, `packet_id / 4` for data packets |

Flags:

| Value | Name | Used on |
|---|---|---|
| 1 | FIRST | first data packet of a frame, and its ACK |
| 2 | LAST | last data packet of a frame, and its ACK |
| 4 | PROBE | probe packets and their ACKs |
| 8 | FEC | parity packets |
| 16 | RECOVERED | ACK for a packet the receiver rebuilt from parity |
| 32 | STREAM_INFO | video size announcement (sender to receiver); `KEY_REQUEST` in the other direction |
| 64 | FRAME_COMPLETE | set on every ACK sent after the frame is complete |
| 128 | KEYFRAME | data packets of a keyframe, and their ACKs |

The packet types are:

- **Data**: header plus 1200 bytes of frame data. The last packet of a frame is padded with zeros to the full 1246 bytes, so every data packet has the same size.
- **Parity**: header plus the XOR of up to four data payloads in the same group. A group containing a single packet gets no parity, because XOR of one packet is just a copy. The `packet_id` field is 0 and `fec_group` names the group.
- **Probe**: header only, 46 bytes, `packet_id` 0 to 3.
- **Stream info**: header plus width and height as 32-bit integers. Sent once a second in video mode so a receiver that starts late can create its decoder.

`decode` rejects anything inconsistent: wrong magic or version, session 0, timestamps above `INT64_MAX`, frame sizes of 0 or above 4 MiB, a FIRST or LAST flag that does not match the packet's position, a parity group that does not exist or has only one packet, or a packet of the wrong length.

### ACKs

ACKs are 56 bytes:

| Bytes | Field | Meaning |
|---|---|---|
| 0-3 | magic | as above |
| 4 | version | 2 |
| 5 | flags | flags of the packet being acknowledged, plus RECOVERED and FRAME_COMPLETE |
| 6-7 | retrans_seq | copied from the packet |
| 8-15 | session | copied from the packet |
| 16-23 | echoed_send | the packet's `send_time`; 0 for RECOVERED ACKs |
| 24-31 | recv_time | receiver clock when the packet arrived |
| 32-39 | frame_recv_time | receiver clock when the frame became complete, or 0 |
| 40-47 | wire_bytes | total UDP payload bytes the receiver has received in this session |
| 48-51 | frame_id | |
| 52-55 | packet_id | |

The receiver sends one ACK per data packet and per probe. It sends nothing for parity packets, but if a parity packet completes a group it sends a RECOVERED ACK for the rebuilt packet.

A `KEY_REQUEST` ACK has flags exactly 32, a frame id, a receive time and every other field zero. It asks the sender for a keyframe.

Both sides must run the same protocol version. A version 1 peer's packets fail the magic check and are dropped.

## 4. The life of one frame

This section follows one frame from capture to the controller update. Later sections give the details.

1. **Capture.** At the start of each frame interval the capture thread checks the backlog. It adds up the wire bytes of frames waiting for the pacer and of packets not yet sent. If sending them at the current rate would take more than two frame intervals, or three frames are already waiting, it skips this frame and logs `FRAME_SKIPPED`. This is LADR's pending-data limit.
2. **Rate decision.** Otherwise it calls `take_encoding_bitrate()` to get the target bitrate, and `encoding_epoch()` to tag the frame with the controller's current rate epoch (section 6.7).
3. **Encode.** In video mode it captures the screen and encodes one frame at that target. In synthetic mode it builds a frame of exactly `target × interval / 8` bytes.
4. **Queue.** The encoded frame goes into `frames` with its target, keyframe flag, epoch and capture time.
5. **Pacing.** The pacer takes the frame, assigns the next frame id, and computes the pacing rate from the controller's pacing multiplier ρ (section 6.3). It sends the data packets, inserting each group's parity packet after the group's last data packet. Then it sends four probes spread over the rest of the frame interval.
6. **Reception.** For each data packet or probe, the receiver records the arrival time and sends an ACK. When the last missing data packet arrives, or parity rebuilds it, the frame is complete and its completion time goes into every later ACK for that frame.
7. **Feedback.** The listener processes ACKs. Once the frame is complete and all four probe ACKs have arrived (or the probe wait has run out, section 7.5), `resolve` builds a `FrameAck` and calls `on_frame_acked`.
8. **Controller.** The controller computes this frame's BUR, updates pacing, the state machine and the bitrate, and returns them. The next encoded frame uses the new bitrate.

## 5. Clocks and delay measurement

Each program uses its own `steady_clock`. The two clocks are not synchronized and their difference is arbitrary, but it is constant (apart from slow drift), so it cancels whenever two one-way delays are subtracted. This is how the paper avoids clock synchronization, and it is why the controller only uses delay differences such as `D − Dmin`.

**Arrival times.** The receiver turns on `SO_TIMESTAMPNS`, so the kernel records when each packet reached the socket. That stamp is on the real-time clock, so the receiver converts it: it subtracts the time the packet spent waiting in the socket buffer (the real-time clock now minus the kernel stamp) from the steady clock now. Without this, every packet that arrived while the receiver was decoding or rendering would look delayed by the decode time, and BUR would be too high.

**One-way delay (OWD)** of a packet is `recv_time − send_time`, mixing the two clocks. Only differences between OWDs mean anything.

**Dmin** is the smallest OWD seen in the last 10 seconds, kept with a monotonic deque: new samples remove larger ones from the back, old samples leave from the front, and the front is always the minimum. RECOVERED ACKs are skipped because they have no send time.

**Frame delay D** is the time from when the sender started the frame (`first_sent`) to when the receiver completed it (`frame_recv_time`). `D − Dmin` is how much longer this frame took than an ideal single packet. It includes the pacing time for the frame, which matters for section 6.3.

**RTT** is measured from every ACK with a send time as `now − echoed_send`, on the sender's clock only. Retransmissions carry their own send time, so there is no ambiguity about which transmission an ACK belongs to. The sender keeps:

- a smoothed RTT and variance (`srtt_us`, `rttvar_us`) in the usual TCP way, with gains of 1/8 and 1/4;
- the retransmission timeout, `max(20 ms, srtt + 4 × rttvar)`;
- `min_rtt`, the smallest RTT in the last 10 seconds, kept the same way as Dmin.

**Receive rate.** Every ACK carries the receiver's running byte total. The sender keeps those totals for roughly the last 200 ms and divides the byte difference by the time difference to get `ack_rate` in Mbps. That counts everything the receiver saw: headers, padding, parity and probes. Before passing it to the controller, `resolve` scales it by the frame's payload bytes over the frame's total UDP bytes (`frame_udp_bytes` in `protocol.h`), so it is in the same units as the encoder's bitrate. Mixing the two units made earlier versions restore the bitrate about 30% too high after every drain.

## 6. The congestion controller

`PudicaAlgorithm::Controller` holds the bitrate, the pacing multiplier, the BUR history and the state machine. It is driven by these calls:

| Call | When |
|---|---|
| `on_frame_acked(FrameAck)` | once per delivered frame, in frame order |
| `take_encoding_bitrate()` | once per encoded frame |
| `on_inflight_age(age, base_rtt)` | on every listener pass |
| `on_frame_loss(now, min_gap)` | when a frame times out undelivered |
| `on_retrans_loss_detected()` | the first time a retransmitted packet is lost again |

The bitrate the encoder should use is `get_final_bitrate()`, the smaller of the controller's own bitrate and the loss ceiling (section 6.10).

### 6.1 BUR of one frame

Eq. 1 gives the raw BUR:

```
R = (D − Dmin) / L
```

where L is the frame interval (1/35 s here). `raw_BUR` clamps negative values to 0.

### 6.2 Probe correction

When a frame is paced slowly, there is a quiet period at the end of each interval in which the receiver learns nothing. Another flow could be using the link then without this flow noticing. The four probes are sent during that period to catch such traffic.

For probe i, the code computes three delays and keeps the smallest (Eq. 4):

- `D_i − Dmin`: the probe's own queueing delay;
- `H_i`: the time from the frame's completion to the probe's arrival, which removes delay caused by this frame's own data still in the queue;
- `T_packet`: the spacing between probes, so that a long queue is not counted once per probe.

The corrected BUR adds the probes' delays (Eq. 5):

```
R_corrected = R + Σ T_i / L
```

A probe whose ACK never arrived is counted as `T_packet`, the largest value Eq. 4 allows. Lost probes are most likely under congestion, so this is the cautious choice.

### 6.3 Pacing

The pacing multiplier is Eq. 2:

```
ρ = γ_p / min(R, 1),   γ_p = 1.25
```

`pacing_multiplier` also floors R at 0.01, so ρ is at most 125. The pacer sends a target-sized frame in `L / ρ`, the "sensible period", which is slightly shorter than the time the frame occupied the bottleneck last time. The idea is to keep a small packet-level queue at the bottleneck for the whole frame, so the receiver sees the link busy, without building a frame-level queue.

In code, the pacing rate is computed from the target bitrate, not the frame's actual size: `wire_rate` is the wire size of a target-sized frame (data, parity and headers) divided by the sensible period. A larger frame, such as a keyframe, therefore takes proportionally longer instead of bursting at a multiple of the link rate. This follows the paper's definition of pacing as ρ times the bitrate.

### 6.4 Smoothed BUR

The rate update uses a weighted average of the samples from the last 200 ms (Eq. 6 and Appendix B):

```
R̃ = Σ ω_k × R_k × B / B_k
```

B is the current bitrate and B_k is the bitrate of sample k's frame. The ratio B/B_k rescales older samples to the current rate: a frame half as big as the current target would have half the BUR. B_k is the frame's actual encoded bitrate, its size × 8 / L. With a real encoder, frame sizes vary widely (in one recorded session the median frame was 17% of the target and the largest was 6.9 times the target), so using the target instead would make the flow look idle and the controller would keep raising the rate.

Each weight is the product of three terms, normalized to sum to 1:

| Term | Formula | Effect |
|---|---|---|
| ω_I | min(R_k + 1, 2) | frames that saw more delay count more |
| ω_II | min(B_k + 10, 50) | larger frames count more, since their delay is less affected by jitter |
| ω_III | k + 20 | newer samples count more (k = 1 for the oldest) |

### 6.5 Multiplicative increase

When R̃ ≤ α (0.85), `next_bitrate` applies Eq. 7 and 8:

```
B_new = B × (1 + ξ),   ξ = γ_MI × ((1 + α)/2 − R̃) / R̃,   γ_MI = 0.3
```

The increase is large when R̃ is small and shrinks to zero as R̃ approaches (1 + α)/2 = 0.925. R̃ is floored at 0.01, so from a nearly idle link the first step can multiply the rate by about 28.

### 6.6 Additive increase with multiplicative decrease

When α < R̃ ≤ 1, Eq. 9 and 10 apply:

```
B_new = B + A,   A = I − γ_MD × B,   γ_MD = 0.05
I = (B_max + 2^τ / log B) × γ_MD / 2
```

The superscript in the paper is τ, so the linear part grows exponentially with τ. τ is the number of uncongested frames since the last reset. The code caps the exponent at 50 and floors log B at 0.2, because log B is negative below 1 Mbps. A is bounded to the range [−1 Mbps, 0.05 × B]. The paper says A is bounded but gives no values; with 2^τ, the upper bound is reached within a few frames, so in practice this phase raises the rate by 5% per step until something resets τ.

When R̃ > 1, `next_bitrate` leaves the rate unchanged; the state machine handles congestion.

τ is reset to zero:

- when the smoothed BUR exceeds 1, as in the paper;
- every five seconds, on five-second boundaries of the wall clock (`system_clock`). The paper says the periodic reset should be "highly synchronized", and using the wall clock means flows on machines with synchronized clocks reset together;
- when a drain ends or a frame is lost.

### 6.7 Waiting for feedback before the next step

The paper postpones each rate increase "until the feedback regarding the current adjustment is received". The controller does this with epochs. Every rate change (an increase, the start or end of a drain, a loss backoff) increments `rate_epoch` and sets `adj_epoch` to the new value. The capture thread tags each frame with the epoch it was encoded in. A new increase is allowed only when the sample being processed comes from a frame with `epoch >= adj_epoch`, that is, a frame encoded after the last change.

An earlier version used "frame id plus the number of undelivered frames", which ignored frames already encoded but still waiting for the pacer, so it could step again before seeing the effect of the previous step. The epoch rule is stricter and makes the rate climb more slowly on long paths: one step per round trip plus a frame or two.

### 6.8 States: fallback, waiting and draining

The controller has four states: `STEADY`, `FALLBACK`, `CONGESTED_WAIT` and `DRAINING`. They follow the paper's section 4.3.

**Temporary fallback.** The first frame with R > 1 while in `STEADY` saves the current bitrate, lowers it by ζ = 15%, and enters `FALLBACK`. The lower rate is used for exactly one encoded frame: `take_encoding_bitrate` returns the lowered value and then restores the saved one. If the next sample has R ≤ 1, the state returns to `STEADY`.

**Next delay.** The same fallback is triggered by `on_inflight_age` when the oldest undelivered frame has been out for longer than `min_rtt + 2L`. This reacts to sudden congestion before any feedback arrives. The paper does not give a threshold. An earlier version used a fixed 2L (57 ms), which fired on every frame on paths with RTT above about 60 ms, even with no congestion.

**Waiting.** A second consecutive sample with R > 1 moves to `CONGESTED_WAIT`, which changes nothing but records the step.

**Draining.** A third consecutive sample with R > 1 starts a drain (Eq. 11):

```
B_new = α × receiving_rate − draining_rate
```

`receiving_rate` is the average of the payload receive rates reported since the first of the congested samples, which is the paper's "average data receiving rate from the onset of congestion". `draining_rate` is the extra rate needed to empty the queue in 200 ms: 8 × queued bytes / 0.2 s.

The paper measures the queue as the number of in-flight packets. On a long path most in-flight bytes are travelling, not queued, so the code subtracts the bandwidth-delay product first:

```
queued_bytes = max(0, in_flight_bytes − receiving_rate × min_rtt / 8)
```

Without this, a 120 ms path drained to as little as 0.2 Mbps.

**End of drain.** The first sample with R ≤ 1 while draining returns to `STEADY`, sets the bitrate to the current payload receive rate (the paper's "one-step recovery"), resets τ and clears the history.

A gap in frame ids between samples, which happens when a frame times out, resets the congested-sample count.

### 6.9 Frame loss backoff

When a frame times out without being delivered, `on_frame_loss` halves the bitrate and enters `DRAINING`. This is not in the paper; it is a safety net for outages. Frames that expire within `max(srtt, 2L)` of the previous backoff are treated as the same loss event and do not halve the rate again.

### 6.10 Loss ceiling for shallow buffers (LADR)

With a shallow bottleneck buffer, packets are dropped before any delay builds up, so a delay-based controller never sees congestion. LADR adds a loss-based ceiling that runs next to Pudica. The encoder uses the smaller of the two rates.

A frame is "lossy" if any of its packets needed a retransmission or parity recovery. The ceiling is set when all of these hold for three consecutive lossy frames (LADR section 3.2):

- the first of them had `D − Dmin` below one frame interval, meaning there was little queueing, so a delay-based controller would not react;
- their ACK rate is below their average bitrate, meaning the network delivered less than was sent.

The ACK rate is the payload bytes of packets that arrived on their first attempt, divided by the sum of each frame's own arrival span (first to last such packet). LADR measures one span across all three frames, but with paced frames that span includes the quiet gap between frames and any skipped frame, which pulls the rate down to about the bitrate and makes random loss trigger the ceiling. Per-frame spans give a clear margin: a paced, uncongested frame arrives at about 1.25 times its bitrate, while a congested one arrives at the bottleneck rate. If no packet arrived, the ACK rate is zero, as LADR specifies.

When the ceiling is set, the code records B_agg (the frames' average bitrate) and B_safe = max(0.2 × ACK rate, B_MIN), using LADR's β = 0.8. The ceiling then follows a cubic curve:

```
ceiling(t) = C × (t − K)^3 + B_agg,   K = cbrt((B_agg − B_safe) / C),   C = 1
```

with t in seconds since the ceiling was set. It starts at B_safe, flattens near B_agg after K seconds, then grows. It is removed when the BUR is at most α and the ceiling has reached 95% of the controller's bitrate.

### 6.11 Reactive rate limit (LADR)

LADR section 3.3 limits the pacing rate to the ACK rate while many packets are waiting to be recovered. It only applies to a session after a retransmitted packet has been lost, which `on_retrans_loss_detected` records and the sender's `retrans_loss_seen` flag remembers for the rest of the session. After that, the limit turns on when at least four packets are waiting for retransmission and off when fewer than two are waiting and the retry queue is empty. While it is on, new packets, retransmissions and the capture thread's backlog estimate all use the ACK rate.

## 7. The sender in detail

### 7.1 Start-up and options

`main` parses `--log`, `--bmin`, `--bmax` and `--synthetic`, then the receiver's IPv4 address, port and duration. The session id is a random 32-bit value shifted left, XORed with the clock. The socket is connected, so `send` and `recv` need no address, and it has a 5 ms receive timeout so the listener runs its timers regularly.

A connected UDP socket reports ICMP "port unreachable" errors as `ECONNREFUSED` on the next call. The sender ignores those, and `ENOBUFS` on send, so starting the receiver late or restarting it does not kill the sender.

### 7.2 Capture loop

The loop runs on absolute deadlines that advance by one frame interval, and it resets the deadline if it falls more than one interval behind. Each pass applies the backlog check from section 4, gets the target and epoch, and, in video mode, sets the encoder bitrate and forces a keyframe if one was requested.

### 7.3 Pacer

For each frame the pacer:

1. sends any queued retransmissions first;
2. sends stream info once a second in video mode;
3. records the frame in `inflight` with its id, start time, size, target, epoch and capture time;
4. computes the sensible period `L / ρ`, the pacing rate, and the probe spacing `(L − L/ρ) / 5`;
5. sends the data packets, accumulating parity as it goes and sending each group's parity after its last data packet, with retransmissions allowed to jump in between packets;
6. sends the four probes, each after one probe spacing;
7. waits until the end of the frame interval, unless another frame is already waiting, in which case it starts that frame immediately.

Pacing uses absolute deadlines (`pace_delay`). Each packet's deadline is the previous deadline plus its serialization time at the pacing rate, and the thread sleeps until that deadline. If the pacer has been idle for more than 0.5 ms, the deadline restarts from now so it cannot save up idle time and then burst. An earlier version slept a relative amount after every packet. Each sleep overshoots by about 100 µs on WSL2 (about 50 µs on typical Linux), and the overshoots added up: a frame meant to take 23 ms took more than 30 ms, longer than the frame interval. The controller then saw BUR above 1 on an idle link and used only about a third of a 20 Mbps link.

### 7.4 ACK processing

For each valid ACK the listener:

- ignores it unless the session matches and the frame is still in `inflight`;
- handles a `KEY_REQUEST` separately (section 7.7);
- updates RTT, the retransmission timeout, `min_rtt` and Dmin, unless the ACK is a RECOVERED one;
- updates the receive-rate window;
- marks the packet (or probe) as acknowledged in `SentFrame::acknowledge`. A packet that arrived on its first attempt also records its arrival time for the loss ceiling. A FRAME_COMPLETE flag marks the whole frame delivered and records the completion time.

Then it runs `timers`.

### 7.5 Turning ACKs into controller samples

`resolve` walks `inflight` in frame order and stops at the first frame that is not ready. A frame is ready when it is delivered and either all four probe ACKs are in, or one retransmission timeout has passed since its last probe was sent. It is also handled when it is older than the frame timeout (10 intervals, 286 ms).

For a delivered frame, `resolve` builds the `FrameAck`:

| Field | Value |
|---|---|
| D, Dmin | frame delay and minimum OWD, in seconds |
| probes | the four T_i values from section 6.2 |
| recv_rate | payload receive rate |
| in_bytes, n_inflight | unacknowledged payload bytes and frames, for draining |
| frame_mbps | actual frame bitrate, B_k |
| lossy, ack_bytes, ack_first_us, ack_last_us | inputs to the loss ceiling |
| epoch | the frame's rate epoch |
| base_rtt | `min_rtt` in seconds |
| wall_us | wall clock, for the τ reset |

A frame that expires undelivered is logged as `FRAME_LOSS` and passed to `on_frame_loss`. Either way the frame's probe summary is logged and it is removed from `inflight`.

Waiting a bounded time for probes matters because frames are processed in order. An earlier version waited for every probe ACK or the full 286 ms timeout. With 2% random loss, about one frame in twelve lost a probe ACK, each one blocked feedback for all later frames, and the controller had no information for about half of the run.

### 7.6 Retransmission

Once all of a frame's packets have been sent, `timers` queues a retransmission for each unacknowledged packet whose last transmission is older than the retransmission timeout, up to eight attempts. The pacer sends retransmissions before new data. Each retransmission carries a new send time and an incremented `retrans_seq`. This is a plain timer; LADR's RACK-TLP, which detects losses from later ACKs, is not implemented.

`timers` also calls `on_inflight_age` with the age of the oldest undelivered frame (section 6.8).

### 7.7 Keyframe requests

When the receiver has lost decoder state, it sends `KEY_REQUEST` messages. The sender honours at most one per `max(200 ms, 2 × srtt)` and sets `force_keyframe`. The capture thread then asks the encoder for an IDR frame. In synthetic mode requests are ignored.

### 7.8 Shutdown

When the duration ends or `q` is pressed, capture stops first. The sender then waits up to one frame timeout for in-flight frames to resolve, stops the other threads, and logs any frame still undelivered as `FRAME_LOSS` with reason `shutdown`. SIGINT and SIGTERM set a flag that `run` checks.

## 8. The receiver in detail

### 8.1 Sessions

The first valid packet sets the session and the sender's address. A packet from a new session is accepted only if it is a stream-info packet or belongs to frame 1. The old session id is then retired and ignored from then on, so stray packets from an old run cannot confuse the new one. After 64 restarts the receiver asks to be restarted, which bounds the retired set. Packets from other addresses are ignored.

### 8.2 Reassembly and limits

Frames are kept in a map by frame id. A new frame is accepted only if its id is not older than the next frame to play, at most 256 ahead, fewer than 256 frames are buffered, and all buffered frames together stay under 16 MiB. Each packet must match the frame's size and start time. Duplicates are ignored but still acknowledged, so the sender stops retransmitting them.

`wire_bytes` counts the UDP payload of every valid packet from the session, including duplicates and probes. It is never increased for rebuilt packets.

### 8.3 Parity recovery

Parity packets are stored by group. After any data or parity packet arrives, `recover` checks the group: if exactly one data packet is missing and the parity is present, it XORs the parity with the other packets to rebuild the missing one, sends a RECOVERED ACK for it, and drops the parity. Recovery works whatever order the packets arrive in. The last packet of a frame is padded, so XOR works on full-size payloads, and `assemble` trims the frame to its real size.

### 8.4 Ordered delivery

After each received packet, and on every timeout, the receiver delivers complete frames in order:

- if the next frame is complete, it is decoded (or counted in headless mode) and `next` advances;
- if the next frame has not arrived and a later one has been waiting longer than the frame timeout, the missing frame is skipped;
- if the next frame is incomplete and older than the frame timeout, it is skipped.

A skipped frame means the decoder's references are now wrong, so `need_key` is set and frames are not decoded until a keyframe arrives. While waiting, every complete non-keyframe triggers a `KEY_REQUEST`, at most every 200 ms. Frames are removed from memory after twice the frame timeout.

### 8.5 Decode errors

If the decoder rejects a frame, the receiver logs the error, sets `need_key`, sends a keyframe request and continues. A corrupt frame costs a short freeze rather than the whole session.

## 9. Video capture, encoding and playback

### Capture (`cloud_gaming/capture/capture.h`)

`FrameCapture` uses the X shared-memory extension (XShm). It needs a 32-bit little-endian BGRA visual, rounds the screen size down to even numbers (the encoder needs even dimensions), and copies each captured image into its own buffer so the encoder can read it while the next capture runs. The shared-memory segment is marked for removal as soon as it is attached, so it disappears even if the program crashes.

### Encoder (`cloud_gaming/capture/encoder.h`)

`VideoEncoder` uses libx264 through FFmpeg with:

- preset `ultrafast` and tune `zerolatency`, so no frames are held back for lookahead;
- no B-frames;
- a keyframe interval of 10 seconds. After a loss the receiver asks for a keyframe, so periodic keyframes, each many times larger than a normal frame, are only a fallback;
- average-bitrate mode with the maximum rate equal to the target and a rate-control buffer of one frame (bitrate / fps). Each frame stays close to its share of the bitrate, which the per-frame BUR measurement relies on. A one-second buffer, used earlier, allowed frames of 7 times the target;
- `forced-idr`, so a requested keyframe is a real IDR frame.

`SetBitrate` only changes the context fields; FFmpeg's libx264 wrapper notices the change before the next frame and reconfigures x264. The BGRA image is converted to YUV 4:2:0 with swscale before encoding. `Encode` returns all packets the encoder produced and reports whether one of them was a keyframe.

### Decoder and renderer (`cloud_gaming/playback/`)

`VideoDecoder` uses FFmpeg's H.264 decoder with one thread and low-delay mode, and converts each picture back to BGRA. `Reset` flushes the decoder before a keyframe after a loss. `VideoRenderer` shows frames in an SDL2 window through a streaming texture, preferring an accelerated renderer and falling back to software. It does not wait for vertical sync.

## 10. The event log

With `--log`, the sender writes one JSON object per line. Every event has `type` and `ts_microsecs` (sender steady clock), and most have `fid`. The first line is a `LOG_HEADER` with the log and protocol versions.

| Event | Fields | Written when |
|---|---|---|
| FRAME_SENT | bytes, target_mbps, frame_interval_us, sensible_us, sender_queue_us | the pacer starts a frame |
| FRAME_DELIVERED | bytes, target_mbps, encoded_mbps, delay_ms, capture_to_ack_ms, lossy | the first ACK with FRAME_COMPLETE arrives |
| FRAME_LOSS | reason (`delivery_timeout` or `shutdown`) | a frame is given up |
| FRAME_SKIPPED | reason | the capture thread skips a frame |
| PROBE_SUMMARY | expected, received | a frame leaves `inflight` |
| PACKET_RETRANSMITTED | packet_id, attempt | a retransmission is sent |
| CONTROL_SAMPLE | bur, bitrate, sample_rate, pacing, delay_ms, recv_rate, n_inflight, n_probes | after each controller update |
| FALLBACK_SET | saved_rate, new_rate, trigger (`congested_frame` or `inflight_age`) | a temporary fallback starts |
| FALLBACK_RESTORED | restored_rate | the fallback ends |
| DRAIN_START | bur, drain_rate, recv_rate, queued_bytes, inflight_bytes, new_bitrate | a drain starts |
| DRAIN_END | restored_bitrate, recv_rate | a drain ends |
| LOSS_BACKOFF | new_bitrate | a frame loss halves the rate |
| LOSS_CEILING_SET | bur, new_bitrate, ack_mbps, frame_mbps | the loss ceiling is set |
| RETRANS_LOSS | bitrate | a retransmission is lost for the first time in the session |
| KEYFRAME_REQUESTED | | the sender accepts a keyframe request |
| ERROR | message | a worker thread failed |

`delay_ms` in FRAME_DELIVERED is the time from the frame's first packet to the completing ACK: a round trip plus the frame's sending time. `capture_to_ack_ms` also includes encoding and time spent waiting for the pacer. In one 20 Mbps run the averages were 45 ms and 69 ms, so about a frame interval passes between capture and the first packet.

The logger has its own mutex and can be used from any thread. Numbers are written with `std::to_string`, which gives six decimal places.

## 11. Experiments and metrics

### 11.1 Emulated experiments

`evaluations/utils.py:emulate` runs one experiment:

1. it writes a mahimahi trace, one line per 1500-byte delivery opportunity;
2. it starts one headless receiver per flow on the host and waits until each one is listening;
3. it writes a shell script that starts each synthetic sender (after an optional delay, inside an extra `mm-delay` for flows with a longer RTT) and runs it under `mm-delay` and `mm-link`, optionally with a drop-tail queue limit;
4. it waits, stops every process group, and saves the logs, traces and a `meta.json` with the settings and git commit.

`evaluations/scenarios.py` builds each experiment from command-line options and writes `results/current_<test>_<time>.json`. The thin scripts such as `const_test.py` call it with a test name. `run_all.py` runs the whole set in sequence.

### 11.2 Metrics

The functions are in `evaluations/metrics.py`.

**Throughput.** `throughput` is the payload of delivered frames sent in a window divided by the window length. `wire_throughput` is the same frames' IP-level size (data, parity, probes, retransmissions and 28 bytes of IP and UDP header per packet), for comparisons with link capacity or with other protocols. `frame_wire_bytes` reproduces the packet format exactly.

**Payload equivalent.** `payload_equivalent(capacity)` finds the largest payload rate whose wire size fits in the capacity. A 20 Mbps link carries at most 14.8 Mbps of payload and a 5 Mbps link 3.4 Mbps. The lower ratio at low rates comes from the probes and the padding of the last packet. Convergence targets are converted this way; without the conversion, the targets could never be reached.

**Delay and stalls.** `summarise` reports average, 95th and 99th percentile of `delay_ms` for delivered frames, the same for `capture_to_ack_ms` when the log has it, and stall rates: the fraction of sent frames that were lost or took more than 100 ms (or 200 ms).

**Convergence.** `convergence_ms` returns the time after an event until ten consecutive points of a series are within 5% of the target, or `None`. The experiments apply it to a 0.5 s trailing average of the per-frame payload rate (`smoothed`), because a single fallback frame is 15% lower and would break any run of ten raw frames.

**Fairness.** `common_rates` finds the window in which all flows were active and measures each flow's throughput in it. `jain` computes Jain's index; a flow that delivered nothing counts as zero rather than being dropped.

**BUR accuracy.** `bur_accuracy.py` fixes the bitrate (`--bmin` equal to `--bmax`) so that the wire load is a known fraction of capacity, and `offered_utilization` compares that load with the BUR the controller estimated over the last 40% of the run. This is only meaningful for a single flow without cross traffic.

**TCP share.** `tcpcubic_compete.py` compares Pudica's wire throughput with CUBIC's TCP payload scaled by 1500/1448 to include its headers, so both sides are measured as link occupancy.

### 11.3 Log checks

`evaluations/sift.py` checks a sender log: every sent frame has exactly one outcome and one probe summary, every controller sample belongs to a delivered frame and has four probe values and a pacing multiplier of at least 1, and every drain follows three samples with BUR above 1. The integration tests run it on every log.

### 11.4 Real network

`realnet/` runs the same kinds of test between two Linux machines. The sender machine starts the receiver over SSH, starts Xvfb with a test pattern or Doom, applies traffic shaping, runs the sender, and collects both logs. `realnet/GUIDE.md` has the setup steps.

Traffic shaping (`realnet/netshape.py`) adds a `prio` root qdisc with a netem child, and filters only UDP packets to the receiver's address and the test ports into netem. Before changing anything it checks that every qdisc on the interface still has the kernel's default handle `0:`. Kernel defaults come back by themselves when the added root is deleted, so they are safe to replace; anything configured by hand has a real handle and is left alone, and the run stops instead. After cleanup it checks that the default is back. While a test runs, all traffic on that interface goes through one queue.

Netem's queue holds 1000 packets unless `QUEUE_PACKETS` is set, which is about half a second at 20 Mbps. Jitter is only allowed together with a rate limit, because netem otherwise gives each packet an independent delay and reorders them.

The `rtt_sensitivity` test runs one flow twice, at a low and a high RTT, and reports the ratio of their throughputs. It is not the same as the emulator's `different_rtprop` test, which runs two flows with different RTTs on one bottleneck at the same time.

### 11.5 Tests

- `codes/test_controller.cc`: bounds, fallback lasting one frame, next-delay threshold relative to RTT, drain rate averaged since onset, drain ignoring bytes in flight on the path, epoch hold-off, one backoff per loss event, and the loss ceiling ignoring random loss but reacting to congestion.
- `codes/test_transport.cc`: header and ACK encoding, parity recovery at every position including short final groups, duplicate handling, keyframe requests, and the frame byte count.
- `tests/test_measurements.py`: metrics, wire units, smoothing, `sift`, trace handling, and traffic shaping against mocked `tc` output.
- `tests/integration_transport.py`: real sender and receiver on localhost behind a relay that drops the first packets of a frame, drops two packets with their parity and then one of the retransmissions, drops one probe ACK of every frame, blocks ACKs for 130 ms, or drops everything; plus one run with real H.264 decoding.

## 12. Differences from the papers

| Topic | Paper | This code | Why |
|---|---|---|---|
| Frame rate | 60 fps | 35 fps | project choice |
| Bounds on A in AI-MD | bounded, values not given | [−1 Mbps, 0.05 × B] | needed a choice |
| log B in I | not specified | floored at 0.2 | log B is negative below 1 Mbps |
| Next-delay threshold | "significant", not given | min RTT + 2 frame intervals | a fixed value fires on every frame on long paths |
| Queue size in drain | in-flight packets | in-flight bytes minus bandwidth-delay product | in-flight bytes on a long path are mostly not queued |
| Missing probe | not discussed | counted as T_packet after one RTO | avoids stalling feedback |
| Frame loss | not discussed | halve once per loss event and drain | outage safety net |
| Loss ceiling ACK rate | span across the n frames (LADR) | sum of per-frame spans | the joint span includes idle gaps and confuses random loss with congestion |
| Loss ceiling delay test | RTT − RTTmin of first lossy frame below δ (LADR) | frame delay − Dmin below one interval | uses the delay already measured per frame |
| FEC | opportunistic, only when losses justify it (LADR) | XOR parity on every frame | simpler; costs about 25% of the link |
| Loss detection | RACK-TLP (LADR) | retransmission timer | simpler |
| Transport | multiple TCP and UDP connections (LADR) | one UDP socket | simpler |

## 13. Known limitations

- **BUR stays near 1 below capacity.** In mahimahi at 20 Mbps the median BUR is about 0.97 while the link is only 50 to 65% used. D − Dmin includes the frame's own sending time, which is set from the previous BUR, so R ≈ R/1.25 + c for any small constant delay c, and the BUR settles at about 5c. The probe term alone adds about 0.1 even with no cross traffic, partly because mahimahi delivers packets in 1 ms steps. On a real link c is probably smaller, but this has not been measured.
- **Fixed parity overhead.** Payload throughput can reach only about 74% of link capacity.
- **Slow growth on long paths.** Waiting for feedback from each new rate means roughly one increase per round trip.
- **Untested paths.** The keyframe request round trip, decode-error recovery and the effect of kernel timestamps have been built and run, but not exercised with real video loss or measured.
- **One sender per receiver**, no encryption or authentication, and no game input or audio.

## 14. Constants

| Name | Value | Where |
|---|---|---|
| FPS, INTERVAL | 35 fps, 28 571 µs | `protocol.h` |
| LOAD_SZ, HEADER_SIZE, MAX_BUF | 1200, 46, 1246 bytes | `protocol.h` |
| ACK_SIZE | 56 bytes | `protocol.h` |
| N_PROBE | 4 | `protocol.h` |
| FEC_K | 4 data packets per parity | `protocol.h` |
| MAX_FRAME_BYTES | 4 MiB | `protocol.h` |
| FRAME_TIMEOUT | 10 frame intervals | `protocol.h` |
| B_MIN, B_MAX | 0.2, 50 Mbps (options `--bmin`, `--bmax`) | `PudicaConfig` |
| γ_p | 1.25 | `PudicaConfig::GAMMA_P` |
| α | 0.85 | `PudicaConfig::ALPHA` |
| γ_MI | 0.3 | `PudicaConfig::GAMMA_MI` |
| γ_MD | 0.05 | `PudicaConfig::GAMMA_MD` |
| A bounds | −1 Mbps, 0.05 × B | `PudicaConfig::A_MIN`, `A_MAX` |
| ζ | 15% | `PudicaConfig::ZETA` |
| Drain window | 200 ms | `PudicaConfig::DRAIN_WIN` |
| Next-delay allowance | 2 frame intervals beyond min RTT | `PudicaConfig::NEXT_DELAY_THRESH` |
| History window | 200 ms | `on_frame_acked` |
| τ reset period | 5 s | `on_frame_acked` |
| Dmin and min RTT windows | 10 s | `Sender::listener` |
| Receive-rate window | about 200 ms | `Sender::listener` |
| Retransmission timeout | max(20 ms, srtt + 4 × rttvar), up to 8 attempts | `Sender::listener`, `Sender::timers` |
| Reactive rate limit | on at 4 pending recoveries, off below 2 | `Sender::resolve` |
| Pending-data limit | 2 frame intervals or 3 queued frames | `Sender::capture` |
| Loss ceiling | 3 frames, β = 0.8, C = 1 Mbps/s³ | `check_shallow_congestion` |
| Keyframe request spacing | receiver 200 ms; sender max(200 ms, 2 × srtt) | `receiver.cc`, `Sender::listener` |
| Encoder | libx264 ultrafast/zerolatency, GOP 10 s, one-frame rate buffer | `encoder.h` |

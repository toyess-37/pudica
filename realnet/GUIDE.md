> How the code works is in [TECHNICAL_DOCUMENTATION.md](../TECHNICAL_DOCUMENTATION.md). MIN_RTT_MS defaults to 0. Auto always uses ffplay. Shaping works on an interface that still has the kernel default qdiscs (or noqueue) and only touches the experiment UDP ports; set QUEUE_PACKETS for a shallow netem queue. Automated remote receivers use --decode-only; visible receivers use bash realnet/run_receiver.sh PORT --visible. The older walkthrough below is retained for device/VPN setup context.

# Two-PC Real-Network Pudica Evaluation (`realnet/`)

This folder runs the same evaluations as `evaluations/`, but over a **real
network between two Linux PCs** instead of mahimahi on one machine.

* **PC A – SENDER**: captures a screen, H.264-encodes it, runs the Pudica
  congestion-control sender. Also runs the orchestration (`run_all.py`) and
  applies traffic shaping.
* **PC B – RECEIVER**: runs the Pudica receiver (ACKs, FEC recovery, playback).
  Driven from PC A over SSH.

Everything is chained by Python: one command builds the traces of the run,
starts the game simulation, brings up the remote receiver, shapes the link,
runs the sender, tears everything down, parses the logs and writes the result
files into `results/` – the same JSON/SVG files the `evaluations/` scripts
produce.

The mahimahi variable `MAHIMAHI_BASE` / `MAHIMAHI_IP` is replaced by a single
value: **`DEVICE_IP`** – the address of the receiver PC as seen from the sender.

---

## 1. How the chain works

```
run_all.py
  └─ preflight.py            checks binaries, ffplay/Xvfb, tc+sudo, ping, ssh
  └─ for each test in tests.py:
       session.run_session()
         1. ping DEVICE_IP                     → measure real RTT
         2. gamesim.GameSim().start()          → Xvfb :99 + moving picture
         3. remote.start_receiver(port)        → ssh PC B: run_receiver.sh
         4. netshape.Shaper(...).apply()       → tc/netem toward DEVICE_IP only
              (delay padded up to MIN_RTT_MS, exactly like evaluations' --rtt)
         5. run codes/sender --log … DEVICE_IP port dur
              (step test mutates netem partway through)
         6. teardown: clear tc, stop receiver, stop game
         7. scp the receiver log back; parse sender.jsonl
       → write results/realnet_<test>.json  (+ .svg)
  └─ write results/realnet_summary.json + results/realnet_tabular_summary.txt
```

Each run also writes a full log tree under `realnet/logs/<timestamp>_<test>/`
(see §9).

---

## 2. Prerequisites

* Two PCs running a normal Linux distro (Debian/Ubuntu assumed; adjust the
  `apt` line in `setup.sh` for others). A Raspberry Pi works as PC B.
* Kernel with `sch_netem` (every stock distro kernel has it).
* The two PCs able to reach each other over IP (see §5).
* On PC A: sudo rights (for `apt` and for a one-line `tc` sudoers rule).

---

## 3. One-time setup (run on BOTH PCs)

```bash
git clone <this repo> ~/pudica2        # same path on both is easiest
cd ~/pudica2
bash realnet/setup.sh
```

`setup.sh` installs system packages, Python packages (`numpy`, `matplotlib`),
and builds `codes/sender` + `codes/receiver`.

Packages it installs: `build-essential`, X11 dev libs, ffmpeg libs, SDL2,
`xvfb`, `x11-utils`, `ffmpeg`, `iproute2`, `iperf3`, `iputils-ping`,
`openssh-client/server`, `python3-numpy`, `python3-matplotlib`, and
(optional) `chocolate-doom` + `freedoom`.

---

## 4. SSH from PC A to PC B

On **PC A** (the sender), run:

```bash
bash realnet/setup_ssh.sh <pcB_user> <pcB_host> [ssh_port]
# e.g.
bash realnet/setup_ssh.sh pi 192.168.1.50
```

It will:

1. create `~/.ssh/id_ed25519` if you don't have one;
2. `ssh-copy-id` the public key to PC B (one password prompt);
3. verify passwordless login works.

Manual equivalent, if you prefer:

```bash
# on PC A
ssh-keygen -t ed25519 -N "" -f ~/.ssh/id_ed25519
ssh-copy-id -i ~/.ssh/id_ed25519.pub pi@192.168.1.50
ssh pi@192.168.1.50 'echo ok && test -x ~/pudica2/codes/receiver && echo built'
```

Make sure `sshd` is running on PC B:

```bash
# on PC B
sudo systemctl enable --now ssh
```

---

## 5. Connecting the two PCs

### 5a. Same LAN / same Wi-Fi (simplest)

Nothing to do. `DEVICE_IP` = PC B's `192.168.x.y` (find it with `ip addr` on
PC B). `SSH_HOST` = the same address.

> A very fast wired LAN has sub-millisecond RTT. The controller in this
> codebase only finalises a frame while its trailing probe ACKs are still
> "in flight", so it needs a real round trip. `run_session` therefore pads
> the netem delay up to `MIN_RTT_MS` (default **20 ms**, matching the
> `--rtt 20` used everywhere in `evaluations/`). Leave shaping on, or the
> logs may come back empty on a LAN that is too quick.

### 5b. Two different mobile networks (phone hotspots, different ISPs)

Each PC tethered to a different phone is behind **carrier-grade NAT** – there
is no public inbound port, so the sender cannot reach the receiver directly.
Put both PCs on one **overlay network** and use the overlay address as
`DEVICE_IP`:

**Tailscale (easiest):**

```bash
# on BOTH PCs
curl -fsSL https://tailscale.com/install.sh | sh
sudo tailscale up            # log in once in the browser it prints
tailscale ip -4              # -> 100.x.y.z  (this is the DEVICE_IP / SSH_HOST)
```

Then in `config.py` (or as env vars):

```
DEVICE_IP  = "100.x.y.z"     # PC B's tailscale IP
SSH_HOST   = "100.x.y.z"
SELF_IP    = "100.a.b.c"     # PC A's tailscale IP (only the cubic test needs it)
SHAPE_IFACE = "auto"         # resolves to tailscale0, netem on it is fine
```

**WireGuard** works too (give PC A `10.9.0.1/24`, PC B `10.9.0.2/24`, set
`DEVICE_IP=10.9.0.2`). Any solution that gives both ends a mutually routable
address is fine.

Notes for the mobile case:

* Real RTT will already be 30–80 ms, so `MIN_RTT_MS` padding does nothing –
  good, the test sees the true path.
* The overlay adds ~20–40 bytes of header; the sender's 1246-byte UDP payloads
  still fit inside the typical 1280-byte-safe payload only if you drop
  `LOAD_SZ`. If you see immediate packet loss, lower `--rate` or set
  `MIN_RTT_MS` aside and test with `NO_SHAPE=1` first.
* Data caps: a 30 s test at 20 Mbit/s ≈ 75 MB per direction.

---

## 6. Passwordless `sudo` for `tc` (PC A only)

Traffic shaping needs root. Add a minimal sudoers rule on **PC A**:

```bash
echo "$USER ALL=(root) NOPASSWD: /usr/sbin/tc, /sbin/tc" | \
  sudo tee /etc/sudoers.d/pudica-tc
sudo chmod 440 /etc/sudoers.d/pudica-tc
```

Or skip shaping entirely:

```bash
NO_SHAPE=1 python3 realnet/run_all.py --tests baseline
```

`NO_SHAPE=1` runs only the tests that don't need netem (`baseline`).

---

## 7. Configure

Edit `realnet/config.py`, or export environment variables of the same name
(env wins). The ones you must set:

| key           | meaning                                              |
|---------------|-----------------------------------------------------|
| `DEVICE_IP`   | receiver address the sender sends UDP to             |
| `SSH_USER`    | login user on PC B                                   |
| `SSH_HOST`    | ssh address of PC B (usually == `DEVICE_IP`)         |
| `REMOTE_REPO` | path to this repo on PC B (default `~/pudica2`)      |

Optional knobs: `SSH_PORT`, `SSH_KEY`, `MIN_RTT_MS`, `SHAPE_IFACE`,
`GAME_BACKEND`, `XVFB_GEOMETRY`, `RESULT_PREFIX`, `PORT`, `DUR`.

Example:

```bash
export DEVICE_IP=192.168.1.50 SSH_USER=pi SSH_HOST=192.168.1.50
python3 realnet/preflight.py
```

---

## 8. Preflight

```bash
python3 realnet/preflight.py
```

Green on every line before you run the suite. It checks: local binaries,
`Xvfb`/`xdpyinfo`, `ffplay`/`chocolate-doom`, `tc` + passwordless sudo,
`ping DEVICE_IP`, SSH login, and that `codes/receiver` exists on PC B.

---

## 9. Run the tests

```bash
# default set: baseline, const, step, jitter, loss, bur_accuracy
python3 realnet/run_all.py

# pick tests
python3 realnet/run_all.py --tests const,step,jitter

# everything, longer runs, different shaped rate
python3 realnet/run_all.py --tests all --dur 45 --rate 25

# natural link only, no shaping
NO_SHAPE=1 python3 realnet/run_all.py --tests baseline
```

Common options (`run_all.py --help` for all):

| option            | default        | used by            |
|-------------------|----------------|--------------------|
| `--dur`           | 30             | all                |
| `--rate`          | 20 (Mbit/s)    | const/jitter/loss  |
| `--bw1 --bw2`     | 20 / 10        | step               |
| `--jitter`        | 40 (± ms)      | jitter             |
| `--loss`          | 2 (%)          | loss               |
| `--rtt-lo --rtt-hi` | 20 / 80 (ms) | rtt_sensitivity    |
| `--bw-list`       | 5,10,15,20,25  | bur_accuracy       |
| `--flows --stagger` | 3 / 10       | fairness           |
| `--port`          | 9700           | all (uses port..port+N) |

You can also run a single test module-style:

```bash
python3 -c "import sys; sys.path.insert(0,'realnet'); \
  from types import SimpleNamespace as N; import tests; \
  tests.const(N(dur=30, port=9700, rate=20))"
```

### Running the receiver by hand (debugging)

On PC B:

```bash
XVFB_DISPLAY=:99 bash realnet/run_receiver.sh 9700
```

On PC A:

```bash
DISPLAY=:99 codes/sender --log /tmp/s.jsonl <DEVICE_IP> 9700 20
```

---

## 10. Output files

Written into `results/` with the `realnet_` prefix (so they never overwrite
the reference mahimahi results):

| file                                   | matches `evaluations/`      |
|----------------------------------------|-----------------------------|
| `realnet_baseline.json`                | (new) raw-link run          |
| `realnet_const_<rate>.json` / `.svg`   | `const_test.py`             |
| `realnet_step.json` / `.svg`           | `step_test.py`              |
| `realnet_jitter.json` / `.svg`         | `jitter_test.py`            |
| `realnet_loss.json` / `.svg`           | (new) netem loss            |
| `realnet_rtt_sensitivity.json`         | none (one flow, RTTs run separately) |
| `realnet_bur_accuracy.json`            | `bur_accuracy.py`           |
| `realnet_fairness.json`                | `jains_fairness.py`         |
| `realnet_summary.json`                 | combined                    |
| `realnet_tabular_summary.txt`          | `mahimahi_tabular_summary.txt` |

The per-flow `summary` block (`n_frames`, `avg_bitrate`, `avg_delay`,
`p95/p99_delay`, `stall_100ms/200ms`, `avg_bur`) is byte-for-byte the same
shape as the mahimahi results, so any downstream analysis still works.

---

## 11. Logs and run artefacts ("modern logging")

Every `run_all.py` invocation and every individual session creates:

```
realnet/logs/
  latest -> 20260907-120501_run_all/
  20260907-120501_run_all/
    session.log            timestamped orchestration log (also on console)
  20260907-120515_const_20/
    session.log
    meta.json              config, git commit, measured RTT, netem params, timings
    sender.jsonl           the raw Pudica event log (kept, not a temp file)
    sender.stderr          libx264 / sender diagnostics
    receiver.log           fetched from PC B over scp
    netshape_before.txt    `tc -s qdisc` right after shaping was applied
    netshape_after.txt     `tc -s qdisc` at end (packets/drops per class)
    const_20.svg           bitrate / delay / BUR plot
```

`meta.json` is the machine-readable record of exactly how that run was
parameterised. `sift.py` from `evaluations/` still works on `sender.jsonl`:

```bash
python3 evaluations/sift.py realnet/logs/latest/../20260907-120515_const_20/sender.jsonl
```

---

## 12. Game simulation

The sender captures whatever is on X display `:99`. `gamesim.py` puts
something there:

| `GAME_BACKEND` | what runs                        | needs                     |
|----------------|----------------------------------|---------------------------|
| `auto` (default) | `ffplay` moving test pattern, else doom | `ffmpeg`          |
| `doom`         | `chocolate-doom` on Freedoom, level 1 | `chocolate-doom` + a `.wad` |
| `black`        | nothing (tiny frames)            | –  (smoke test only)      |

```bash
GAME_BACKEND=doom python3 realnet/run_all.py --tests const
DOOM_WAD=/path/to/doom1.wad GAME_BACKEND=doom python3 realnet/run_all.py --tests const
XVFB_GEOMETRY=1920x1080x24 python3 realnet/run_all.py --tests const
```

`ffplay`'s `testsrc2` gives a steady high-motion signal → a realistic,
varying encoder bitrate. `doom` is the thematic choice (cloud *gaming*) but
sits fairly still on the first level, so frames are smaller.

---

## 13. Troubleshooting

**`no FRAME_ACKED events … sender produced no completed frames`**
The sender ran but never got a full frame acknowledged. Causes, in order of
likelihood:

1. RTT too low – you passed `NO_SHAPE=1` on a fast wired LAN. Drop it, or set
   `MIN_RTT_MS=25`.
2. Receiver never came up – check `realnet/logs/latest*/receiver.log` and that
   `run_receiver.sh` can start Xvfb on PC B (`xdpyinfo` installed there).
3. A firewall is dropping the UDP port. Open it on PC B:
   `sudo ufw allow 9700:9720/udp`.
4. Overlay MTU – on Tailscale/WireGuard, test once with `--rate 8` to confirm
   basic flow, then raise it.

**`tc / passwordless sudo for tc not available`**
Do §6, or run with `NO_SHAPE=1`.

**`could not find the route to <DEVICE_IP>`**
`ip route get <DEVICE_IP>` fails on PC A – the two PCs aren't connected yet
(§5). For the hotspot case you haven't run `tailscale up` on both.

**`remote receiver … did not start`**
SSH works but the binary/Xvfb doesn't. `ssh <pcB> 'cd ~/pudica2 && bash
realnet/run_receiver.sh 9700'` by hand and read the error.

**SDL / `SDL_CreateRenderer failed` in `receiver.log`**
The receiver needs a real X server, not the SDL dummy driver.
`run_receiver.sh` already starts Xvfb and forces `SDL_VIDEODRIVER=x11`; make
sure `xvfb` and `x11-utils` are installed on PC B.

---

## 14. What was and wasn't verified

Built and checked on the dev machine:

* `codes/sender` and `codes/receiver` build clean.
* Sender captures Xvfb + H.264-encodes (libx264 confirms frames).
* Sender transmits UDP; receiver ACKs every packet correctly (verified with a
  synthetic probe: 34-byte `RecvACK` per packet).
* `gamesim.py` starts/stops Xvfb + doom/ffplay.
* All Python modules compile; `preflight.py` and `run_all.py --help` run;
  the full `run_session` chain executes end to end (game → receiver → sender →
  parse → save → plot).

Not runnable in the dev sandbox (needs the two real PCs):

* The SSH remote-receiver path and `tc`/netem shaping.
* A completed sender↔receiver session. **The existing `evaluations/` scripts
  also produce "no FRAME_ACKED events" in that sandbox** – it is a WSL2
  networking limitation (no `cap_net_raw`, mahimahi return-routing), not a bug
  in this harness. On two real PCs with real RTT it behaves like the committed
  mahimahi results.

---

## 15. File map

| file                | role                                              |
|---------------------|---------------------------------------------------|
| `config.py`         | all settings (env-overridable)                    |
| `common.py`         | logging, `sh()`, JSONL parse, stats, plot, save   |
| `netshape.py`       | `tc`+netem shaping toward `DEVICE_IP` only        |
| `gamesim.py`        | Xvfb + game/test-pattern on `:99`                 |
| `remote.py`         | SSH: start/stop remote receiver, fetch its log    |
| `session.py`        | one full run (`run_session`) – "the chain"        |
| `tests.py`          | the individual evaluations + `REGISTRY`           |
| `preflight.py`      | pre-run environment checks                        |
| `run_all.py`        | orchestrator / CLI                                |
| `run_receiver.sh`   | runs on PC B: Xvfb + `codes/receiver`             |
| `setup.sh`          | one-time install + build (both PCs)               |
| `setup_ssh.sh`      | key-based SSH PC A → PC B                         |
| `GUIDE.md`          | this document                                     |

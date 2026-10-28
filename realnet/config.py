import os
from pathlib import Path


def _env(name, default):
    v = os.environ.get(name)
    return v if v is not None and v != "" else default


REALNET_DIR = Path(__file__).resolve().parent
REPO_ROOT = REALNET_DIR.parent
CODES_DIR = REPO_ROOT / "codes"
SENDER_BIN = CODES_DIR / "sender"
RECEIVER_BIN = CODES_DIR / "receiver"
RESULTS_DIR = REPO_ROOT / "results"
LOG_DIR = REALNET_DIR / "logs"

# Prefix for the JSON/SVG files written into results/ so they never overwrite
# the reference mahimahi results committed in the repo.
RESULT_PREFIX = _env("RESULT_PREFIX", "realnet_")


# Receiver address as seen from the sender. Across two ISPs use a Tailscale or
# WireGuard address so both ends are directly routable.
DEVICE_IP = _env("DEVICE_IP", "192.168.1.50")

# Sender address the receiver can reach back on. Only the cubic test needs it.
SELF_IP = _env("SELF_IP", "192.168.1.40")


SSH_HOST = _env("SSH_HOST", DEVICE_IP)          # host/IP for ssh
SSH_USER = _env("SSH_USER", os.environ.get("USER", "user"))
SSH_PORT = int(_env("SSH_PORT", "22"))
SSH_KEY = _env("SSH_KEY", str(Path.home() / ".ssh" / "id_ed25519"))
# Path to this same git checkout on the RECEIVER PC.
REMOTE_REPO = _env("REMOTE_REPO", "~/pudica2")


# Traffic shaping (tc + netem) applied on the SENDER, only to packets
# addressed to DEVICE_IP. All other traffic on the machine is untouched.
# Needs passwordless sudo for `tc` (see GUIDE.md).
# Local interface that carries packets to DEVICE_IP. "auto" = derive it with
# `ip route get DEVICE_IP`.
SHAPE_IFACE = _env("SHAPE_IFACE", "auto")

# Optional RTT floor; transport feedback no longer requires artificial delay.
MIN_RTT_MS = int(_env("MIN_RTT_MS", "0"))

# Set to "1" to skip all shaping (pure real link, natural RTT). Tests that
# need netem (step / jitter / loss / rtt_sensitivity / const-with-rate) are skipped.
NO_SHAPE = _env("NO_SHAPE", "0") == "1"

# netem queue in packets; netem's own default of 1000 is a very deep buffer
# (about 0.5 s at 20 Mbit/s). Set e.g. QUEUE_PACKETS=8 for shallow-buffer runs.
QUEUE_PACKETS = int(_env("QUEUE_PACKETS", "1000"))


XVFB_DISPLAY = _env("XVFB_DISPLAY", ":99")
XVFB_GEOMETRY = _env("XVFB_GEOMETRY", "1280x720x24")

# "auto"  -> ffplay test pattern (reliable motion, good encoder load)
# "doom"  -> chocolate-doom on Freedoom (thematic; needs the wad)
# "black" -> nothing on screen (tiny frames; only for a quick smoke test)
GAME_BACKEND = _env("GAME_BACKEND", "auto")
DOOM_WAD = _env("DOOM_WAD", "")  # explicit .wad path; empty = search common spots


PORT = int(_env("PORT", "9700"))
DUR = int(_env("DUR", "30"))

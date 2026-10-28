#!/usr/bin/env bash
# realnet/setup.sh
#
# One-time setup. Run this on BOTH PCs (sender and receiver).
# Installs the system packages, the Python packages, and builds the binaries.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==> apt packages"
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  libx11-dev libxext-dev \
  libavcodec-dev libavutil-dev libswscale-dev \
  libsdl2-dev \
  xvfb x11-utils \
  ffmpeg \
  iproute2 iperf3 iputils-ping \
  openssh-client openssh-server \
  python3 python3-numpy python3-matplotlib \
  rsync
# optional, only needed for GAME_BACKEND=doom
sudo apt-get install -y chocolate-doom freedoom || \
  echo "   (chocolate-doom/freedoom not installed - ffplay backend still works)"

echo "==> build sender + receiver"
make -C "$HERE/codes" clean
make -C "$HERE/codes"

echo "==> quick sanity"
"$HERE/codes/sender"   2>&1 | head -1 || true
"$HERE/codes/receiver" 2>&1 | head -1 || true

cat <<EOF

Done.

Next:
  * edit realnet/config.py  (DEVICE_IP, SSH_USER/SSH_HOST, REMOTE_REPO)
  * on the SENDER PC run:   bash realnet/setup_ssh.sh
  * then:                   python3 realnet/preflight.py
  * then:                   python3 realnet/run_all.py
EOF

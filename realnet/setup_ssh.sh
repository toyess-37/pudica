#!/usr/bin/env bash
# realnet/setup_ssh.sh
#
# Run this ONCE on the SENDER PC. It sets up key-based SSH to the RECEIVER PC
# so run_all.py can start/stop the receiver without a password prompt, and
# reminds you about passwordless sudo for tc.
#
# Usage:
#   bash realnet/setup_ssh.sh <receiver_user> <receiver_host> [ssh_port]
set -euo pipefail

USER_R="${1:?usage: setup_ssh.sh <receiver_user> <receiver_host> [ssh_port]}"
HOST_R="${2:?usage: setup_ssh.sh <receiver_user> <receiver_host> [ssh_port]}"
PORT_R="${3:-22}"
KEY="$HOME/.ssh/id_ed25519"

if [ ! -f "$KEY" ]; then
  echo "==> generating $KEY"
  mkdir -p "$HOME/.ssh"
  chmod 700 "$HOME/.ssh"
  ssh-keygen -t ed25519 -N "" -f "$KEY"
fi

echo "==> copying public key to $USER_R@$HOST_R:$PORT_R  (one password prompt)"
ssh-copy-id -i "$KEY.pub" -p "$PORT_R" "$USER_R@$HOST_R"

echo "==> testing"
ssh -i "$KEY" -p "$PORT_R" -o BatchMode=yes "$USER_R@$HOST_R" \
  'echo "ssh ok on $(hostname)"'

cat <<EOF

SSH is ready.

Put these in realnet/config.py (or export as env vars):
  SSH_USER   = "$USER_R"
  SSH_HOST   = "$HOST_R"
  SSH_PORT   = $PORT_R
  DEVICE_IP  = "<receiver address the sender should send UDP to>"
  REMOTE_REPO = "<path to this repo on the receiver PC>"

Passwordless sudo for tc (needed on the SENDER for traffic shaping) - run on
the SENDER PC:
  echo "\$USER ALL=(root) NOPASSWD: /usr/sbin/tc, /sbin/tc" | \\
     sudo tee /etc/sudoers.d/pudica-tc
  sudo chmod 440 /etc/sudoers.d/pudica-tc

(or run the whole suite with NO_SHAPE=1 to skip shaping entirely.)
EOF

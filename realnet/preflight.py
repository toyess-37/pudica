import logging
import shutil
import sys

import config
from common import setup_logging, sh, sh_ok

log = logging.getLogger("realnet")


def _row(ok, label, detail=""):
    mark = "ok  " if ok else "FAIL"
    log.info("[%s] %-30s %s", mark, label, detail)
    return ok


def main() -> int:
    setup_logging(verbose=True)
    log.info("preflight  (DEVICE_IP=%s  ssh=%s@%s:%d)",
             config.DEVICE_IP, config.SSH_USER, config.SSH_HOST, config.SSH_PORT)
    ok = True

    ok &= _row(config.SENDER_BIN.exists(), "sender binary",
               str(config.SENDER_BIN))
    ok &= _row(config.RECEIVER_BIN.exists(), "receiver binary (local build)",
               str(config.RECEIVER_BIN))
    ok &= _row(bool(shutil.which("Xvfb")), "Xvfb")
    ok &= _row(bool(shutil.which("xdpyinfo")), "xdpyinfo (x11-utils)")

    backend = config.GAME_BACKEND
    if backend in ("auto", "ffplay"):
        ok &= _row(bool(shutil.which("ffplay")), "ffplay (ffmpeg)",
             "required by deterministic auto/ffplay backend")
    if backend == "doom":
        ok &= _row(bool(shutil.which("chocolate-doom")), "chocolate-doom",
             "optional unless GAME_BACKEND=doom")

    # shaping
    if config.NO_SHAPE:
        _row(True, "traffic shaping", "disabled (NO_SHAPE=1)")
    else:
        import netshape
        tc = netshape._tc_bin()
        has_tc = bool(shutil.which("tc")) or tc != "tc"
        has_sudo = netshape.have_tc()
        ok &= _row(has_tc, "tc (iproute2)", tc)
        ok &= _row(has_sudo, "passwordless sudo for tc",
                   "see GUIDE.md if this fails")

    # connectivity
    ping = sh(["ping", "-n", "-c", "2", "-w", "5", config.DEVICE_IP], check=False, quiet=True, timeout=8)
    ok &= _row(ping.returncode == 0, "ping receiver", config.DEVICE_IP)

    ssh_ok = False
    try:
        import remote
        remote.check()
        ssh_ok = True
    except Exception as e:
        log.info("%s", e)
    ok &= _row(ssh_ok, "ssh + remote receiver binary")

    log.info("-" * 60)
    log.info("preflight %s", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

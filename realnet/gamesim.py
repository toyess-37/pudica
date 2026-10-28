import logging
import os
import shutil
import signal
import subprocess
import time
from pathlib import Path

import config

log = logging.getLogger("realnet")

_WAD_SEARCH = [
    "/usr/share/games/doom/freedoom1.wad",
    "/usr/share/games/doom/freedoom2.wad",
    "/usr/share/doom/freedoom1.wad",
    str(Path.home() / "doom1.wad"),
]


def _find_wad() -> str | None:
    if config.DOOM_WAD and Path(config.DOOM_WAD).is_file():
        return config.DOOM_WAD
    for p in _WAD_SEARCH:
        if Path(p).is_file():
            return p
    return None


class GameSim:

    def __init__(self):
        self.display = config.XVFB_DISPLAY
        self._xvfb = None
        self._content = None
        self._env = dict(os.environ, DISPLAY=self.display)

    # -- Xvfb ------------------------------------------------------------
    def _display_up(self) -> bool:
        return subprocess.run(["xdpyinfo"],env=self._env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=5).returncode==0

    def _start_xvfb(self):
        if self._display_up():
            log.info("reusing X display %s", self.display)
            return
        w_h_d = config.XVFB_GEOMETRY
        self._xvfb = subprocess.Popen(
            ["Xvfb", self.display, "-screen", "0", w_h_d, "-nolisten", "tcp"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        for _ in range(50):
            time.sleep(0.1)
            if self._xvfb.poll() is not None:
                raise RuntimeError("owned Xvfb exited before readiness")
            if self._display_up():
                log.info("started Xvfb on %s (%s)", self.display, w_h_d)
                return
        raise RuntimeError("Xvfb did not come up")

    # -- content -------------------------------------------------------
    def _geom(self):
        w, h, _ = config.XVFB_GEOMETRY.split("x")
        return int(w), int(h)

    def _start_content(self):
        backend = config.GAME_BACKEND
        if backend == "auto":
            backend = "ffplay"
        w, h = self._geom()

        if backend == "black":
            log.info("game backend: black screen (frames will be tiny)")
            self._content = None
            return

        if backend == "doom":
            wad = _find_wad()
            if not wad or not shutil.which("chocolate-doom"):
                raise RuntimeError("doom backend requested but chocolate-doom "
                                   "or a .wad is missing")
            cmd = ["chocolate-doom", "-iwad", wad, "-warp", "1",
                   "-nomonsters", "-nomusic", "-nosound", "-nograbmouse"]
            log.info("game backend: chocolate-doom (%s)", Path(wad).name)
        elif backend == "ffplay":
            if not shutil.which("ffplay"):
                raise RuntimeError("ffplay not found (install the ffmpeg package)")
            src = f"testsrc2=size={w}x{h}:rate=35"
            cmd = ["ffplay", "-loglevel", "quiet", "-f", "lavfi", "-i", src,
                   "-fs", "-autoexit", "-loop", "0", "-an"]
            log.info("game backend: ffplay test pattern %dx%d", w, h)

        else:
            raise ValueError(f"unknown game backend: {backend}")

        self._content = subprocess.Popen(
            cmd, env=self._env,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        time.sleep(2.0)  # let the first frames render
        if self._content.poll() is not None:
            raise RuntimeError(f"game content process exited immediately: {cmd}")

    # -- lifecycle ---------------------------------------------------------
    def start(self):
        try:
            self._start_xvfb()
            self._start_content()
            return self
        except BaseException:
            self.stop()
            raise

    def stop(self):
        for p in (self._content, self._xvfb):
            if p and p.poll() is None:
                p.send_signal(signal.SIGTERM)
                try:
                    p.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait(timeout=3)
        self._content = self._xvfb = None

    @property
    def env(self) -> dict:
        return self._env

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()
        return False

import json
import logging
import subprocess
import sys
import time
from pathlib import Path


import config


log = logging.getLogger("realnet")


def setup_logging(run_dir: Path | None = None, verbose: bool = True) -> None:
    root = logging.getLogger("realnet")
    root.setLevel(logging.DEBUG)
    for h in list(root.handlers):
        root.removeHandler(h)
        h.close()

    fmt = logging.Formatter("%(asctime)s  %(levelname)-5s  %(message)s", "%H:%M:%S")

    con = logging.StreamHandler(sys.stdout)
    con.setLevel(logging.DEBUG if verbose else logging.INFO)
    con.setFormatter(fmt)
    root.addHandler(con)

    if run_dir is not None:
        run_dir.mkdir(parents=True, exist_ok=True)
        fh = logging.FileHandler(run_dir / "session.log")
        fh.setLevel(logging.DEBUG)
        fh.setFormatter(logging.Formatter(
            "%(asctime)s  %(levelname)-5s  %(message)s", "%Y-%m-%d %H:%M:%S"))
        root.addHandler(fh)


def new_run_dir(name: str) -> Path:
    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime()) + "_" + str(time.time_ns())
    d = config.LOG_DIR / f"{stamp}_{name}"
    d.mkdir(parents=True, exist_ok=True)
    latest = config.LOG_DIR / "latest"
    try:
        if latest.is_symlink() or latest.exists():
            latest.unlink()
        latest.symlink_to(d.name)
    except OSError:
        pass
    return d


def sh(cmd, *, check=True, capture=True, timeout=20, quiet=False):
    shell = isinstance(cmd, str)
    if not quiet:
        log.debug("$ %s", cmd if shell else " ".join(map(str, cmd)))
    r = subprocess.run(
        cmd, shell=shell, text=True, timeout=timeout,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    if check and r.returncode != 0:
        raise RuntimeError(
            f"command failed ({r.returncode}): {cmd}\n"
            f"{(r.stderr or '').strip()}"
        )
    return r


def sh_ok(cmd) -> bool:
    try:
        return sh(cmd, check=False, quiet=True).returncode == 0
    except Exception:
        return False


sys.path.insert(0,str(config.REPO_ROOT / "evaluations"))
from metrics import parse_jsonl, summarise, load_events, provenance, common_rates, jain, convergence_ms, offered_utilization, payload_equivalent

def count_events(path, etype):
    return sum(e.get("type")==etype for e in load_events(path))

def save(data: dict, name: str) -> Path:
    config.RESULTS_DIR.mkdir(exist_ok=True)
    out = config.RESULTS_DIR / f"{config.RESULT_PREFIX}{name}_{time.time_ns()}.json"
    out.write_text(json.dumps({**data, "provenance":provenance()}, indent=2, allow_nan=False))
    log.info("wrote %s", out)
    return out


def plot_single(*args, **kwargs):
    from utils import plot_single as plot
    return plot(*args, **kwargs)

def write_tabular(rows: list[dict], path: Path) -> None:
    with open(path, "w") as f:
        f.write(f"{'label':<25} {'avg_rate':>10} {'avg_delay':>10} "
                f"{'p99_delay':>10} {'stall_100ms%':>12}\n")
        f.write("-" * 75 + "\n")
        for s in rows:
            s = {**s, "avg_delay":s["avg_delay"] if s["avg_delay"] is not None else float("nan"), "p99_delay":s["p99_delay"] if s["p99_delay"] is not None else float("nan")}
            f.write(f"{s['label']:<25} {s['avg_bitrate']:>9.2f}m "
                    f"{s['avg_delay']:>8.1f}ms {s['p99_delay']:>8.1f}ms "
                    f"{s['stall_100ms'] * 100:>11.3f}%\n")
    log.info("wrote %s", path)

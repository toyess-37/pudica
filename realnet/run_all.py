import argparse
import json
import sys
import time
import types

import config
from common import setup_logging, new_run_dir, write_tabular, log, provenance
from tests import REGISTRY

DEFAULT_TESTS = ["baseline", "const", "step", "jitter", "loss", "bur_accuracy"]


def build_args(cli) -> types.SimpleNamespace:
    return types.SimpleNamespace(
        dur=cli.dur, port=cli.port,
        rate=cli.rate,
        bw1=cli.bw1, bw2=cli.bw2,
        jitter=cli.jitter, loss=cli.loss,
        rtt_lo=cli.rtt_lo, rtt_hi=cli.rtt_hi,
        bw_list=cli.bw_list, bur_dur=cli.bur_dur,
        flows=cli.flows, stagger=cli.stagger,
    )


def main() -> int:
    p = argparse.ArgumentParser(description="Run the two-PC Pudica tests in sequence.",
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--tests", default=",".join(DEFAULT_TESTS),
                   help=f"comma list from {list(REGISTRY)} or 'all'")
    p.add_argument("--dur", type=int, default=config.DUR)
    p.add_argument("--port", type=int, default=config.PORT)
    p.add_argument("--rate", type=float, default=20.0, help="shaped Mbit/s")
    p.add_argument("--bw1", type=float, default=20.0, help="step: high rate")
    p.add_argument("--bw2", type=float, default=10.0, help="step: low rate")
    p.add_argument("--jitter", type=int, default=40, help="jitter test: +/- ms")
    p.add_argument("--loss", type=float, default=2.0, help="loss test: percent")
    p.add_argument("--rtt-lo", dest="rtt_lo", type=int, default=20)
    p.add_argument("--rtt-hi", dest="rtt_hi", type=int, default=80)
    p.add_argument("--bw-list", dest="bw_list", default="5,10,15,20,25")
    p.add_argument("--bur-dur", dest="bur_dur", type=int, default=20)
    p.add_argument("--flows", type=int, default=3)
    p.add_argument("--stagger", type=int, default=10)
    p.add_argument("--skip-preflight", action="store_true")
    cli = p.parse_args()

    run_dir = new_run_dir("run_all")
    setup_logging(run_dir, verbose=True)

    wanted = list(REGISTRY) if cli.tests == "all" else \
        [t.strip() for t in cli.tests.split(",") if t.strip()]
    bad = [t for t in wanted if t not in REGISTRY]
    if bad:
        log.error("unknown tests: %s   (known: %s)", bad, list(REGISTRY))
        return 2

    if not cli.skip_preflight:
        import preflight
        if preflight.main() != 0:
            log.error("preflight failed - fix the above or pass --skip-preflight")
            return 1
        setup_logging(run_dir, verbose=True)  # preflight reset handlers

    if config.NO_SHAPE:
        wanted = [t for t in wanted if not REGISTRY[t][1]]
        log.warning("NO_SHAPE=1: running only unshaped tests: %s", wanted)

    args = build_args(cli)
    log.info("test plan: %s", wanted)
    results, t_start = {}, time.time()

    for name in wanted:
        fn = REGISTRY[name][0]
        t0 = time.time()
        try:
            results[name] = {"ok": True, "result": fn(args),
                             "secs": round(time.time() - t0, 1)}
            log.info("[OK]   %s  (%.1fs)", name, time.time() - t0)
        except Exception as e:
            results[name] = {"ok": False, "error": str(e),
                             "secs": round(time.time() - t0, 1)}
            log.exception("[FAIL] %s: %s", name, e)

    # combined outputs
    summary = {
        "provenance":provenance(),
        "device_ip": config.DEVICE_IP, "min_rtt_ms": config.MIN_RTT_MS,
        "no_shape": config.NO_SHAPE, "dur_s": cli.dur,
        "when": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "tests": {k: (v.get("result") if v["ok"] else {"error": v["error"]})
                  for k, v in results.items()},
    }
    out = config.RESULTS_DIR / f"{config.RESULT_PREFIX}summary_{time.time_ns()}.json"
    config.RESULTS_DIR.mkdir(exist_ok=True)
    out.write_text(json.dumps(summary, indent=2, default=str, allow_nan=False))
    log.info("wrote %s", out)

    rows = []
    for k, v in results.items():
        r = v.get("result")
        if isinstance(r, dict) and "label" in r:
            rows.append(r)
        elif isinstance(r, dict) and "summary" in r:
            rows.append(r["summary"])
    if rows:
        write_tabular(rows, config.RESULTS_DIR /
                      f"{config.RESULT_PREFIX}tabular_summary_{time.time_ns()}.txt")

    n_ok = sum(1 for v in results.values() if v["ok"])
    log.info("=" * 60)
    log.info("%d/%d tests ok in %.1fs   logs: %s",
             n_ok, len(results), time.time() - t_start, run_dir)
    return 0 if n_ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())

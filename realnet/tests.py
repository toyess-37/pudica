import json
import logging

import numpy as np

import config
from common import count_events, save, summarise
from session import run_session

log = logging.getLogger("realnet")


def baseline(a):
    r = run_session(name="baseline", dur=a.dur, port=a.port,
                    plot_title="baseline over the real link")
    save({"test": "baseline", "summary": r["summary"]}, "baseline")
    return r["summary"]


def const(a):
    bw = a.rate
    r = run_session(name=f"const_{bw}", dur=a.dur, port=a.port, rate_mbit=bw,
                    plot_title=f"constant {bw} Mbit/s")
    br = r["bitrates"]
    body = {
        "test": f"const_{bw}", "bw_Mbps": bw,
        "std_bitrate": round(float(np.std(br)), 3) if br else 0,
        "summary": r["summary"],
    }
    save(body, f"const_{bw}")
    return r["summary"]


def step(a):
    bw1, bw2 = a.bw1, a.bw2
    swap = a.dur / 3
    sched = [(swap, {"rate_mbit": bw2}), (2 * swap, {"rate_mbit": bw1})]
    r = run_session(name="step", dur=a.dur, port=a.port, rate_mbit=bw1,
                    schedule=sched,
                    plot_title=f"step {bw1} -> {bw2} -> {bw1} Mbit/s")

    from common import convergence_ms, payload_equivalent
    br = r["bitrates"]
    # Shaped rates are link capacity; delivered video payload can only reach
    # its payload equivalent. Real video frames vary, so judge a 0.5 s mean.
    down, up = payload_equivalent(bw2), payload_equivalent(bw1)
    body = {"test":"step", "target_down_Mbps":down, "target_up_Mbps":up, "smooth_ms":500,
            "convergence_down_ms":convergence_ms(br,swap*1000,down,end_ms=2*swap*1000,smooth_ms=500),
            "convergence_up_ms":convergence_ms(br,2*swap*1000,up,smooth_ms=500),
            "summary":r["summary"]}
    save(body, "step")
    return r["summary"]


def jitter(a):
    r = run_session(name="jitter", dur=a.dur, port=a.port, rate_mbit=a.rate,
                    jitter_ms=a.jitter,
                    plot_title=f"{a.jitter} ms delay jitter on {a.rate} Mbit/s")
    save({"test": "jitter", "jitter_ms": a.jitter, "summary": r["summary"]},
         "jitter")
    return r["summary"]


def loss(a):
    r = run_session(name="loss", dur=a.dur, port=a.port, rate_mbit=a.rate,
                    loss_pct=a.loss,
                    plot_title=f"{a.loss}% loss on {a.rate} Mbit/s")
    body = {
        "test": "loss", "loss_pct": a.loss,
        "frame_loss_events": count_events(r["sender_jsonl"], "FRAME_LOSS"),
        "summary": r["summary"],
    }
    save(body, "loss")
    return r["summary"]


def rtt_sensitivity(a):
    out = {}
    for tag, target in (("rtt_lo", a.rtt_lo), ("rtt_hi", a.rtt_hi)):
        r = run_session(name=f"rtt_sensitivity_{tag}", dur=a.dur, port=a.port,
                        rate_mbit=a.rate, target_rtt_ms=target,
                        plot_title=f"{tag}")
        out[tag] = r["summary"]
    lo = out["rtt_lo"]["avg_bitrate"]
    hi = out["rtt_hi"]["avg_bitrate"]
    body = {
        "test": "rtt_sensitivity", "flows": 1, "runs": "sequential",
        "rtt_lo_ms": a.rtt_lo, "rtt_hi_ms": a.rtt_hi,
        "lo_avg_Mbps": lo, "hi_avg_Mbps": hi,
        "hi_over_lo_throughput": round(hi / lo, 3) if lo > 0 else None,
        "per_rtt": out,
    }
    save(body, "rtt_sensitivity")
    return body


def bur_accuracy(a):
    rows = []
    for bw in [float(x) for x in str(a.bw_list).split(",")]:
        r = run_session(name=f"bur_{bw}", dur=a.bur_dur, port=a.port,
                        rate_mbit=bw, plot_title=f"bur @ {bw} Mbit/s")
        burs = r["burs"]
        from common import offered_utilization
        rows.append({"bw":bw, **offered_utilization(r["bitrates"].events,bw),
                     "avg_bitrate":r["summary"]["avg_bitrate"], "run_dir":str(r["run_dir"])})
        log.info("bw=%s Mbit/s  converged bur=%.4f", bw, rows[-1]["est_bur"])
    save({"test": "bur_accuracy", "data": rows}, "bur_accuracy")
    return rows


def fairness(a):
    import contextlib
    import subprocess
    import time
    import netshape
    import remote
    from common import new_run_dir,parse_jsonl,common_rates,jain,provenance
    from gamesim import GameSim
    from session import measure_rtt_ms,stop_process
    if a.flows<=0 or a.stagger<0 or a.dur<=(a.flows-1)*a.stagger:
        raise ValueError("fairness needs positive flow count and a common active window")
    run=new_run_dir("fairness")
    ports=list(range(a.port,a.port+a.flows))
    logs=[run/f"flow{i}.jsonl" for i in range(a.flows)]
    metadata={**provenance(),"ports":ports,"workload":config.GAME_BACKEND,"ok":False}
    try:
        with contextlib.ExitStack() as cleanup:
            game=GameSim();cleanup.callback(game.stop);game.start()
            for i,port in enumerate(ports):
                cleanup.callback(remote.fetch_receiver_log,port,run/f"receiver{i}.log")
                cleanup.callback(remote.stop_receiver,port);remote.start_receiver(port)
            if not config.NO_SHAPE:
                rtt=measure_rtt_ms(config.DEVICE_IP)
                if config.MIN_RTT_MS and rtt is None:raise RuntimeError("destination RTT unavailable")
                delay=max(0,config.MIN_RTT_MS-(rtt or 0))
                iface=netshape.pick_iface(config.DEVICE_IP) if config.SHAPE_IFACE=="auto" else config.SHAPE_IFACE
                shaper=netshape.Shaper(iface,config.DEVICE_IP,ports=ports,rate_mbit=a.rate,delay_ms=delay,
                                       limit_pkts=config.QUEUE_PACKETS)
                cleanup.callback(shaper.clear);shaper.apply()
            processes=[]
            for i,port in enumerate(ports):
                if i:time.sleep(a.stagger)
                errors=cleanup.enter_context(open(run/f"sender{i}.stderr","w"))
                process=subprocess.Popen([str(config.SENDER_BIN),"--log",str(logs[i]),
                                          config.DEVICE_IP,str(port),str(a.dur)],
                                         env=game.env,stdin=subprocess.DEVNULL,
                                         stdout=subprocess.DEVNULL,stderr=errors)
                cleanup.callback(stop_process,process);processes.append(process)
            for process in processes:
                if process.wait(timeout=a.dur+10):raise RuntimeError("fairness sender failed")
        data=[parse_jsonl(path) for path in logs]
        rates,common=common_rates([d[1] for d in data])
        body={"test":"fairness","bw_Mbps":a.rate,"flows":a.flows,"run_dir":str(run),
              "common_window_us":common,"delivered_Mbps_per_flow":rates,"jains_index":jain(rates),
              "per_flow":[summarise(*d,label=f"flow{i}") for i,d in enumerate(data)]}
        save(body,"fairness");metadata["ok"]=True;return body
    finally:(run/"meta.json").write_text(json.dumps(metadata,indent=2))


# name -> (function, needs_shaping)
REGISTRY = {
    "baseline": (baseline, False),
    "const": (const, True),
    "step": (step, True),
    "jitter": (jitter, True),
    "loss": (loss, True),
    "rtt_sensitivity": (rtt_sensitivity, True),
    "bur_accuracy": (bur_accuracy, True),
    "fairness": (fairness, True),
}

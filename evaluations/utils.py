import json
import logging
import math
import os
import shlex
import signal
import subprocess
import time
import uuid
from pathlib import Path
import numpy as np
from metrics import (Series,load_events,parse_jsonl,summarise,provenance,throughput,
                     common_rates,jain,window,convergence_ms,probe_summary,offered_utilization)
log=logging.getLogger(__name__)
_ROOT=Path(__file__).resolve().parent.parent
SENDER_BIN=str(_ROOT/"codes"/"sender")
RECEIVER_BIN=str(_ROOT/"codes"/"receiver")
RESULTS_DIR=_ROOT/"results"
TRACES_DIR=_ROOT/"traces"
ZEUS_DIR=_ROOT/"zeus_traces"
TARGET_IP='"$PUDICA_TARGET"'
PKT_BITS=1500*8 # Mahimahi grants 1500 bytes per trace timestamp.
FRAME_MS=1000/35

def const_trace(path,bw_mbps,secs):
    phases_trace(path,[(bw_mbps,secs)])

def phases_trace(path,phases):
    previous=0
    with open(path,"w") as stream:
        for bw,end in phases:
            if not math.isfinite(bw) or not math.isfinite(end) or bw<=0 or end*1000<=previous:raise ValueError("positive capacity and increasing phase ends required")
            t=previous
            while t<end*1000:
                stream.write(f"{int(t)}\n");t+=PKT_BITS/(bw*1000)
            previous=end*1000

def step_trace(path,bw1,bw2,swap_s,secs):
    if not 0<swap_s<secs:raise ValueError("swap must be within duration")
    phases_trace(path,[(bw1,swap_s),(bw2,secs)])

def make_script(path,cmds):
    script=Path(path)/"run.sh"
    script.write_text('#!/usr/bin/env bash\nset -euo pipefail\nexport PUDICA_TARGET="$MAHIMAHI_BASE"\n'+"\n".join(cmds)+"\n")
    script.chmod(0o755);return script

def sender_cmd(port,dur,log_path,bmin=None,bmax=None):
    cmd=[SENDER_BIN,"--synthetic","--log",str(log_path)]
    if bmin is not None:cmd+=["--bmin",str(bmin)]
    if bmax is not None:cmd+=["--bmax",str(bmax)]
    return shlex.join(cmd)+f" {TARGET_IP} {int(port)} {float(dur)}"

def cleanup(procs):
    errors=[]
    for proc in reversed(procs):
        if proc and proc.poll() is None:
            try:
                os.killpg(proc.pid,signal.SIGTERM)
                try:proc.wait(timeout=3)
                except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait(timeout=3)
            except ProcessLookupError:proc.wait(timeout=3)
            except Exception as error:errors.append(error)
    if errors:raise RuntimeError(f"process cleanup failed: {errors}")

def wait_for_experiment(proc,timeout_s):
    try:code=proc.wait(timeout=timeout_s)
    except subprocess.TimeoutExpired as error:raise RuntimeError("experiment timed out") from error
    if code:raise RuntimeError(f"experiment exited with status {code}")
    return True

def new_run_dir(name):
    path=RESULTS_DIR/"runs"/f"{name}_{time.time_ns()}_{uuid.uuid4().hex[:8]}"
    path.mkdir(parents=True);return path

def emulate(up,down,dur,rtt,ports,*,starts=None,extra_delays=None,queue=None,
            bmin=None,bmax=None,extra_commands=(),servers=(),name="experiment"):
    if not math.isfinite(dur) or dur<=0 or rtt<0 or rtt%2:raise ValueError("positive duration and nonnegative even RTT required")
    if not ports or len(set(ports))!=len(ports) or any(not 1<=p<=65535 for p in ports):
        raise ValueError("distinct valid receiver ports required")
    up=Path(up).resolve();down=Path(down).resolve()
    if not up.is_file() or not down.is_file():raise FileNotFoundError("both traces are required")
    run=new_run_dir(name);procs=[];files=[]
    starts=starts or [0]*len(ports);extra_delays=extra_delays or [0]*len(ports)
    if len(starts)!=len(ports) or len(extra_delays)!=len(ports):raise ValueError("flow configuration mismatch")
    if any(x<0 or x%2 for x in extra_delays) or any(not math.isfinite(x) or x<0 for x in starts):raise ValueError("invalid delay/stagger")
    logs=[run/f"flow{i}.jsonl" for i in range(len(ports))]
    metadata={**provenance(),"workload":"synthetic fixed payload", "rtt_ms":rtt,
              "ports":ports,"duration_s":dur,"starts_s":starts,"extra_rtt_ms":extra_delays,
              "uplink":str(run/"uplink.trace"),"downlink":str(run/"downlink.trace"),
              "bmin_mbps":.2 if bmin is None else bmin,"bmax_mbps":50 if bmax is None else bmax,
              "queue_packets":queue}
    try:
        for i,port in enumerate(ports):
            stdout=open(run/f"receiver{i}.log","w");files.append(stdout)
            p=subprocess.Popen([RECEIVER_BIN,str(port),"--headless"],stdout=stdout,stderr=stdout,start_new_session=True)
            procs.append(p)
            deadline=time.monotonic()+5
            while "listening on" not in (run/f"receiver{i}.log").read_text():
                if p.poll() is not None:raise RuntimeError(f"receiver failed: {(run/f'receiver{i}.log').read_text()}")
                if time.monotonic()>deadline:raise RuntimeError("receiver readiness timeout")
                time.sleep(.02)
        for command in servers:
            p=subprocess.Popen(command,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True);procs.append(p)
        commands=["pids=()"]
        for i,(port,start,extra) in enumerate(zip(ports,starts,extra_delays)):
            command=sender_cmd(port,dur,logs[i],bmin,bmax)
            if extra:command=f"mm-delay {extra//2} {command}"
            commands.append(f"(sleep {float(start)}; exec {command}) >{shlex.quote(str(run/f'sender{i}.out'))} 2>{shlex.quote(str(run/f'sender{i}.err'))} &")
            commands.append('pids+=("$!")')
        for command in extra_commands:commands.extend([command+" &",'pids+=("$!")'])
        commands+=['status=0','for pid in "$'+'{pids[@]}"; do wait "$pid" || status=1; done','exit "$status"']
        script=make_script(run,commands)
        cmd=["mm-delay",str(rtt//2),"mm-link"]
        if queue is not None:
            if queue<=0:raise ValueError("queue must be positive")
            cmd+=["--uplink-queue=droptail",f"--uplink-queue-args=packets={queue}"]
        cmd+=[str(up),str(down),"--",str(script)]
        output=open(run/"mahimahi.log","w");files.append(output)
        p=subprocess.Popen(cmd,stdout=output,stderr=output,start_new_session=True);procs.append(p)
        wait_for_experiment(p,dur+max(starts,default=0)+15)
        for proc in procs[:-1]:
            if proc.poll() is not None:raise RuntimeError(f"receiver/server exited unexpectedly ({proc.returncode})")
        metadata["ok"]=True
        return [parse_jsonl(path) for path in logs],run
    except Exception:metadata["ok"]=False;raise
    finally:
        try:cleanup(procs)
        finally:
            for stream in files:stream.close()
            (run/"meta.json").write_text(json.dumps(metadata,indent=2))
            import shutil
            shutil.copyfile(up,run/"uplink.trace");shutil.copyfile(down,run/"downlink.trace")

def save(data,prefix):
    RESULTS_DIR.mkdir(exist_ok=True)
    out=RESULTS_DIR/f"current_{prefix}_{time.time_ns()}.json"
    out.write_text(json.dumps({**data,"provenance":provenance()},indent=2,allow_nan=False));return out

def smooth(data,window=10):
    if window<=1 or len(data)<window:return np.asarray(data)
    return np.convolve(data,np.ones(window)/window,mode="same")

def plot_single(burs,bitrates,delays,title="",out_svg="out.svg",window=10):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig,axes=plt.subplots(3,1,figsize=(12,9),sharex=True);fig.suptitle(title)
    for ax,data,label in zip(axes,(bitrates,delays,burs),
                             ("Delivered payload per offered frame (Mbps)","ACK completion round-trip (ms)","BUR")):
        ax.plot(data.times,data,alpha=.5);ax.set_ylabel(label);ax.grid(True)
    axes[-1].set_xlabel("elapsed time (ms)");fig.tight_layout();fig.savefig(out_svg);plt.close(fig)

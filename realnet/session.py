import contextlib
import json
import logging
import math
import re
import subprocess
import time
from pathlib import Path
import config
import netshape
import remote
from common import parse_jsonl,sh,provenance
from gamesim import GameSim
log=logging.getLogger("realnet")

def measure_rtt_ms(host,count=5):
    result=sh(["ping","-n","-c",str(count),"-w",str(count+3),host],check=False,quiet=True,timeout=count+5)
    match=re.search(r"=\s*[\d.]+/([\d.]+)/",result.stdout)
    return float(match.group(1)) if result.returncode==0 and match else None

def stop_process(process):
    if process.poll() is None:
        process.terminate()
        try:process.wait(timeout=3)
        except subprocess.TimeoutExpired:process.kill();process.wait(timeout=3)

def _sender_cmd(run_dir,port,dur,bmin,bmax):
    command=[str(config.SENDER_BIN),"--log",str(run_dir/"sender.jsonl")]
    if bmin is not None:command+=["--bmin",str(bmin)]
    if bmax is not None:command+=["--bmax",str(bmax)]
    return command+[config.DEVICE_IP,str(port),str(dur)]

def run_session(*,name,dur,port,rate_mbit=None,delay_ms=0,jitter_ms=0,loss_pct=0,
                schedule=None,bmin=None,bmax=None,plot_title=None,target_rtt_ms=None):
    from common import new_run_dir,plot_single,summarise
    if dur<=0 or delay_ms<0:raise ValueError("invalid duration/delay")
    schedule=sorted(schedule or [])
    if any(t<0 or t>=dur for t,_ in schedule):raise ValueError("schedule change outside experiment")
    run=new_run_dir(name)
    rtt=measure_rtt_ms(config.DEVICE_IP)
    floor=config.MIN_RTT_MS if target_rtt_ms is None else target_rtt_ms
    if floor<0:raise ValueError("invalid RTT target")
    if floor and rtt is None:raise RuntimeError("cannot calculate RTT padding when destination ping failed")
    # Only outgoing data is delayed, so added delay contributes once to RTT.
    padding=0 if config.NO_SHAPE else max(0,math.ceil(floor-(rtt or 0)))
    effective_delay=max(delay_ms+padding,jitter_ms) if jitter_ms else delay_ms+padding
    meta={**provenance(),"name":name,"device_ip":config.DEVICE_IP,"dur_s":dur,"port":port,
          "workload":config.GAME_BACKEND,"measured_rtt_ms":rtt,"requested_rtt_ms":floor,
          "netem_delay_ms":effective_delay,"netem_jitter_ms":jitter_ms,"netem_loss_pct":loss_pct,
          "netem_rate_mbit":rate_mbit,"netem_limit_pkts":config.QUEUE_PACKETS,"no_shape":config.NO_SHAPE,"schedule":schedule,
          "started_utc":time.strftime("%Y-%m-%dT%H:%M:%SZ",time.gmtime()),"ok":False}
    try:
        with contextlib.ExitStack() as cleanup:
            game=GameSim();cleanup.callback(game.stop);game.start()
            cleanup.callback(remote.fetch_receiver_log,port,run/"receiver.log")
            cleanup.callback(remote.stop_receiver,port)
            remote.start_receiver(port)
            shaper=None
            if not config.NO_SHAPE:
                iface=netshape.pick_iface(config.DEVICE_IP) if config.SHAPE_IFACE=="auto" else config.SHAPE_IFACE
                shaper=netshape.Shaper(iface,config.DEVICE_IP,ports=[port],rate_mbit=rate_mbit,
                                      delay_ms=effective_delay,jitter_ms=jitter_ms,loss_pct=loss_pct,
                                      limit_pkts=config.QUEUE_PACKETS)
                cleanup.callback(shaper.clear);shaper.apply()
                (run/"netshape_before.txt").write_text(shaper.snapshot())
            errors=cleanup.enter_context(open(run/"sender.stderr","w"))
            sender=subprocess.Popen(_sender_cmd(run,port,dur,bmin,bmax),env=game.env,
                                    stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=errors)
            cleanup.callback(stop_process,sender)
            origin=time.monotonic()
            for at,values in schedule:
                while time.monotonic()<origin+at:
                    if sender.poll() is not None:raise RuntimeError("sender exited before scheduled change")
                    time.sleep(min(.05,max(0,origin+at-time.monotonic())))
                if shaper:shaper.change(**values)
            if sender.wait(timeout=dur+10):raise RuntimeError(f"sender failed; see {run/'sender.stderr'}")
            if shaper:(run/"netshape_after.txt").write_text(shaper.snapshot())
        burs,rates,delays=parse_jsonl(run/"sender.jsonl")
        summary=summarise(burs,rates,delays,label=name)
        plot_single(burs,rates,delays,title=plot_title or name,out_svg=str(run/f"{name}.svg"))
        meta["ok"]=True
        return {"run_dir":run,"meta":meta,"summary":summary,"burs":burs,"bitrates":rates,
                "delays":delays,"sender_jsonl":run/"sender.jsonl"}
    finally:
        meta["finished_utc"]=time.strftime("%Y-%m-%dT%H:%M:%SZ",time.gmtime())
        (run/"meta.json").write_text(json.dumps(meta,indent=2,allow_nan=False))

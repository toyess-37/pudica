import argparse
import json
import math
import os
import shlex
import shutil
import tempfile
from pathlib import Path
from utils import (
    ZEUS_DIR, PKT_BITS, TARGET_IP, Series, emulate, const_trace, phases_trace,
    save, summarise, offered_utilization, common_rates, jain, window,
    convergence_ms, throughput, plot_single, _ROOT,
)
from metrics import payload_equivalent, wire_throughput, TCP_WIRE_FACTOR
SMOOTH_MS=500 # convergence is judged on a 0.5 s trailing mean of delivered payload

def parser(kind):
    p=argparse.ArgumentParser(description=f"{kind}: synthetic workload; measured delivered payload throughput")
    p.add_argument("--dur",type=int,default=30);p.add_argument("--rtt",type=int,default=20)
    p.add_argument("--port",type=int,default=9700);p.add_argument("--plot",action="store_true")
    p.add_argument("--bw",type=float,default=20)
    p.add_argument("--bw1",type=float,default=20);p.add_argument("--bw2",type=float,default=10)
    p.add_argument("--bw3",type=float,default=None);p.add_argument("--swap",type=float,default=None)
    p.add_argument("--swap2",type=float,default=None);p.add_argument("--flows",type=int,default=3)
    p.add_argument("--stagger",type=float,default=5)
    p.add_argument("--target",type=float,default=None,help="convergence target in delivered payload Mbps (default: payload equivalent of the capacity share)")
    p.add_argument("--rtt0",type=int,default=20);p.add_argument("--rtt1",type=int,default=60)
    p.add_argument("--jitter",type=int,default=40);p.add_argument("--period",type=int,default=500)
    p.add_argument("--bw-list",default="5,10,15,20,25")
    p.add_argument("--fractions",default=".25,.5,.75")
    p.add_argument("--buf",type=int,default=7);p.add_argument("--cubic-delay",type=float,default=5)
    p.add_argument("--cubic-dur",type=int,default=None);p.add_argument("--iperf-port",type=int,default=9300)
    p.add_argument("--trace");p.add_argument("--traces",default=str(ZEUS_DIR))
    p.add_argument("--full",action="store_true");p.add_argument("--filter",default="")
    return p

def trace_duration(path):
    last=None
    with open(path) as stream:
        for line in stream:
            if not line.strip():continue
            value=int(line)
            if value<0 or (last is not None and value<last):raise ValueError("trace timestamps must be monotonic")
            last=value
    if last is None:raise ValueError(f"empty trace: {path}")
    return max(1,(last+999)//1000)

def find_mahimahi_traces(filter_str=""):
    directories=[Path(os.environ.get("MAHIMAHI_TRACE_DIR","/usr/share/mahimahi/traces")),
                 _ROOT.parent/"mahimahi"/"traces"]
    return [(up.stem,up,up.with_suffix(".down")) for d in directories for up in sorted(d.glob("*.up"))
            if up.with_suffix(".down").is_file() and filter_str.lower() in up.stem.lower()]

def run_trace(name,up,down,rtt,dur,port):
    data,run=emulate(up,down,dur,rtt,[port],name=name)
    return {**summarise(*data[0],label=name),"run_dir":str(run)}

def main(kind):
    a=parser(kind).parse_args()
    if a.dur<=0 or not math.isfinite(a.bw) or a.bw<=0:raise ValueError("positive duration and capacity required")
    with tempfile.TemporaryDirectory(prefix="pudica-input-") as directory:
        trace=Path(directory)/"uplink.trace"
        if kind in ("zeus_trace","zeus_batch","tests_mm"):
            if kind=="zeus_trace":
                if not a.trace:raise ValueError("--trace is required")
                up=Path(a.trace);up=up if up.is_file() else ZEUS_DIR/a.trace
                pairs=[(up.stem,up,up)]
            elif kind=="zeus_batch":
                path=Path(a.traces);path=path if path.exists() else ZEUS_DIR/a.traces
                paths=[path] if path.is_file() else sorted(path.glob("*.up"))
                pairs=[(x.stem,x,x) for x in paths]
            else:pairs=find_mahimahi_traces(a.filter)
            if not pairs:raise FileNotFoundError("no external traces found; install/configure traces or omit this optional suite")
            rows=[run_trace(name,up,down,a.rtt,trace_duration(up) if a.full else a.dur,a.port)
                  for name,up,down in pairs]
            print(save({"test":kind,"results":rows},kind));return
        if kind=="bur_accuracy":
            rows=[]
            for bw in map(float,a.bw_list.split(",")):
                const_trace(trace,bw,a.dur+2)
                for fraction in map(float,a.fractions.split(",")):
                    if not 0<fraction<1:raise ValueError("BUR offered fractions must be between zero and one")
                    # The fraction is of link capacity on the wire; pick the
                    # payload rate whose data+FEC+probe load matches it.
                    rate=payload_equivalent(bw*fraction)
                    data,run=emulate(trace,trace,a.dur,a.rtt,[a.port],bmin=rate,bmax=rate,name="bur")
                    rows.append({"bw":bw,"wire_fraction":fraction,"payload_mbps":rate,
                                 **offered_utilization(data[0][1].events,bw),
                                 "summary":summarise(*data[0]),"run_dir":str(run)})
            print(save({"test":kind,"data":rows},kind));return
        ports=[a.port];starts=[0];extra=None
        total=a.dur
        if kind in ("fairness","convergence"):
            if a.flows<=0 or not math.isfinite(a.stagger) or a.stagger<0:raise ValueError("invalid flow count/stagger")
            if a.dur<=(a.flows-1)*a.stagger:raise ValueError("duration must leave a common active window")
            ports=list(range(a.port,a.port+a.flows));starts=[i*a.stagger for i in range(a.flows)]
            total+=starts[-1]
        elif kind=="different_rtprop":
            if a.rtt1<a.rtt0:raise ValueError("--rtt1 must be at least --rtt0")
            ports=[a.port,a.port+1];starts=[0,0];extra=[0,a.rtt1-a.rtt0];a.rtt=a.rtt0
        if kind=="step":
            swap=a.swap if a.swap is not None else a.dur/3
            phases=[(a.bw1,swap)]
            if a.bw3 is not None:
                swap2=a.swap2 if a.swap2 is not None else 2*a.dur/3
                phases.extend([(a.bw2,swap2),(a.bw3,a.dur+2)])
            else:phases.append((a.bw2,a.dur+2))
            if swap<=0 or swap>=a.dur or (a.bw3 is not None and not swap<swap2<a.dur):
                raise ValueError("bandwidth changes must fall within duration")
            phases_trace(trace,phases)
        elif kind=="service_gaps":
            if a.period<=0 or a.jitter<0:raise ValueError("invalid service-gap parameters")
            with open(trace,"w") as stream:
                t=0.;cycle=0.
                while t<(a.dur+2)*1000:
                    stream.write(f"{int(t)}\n")
                    gap=PKT_BITS/(a.bw*1000);t+=gap;cycle+=gap
                    if cycle>=a.period:t+=a.jitter;cycle=0
        else:const_trace(trace,a.bw,total+2)
        if kind=="cubic":
            if not shutil.which("iperf3"):raise RuntimeError("iperf3 required")
            cubic_dur=a.cubic_dur if a.cubic_dur is not None else int(a.dur-a.cubic_delay)
            if not math.isfinite(a.cubic_delay) or a.cubic_delay<0 or cubic_dur<=0 or a.cubic_delay+cubic_dur>a.dur:
                raise ValueError("CUBIC window must fall within Pudica duration")
            iperf=Path(directory)/"iperf.json";mark=Path(directory)/"cubic_start"
            marker=shlex.join(["python3","-c",f"import time;open({str(mark)!r},'w').write(str(time.monotonic_ns()//1000))"])
            command=f"(sleep {a.cubic_delay}; {marker}; iperf3 -C cubic -c {TARGET_IP} -p {a.iperf_port} -t {cubic_dur} --get-server-output -J >{shlex.quote(str(iperf))})"
            data,run=emulate(trace,trace,a.dur,a.rtt,ports,queue=a.buf,extra_commands=[command],
                              servers=[["iperf3","-s","-p",str(a.iperf_port),"-J"]],name=kind)
            report=json.loads(iperf.read_text())
            if "error" in report:raise RuntimeError(report["error"])
            server=report.get("server_output_json")
            if not isinstance(server,dict) or "intervals" not in server:
                raise RuntimeError("iperf receiver interval data missing")
            intervals=server["intervals"]
            anchor=int(mark.read_text())
            own_start,own_end=window(data[0][1].events)
            intervals=[i for i in intervals
                       if anchor+round(i["sum"]["start"]*1e6)>=own_start
                       and anchor+round(i["sum"]["end"]*1e6)<=own_end]
            if not intervals:raise ValueError("no complete CUBIC receiver interval in common window")
            start=anchor+round(intervals[0]["sum"]["start"]*1e6)
            end=anchor+round(intervals[-1]["sum"]["end"]*1e6)
            cubic=sum(i["sum"]["bytes"] for i in intervals)*8/(end-start)
            pudica=throughput(data[0][1].events,start,end)
            # Shares compare link occupancy: Pudica carries ~30% FEC/header
            # overhead, TCP ~3.5%; comparing payloads understates Pudica.
            pudica_wire=wire_throughput(data[0][1].events,start,end);cubic_wire=cubic*TCP_WIRE_FACTOR
            shutil.copyfile(iperf,run/"iperf.json")
            body={"test":kind,"pudica_avg_Mbps":pudica,"cubic_Mbps":cubic,
                  "pudica_wire_Mbps":pudica_wire,"cubic_wire_Mbps":cubic_wire,
                  "pudica_share":pudica_wire/(pudica_wire+cubic_wire) if pudica_wire+cubic_wire else None,
                  "share_metric":"IP-level link occupancy (Pudica data+FEC+probes; TCP payload x 1500/1448)",
                  "window_us":[start,end],"window_alignment":"receiver intervals aligned to client launch; includes connection setup offset",
                  "cubic_metric":"received TCP payload Mbps",
                  "summary":summarise(*data[0]),"run_dir":str(run)}
        else:
            data,run=emulate(trace,trace,a.dur,a.rtt,ports,starts=starts,extra_delays=extra,name=kind)
            body={"test":kind,"run_dir":str(run),"summary":summarise(*data[0])}
            if len(data)>1:
                rates,common=common_rates([d[1] for d in data])
                body.update(per_flow=[summarise(*d,label=f"flow{i}") for i,d in enumerate(data)],
                            common_window_us=common,delivered_Mbps_per_flow=rates,jains_index=jain(rates))
                if kind=="different_rtprop":
                    body["throughput_ratio"]=max(rates)/min(rates) if min(rates)>0 else None
            if kind=="convergence":
                from_ms=starts[-1]*1000;target=a.target or payload_equivalent(a.bw/a.flows)
                origin=min(window(d[1].events)[0] for d in data)
                body.update(target_payload_Mbps=target,smooth_ms=SMOOTH_MS,convergence_ms_per_flow=[
                    convergence_ms(Series(d[1],times=[t+(window(d[1].events)[0]-origin)/1000 for t in d[1].times]),
                                   from_ms,target,smooth_ms=SMOOTH_MS) for d in data])
            if kind=="step":
                down=a.target or payload_equivalent(a.bw2)
                body.update(target_down_Mbps=down,smooth_ms=SMOOTH_MS,
                            convergence_down_ms=convergence_ms(data[0][1],swap*1000,down,smooth_ms=SMOOTH_MS,
                                                                  end_ms=swap2*1000 if a.bw3 is not None else a.dur*1000))
                if a.bw3 is not None:
                    up=payload_equivalent(a.bw3)
                    body.update(target_up_Mbps=up,
                                convergence_up_ms=convergence_ms(data[0][1],swap2*1000,up,smooth_ms=SMOOTH_MS))
            if kind=="service_gaps":body.update(service_gap_ms=a.jitter,service_period_ms=a.period,impairment="link service gaps")
        output=save(body,kind);print(output)
        if a.plot:plot_single(*data[0],title=kind,out_svg=str(output.with_suffix(".svg")))

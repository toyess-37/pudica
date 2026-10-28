import json
import math
import statistics
import subprocess
from pathlib import Path

# Wire format (codes/protocol.h): 46-byte header, 1200-byte payload padded to full
# size, one XOR parity per group of up to 4 packets (none for a lone packet),
# four header-only probes per frame; 28 bytes of IPv4/UDP per datagram.
HEADER,LOAD,FEC_K,PROBES,IP_UDP=46,1200,4,4,28
FRAME_US=1e6/35
TCP_WIRE_FACTOR=1500/1448 # IPv4+TCP with timestamps: 1448 payload bytes per 1500-byte packet

def frame_wire_bytes(payload_bytes):
    packets=math.ceil(payload_bytes/LOAD)
    parity=sum(min(FEC_K,packets-i)>1 for i in range(0,packets,FEC_K))
    return (packets+parity)*(HEADER+LOAD+IP_UDP)+PROBES*(HEADER+IP_UDP)

def payload_equivalent(capacity_mbps,frame_interval_us=FRAME_US):
    if not math.isfinite(capacity_mbps) or capacity_mbps<=0:raise ValueError("positive capacity required")
    low,high=0.0,capacity_mbps
    for _ in range(60):
        mid=(low+high)/2
        wire=frame_wire_bytes(mid*frame_interval_us/8)*8/frame_interval_us
        low,high=(mid,high) if wire<=capacity_mbps else (low,mid)
    return low

class Series(list):
    def __init__(self, values=(), *, times=(), events=()):
        super().__init__(values)
        self.times=list(times)
        self.events=list(events)

def load_events(path):
    events=[]
    with open(path) as stream:
        for number,line in enumerate(stream,1):
            if line.strip():
                try: event=json.loads(line)
                except json.JSONDecodeError as error: raise ValueError(f"{path}:{number}: malformed JSON") from error
                if not isinstance(event,dict):raise ValueError(f"{path}:{number}: expected an object")
                events.append(event)
    return events

def parse_jsonl(path):
    events=load_events(path)
    sent=[e for e in events if e.get("type")=="FRAME_SENT"]
    if not sent:raise ValueError(f"{path}: no version-2 FRAME_SENT events; legacy target logs cannot measure delivery")
    origin=sent[0]["ts_microsecs"]
    delivered={e["fid"]:e for e in events if e.get("type")=="FRAME_DELIVERED"}
    samples=[e for e in events if e.get("type")=="CONTROL_SAMPLE"]
    def series(values,records):
        return Series(values,times=[(e["ts_microsecs"]-origin)/1000 for e in records],events=events)
    return (series([e["bur"] for e in samples],samples),
            series([8*e["bytes"]/e["frame_interval_us"] if e["fid"] in delivered else 0 for e in sent],sent),
            series([e["delay_ms"] for e in delivered.values()],list(delivered.values())))

def window(events):
    frames=[e for e in events if e.get("type")=="FRAME_SENT"]
    if not frames:raise ValueError("no sent frames")
    return frames[0]["ts_microsecs"],max(e["ts_microsecs"]+e["frame_interval_us"] for e in frames)

def throughput(events,start=None,end=None):
    own_start,own_end=window(events)
    start=own_start if start is None else start
    end=own_end if end is None else end
    if end<=start:raise ValueError("empty measurement window")
    delivered={e["fid"] for e in events if e.get("type")=="FRAME_DELIVERED"}
    count=sum(e["bytes"] for e in events if e.get("type")=="FRAME_SENT"
              and start<=e["ts_microsecs"]<end and e["fid"] in delivered)
    return 8*count/(end-start)

def wire_throughput(events,start=None,end=None):
    own_start,own_end=window(events)
    start=own_start if start is None else start
    end=own_end if end is None else end
    if end<=start:raise ValueError("empty measurement window")
    delivered={e["fid"] for e in events if e.get("type")=="FRAME_DELIVERED"}
    total=sum(frame_wire_bytes(e["bytes"]) for e in events if e.get("type")=="FRAME_SENT"
              and start<=e["ts_microsecs"]<end and e["fid"] in delivered)
    total+=sum(HEADER+LOAD+IP_UDP for e in events if e.get("type")=="PACKET_RETRANSMITTED"
               and start<=e["ts_microsecs"]<end)
    return 8*total/(end-start)

def common_rates(series):
    if not series:raise ValueError("no flows")
    windows=[window(s.events) for s in series]
    start=max(w[0] for w in windows);end=min(w[1] for w in windows)
    if end<=start:raise ValueError("flows have no common active window")
    return [throughput(s.events,start,end) for s in series],(start,end)

def jain(rates):
    if not rates or not any(rates):return 0.0
    return sum(rates)**2/(len(rates)*sum(r*r for r in rates))

def percentile(values,p):
    if not values:return None
    values=sorted(values);position=(len(values)-1)*p/100
    low=int(position);high=min(low+1,len(values)-1)
    return round(values[low]+(values[high]-values[low])*(position-low),3)

def summarise(burs,bitrates,delays,label="flow"):
    events=getattr(bitrates,"events",None)
    if not events:raise ValueError("summary requires version-2 event metadata")
    sent={e["fid"]:e for e in events if e.get("type")=="FRAME_SENT"}
    delivered={e["fid"]:e for e in events if e.get("type")=="FRAME_DELIVERED" and e["fid"] in sent}
    failures=len(sent)-len(delivered)
    def stall(threshold):
        return (failures+sum(e["delay_ms"]>threshold for e in delivered.values()))/len(sent)
    # Includes encoding and time queued at the sender; absent in older logs.
    capture=[e["capture_to_ack_ms"] for e in delivered.values() if e.get("capture_to_ack_ms")]
    return dict(label=label,n_frames=len(sent),delivered_frames=len(delivered),frame_losses=failures,
                avg_bitrate=round(throughput(events),3),bitrate_metric="delivered payload Mbps over offered window",
                avg_target_mbps=round(statistics.mean(e["target_mbps"] for e in sent.values()),3),
                avg_delay=round(statistics.mean(delays),3) if delays else None,
                p95_delay=percentile(delays,95),p99_delay=percentile(delays,99),
                delay_metric="ACK round-trip frame completion ms, delivered frames only",
                stall_100ms=round(stall(100),4),stall_200ms=round(stall(200),4),
                avg_capture_delay=round(statistics.mean(capture),3) if capture else None,
                p95_capture_delay=percentile(capture,95),
                avg_bur=round(statistics.mean(burs),4) if burs else None)

def smoothed(series,smooth_ms):
    times=getattr(series,"times",None)
    if times is None or len(times)!=len(series):raise ValueError("timestamped series required")
    out=[];start=0;total=0.0
    for i,(time,value) in enumerate(zip(times,series)):
        total+=value
        while times[start]<=time-smooth_ms:total-=series[start];start+=1
        out.append(total/(i-start+1))
    return Series(out,times=times,events=getattr(series,"events",()))

def convergence_ms(series,from_ms,target,threshold=.05,window_size=10,end_ms=None,smooth_ms=0):
    if not math.isfinite(target) or target<=0 or window_size<1 or not 0<threshold<1 or smooth_ms<0:
        raise ValueError("invalid convergence target/window/threshold")
    times=getattr(series,"times",None)
    if times is None or len(times)!=len(series):raise ValueError("timestamped series required")
    if smooth_ms:series=smoothed(series,smooth_ms)
    for i,time in enumerate(times):
        if time<from_ms or i+window_size>len(series):continue
        if end_ms is not None and times[i+window_size-1]>=end_ms:continue
        if all(abs(v-target)<=threshold*target for v in series[i:i+window_size]):
            return round(time-from_ms,1)
    return None

def provenance():
    root=Path(__file__).resolve().parents[1]
    def git(*args):
        result=subprocess.run(["git","-C",str(root),*args],capture_output=True,text=True,check=True)
        return result.stdout.strip()
    return {"git_commit":git("rev-parse","HEAD"),"dirty":bool(git("status","--porcelain")),"protocol":2,"log_version":2,"fps":35}

def probe_summary(events):
    frames=[e for e in events if e.get("type")=="PROBE_SUMMARY"]
    if not frames:raise ValueError("no probe summaries; legacy logs do not expose probe loss")
    expected=sum(e["expected"] for e in frames);received=sum(e["received"] for e in frames)
    return dict(frames=len(frames),expected=expected,received=received,lost=expected-received,
                loss_rate=(expected-received)/expected if expected else None)

def offered_utilization(events,bw_mbps,tail_fraction=.6):
    if not math.isfinite(bw_mbps) or bw_mbps<=0 or not 0<=tail_fraction<1:
        raise ValueError("invalid capacity/tail fraction")
    frames=[e for e in events if e.get("type")=="FRAME_SENT"]
    samples={e["fid"]:e for e in events if e.get("type")=="CONTROL_SAMPLE"}
    start,end=window(events);start+=int((end-start)*tail_fraction)
    paired=[e for e in frames if e["ts_microsecs"]>=start and e["fid"] in samples]
    if not paired:raise ValueError("no paired BUR samples in measurement tail")
    retries={}
    for x in events:
        if x.get("type")=="PACKET_RETRANSMITTED":retries[x["fid"]]=retries.get(x["fid"],0)+1
    actual=[];estimated=[]
    for e in paired:
        actual.append(frame_wire_bytes(e["bytes"])*8/(bw_mbps*e["frame_interval_us"]))
        actual[-1]+=retries.get(e["fid"],0)*(HEADER+LOAD+IP_UDP)*8/(bw_mbps*e["frame_interval_us"])
        estimated.append(samples[e["fid"]]["bur"])
    return {"reference_metric":"offered data+FEC+probes serialization / frame interval (single-flow load; not measured cross-traffic occupancy)",
            "offered_utilization":statistics.mean(actual),"est_bur":statistics.mean(estimated),
            "mean_abs_error":statistics.mean(abs(a-b) for a,b in zip(actual,estimated)),"n_pairs":len(paired)}

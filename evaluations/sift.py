import argparse
from metrics import load_events

def check(events):
    bad=[]
    sent={};delivered=set();lost=set();summaries=set();samples={}
    for e in events:
        kind=e.get("type");fid=e.get("fid")
        if kind=="FRAME_SENT":
            if fid in sent:bad.append(f"duplicate send {fid}")
            sent[fid]=e
        elif kind=="FRAME_DELIVERED":
            if fid not in sent or fid in delivered:bad.append(f"invalid delivery {fid}")
            delivered.add(fid)
        elif kind=="FRAME_LOSS":
            if fid not in sent or fid in lost or fid in delivered:bad.append(f"invalid loss {fid}")
            lost.add(fid)
        elif kind=="CONTROL_SAMPLE":
            if fid in samples or fid not in delivered:bad.append(f"invalid controller sample {fid}")
            if e.get("n_probes")!=4:bad.append(f"sample {fid} lacks four distinct probes")
            if e.get("pacing",0)<1:bad.append(f"invalid pacing {fid}")
            samples[fid]=e
        elif kind=="PROBE_SUMMARY":
            if fid not in sent or fid in summaries:bad.append(f"invalid probe summary {fid}")
            if not 0<=e.get("received",-1)<=e.get("expected",-1)==4:bad.append(f"invalid probe counts {fid}")
            summaries.add(fid)
    for e in events:
        if e.get("type")=="DRAIN_START" and e.get("fid",0)>0 and e.get("drain_rate",0)>0:
            fid=e["fid"]
            preceding=[samples[k] for k in sorted(samples) if k<=fid][-3:]
            if len(preceding)!=3 or any(s["bur"]<=1 for s in preceding):
                bad.append(f"drain {fid} lacks three congested samples")
    if set(sent)!=delivered|lost:bad.append("sent frames lack final outcomes")
    if set(sent)!=summaries:bad.append("sent frames lack probe summaries")
    return bad

def main():
    p=argparse.ArgumentParser();p.add_argument("--log",required=True);a=p.parse_args()
    bad=check(load_events(a.log))
    for problem in bad:print(problem)
    print(f"{len(bad)} invariant failure(s)")
    return 1 if bad else 0

if __name__=="__main__":raise SystemExit(main())

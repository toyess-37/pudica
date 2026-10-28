import ipaddress
import json
import logging
import math
import os
import re
import shlex
import shutil
from common import sh
log=logging.getLogger("realnet")
_ROOT_HANDLE="7d1:"
_NETEM_PARENT="7d1:4"
_NETEM_HANDLE="7d4:"

def _tc_bin():
    return shutil.which("tc") or next((p for p in ("/usr/sbin/tc","/sbin/tc") if os.path.exists(p)),"tc")
def _tc(args,check=True):
    return sh(["sudo","-n",_tc_bin(),*shlex.split(args)],check=check,timeout=10)
def have_tc():
    return _tc("qdisc show",check=False).returncode==0
def pick_iface(device_ip):
    host=str(ipaddress.IPv4Address(device_ip))
    result=sh(["ip","-o","route","get",host],timeout=10)
    tokens=result.stdout.split()
    if "dev" not in tokens:raise RuntimeError("no route interface")
    return tokens[tokens.index("dev")+1]
def _netem_opts(rate_mbit,delay_ms,jitter_ms,loss_pct,limit_pkts=None):
    values=[delay_ms,jitter_ms,loss_pct]+([] if rate_mbit is None else [rate_mbit])
    if any(not math.isfinite(float(v)) or float(v)<0 for v in values) or loss_pct>100:
        raise ValueError("invalid shaping parameter")
    if rate_mbit is not None and rate_mbit<=0:raise ValueError("rate must be positive")
    if jitter_ms and delay_ms<=0:raise ValueError("jitter requires a positive base delay")
    # netem gives each packet its own random delay, so jitter reorders packets
    # unless a rate is set: then each one is scheduled after the previous.
    if jitter_ms and not rate_mbit:raise ValueError("jitter needs a rate limit or packets get reordered")
    if limit_pkts is not None and (not isinstance(limit_pkts,int) or limit_pkts<=0):
        raise ValueError("queue limit must be a positive packet count")
    parts=[] if limit_pkts is None else ["limit",str(limit_pkts)]
    if delay_ms>0:
        parts+=["delay",f"{delay_ms}ms"]
        if jitter_ms:parts += [f"{jitter_ms}ms","distribution","normal"]
    if loss_pct:parts+=["loss",f"{loss_pct}%"]
    if rate_mbit:parts+=["rate",f"{rate_mbit}mbit"]
    return " ".join(parts) or "delay 0ms"

class Shaper:
    def __init__(self,iface,device_ip,*,ports,rate_mbit=None,delay_ms=0,jitter_ms=0,loss_pct=0,limit_pkts=None):
        if not re.fullmatch(r"[a-zA-Z0-9_.:-]+",iface):raise ValueError("invalid interface")
        self.iface=iface;self.device_ip=str(ipaddress.IPv4Address(device_ip))
        self.ports=list(ports)
        if not self.ports or any(not isinstance(p,int) or not 1<=p<=65535 for p in self.ports):
            raise ValueError("explicit UDP experiment ports required")
        self.rate_mbit=rate_mbit;self.delay_ms=delay_ms;self.jitter_ms=jitter_ms;self.loss_pct=loss_pct
        self.limit_pkts=limit_pkts
        self.applied=False
    def _qdiscs(self):
        return json.loads(_tc(f"-j qdisc show dev {self.iface}").stdout)
    def _roots(self):
        return [x for x in self._qdiscs() if x.get("root")]
    # Kernel default qdiscs have handle 0: and come back when our root is deleted;
    # anything configured by hand gets a real handle and is left alone.
    @staticmethod
    def _kernel_default(qdiscs):
        return all(x.get("handle","0:")=="0:" for x in qdiscs)
    def apply(self):
        if self.applied:raise RuntimeError("shaper already applied")
        if not self._kernel_default(self._qdiscs()):
            raise RuntimeError("configured qdisc found and preserved; remove it, use another interface, or NO_SHAPE=1")
        opts=_netem_opts(self.rate_mbit,self.delay_ms,self.jitter_ms,self.loss_pct,self.limit_pkts)
        _tc(f"qdisc add dev {self.iface} root handle {_ROOT_HANDLE} prio bands 4")
        self.applied=True
        try:
            _tc(f"qdisc add dev {self.iface} parent {_NETEM_PARENT} handle {_NETEM_HANDLE} netem {opts}")
            for i,port in enumerate(self.ports,1):
                _tc(f"filter add dev {self.iface} protocol ip parent {_ROOT_HANDLE} prio {i} u32 "
                    f"match ip dst {self.device_ip}/32 match ip protocol 17 0xff "
                    f"match ip dport {port} 0xffff flowid {_NETEM_PARENT}")
        except BaseException:self.clear();raise
    def change(self,**values):
        if not self.applied:raise RuntimeError("shaper not applied")
        proposed={key:values.get(key,getattr(self,key)) for key in ("rate_mbit","delay_ms","jitter_ms","loss_pct")}
        unknown=set(values)-set(proposed)
        if unknown:raise TypeError(f"unknown shaping fields: {unknown}")
        opts=_netem_opts(**proposed,limit_pkts=self.limit_pkts)
        _tc(f"qdisc change dev {self.iface} parent {_NETEM_PARENT} handle {_NETEM_HANDLE} netem {opts}")
        for key,value in proposed.items():setattr(self,key,value)
    def clear(self):
        if not self.applied:return
        roots=self._roots()
        if not any(x.get("handle")==_ROOT_HANDLE and x.get("kind")=="prio" for x in roots):
            raise RuntimeError("shaper root changed externally; preserved current qdisc")
        _tc(f"qdisc del dev {self.iface} root handle {_ROOT_HANDLE}")
        self.applied=False
        if not self._kernel_default(self._qdiscs()):
            raise RuntimeError(f"{self.iface}: default qdisc not restored after shaping; check `tc qdisc show`")
    def snapshot(self):return _tc(f"-s qdisc show dev {self.iface}").stdout
    def __enter__(self):self.apply();return self
    def __exit__(self,*exc):self.clear()

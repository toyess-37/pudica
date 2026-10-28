import contextlib
import json
import os
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"evaluations"))
from metrics import parse_jsonl,summarise
from sift import check

def port():
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1",0));return sock.getsockname()[1]

def stop(process):
    if process.poll() is None:
        process.terminate()
        try:process.wait(timeout=3)
        except subprocess.TimeoutExpired:process.kill();process.wait(timeout=3)

class Relay:
    def __init__(self,destination,mode):
        self.destination=("127.0.0.1",destination);self.mode=mode
        self.socket=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);self.socket.bind(("127.0.0.1",0))
        self.port=self.socket.getsockname()[1];self.running=True;self.client=None
        self.started=None;self.held=[];self.thread=threading.Thread(target=self.run);self.thread.start()
    def run(self):
        while self.running:
            ready,_,_=select.select([self.socket],[],[],.01)
            if not ready:continue
            packet,peer=self.socket.recvfrom(4096)
            if peer==self.destination:
                if self.client is None:continue
                if self.mode=="no_acks":continue
                if self.mode=="probe_loss" and packet[5]&4 and int.from_bytes(packet[52:56],"big")==3:continue
                if self.mode=="ack_blackout" and time.monotonic()-self.started<.13:continue
                self.socket.sendto(packet,self.client);continue
            self.client=peer
            if self.started is None:self.started=time.monotonic()
            if self.mode=="blackhole":continue
            flags=packet[5];attempt=int.from_bytes(packet[6:8],"big")
            fid=int.from_bytes(packet[32:36],"big");pid=int.from_bytes(packet[36:40],"big")
            if self.mode=="fec" and fid==1 and not flags&12:
                if pid==0:continue
                if pid<4:self.held.append(packet);continue
            if self.mode=="fec" and fid==1 and flags&8:
                self.socket.sendto(packet,self.destination)
                for saved in self.held:self.socket.sendto(saved,self.destination)
                self.held.clear();continue
            if self.mode=="retrans" and fid==1:
                if flags&8:continue
                if not flags&4 and ((attempt==0 and pid<2) or (attempt==1 and pid==0)):continue
            self.socket.sendto(packet,self.destination)
    def close(self):
        self.running=False;self.thread.join(timeout=2);self.socket.close()

def run(mode,real_video=False):
    with tempfile.TemporaryDirectory() as directory,contextlib.ExitStack() as cleanup:
        path=Path(directory);p=port()
        receiver=subprocess.Popen([str(ROOT/"codes/receiver"),str(p),"--decode-only" if real_video else "--headless"],
                                  stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        cleanup.callback(stop,receiver)
        assert "listening on" in receiver.stdout.readline()
        relay=Relay(p,mode);cleanup.callback(relay.close)
        env=os.environ.copy()
        if real_video:
            if not shutil.which("Xvfb"):raise RuntimeError("Xvfb required for video regression")
            display=f":{200+os.getpid()%500}"
            xvfb=subprocess.Popen(["Xvfb",display,"-screen","0","320x240x24","-nolisten","tcp"],
                                  stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
            cleanup.callback(stop,xvfb);env["DISPLAY"]=display
            for _ in range(50):
                status=subprocess.run(["xdpyinfo"],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                if status.returncode==0:break
                if xvfb.poll() is not None:raise RuntimeError("Xvfb failed")
                time.sleep(.02)
            else:raise RuntimeError("Xvfb not ready")
        arguments=[str(ROOT/"codes/sender"),"--log",str(path/"log"),"--bmin","2","--bmax","2",
                   "127.0.0.1",str(relay.port),"0.6"]
        if not real_video:arguments.insert(1,"--synthetic")
        sender=subprocess.run(arguments,env=env,stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=8)
        assert sender.returncode==0,sender.stderr
        events=[json.loads(line) for line in (path/"log").read_text().splitlines()]
        problems=check(events);assert not problems,problems
        summary=summarise(*parse_jsonl(path/"log"))
        retries=[e for e in events if e["type"]=="PACKET_RETRANSMITTED"]
        if mode in ("blackhole","no_acks"):
            assert summary["delivered_frames"]==0 and retries
        else:assert summary["delivered_frames"]>0,summary
        if mode=="probe_loss":
            assert any(e["type"]=="CONTROL_SAMPLE" for e in events),"lost probe ACKs must not starve the controller"
            assert summary["frame_losses"]==0,summary
        if mode=="fec":
            assert any(e["type"]=="FRAME_DELIVERED" and e["fid"]==1 and e["lossy"] for e in events)
        if mode=="retrans":assert any(e["attempt"]>=2 for e in retries),retries
        if mode=="ack_blackout":assert retries
        stop(receiver)
        out,err=receiver.communicate(timeout=3)
        assert receiver.returncode==0,err
        if real_video:
            match=re.search(r"decoded=(\d+)",out);assert match and int(match.group(1))>0,(out,err)
        print(f"[PASS] {mode}{' real H264' if real_video else ''}: {summary['delivered_frames']}/{summary['n_frames']} frames")

if __name__=="__main__":
    for mode in ("normal","fec","retrans","probe_loss","ack_blackout","blackhole","no_acks"):run(mode)
    run("normal",real_video=True)

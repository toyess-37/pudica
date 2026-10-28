import logging
import shlex
import subprocess
import time
import uuid
import config
log=logging.getLogger("realnet")
_owned={}
_finished={}
def _ssh_prefix():
    return ["ssh","-p",str(config.SSH_PORT),"-i",config.SSH_KEY,"-o","BatchMode=yes",
            "-o","ConnectTimeout=8","-o","StrictHostKeyChecking=accept-new",
            f"{config.SSH_USER}@{config.SSH_HOST}"]
def ssh(command,*,check=True,timeout=15):
    result=subprocess.run(_ssh_prefix()+[command],capture_output=True,text=True,timeout=timeout)
    if check and result.returncode:raise RuntimeError(f"remote command failed: {result.stderr.strip()}")
    return result
def repo_path():
    path=config.REMOTE_REPO
    if path.startswith("~/"):return '"$HOME"/'+shlex.quote(path[2:])
    return shlex.quote(path)
def scp_from(remote_path,local_path):
    command=["scp","-P",str(config.SSH_PORT),"-i",config.SSH_KEY,"-o","BatchMode=yes",
             "-o","ConnectTimeout=8","-o","StrictHostKeyChecking=accept-new",
             f"{config.SSH_USER}@{config.SSH_HOST}:{remote_path}",str(local_path)]
    subprocess.run(command,capture_output=True,text=True,timeout=15,check=True)
    return True
def check():
    ssh(f"test -x {repo_path()}/codes/receiver")
def start_receiver(port):
    if port in _owned:raise RuntimeError("receiver port already owned by this run")
    if not isinstance(port,int) or not 1<=port<=65535:raise ValueError("invalid receiver port")
    token=uuid.uuid4().hex
    path=f"/tmp/pudica_receiver_{token}"
    body=f"cd {repo_path()}; export XVFB_DISPLAY={shlex.quote(config.XVFB_DISPLAY)}; export PUDICA_RUN_TOKEN={token}; exec bash realnet/run_receiver.sh {port} --decode-only"
    result=ssh("umask 077; "+f"nohup setsid bash -ec {shlex.quote(body)} >{path}.log 2>&1 </dev/null & echo $!")
    pid=int(result.stdout.strip())
    _owned[port]=(pid,path,token)
    try:
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            result=ssh(f"kill -0 {pid} && grep -q 'listening on' {path}.log",check=False)
            if result.returncode==0:return
            if ssh(f"kill -0 {pid}",check=False).returncode:
                message=ssh(f"cat {path}.log",check=False).stdout
                raise RuntimeError(f"receiver failed: {message}")
            time.sleep(.1)
        raise RuntimeError("receiver readiness timeout")
    except BaseException:stop_receiver(port);raise
def stop_receiver(port):
    state=_owned.get(port)
    if not state:return
    pid,path,token=state
    # Exact group launched by setsid. Bound both the remote wait and SSH.
    ownership=ssh(f"test ! -e /proc/{pid} || tr '\\0' '\\n' </proc/{pid}/environ | grep -Fxq PUDICA_RUN_TOKEN={token}",check=False)
    if ownership.returncode:raise RuntimeError("receiver PID ownership changed; refusing to signal it")
    command=f"kill -TERM -- -{pid} 2>/dev/null || true; "
    command+=f"for i in $(seq 1 30); do kill -0 {pid} 2>/dev/null || exit 0; sleep 0.1; done; "
    command+=f"kill -KILL -- -{pid} 2>/dev/null || true"
    ssh("bash -c "+shlex.quote(command),timeout=10)
    _finished[port]=_owned.pop(port)
def fetch_receiver_log(port,local_path):
    state=_owned.get(port) or _finished.get(port)
    if not state:return False
    return scp_from(state[1]+".log",local_path)

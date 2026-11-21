#!/usr/bin/env python3
"""Exercise the real shell policy in an isolated /run and mocked transports."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

overlay = Path(sys.argv[1]).resolve()
timeout = Path(sys.argv[2]).resolve()
init = overlay / "etc/init.d/S41wifi"
monitor = overlay / "usr/bin/gateway-wifi-prefer"
mock = r'''#!/usr/bin/python3
import json,os,pathlib,sys,time
root=pathlib.Path(os.environ['MOCK_ROOT']);name=pathlib.Path(sys.argv[0]).name;a=sys.argv[1:]
with (root/'calls').open('a') as f:f.write(json.dumps([name,*a])+'\n')
if name=='gateway-timeout':
 cmd=a[1:]
 if cmd[0]=='/etc/init.d/S41wifi':
  (root/'recover-called').touch();cmd=['/bin/sleep','30']
 os.execv(os.environ['REAL_TIMEOUT'],[os.environ['REAL_TIMEOUT'],'1',*cmd])
if name=='cut':
 if a[-1]=='/proc/uptime':
  p=root/'clock';n=int(p.read_text())+60 if p.exists() else 60;p.write_text(str(n));print(n);sys.exit(0)
 os.execv('/usr/bin/cut',['cut',*a])
if name=='sleep':time.sleep(.03);sys.exit(0)
if name=='wpa_cli':
 if a[-1]=='status':
  up=os.environ['SCENARIO']=='connected' or (root/'recover-called').exists()
  print('wpa_state=COMPLETED\nid=1' if up else 'wpa_state=DISCONNECTED')
 elif a[-1]=='list_networks':print('network id / ssid / bssid / flags\n0\tA\tany\t\n1\tB\tany\t[CURRENT]')
 elif a[-1]=='ping':print('PONG')
 sys.exit(0)
if name=='ip':sys.exit(0)
if name in ('start-stop-daemon','modprobe','pidof','logger','gateway-event'):sys.exit(0)
raise SystemExit(97)
'''

def setup(temp, scenario="connected"):
    root = Path(temp)
    bins = root / "bin"
    bins.mkdir()
    for name in ["gateway-timeout", "cut", "sleep", "wpa_cli", "ip", "modprobe", "pidof", "logger", "gateway-event", "start-stop-daemon"]:
        p = bins / name
        p.write_text(mock)
        p.chmod(0o755)
    runtime = root / "run"
    runtime.mkdir()
    env = {**os.environ, "PATH": str(bins) + ":" + os.environ["PATH"], "MOCK_ROOT": str(root),
           "REAL_TIMEOUT": str(timeout), "GW_WIFI_RUN_DIR": str(runtime),
           "GW_WIFI_CONFIG": str(root / "no-wifi-config"), "SCENARIO": scenario}
    return root, runtime, env

def calls(root):
    p = root / "calls"
    return [json.loads(line) for line in p.read_text().splitlines()] if p.exists() else []

def stop(p):
    os.killpg(p.pid, signal.SIGTERM)
    try:
        p.wait(timeout=4)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()

for action in ["stop-transport", "stop"]:
    with tempfile.TemporaryDirectory() as temp:
        root, runtime, env = setup(temp)
        r = subprocess.run(["/bin/sh", str(init), action], env=env, capture_output=True, timeout=10)
        assert r.returncode == 0, r.stderr
        monitor_stops = [a for a in calls(root) if a[0] == 'start-stop-daemon' and '-K' in a and str(runtime/'gateway-wifi-prefer.pid') in a]
        assert bool(monitor_stops) == (action == 'stop')
        print('PASS ' + action + '_supervisor_policy', flush=True)

for kind in ["dead-owner", "empty-old-lock", "live-owner"]:
    with tempfile.TemporaryDirectory() as temp:
        root, runtime, env = setup(temp)
        lock = runtime / "gateway-wifi-recover.lock"
        lock.mkdir()
        if kind != 'empty-old-lock':
            (lock/'pid').write_text(str(os.getpid() if kind == 'live-owner' else 99999999))
        r = subprocess.run(["/bin/sh", str(init), "recover"], env=env, capture_output=True, timeout=10)
        assert r.returncode == 0, (kind, r.stderr)
        assert lock.exists() == (kind == 'live-owner')
        attempts = [a for a in calls(root) if a[0]=='modprobe']
        assert bool(attempts) == (kind != 'live-owner')
        print('PASS recovery_lock_' + kind, flush=True)

for scenario in ["connected", "disconnected"]:
    with tempfile.TemporaryDirectory() as temp:
        root, runtime, env = setup(temp, scenario)
        p = subprocess.Popen(["/bin/sh", str(monitor)], env=env, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
        try:
            end = time.monotonic() + 10
            while time.monotonic() < end:
                c = calls(root)
                states = [a for a in c if a[0]=='wpa_cli' and a[-1]=='status']
                if len(states) >= 4:
                    break
                assert p.poll() is None, 'supervisor exited during recovery'
                time.sleep(.05)
            assert len(states) >= 4, (scenario, c)
            assert p.poll() is None
            if scenario == 'connected':
                assert not (root/'recover-called').exists()
                assert ['wpa_cli','-i','wlan0','disable_network','0'] in c
                assert not any(a[0]=='wpa_cli' and any(x in a for x in ['select_network','scan','enable_network']) for a in c)
            else:
                assert (root/'recover-called').exists()
                assert (runtime/'gateway-wifi-recover.log').exists()
            print('PASS monitor_' + ('keeps_working_network' if scenario=='connected' else 'survives_hung_recovery'), flush=True)
        finally:
            stop(p)

with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    (root/'events.log').write_bytes(b'x'*65536)
    env = {**os.environ, 'GW_EVENT_DIR':str(root), 'GW_EVENT_LOCK':str(root/'lock')}
    r = subprocess.run(['/bin/sh',str(overlay/'usr/bin/gateway-event'),'test-event'],env=env,capture_output=True,timeout=5)
    assert r.returncode == 0, r.stderr
    assert (root/'events.log.1').stat().st_size == 65536
    assert 'boot=' in (root/'events.log').read_text() and 'test-event' in (root/'events.log').read_text()
    assert not (root/'lock').exists()
    print('PASS flash_event_rotation_and_lock_release',flush=True)
print('WIFI_RECOVERY_AND_EVENT_TESTS_PASS=8')

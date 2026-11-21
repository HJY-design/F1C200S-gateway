#!/usr/bin/env python3
"""Run the real native timeout executable, including hostile child behavior."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

exe = str(Path(sys.argv[1]).resolve())

def run(name, seconds, command, expected, maximum=5):
    start = time.monotonic()
    result = subprocess.run([exe, str(seconds), *command], capture_output=True, timeout=maximum)
    elapsed = time.monotonic() - start
    assert result.returncode == expected, (name, result.returncode, result.stderr)
    assert elapsed < maximum, (name, elapsed)
    print(f"PASS {name} {elapsed:.2f}s", flush=True)

run("normal_success", 1, ["/bin/sh", "-c", "exit 0"], 0)
run("error_status_is_preserved", 1, ["/bin/sh", "-c", "exit 17"], 17)
run("missing_command", 1, ["/definitely-no-such-gateway-command"], 127)
run("invalid_budget", 0, ["/bin/true"], 125)
run("hung_process_is_bounded", 1, ["/bin/sleep", "30"], 124)
run("sigterm_ignoring_process_is_killed", 1,
    [sys.executable, "-c", "import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);time.sleep(30)"], 124)
with tempfile.TemporaryDirectory() as temp:
    marker = Path(temp) / "escaped"
    code = f"import signal,time,pathlib;signal.signal(signal.SIGTERM,signal.SIG_IGN);time.sleep(4);pathlib.Path({str(marker)!r}).touch()"
    p = subprocess.Popen([exe, "1", "/bin/sh", "-c", '"$1" -c "$2" & wait', "sh", sys.executable, code])
    assert p.wait(timeout=5) == 124
    time.sleep(1.4)
    assert not marker.exists(), "a descendant survived the process-group timeout"
    print("PASS timed_out_descendants_cannot_write_later", flush=True)
p = subprocess.Popen([exe, "30", "/bin/sleep", "30"])
time.sleep(0.2)
p.send_signal(signal.SIGTERM)
assert p.wait(timeout=4) == 128 + signal.SIGTERM
print("PASS interrupted_wrapper_stops_child", flush=True)
print("GATEWAY_TIMEOUT_TESTS_PASS=8")

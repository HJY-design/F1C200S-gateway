"""Exercise direct NRST open-drain behavior in gpio-sim, not target hardware."""
import json
import pathlib
import re
import signal
import subprocess
import time

base = pathlib.Path(__file__).resolve().parents[1]
test = base / 'artifacts/gpio-simulator-direct-reset-20261005'
test.mkdir(exist_ok=True)
socket_path = test / 'gateway-control.sock'
config = pathlib.Path('/sys/kernel/config/gpio-sim/f1c200s-gateway-test')
if config.exists() or socket_path.exists():
    raise RuntimeError('Existing test resource; refusing to replace it')
subprocess.run(['modprobe', 'gpio-sim'], check=True)
if not pathlib.Path('/sys/kernel/config/gpio-sim').is_dir():
    subprocess.run(['mount', '-t', 'configfs', 'configfs', '/sys/kernel/config'], check=True)
source = base / 'buildroot-external/package/gateway-control/src/gateway-ctl.c'
binary = test / 'gateway-ctl-native'
subprocess.run(['gcc', '-Os', '-Wall', '-Wextra', '-Werror', f'-DSOCKET_PATH="{socket_path}"', str(source), '-lgpiod', '-o', str(binary)], check=True)
bank = config / 'bank0'
daemon = None
results = {}
try:
    config.mkdir()
    bank.mkdir()
    # No PE4/interface gate exists in the exported board.
    (bank / 'num_lines').write_text('2')
    for number, name in enumerate(['F407_RESET_ASSERT', 'F407_BOOT0_SET']):
        line = bank / f'line{number}'
        line.mkdir()
        (line / 'name').write_text(name)
    (config / 'live').write_text('1')
    device = (config / 'dev_name').read_text().strip()
    chip = (bank / 'chip_name').read_text().strip()
    sysfs = pathlib.Path('/sys/devices/platform') / device / chip
    values = [sysfs / f'sim_gpio{i}/value' for i in range(2)]
    reset_pull = sysfs / 'sim_gpio0/pull'
    external_pull = 'pull-up'  # Stand-in for the target NRST pull-up.
    def state():
        # gpio-sim_direction_input() only updates direction_map: its pad value
        # does not follow an existing pull until gpio_sim_apply_pull() runs.
        # Reapply the external pull on each sample to model a continuous pull.
        # An actively driven output ignores it, so contention stays detectable.
        reset_pull.write_text(external_pull)
        return [int(v.read_text().strip()) for v in values]
    def reset_debug():
        text = pathlib.Path('/sys/kernel/debug/gpio').read_text()
        return next(line.strip() for line in text.splitlines()
                    if 'gateway-control-reset' in line)
    def assert_reset_input():
        direction = reset_debug()
        assert re.search(r'\b(?:in|input)\b', direction), direction
    daemon = subprocess.Popen([str(binary), 'daemon'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    deadline = time.monotonic() + 3
    while not socket_path.exists():
        if daemon.poll() is not None:
            raise RuntimeError(daemon.communicate())
        if time.monotonic() > deadline:
            raise TimeoutError('Daemon startup')
        time.sleep(.01)
    assert state() == [1, 0], state()
    results['initial_levels'] = state()
    results['initial_reset_direction'] = reset_debug()
    assert_reset_input()
    def command(name):
        return subprocess.run([str(binary), name], check=True, capture_output=True, text=True).stdout.strip()
    results['normal_reply'] = command('normal')
    assert state() == [1, 0], state()
    # A button/ST-Link pulling the released pad low must affect its level.
    # A push-pull high output would ignore this gpio-sim external pull.
    external_pull = 'pull-down'
    assert state() == [0, 0], state()
    assert_reset_input()
    results['external_reset_while_released'] = dict(
        levels=state(), direction=reset_debug(), reply=command('status'))
    external_pull = 'pull-up'
    assert state() == [1, 0], state()
    def pulse(name, expected_boot):
        client = subprocess.Popen([str(binary), name], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        trace = []
        deadline = time.monotonic() + 4
        while client.poll() is None and time.monotonic() < deadline:
            trace.append((time.monotonic(), state(), reset_debug()))
            time.sleep(.001)
        stdout, stderr = client.communicate(timeout=2)
        assert client.returncode == 0, (stdout, stderr)
        lows = [t for t, levels, direction in trace if levels[0] == 0]
        assert len(lows) > 1, trace
        assert all(levels[1] == expected_boot for _, levels, _ in trace if levels[0] == 0)
        low_directions = [direction for _, levels, direction in trace if levels[0] == 0]
        assert any('output-low' in direction for direction in low_directions), low_directions
        observed_ms = (lows[-1] - lows[0]) * 1000
        assert observed_ms >= 10, observed_ms
        assert state() == [1, expected_boot], state()
        assert_reset_input()
        return dict(reply=stdout.strip(), observed_gpio_low_ms=round(observed_ms, 2),
                    low_direction=low_directions[0], final_levels=state(),
                    final_reset_direction=reset_debug())
    results['reset'] = pulse('reset', 0)
    results['rom'] = pulse('rom', 1)
    results['normal_after_rom'] = command('normal')
    assert state() == [1, 0], state()
    rejected = subprocess.run([str(binary), 'disable'], capture_output=True, text=True)
    assert rejected.returncode == 1, rejected
    assert 'No target interface gate' in rejected.stdout, rejected.stdout
    assert state() == [1, 0], state()
    results['disable_rejected_without_hardware_gate'] = rejected.stdout.strip()
    daemon.send_signal(signal.SIGTERM)
    stdout, stderr = daemon.communicate(timeout=4)
    assert daemon.returncode == 0, (stdout, stderr)
    assert not stderr, stderr
    assert state() == [1, 0], state()
    results['clean_exit_levels'] = state()
    results['daemon_exit'] = daemon.returncode
    results['daemon_stderr'] = stderr
    daemon = None
    results['pass'] = True
    results['simulator_external_pull_model'] = (
        'Reapply pull at every sample: gpio-sim does not refresh value_map '
        'when its direction changes to input. GPIO direction checked independently.')
    results['scope'] = ('Native x86 Ubuntu VM + Linux gpio-sim: low reset pulse, '
                        'input on release, external reset and clean exit. '
                        'No F1C GPIO/boot waveform, ESP EN or F407 hardware test.')
finally:
    if daemon is not None:
        daemon.send_signal(signal.SIGTERM)
        stdout, stderr = daemon.communicate(timeout=4)
        results['daemon_exit'] = daemon.returncode
        results['daemon_stderr'] = stderr
    if config.exists():
        (config / 'live').write_text('0')
        if bank.exists():
            for number in range(2):
                line = bank / f'line{number}'
                if line.exists(): line.rmdir()
            bank.rmdir()
        config.rmdir()
    if socket_path.exists(): socket_path.unlink()
    (test / 'result.json').write_text(json.dumps(results, ensure_ascii=False, indent=2))
    print(json.dumps(results, ensure_ascii=False))

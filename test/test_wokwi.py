"""The real firmware on a simulated ESP32 (Wokwi), against a real SenseHive.

Wokwi runs the compiled PowerPulse firmware on a simulated ESP32 DevKit with
Wi-Fi ("Wokwi-GUEST") and internet access. The node joins the public broker
broker.hivemq.com, where SenseHive (started from source on this machine) also
listens. Each run uses its own topics and its own secure root, so runs never
meet, and a throwaway key from a throwaway SenseHive.

  auto:   SenseHive issues a key, the firmware is built with it, the node is
          found by itself, shows online, and offline when the simulation stops
          (the broker sends its last will: a power cut).
  manual: the topic is added in SenseHive first, then the node's beacons are
          logged, online, and offline when the simulation stops.

Needs: WOKWI_CLI_TOKEN (free at https://wokwi.com/dashboard/ci), wokwi-cli,
arduino-cli with the esp32 core, internet. Run with
PP_TEST_BROKER=broker.hivemq.com:1883.
"""
import json
import os
import re
import shutil
import subprocess
import time

import pytest

import firmware
from conftest import BROKER_HOST, BROKER_PORT, IN_CI, base_topic, new_key, node, wait_for

WOKWI_CLI = os.getenv('WOKWI_CLI', 'wokwi-cli')
SIM_SECONDS = 70
HERE = os.path.dirname(os.path.abspath(__file__))

pytestmark = pytest.mark.skipif(not os.getenv('WOKWI_CLI_TOKEN') and not os.getenv('PP_REQUIRE_WOKWI'),
                                reason='needs WOKWI_CLI_TOKEN (https://wokwi.com/dashboard/ci)')


def simulate(variant, out, log):
    """Starts the simulation in the background; returns the process."""
    name = firmware.sketch_name(variant)
    shutil.copy(os.path.join(HERE, 'wokwi', 'diagram.json'), out / 'diagram.json')
    (out / 'wokwi.toml').write_text(f"[wokwi]\nversion = 1\nfirmware = '{name}.ino.merged.bin'\nelf = '{name}.ino.elf'\n")
    cli = open(out / 'cli.log', 'w')
    proc = subprocess.Popen([WOKWI_CLI, '--timeout', str(SIM_SECONDS * 1000), '--timeout-exit-code', '0',
                             '--serial-log-file', str(log), str(out)], stdout=cli, stderr=subprocess.STDOUT)
    proc.cli_log = out / 'cli.log'
    return proc


class WokwiDropped(Exception):
    """Wokwi's cloud dropped the simulation (its API or connection), not the firmware."""


def finish(proc, log):
    rc = proc.wait(SIM_SECONDS + 60)
    text = log.read_text(errors='replace') if log.exists() else ''
    cli = proc.cli_log.read_text(errors='replace') if proc.cli_log.exists() else ''
    if IN_CI or rc:
        print(cli[-2000:])
        print(text[-6000:])
    if rc and ('API Error' in cli or 'transport closed' in cli or 'ECONNRESET' in cli):
        raise WokwiDropped(cli.strip().splitlines()[-1] if cli.strip() else 'no output')
    assert rc == 0, f'wokwi-cli exit {rc}'
    return text


def with_retry(run, tmp_path):
    """One more try only when Wokwi's cloud dropped the simulation; a firmware failure fails at once."""
    try:
        return run(tmp_path / 'try1')
    except WokwiDropped as e:
        print(f'Wokwi dropped the simulation ({e}); trying once more')
        return run(tmp_path / 'try2')


def settings(base):
    return {'PP_WIFI_SSID': 'Wokwi-GUEST', 'PP_WIFI_PASS': '', 'PP_MQTT_HOST': BROKER_HOST,
            'PP_MQTT_PORT': str(BROKER_PORT), 'PP_BASE_TOPIC': base, 'PP_LOCATION': 'wokwi'}


def report(variant, stats):
    """Prints the measured numbers (and appends them to PP_STATS_FILE as JSON lines)."""
    line = json.dumps({'build': variant, **stats}, sort_keys=True)
    print('PP-STATS ' + line)
    if os.getenv('PP_STATS_FILE'):
        with open(os.environ['PP_STATS_FILE'], 'a', encoding='utf-8') as f:
            f.write(line + '\n')
    if os.getenv('GITHUB_STEP_SUMMARY'):                   # a table on the CI run's page
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a', encoding='utf-8') as f:
            f.write(f'### PowerPulse {variant} build on a simulated ESP32\n\n| Measure | Value |\n| --- | --- |\n')
            f.writelines(f'| {k} | {v} |\n' for k, v in sorted(stats.items()))
            f.write('\n')


def secure_rejected(sensehive):
    return sum(sensehive['http'].get(sensehive['url'] + '/api/secure/status').json()['rejected'].values())


def test_auto_discovery_on_a_simulated_esp32(sensehive, tmp_path):
    with_retry(lambda tmp: auto_discovery(sensehive, tmp), tmp_path)


def auto_discovery(sensehive, tmp_path):
    key = new_key(sensehive, 'powerpulse-wokwi')
    base = base_topic('wokwi-auto')
    topic = f"{sensehive['root']}/{base}/powerbeacon"
    t = time.monotonic()
    out = firmware.build('auto', tmp_path / 'fw', {**settings(base), 'SH_KEY_ID': key['kid'], 'SH_KEY': key['key'],
                                                   'SH_ROOT': sensehive['root']})
    stats = {'build_s': round(time.monotonic() - t, 1), 'flash_bytes': (out / 'PowerPulse-v2.0-auto.ino.bin').stat().st_size}
    refused_before = secure_rejected(sensehive)
    log = tmp_path / 'serial.log'
    t0 = time.monotonic()
    proc = simulate('auto', out, log)
    try:
        n = wait_for(lambda: node(sensehive, topic), timeout=SIM_SECONDS, step=0.5, what='the node to be discovered')
        stats['discovered_s'] = round(time.monotonic() - t0, 1)
        assert n['secure'] and n['key_id'] == key['kid'] and n['node_name'] == 'powerpulse-wokwi'
        n = wait_for(lambda: (x := node(sensehive, topic)) and x['status'] == 'active' and x,
                     timeout=SIM_SECONDS, step=0.5, what='online')
        stats['online_s'] = round(time.monotonic() - t0, 1)
        wait_for(lambda: node(sensehive, topic)['entry_count'] >= 2, timeout=SIM_SECONDS, step=0.5, what='two beacons')
        assert n['lwt_topic'] == f'{base}/status'
        assert n['last_value']['firmware'] == 'powerpulse-v2.0-auto' and n['last_value']['node_id'] == base
    finally:
        text = finish(proc, log)
    stopped = time.monotonic()
    assert 'Sealed beacon sent' in text and 'Guru Meditation' not in text
    wait_for(lambda: node(sensehive, topic)['status'] == 'inactive', timeout=90, step=0.5,
             what='the last will after the simulation stopped')
    n = node(sensehive, topic)
    sizes = [int(x) for x in re.findall(r'Sealed beacon sent to \S+ \((\d+) bytes\)', text)]
    stats.update(offline_after_power_cut_s=round(time.monotonic() - stopped, 1), beacons_sent=len(sizes),
                 beacons_stored=n['entry_count'], sealed_bytes=max(sizes), plain_bytes=len(json.dumps(n['last_value'])),
                 refused=secure_rejected(sensehive) - refused_before, sim_seconds=SIM_SECONDS)
    assert stats['refused'] == 0 and n['entry_count'] == len(sizes)
    report('auto', stats)


def test_manual_topic_on_a_simulated_esp32(sensehive, tmp_path):
    with_retry(lambda tmp: manual_topic(sensehive, tmp), tmp_path)


def manual_topic(sensehive, tmp_path):
    base = base_topic('wokwi-manual')
    topic = f'{base}/powerbeacon'
    r = sensehive['http'].post(sensehive['url'] + '/api/nodes', json={
        'topic': topic, 'tag': 'powerpulse', 'node_name': base, 'lwt_topic': f'{base}/status'})
    assert r.status_code in (200, 201), r.text
    t = time.monotonic()
    out = firmware.build('manual', tmp_path / 'fw', settings(base))
    stats = {'build_s': round(time.monotonic() - t, 1),
             'flash_bytes': (out / 'PowerPulse-v2.0-manual.ino.bin').stat().st_size}
    log = tmp_path / 'serial.log'
    t0 = time.monotonic()
    proc = simulate('manual', out, log)
    try:
        wait_for(lambda: (x := node(sensehive, topic)) and x['status'] == 'active', timeout=SIM_SECONDS, step=0.5,
                 what='online')
        stats['online_s'] = round(time.monotonic() - t0, 1)
        n = wait_for(lambda: (x := node(sensehive, topic)) and x['entry_count'] >= 1 and x, timeout=SIM_SECONDS,
                     step=0.5, what='the first beacon')
        stats['first_beacon_s'] = round(time.monotonic() - t0, 1)
        wait_for(lambda: node(sensehive, topic)['entry_count'] >= 2, timeout=SIM_SECONDS, step=0.5, what='two beacons')
        assert not n['secure'] and n['last_value']['firmware'] == 'powerpulse-v2.0-manual'
    finally:
        text = finish(proc, log)
    stopped = time.monotonic()
    assert 'Beacon sent' in text and 'Guru Meditation' not in text
    wait_for(lambda: node(sensehive, topic)['status'] == 'inactive', timeout=90, step=0.5,
             what='the last will after the simulation stopped')
    n = node(sensehive, topic)
    stats.update(offline_after_power_cut_s=round(time.monotonic() - stopped, 1),
                 beacons_sent=text.count('[MQTT] Beacon sent'), beacons_stored=n['entry_count'],
                 plain_bytes=len(json.dumps(n['last_value'])), sim_seconds=SIM_SECONDS)
    report('manual', stats)

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
import os
import shutil
import subprocess

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
    return subprocess.Popen([WOKWI_CLI, '--timeout', str(SIM_SECONDS * 1000), '--timeout-exit-code', '0',
                             '--serial-log-file', str(log), str(out)])


def finish(proc, log):
    rc = proc.wait(SIM_SECONDS + 60)
    text = log.read_text(errors='replace') if log.exists() else ''
    if IN_CI or rc:
        print(text[-6000:])
    assert rc == 0, f'wokwi-cli exit {rc}'
    return text


def settings(base):
    return {'PP_WIFI_SSID': 'Wokwi-GUEST', 'PP_WIFI_PASS': '', 'PP_MQTT_HOST': BROKER_HOST,
            'PP_MQTT_PORT': str(BROKER_PORT), 'PP_BASE_TOPIC': base, 'PP_LOCATION': 'wokwi'}


def test_auto_discovery_on_a_simulated_esp32(sensehive, tmp_path):
    key = new_key(sensehive, 'powerpulse-wokwi')
    base = base_topic('wokwi-auto')
    topic = f"{sensehive['root']}/{base}/powerbeacon"
    out = firmware.build('auto', tmp_path / 'fw', {**settings(base), 'SH_KEY_ID': key['kid'], 'SH_KEY': key['key'],
                                                   'SH_ROOT': sensehive['root']})
    log = tmp_path / 'serial.log'
    proc = simulate('auto', out, log)
    try:
        n = wait_for(lambda: node(sensehive, topic), timeout=SIM_SECONDS, step=1, what='the node to be discovered')
        assert n['secure'] and n['key_id'] == key['kid'] and n['node_name'] == 'powerpulse-wokwi'
        n = wait_for(lambda: (x := node(sensehive, topic)) and x['status'] == 'active' and x['entry_count'] >= 2 and x,
                     timeout=SIM_SECONDS, step=1, what='online with two beacons')
        assert n['lwt_topic'] == f'{base}/status'
        assert n['last_value']['firmware'] == 'powerpulse-v2.0-auto' and n['last_value']['node_id'] == base
    finally:
        text = finish(proc, log)
    assert 'Sealed beacon sent' in text and 'Traceback' not in text and 'Guru Meditation' not in text
    wait_for(lambda: node(sensehive, topic)['status'] == 'inactive', timeout=60, step=1,
             what='the last will after the simulation stopped')


def test_manual_topic_on_a_simulated_esp32(sensehive, tmp_path):
    base = base_topic('wokwi-manual')
    topic = f'{base}/powerbeacon'
    r = sensehive['http'].post(sensehive['url'] + '/api/nodes', json={
        'topic': topic, 'tag': 'powerpulse', 'node_name': base, 'lwt_topic': f'{base}/status'})
    assert r.status_code in (200, 201), r.text
    out = firmware.build('manual', tmp_path / 'fw', settings(base))
    log = tmp_path / 'serial.log'
    proc = simulate('manual', out, log)
    try:
        n = wait_for(lambda: (x := node(sensehive, topic)) and x['status'] == 'active' and x['entry_count'] >= 2 and x,
                     timeout=SIM_SECONDS, step=1, what='online with two beacons')
        assert not n['secure'] and n['last_value']['firmware'] == 'powerpulse-v2.0-manual'
    finally:
        text = finish(proc, log)
    assert 'Beacon sent' in text and 'Guru Meditation' not in text
    wait_for(lambda: node(sensehive, topic)['status'] == 'inactive', timeout=60, step=1,
             what='the last will after the simulation stopped')

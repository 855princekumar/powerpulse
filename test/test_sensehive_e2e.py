"""PowerPulse against SenseHive, end to end, without hardware.

A real SenseHive (started from its source) and a real MQTT broker. A simulated
PowerPulse node publishes exactly what the firmware publishes: the retained
status with its last will, and the beacon every few seconds. In the
auto-discovery build the beacon is sealed by the firmware's own C code
(sensehive_secure.h, built on the PC as test/host/seal_cli), so a change that
breaks the firmware's encryption fails here.

    SENSEHIVE_DIR   sense-hive/version-1.0 (default: ../sense-hive/version-1.0 next to this repo)
    PP_TEST_BROKER  host:port of a broker that accepts anonymous clients (default localhost:1883)
    SEAL_CLI        the built seal_cli (default: test/host/seal_cli)

    python -m pytest -v test/
"""
import json
import secrets
import sys
import time
import uuid

import paho.mqtt.client as mqtt
import pytest

from conftest import (BROKER_HOST, BROKER_PORT, SEAL_CLI, SENSEHIVE_DIR, base_topic, need, new_key, node, nodes,
                      rejected, seal, wait_for)


# ---------------------------------------------------------------- the simulated node

def new_client(client_id):
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    except AttributeError:                       # paho-mqtt 1.x
        return mqtt.Client(client_id=client_id)


class PowerPulseNode:
    """Publishes what PowerPulse v2.0 publishes (see the .ino): <base>/status retained
    (ON on connect, OFF as the last will) and the beacon, plain or sealed."""

    def __init__(self, base, secure=None, location='lab'):
        self.base, self.secure, self.location = base, secure, location
        self.client = new_client(f'{base}-{uuid.uuid4().hex[:6]}')
        will = json.dumps({'node_id': base, 'location': location, 'status': 'OFF', 'reason': 'lwt'}, separators=(',', ':'))
        self.client.will_set(self.status_topic, will, qos=0, retain=True)
        self.started = time.time()

    @property
    def status_topic(self):
        return f'{self.base}/status'

    @property
    def beacon_topic(self):
        if self.secure:
            return f"{self.secure['root']}/{self.base}/powerbeacon"
        return f'{self.base}/powerbeacon'

    def power_on(self):
        self.client.connect(BROKER_HOST, BROKER_PORT, keepalive=15)
        self.client.loop_start()
        on = json.dumps({'node_id': self.base, 'location': self.location, 'status': 'ON'}, separators=(',', ':'))
        self.client.publish(self.status_topic, on, retain=True).wait_for_publish(5)
        return self

    def beacon(self):
        doc = {'node_id': self.base, 'location': self.location, 'ip': '10.0.0.42', 'mac': 'AA:BB:CC:00:11:22',
               'uptime_sec': int(time.time() - self.started), 'wifi_ssid': 'test', 'rssi_dbm': -61,
               'broker': 'lan', 'firmware': 'powerpulse-v2.0-' + ('auto' if self.secure else 'manual'),
               'status': 'ON', 'timestamp': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
        if self.secure:
            doc['sensehive_lwt'] = self.status_topic
            payload = seal(self.secure['kid'], self.secure['key'], self.beacon_topic, json.dumps(doc, separators=(',', ':')))
        else:
            payload = json.dumps(doc, separators=(',', ':'))
        self.client.publish(self.beacon_topic, payload).wait_for_publish(5)
        return doc

    def power_loss(self):
        """The plug is pulled: the connection drops without a goodbye, the broker sends the last will."""
        self.client.loop_stop()
        self.client.socket().close()

    def clear_retained(self):
        c = new_client(f'cleanup-{uuid.uuid4().hex[:6]}')
        c.connect(BROKER_HOST, BROKER_PORT)
        c.loop_start()
        c.publish(self.status_topic, b'', retain=True).wait_for_publish(5)
        c.loop_stop()
        c.disconnect()


# ---------------------------------------------------------------- the firmware's encryption

KEY = '00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff'
KID = '0123456789abcdef'


@pytest.mark.parametrize('payload', ['{"a":1}', '', 'x' * 1000, json.dumps({'node_id': 'pp', 'temp': '22 C'})],
                         ids=['json', 'empty', '1000-bytes', 'beacon-like'])
@pytest.mark.parametrize('topic', ['sensehive/secure/powerpulse-a1b2c3/powerbeacon', 'plant/enc/node 1/x'],
                         ids=['default-root', 'custom-root'])
def test_the_firmware_seals_exactly_like_sensehive(payload, topic):
    need(SEAL_CLI.is_file(), f'{SEAL_CLI} not built')
    need((SENSEHIVE_DIR / 'secure.py').is_file(), f'SenseHive not found at {SENSEHIVE_DIR}')
    sys.path.insert(0, str(SENSEHIVE_DIR))
    import secure                                                # SenseHive's own implementation
    nonce = bytes(range(16))
    t = 1760000000
    ours = seal(KID, KEY, topic, payload, t=t, nonce_hex=nonce.hex())
    theirs = secure.seal(bytes.fromhex(KEY), KID, topic, payload, t=t, nonce=nonce)
    assert ours == theirs
    env = secure.parse(ours, 65536)
    assert env['k'] == KID and secure.open_sealed(bytes.fromhex(KEY), topic, env) == payload.encode()


def test_the_firmware_refuses_bad_keys_and_oversized_payloads():
    need(SEAL_CLI.is_file(), f'{SEAL_CLI} not built')
    for kid, key in (('short', KEY), (KID, KEY[:-2]), ('0123456789abcdeg', KEY), (KID, 'zz' + KEY[2:])):
        with pytest.raises(ValueError, match='exit 3'):
            seal(kid, key, 't', 'x')
    with pytest.raises(ValueError, match='exit 5'):
        seal(KID, KEY, 't', 'x' * 1025)                         # SH_MAX_PAYLOAD
    a, b = seal(KID, KEY, 't', 'same', t=1), seal(KID, KEY, 't', 'same', t=1)
    assert a != b                                               # a fresh random nonce every time


# ---------------------------------------------------------------- end to end

def test_auto_discovery_a_secure_node_appears_by_itself_and_shows_power_loss(sensehive):
    need(SEAL_CLI.is_file(), f'{SEAL_CLI} not built')
    key = new_key(sensehive, 'powerpulse-lab')
    pp = PowerPulseNode(base_topic('auto'), secure=key)
    try:
        pp.power_on()
        assert node(sensehive, pp.beacon_topic) is None
        sent = pp.beacon()
        n = wait_for(lambda: node(sensehive, pp.beacon_topic), what='the secure topic to be discovered')
        assert n['secure'] and n['key_id'] == key['kid'] and n['node_name'] == 'powerpulse-lab'
        assert n['lwt_topic'] == pp.status_topic
        n = wait_for(lambda: (x := node(sensehive, pp.beacon_topic)) and x['status'] == 'active' and x,
                     what='the node to show online')
        assert n['last_value']['firmware'] == 'powerpulse-v2.0-auto'
        assert n['last_value']['node_id'] == sent['node_id']
        pp.beacon()
        wait_for(lambda: node(sensehive, pp.beacon_topic)['entry_count'] == 2, what='the second beacon')
        keys = sensehive['http'].get(sensehive['url'] + '/api/secure/keys').json()['keys']
        mine = next(k for k in keys if k['kid'] == key['kid'])
        assert mine['status'] == 'active' and mine['topic'] == pp.beacon_topic and mine['messages'] == 2
        assert key['key'] not in json.dumps(keys)
        pp.power_loss()
        wait_for(lambda: node(sensehive, pp.beacon_topic)['status'] == 'inactive', timeout=20,
                 what='the last will to show the node offline')
        hist = sensehive['http'].get(f"{sensehive['url']}/api/nodes/{n['id']}/history").json()['history']
        assert len(hist) == 2 and all(h['payload']['firmware'] == 'powerpulse-v2.0-auto' for h in hist)
    finally:
        pp.clear_retained()


def test_manual_a_plain_node_is_logged_only_once_added(sensehive):
    pp = PowerPulseNode(base_topic('manual'))
    try:
        pp.power_on()
        pp.beacon()
        time.sleep(2)
        assert node(sensehive, pp.beacon_topic) is None             # plain topics are never discovered
        r = sensehive['http'].post(sensehive['url'] + '/api/nodes', json={
            'topic': pp.beacon_topic, 'tag': 'powerpulse', 'node_name': pp.base, 'lwt_topic': pp.status_topic})
        assert r.status_code in (200, 201), r.text
        n = wait_for(lambda: (x := node(sensehive, pp.beacon_topic)) and x['status'] == 'active' and x,
                     what='the retained ON status')
        assert not n['secure']
        pp.beacon()
        n = wait_for(lambda: (x := node(sensehive, pp.beacon_topic)) and x['entry_count'] >= 1 and x, what='a beacon')
        assert n['last_value']['firmware'] == 'powerpulse-v2.0-manual'
        pp.power_loss()
        wait_for(lambda: node(sensehive, pp.beacon_topic)['status'] == 'inactive', timeout=20,
                 what='the last will to show the node offline')
    finally:
        pp.clear_retained()


def test_strangers_on_the_secure_root_are_never_discovered(sensehive):
    need(SEAL_CLI.is_file(), f'{SEAL_CLI} not built')
    root = sensehive['root']
    before = rejected(sensehive)
    pp = PowerPulseNode(base_topic('stranger'))
    try:
        pp.power_on()
        plain = f'{root}/{pp.base}/powerbeacon'                      # a plain beacon under the root
        pp.client.publish(plain, json.dumps({'status': 'ON'})).wait_for_publish(5)
        forged = f'{root}/{pp.base}-forged/powerbeacon'              # sealed with a key SenseHive never issued
        pp.client.publish(forged, seal(secrets.token_hex(8), secrets.token_hex(32), forged, '{"x":1}')).wait_for_publish(5)
        wait_for(lambda: rejected(sensehive)['format'] > before['format']
                 and rejected(sensehive)['unknown_key'] > before['unknown_key'], what='both to be refused')
        topics = [n['topic'] for n in nodes(sensehive)]
        assert plain not in topics and forged not in topics
    finally:
        pp.client.loop_stop()
        pp.client.disconnect()
        pp.clear_retained()


def test_one_key_per_node_a_copied_key_cannot_add_a_second_node(sensehive):
    need(SEAL_CLI.is_file(), f'{SEAL_CLI} not built')
    key = new_key(sensehive, 'shared-by-mistake')
    first, second = PowerPulseNode(base_topic('first'), secure=key), PowerPulseNode(base_topic('second'), secure=key)
    try:
        first.power_on()
        first.beacon()
        wait_for(lambda: node(sensehive, first.beacon_topic), what='the first node')
        before = rejected(sensehive)['wrong_topic']
        second.power_on()
        second.beacon()
        wait_for(lambda: rejected(sensehive)['wrong_topic'] > before, what='the second node to be refused')
        assert node(sensehive, second.beacon_topic) is None
    finally:
        for pp in (first, second):
            pp.client.loop_stop()
            pp.client.disconnect()
            pp.clear_retained()

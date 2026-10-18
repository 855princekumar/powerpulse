"""Shared by the tests: settings, a SenseHive started from source, and helpers.

    SENSEHIVE_DIR   sense-hive/version-1.0 (default: ../sense-hive/version-1.0 next to this repo)
    PP_TEST_BROKER  host:port of the MQTT broker (default localhost:1883)
    SEAL_CLI        the built test/host/seal_cli
"""
import os
import secrets
import socket
import subprocess
import sys
import time
import uuid
from pathlib import Path

import pytest
import requests

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
SENSEHIVE_DIR = Path(os.getenv('SENSEHIVE_DIR', REPO.parent / 'sense-hive' / 'version-1.0'))
BROKER_HOST, BROKER_PORT = (os.getenv('PP_TEST_BROKER', 'localhost:1883').split(':') + ['1883'])[:2]
BROKER_PORT = int(BROKER_PORT)
SEAL_CLI = Path(os.getenv('SEAL_CLI', HERE / 'host' / ('seal_cli.exe' if os.name == 'nt' else 'seal_cli')))
PASSWORD = 'pp-' + secrets.token_hex(8)
IN_CI = bool(os.getenv('CI'))


def need(ok, why):
    """Skip on a developer machine, fail in CI: CI must run every test."""
    if not ok:
        (pytest.fail if IN_CI else pytest.skip)(why)


def broker_up():
    try:
        socket.create_connection((BROKER_HOST, BROKER_PORT), timeout=2).close()
        return True
    except OSError:
        return False


def seal(kid, key_hex, topic, payload, t=None, nonce_hex=None):
    """The firmware's sh_seal(), run through seal_cli."""
    args = [str(SEAL_CLI), kid, key_hex, topic, str(int(time.time() if t is None else t)), payload]
    if nonce_hex:
        args.append(nonce_hex)
    out = subprocess.run(args, capture_output=True, timeout=10)
    if out.returncode:
        raise ValueError(f'seal_cli exit {out.returncode}: {out.stderr.decode().strip()}')
    return out.stdout


def wait_for(fn, timeout=15, step=0.25, what='condition'):
    end = time.time() + timeout
    last = None
    while time.time() < end:
        last = fn()
        if last:
            return last
        time.sleep(step)
    raise AssertionError(f'timed out waiting for {what}; last value: {last!r}')


# ---------------------------------------------------------------- SenseHive

def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


@pytest.fixture(scope='module')
def sensehive(tmp_path_factory):
    need((SENSEHIVE_DIR / 'app.py').is_file(), f'SenseHive not found at {SENSEHIVE_DIR} (set SENSEHIVE_DIR)')
    need(broker_up(), f'no MQTT broker at {BROKER_HOST}:{BROKER_PORT} (set PP_TEST_BROKER)')
    need(SEAL_CLI.is_file(), f'{SEAL_CLI} not built (see test/host/seal_cli.c)')
    data = tmp_path_factory.mktemp('sensehive')
    port = free_port()
    root = f'pp-test-{uuid.uuid4().hex[:8]}/secure'           # its own root: parallel runs never meet
    env = dict(os.environ, DB_PATH=str(data / 'sh.db'), HOST='127.0.0.1', PORT=str(port),
               DEFAULT_BROKER_HOST=BROKER_HOST, DEFAULT_BROKER_PORT=str(BROKER_PORT), SENSEHIVE_FORCE_BROKER='true',
               SENSEHIVE_ADMIN_PASSWORD=PASSWORD, SENSEHIVE_SECURE_ROOT=root, PYTHONUNBUFFERED='1')
    log = open(data / 'sensehive.log', 'w')
    proc = subprocess.Popen([sys.executable, 'app.py'], cwd=SENSEHIVE_DIR, env=env, stdout=log, stderr=subprocess.STDOUT)
    base = f'http://127.0.0.1:{port}'
    http = requests.Session()
    try:
        wait_for(lambda: proc.poll() is None and _healthy(base), timeout=30, what='SenseHive to start')
        r = http.post(base + '/api/login', json={'username': 'admin', 'password': PASSWORD})
        assert r.status_code == 200, r.text
        wait_for(lambda: http.get(base + '/api/status').json().get('connected'), timeout=20, what='the broker connection')
        yield {'url': base, 'http': http, 'root': root, 'log': data / 'sensehive.log'}
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        if IN_CI:
            print((data / 'sensehive.log').read_text(errors='replace')[-4000:])


def _healthy(base):
    try:
        return requests.get(base + '/api/health', timeout=2).ok
    except requests.RequestException:
        return False


def nodes(sh):
    return sh['http'].get(sh['url'] + '/api/nodes').json()


def node(sh, topic):
    return next((n for n in nodes(sh) if n['topic'] == topic), None)


def new_key(sh, label):
    r = sh['http'].post(sh['url'] + '/api/secure/keys', json={'count': 1, 'label': label})
    assert r.status_code == 200, r.text
    k = r.json()['keys'][0]
    return {'kid': k['kid'], 'key': k['key'], 'root': sh['root']}


def rejected(sh):
    return sh['http'].get(sh['url'] + '/api/secure/status').json()['rejected']


def base_topic(kind):
    return f'pp-{kind}-{uuid.uuid4().hex[:8]}'



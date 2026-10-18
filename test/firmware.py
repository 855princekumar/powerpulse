"""Builds a PowerPulse firmware with settings filled in, the way a user fills in
config.h, so CI and the Wokwi test run exactly what a user uploads.

    python test/firmware.py auto  --out build/auto   PP_WIFI_SSID=Wokwi-GUEST SH_KEY_ID=... SH_KEY=...
    python test/firmware.py manual --out build/manual

Needs arduino-cli with the esp32 core and the libraries (see Readme.md).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
FQBN = 'esp32:esp32:esp32'
NUMBERS = {'PP_MQTT_PORT'}
ALLOWED = {'PP_WIFI_SSID', 'PP_WIFI_PASS', 'PP_MQTT_HOST', 'PP_MQTT_PORT', 'PP_MQTT_USER', 'PP_MQTT_PASS',
           'PP_BASE_TOPIC', 'PP_LOCATION', 'SH_KEY_ID', 'SH_KEY', 'SH_ROOT'}


def sketch_name(variant):
    return f'PowerPulse-v2.0-{variant}'


def c_string(value):
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def build(variant, out, settings=None, arduino_cli=None):
    """Compiles the variant with `settings` ({name: value}) written into a copy of
    its config.h. Returns the output folder (firmware .bin and .elf)."""
    settings = settings or {}
    unknown = set(settings) - ALLOWED
    if unknown:
        raise ValueError(f'unknown settings: {sorted(unknown)}')
    if variant == 'manual' and {'SH_KEY_ID', 'SH_KEY', 'SH_ROOT'} & set(settings):
        raise ValueError('the manual build has no SenseHive key')
    name = sketch_name(variant)
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        sketch = Path(tmp) / name
        shutil.copytree(REPO / 'Firmware' / name, sketch)
        lines = [f'#define {k} {v if k in NUMBERS else c_string(v)}' for k, v in sorted(settings.items())]
        config = sketch / 'config.h'
        text = config.read_text(encoding='utf-8')
        config.write_text(text.replace('#pragma once\n', '#pragma once\n' + ''.join(x + '\n' for x in lines), 1),
                          encoding='utf-8')
        cli = arduino_cli or os.getenv('ARDUINO_CLI', 'arduino-cli')
        subprocess.run([cli, 'compile', '--fqbn', FQBN, '--warnings', 'all', '--output-dir', str(out), str(sketch)],
                       check=True)
    for suffix in ('.ino.merged.bin', '.ino.elf'):
        if not (out / (name + suffix)).is_file():
            raise FileNotFoundError(out / (name + suffix))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('variant', choices=('auto', 'manual'))
    ap.add_argument('--out', required=True)
    ap.add_argument('settings', nargs='*', help='NAME=value, as in config.h')
    a = ap.parse_args()
    settings = dict(s.split('=', 1) for s in a.settings)
    print(build(a.variant, a.out, settings))


if __name__ == '__main__':
    sys.exit(main())

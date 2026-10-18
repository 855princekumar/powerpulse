# PowerPulse v2.0 with SenseHive

PowerPulse v2.0 is made to work with
[SenseHive](https://github.com/855princekumar/sense-hive), a self-hosted MQTT
logger and dashboard. It comes in two builds of the same firmware:

| | **Auto-discovery** (`PowerPulse-v2.0-auto`) | **Manual** (`PowerPulse-v2.0-manual`) |
| --- | --- | --- |
| In SenseHive | Appears by itself under **Secure topics** | Added by hand under **Open topics** |
| On the broker | Encrypted and signed (only SenseHive can read it) | Plain JSON (anyone on the broker can read it) |
| Needs | A key from SenseHive (one per node) | Nothing extra |
| Online / offline | Yes: the node tells SenseHive its status topic | Yes: you enter the status topic when adding |
| Best for | Fleets, public brokers, "flash it and plug it in" | A few nodes, your own broker, other tools reading the beacon |

Both keep PowerPulse's core idea: the broker's last will on `<base>/status`
says **OFF** the moment power is lost, so SenseHive shows the node offline
without guessing from missing heartbeats.

![SenseHive secure topics](https://raw.githubusercontent.com/855princekumar/sense-hive/main/assets/secure-topics.png)

## Contents

- [Minimum requirements](#minimum-requirements)
- [Auto-discovery: flash it, plug it in](#auto-discovery-flash-it-plug-it-in)
- [Manual: add the topics yourself](#manual-add-the-topics-yourself)
- [Uploading the firmware](#uploading-the-firmware)
- [The setup portal](#the-setup-portal)
- [Topics and messages](#topics-and-messages)
- [How auto-discovery works](#how-auto-discovery-works)
- [What changed from v1.1](#what-changed-from-v11)
- [How it is tested](#how-it-is-tested)
- [Security notes](#security-notes)
- [Troubleshooting](#troubleshooting)

## Minimum requirements

| | Minimum |
| --- | --- |
| Board | ESP32 with 4 MB flash (ESP32-WROOM DevKit used in the prototype). The firmware uses about 1.06 MB of flash and 50 KB of RAM |
| Network | 2.4 GHz Wi-Fi with internet (time from NTP; the public broker if you use it) |
| Broker | Any MQTT 3.1.1 broker that SenseHive also uses (default: the public `broker.hivemq.com`) |
| SenseHive | 1.7.0 or newer for auto-discovery (secure topics); any version for manual |
| Clock | Internet time (NTP) for the auto build: SenseHive refuses messages more than 5 minutes off. The node waits for the time before it sends |
| To build | Arduino IDE 2 or arduino-cli, ESP32 core **3.x** (tested with 3.3.12), WiFiManager 2.0.17, PubSubClient 2.8, ArduinoJson 7 |

## Auto-discovery: flash it, plug it in

1. **In SenseHive** (1.7+): **Topic logs**, then the **Secure topics** card,
   then **Generate keys**. One key per node; give them a label (the name shown
   in SenseHive, e.g. `powerpulse-pump-house`). Download the CSV: each row has a
   key id (16 hex) and a key (64 hex). The keys are shown only once.

   ![Generate keys in SenseHive](https://raw.githubusercontent.com/855princekumar/sense-hive/main/assets/secure-generate.png)

2. **In `Firmware/PowerPulse-v2.0-auto/config.h`** fill in one row:

   ```c
   #define SH_KEY_ID "0123456789abcdef"                                   // key_id from the CSV
   #define SH_KEY    "00112233...(64 hex)...ccddeeff"                     // key from the CSV
   ```

   Optionally also your Wi-Fi (`PP_WIFI_SSID`, `PP_WIFI_PASS`) and broker
   (`PP_MQTT_HOST`, default `broker.hivemq.com`, which must be the broker
   SenseHive uses). Leave Wi-Fi empty to choose it in the setup portal instead.

3. **Upload** (below), power the node, and that is all. Within seconds of its
   first beacon, the node appears in SenseHive under **Secure topics** as
   `sensehive/secure/powerpulse-a1b2c3/powerbeacon`, with its label, its
   values, and **Active**. Pull the plug: within about 25 seconds it shows
   **Inactive** (the broker's last will, after its 15 s keepalive runs out).

Prefer not to put keys in source code? Leave `config.h` empty, upload once, and
enter the Wi-Fi, key id and key in the [setup portal](#the-setup-portal). Every
node can then run the same firmware file.

The first valid message binds the key to the node's topic for good. A key not
used within 24 hours expires; generate a new one.

## Manual: add the topics yourself

1. Optionally fill in `Firmware/PowerPulse-v2.0-manual/config.h` (Wi-Fi, broker,
   `PP_BASE_TOPIC`), or leave it empty and use the setup portal.
2. Upload and power the node. The serial monitor (115200 baud) prints its
   topics:

   ```text
   [PowerPulse] Beacon topic powerpulse-a1b2c3/powerbeacon, status (last will) topic powerpulse-a1b2c3/status
   ```

3. **In SenseHive:** **Add Node** with

   | Field | Value |
   | ----- | ----- |
   | MQTT Topic | `powerpulse-a1b2c3/powerbeacon` |
   | Node ID | anything, e.g. `powerpulse-a1b2c3` |
   | Tag | e.g. `powerpulse` |
   | LWT / status topic | `powerpulse-a1b2c3/status` |

   The beacons appear on the card, and the status shows **Active** (the
   retained `{"status":"ON"}`) or **Inactive** after a power cut.

Until the topic is added, SenseHive ignores the node: plain topics are never
discovered by themselves.

## Uploading the firmware

**Arduino IDE 2:** add the ESP32 boards
(`https://espressif.github.io/arduino-esp32/package_esp32_index.json` in
Preferences, then Boards Manager, **esp32 by Espressif**, 3.x). Install the
libraries **WiFiManager** (tzapu) 2.0.17, **PubSubClient** 2.8 and
**ArduinoJson** 7. Open `PowerPulse-v2.0-auto.ino` (or `-manual`), choose
**ESP32 Dev Module**, and Upload.

**arduino-cli:**

```bash
arduino-cli core install esp32:esp32@3.3.12
arduino-cli lib install "WiFiManager@2.0.17" "PubSubClient@2.8" "ArduinoJson@7.4.2"
arduino-cli compile --fqbn esp32:esp32:esp32 Firmware/PowerPulse-v2.0-auto
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 Firmware/PowerPulse-v2.0-auto
```

**Ready-made binaries:** every CI run builds both firmwares with an empty
`config.h` (artifact `powerpulse-v2.0-firmware`). Flash one with
`esptool.py write_flash 0x0 PowerPulse-v2.0-auto.ino.merged.bin` and set
everything in the setup portal.

## The setup portal

Hold **GPIO27 to GND** while powering on (or on first boot without Wi-Fi), join
the Wi-Fi network **PowerPulse-Setup** and open `192.168.4.1`:

| Field | Meaning |
| ----- | ------- |
| Wi-Fi | The network to join |
| MQTT host, port, username, password | The broker SenseHive uses |
| Last will QoS | 0 or 1 |
| Base topic | Empty: `powerpulse-` and the last 6 hex digits of the MAC |
| Location / tag | Sent in every beacon |
| SenseHive key id, key, secure root | Auto build only. An empty key field keeps the saved key |

Saved values win over `config.h`. The portal closes after 5 minutes without a
save and the node restarts.

## Topics and messages

| Topic | Build | Retained | Message |
| ----- | ----- | -------- | ------- |
| `<base>/status` | both | yes | `{"node_id","location","status":"ON"}` on connect; `{"status":"OFF","reason":"lwt"}` from the broker when the node disappears |
| `<base>/powerbeacon` | manual | no | every 10 s: `node_id, location, ip, mac, uptime_sec, wifi_ssid, rssi_dbm, broker, firmware, status, timestamp` (UTC, ISO 8601) |
| `sensehive/secure/<base>/powerbeacon` | auto | no | every 10 s: the same beacon plus `sensehive_lwt`, sealed (below) |

The status topic is the same in both builds and in v1.x, so existing
dashboards, Node-RED flows and alerts keep working.

## How auto-discovery works

```text
 ESP32 (PowerPulse auto)                 MQTT broker                    SenseHive 1.7+
 ----------------------                  -----------                    --------------
 connect, last will on <base>/status --> holds the will
 <base>/status  {"status":"ON"}  ------> retained
 beacon JSON + "sensehive_lwt"
   sealed with the key (sst1)  --------> sensehive/secure/<base>/...  -> subscribed to sensehive/secure/#
                                                                         checks key id, topic, time, signature
                                                                         decrypts, adds the topic (first time)
                                                                         follows <base>/status for ON/OFF
 power cut  ...........................> sends the will: OFF  -------> Inactive
```

Sealing follows SenseHive's protocol **sst1**
([spec](https://github.com/855princekumar/sense-hive/blob/main/docs/SECURE-TOPICS.md#the-protocol-sst1)):
AES-256-CTR with a fresh random nonce, then HMAC-SHA256 over the topic, the key
id, the time, the nonce and the ciphertext. The code is
[`sensehive_secure.h`](../Firmware/PowerPulse-v2.0-auto/sensehive_secure.h),
plain C on the ESP32's built-in mbedTLS, so no extra library is needed.

## What changed from v1.1

| | v1.1 | v2.0 |
| --- | --- | --- |
| Builds | One | Auto-discovery (secure) and manual (plain), one shared sketch |
| Settings | Portal only | `config.h` (upload and done) or the portal |
| Default base topic | `espnode-1` (the same on every board) | `powerpulse-<MAC>`: unique per board |
| Time | NTPClient library | The ESP32's built-in SNTP, UTC ISO timestamps |
| MQTT buffer | PubSubClient default (256 bytes) | 1 KB: a long or sealed beacon is never dropped |
| Beacon retained | When QoS was set to 1 (the QoS was passed as the retain flag) | Never |
| Watchdog | Armed before the portal (an open portal could trip it) | Armed after Wi-Fi setup; reset before each connect |
| Portal | Waits forever | Restarts after 5 minutes |
| Status payload | `ON` / `OFF` | Unchanged |

## How it is tested

Every push and pull request (and weekly, against SenseHive's latest `main`):

| Test | What it proves |
| ---- | -------------- |
| Firmware builds | Both builds compile for the ESP32 (core 3.3.12), as shipped and with every setting filled in; the two sketches are the same file |
| The firmware's encryption | `sensehive_secure.h` is compiled on the test machine and must produce **byte for byte** what SenseHive's own code produces, and SenseHive must open it |
| End to end (simulated node) | SenseHive started from source, a real Mosquitto broker, a simulated node publishing exactly what the firmware publishes (sealed by the firmware's own code): the auto node appears by itself, shows online, then offline on a power cut; the manual node is ignored until added, then logged; plain or forged messages on the secure root and a copied key are refused |
| Real firmware on a simulated ESP32 | [Wokwi](https://wokwi.com) runs the compiled firmware with Wi-Fi and internet against SenseHive over the public broker, for both builds (needs the `WOKWI_CLI_TOKEN` secret) |

Run the tests on your machine (Docker, from the folder holding both clones):

```bash
docker run --rm -v "$PWD/powerpulse:/pp:ro" -v "$PWD/sense-hive:/sh:ro" ubuntu:24.04 sh /pp/test/run_local.sh
```

The Wokwi test needs a token: create one at
[wokwi.com/dashboard/ci](https://wokwi.com/dashboard/ci) and add it as the
repository secret `WOKWI_CLI_TOKEN`. Without it, that job is skipped with a
notice.

## Security notes

- **One key per node.** A copied key is refused on any other topic. If a node
  is lost or stolen, revoke its key in SenseHive.
- **The key is in the node's flash.** Anyone with the board and a USB cable can
  read it. For high-value sites, turn on the ESP32's flash encryption.
- **The status topic is plain.** A last will comes from the broker, not the
  node, so it cannot be signed. Anyone who can publish on the broker could fake
  an ON or OFF; the beacon data itself cannot be faked. On your own broker, use
  credentials and ACLs.
- **`config.h` with a key is a secret.** Do not commit a filled-in `config.h`;
  CI refuses one.

## Troubleshooting

| Serial message | Meaning and fix |
| -------------- | --------------- |
| `[SH] No valid SenseHive key` | `SH_KEY_ID` must be 16 hex characters and `SH_KEY` 64, from the same CSV row |
| `[SH] Waiting for the clock (NTP)` | No internet time yet. Check that the network allows NTP (UDP 123) |
| `[MQTT] Connecting to ...` repeating | Wrong broker, port or credentials, or the broker is down |
| `[WiFi] Setup portal timed out` | No network saved or reachable: open the portal and choose one |
| Node never appears in SenseHive | Same broker? SenseHive 1.7+? Key not expired (24 h unused)? The **Refused** count on the Secure topics card (hover it) tells why |
| Shows *No LWT* | The node has not sent a beacon yet, or SenseHive is older than 1.7.0 |

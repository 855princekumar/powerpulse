// PowerPulse v2.0 (auto-discovery): fill this in, upload, done.
//
// Everything here can also be set later in the setup portal (hold GPIO27 LOW at
// boot, join the "PowerPulse-Setup" Wi-Fi). Values saved in the portal win over
// the ones below. Each value can also be given as a build flag (-DPP_...=...).
#pragma once

// This build: encrypted for SenseHive secure topics, found by SenseHive by itself.
#define PP_SECURE 1

// ---- Wi-Fi ---------------------------------------------------------------
// Empty: the setup portal opens on first boot to choose the network.
#ifndef PP_WIFI_SSID
#define PP_WIFI_SSID ""
#endif
#ifndef PP_WIFI_PASS
#define PP_WIFI_PASS ""
#endif

// ---- MQTT broker (the same broker SenseHive listens to) --------------------
#ifndef PP_MQTT_HOST
#define PP_MQTT_HOST "broker.hivemq.com"
#endif
#ifndef PP_MQTT_PORT
#define PP_MQTT_PORT 1883
#endif
#ifndef PP_MQTT_USER
#define PP_MQTT_USER ""
#endif
#ifndef PP_MQTT_PASS
#define PP_MQTT_PASS ""
#endif

// ---- This node --------------------------------------------------------------
// Empty: "powerpulse-" and the last 6 hex digits of the MAC, unique per board.
// Keep it the same once the node is in use: SenseHive binds the key to the topic.
#ifndef PP_BASE_TOPIC
#define PP_BASE_TOPIC ""
#endif
#ifndef PP_LOCATION
#define PP_LOCATION "unknown"
#endif

// ---- SenseHive secure topic key ------------------------------------------
// From SenseHive: Topic logs > Secure topics > Generate keys (one key per node).
// The key id is 16 hex characters, the key 64. Keep this file private once filled.
#ifndef SH_KEY_ID
#define SH_KEY_ID ""
#endif
#ifndef SH_KEY
#define SH_KEY ""
#endif
// SenseHive's secure topic root (SENSEHIVE_SECURE_ROOT, default sensehive/secure).
#ifndef SH_ROOT
#define SH_ROOT "sensehive/secure"
#endif

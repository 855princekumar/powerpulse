// PowerPulse v2.0 (manual): fill this in, upload, then add the topics in SenseHive by hand.
//
// Everything here can also be set later in the setup portal (hold GPIO27 LOW at
// boot, join the "PowerPulse-Setup" Wi-Fi). Values saved in the portal win over
// the ones below. Each value can also be given as a build flag (-DPP_...=...).
#pragma once

// This build: plain topics. In SenseHive, Add Node with topic <base>/powerbeacon and
// last will topic <base>/status (the serial log prints both at boot).
#define PP_SECURE 0

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
// Set it to something you will recognise when adding the topic in SenseHive.
#ifndef PP_BASE_TOPIC
#define PP_BASE_TOPIC ""
#endif
#ifndef PP_LOCATION
#define PP_LOCATION "unknown"
#endif

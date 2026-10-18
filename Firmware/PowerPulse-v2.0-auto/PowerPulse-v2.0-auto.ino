// PowerPulse v2.0: a power presence beacon for the ESP32, made for SenseHive.
//
// One sketch, two builds (set in config.h):
//   PowerPulse-v2.0-auto    PP_SECURE 1: encrypted and signed for SenseHive secure topics;
//                           SenseHive finds the node by itself (auto-discovery).
//   PowerPulse-v2.0-manual  PP_SECURE 0: plain topics, added in SenseHive by hand.
// The two folders hold the same .ino; only config.h differs (CI checks this).
//
// Topics (<base> is the base topic):
//   <base>/status         retained, plain: {"status":"ON"} on connect, {"status":"OFF"} as the
//                         broker's last will when power or network is lost
//   <base>/powerbeacon    every 10 s (manual build)
//   <root>/<base>/powerbeacon  every 10 s, sealed (auto build; root is SenseHive's secure root)
//
// Libraries: WiFiManager (tzapu) 2.0.17, PubSubClient 2.8, ArduinoJson 7.
// Board: ESP32 (Arduino core 3.x). Docs: Readme.md and docs/SENSEHIVE.md.
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "config.h"
#ifndef PP_SECURE
#define PP_SECURE 1
#endif
#if PP_SECURE
#include "sensehive_secure.h"
#endif

// ================== CONFIG ==================
#define CONFIG_PIN 27
#define HEARTBEAT_INTERVAL 10000   // ms

#define FALLBACK_MQTT_HOST "broker.hivemq.com"
#define FALLBACK_MQTT_PORT 1883

#if PP_SECURE
#define FIRMWARE_VERSION "powerpulse-v2.0-auto"
#else
#define FIRMWARE_VERSION "powerpulse-v2.0-manual"
#endif

// Stability tuning
#define WIFI_CONNECT_TIMEOUT_MS    30000
#define WIFI_RETRY_INTERVAL_MS     5000
#define WIFI_MAX_FAILURE_TIME_MS   90000   // 90 seconds
#define MQTT_RETRY_INTERVAL_MS     5000
#define PORTAL_TIMEOUT_SEC         300     // an open setup portal restarts the node after this

#define WDT_TIMEOUT_SEC 8
#define VALID_TIME 1700000000UL            // the clock is set (NTP) once time() is past this
// ============================================


// ================== GLOBALS ==================
WiFiClient espClient;
PubSubClient mqtt(espClient);
Preferences prefs;

unsigned long lastPublish = 0;
unsigned long bootMillis  = 0;

// WiFi / MQTT state
unsigned long wifiFailStart = 0;
unsigned long lastWifiRetry = 0;
unsigned long lastMqttRetry = 0;
unsigned long lastWarning   = 0;
bool wdtArmed = false;

// Settings: config.h, overridden by what was saved in the setup portal (NVS)
String mqttHost, mqttUser, mqttPass;
int    mqttPort, qos;
String baseTopic, locationTag;
String activeBroker = "none";
String clientId;

#if PP_SECURE
String shKeyId, shKeyHex, shRoot;
uint8_t shKey[SH_KEY_LEN];
bool shKeyOk = false;
#endif
// ============================================


// ================== SETTINGS =================
String pick(const char* key, const char* fallback) {
  String v = prefs.getString(key, "");
  v.trim();
  return v.length() ? v : String(fallback);
}

String defaultBaseTopic() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[24];
  snprintf(buf, sizeof buf, "powerpulse-%02x%02x%02x", mac[3], mac[4], mac[5]);
  return String(buf);
}

void loadConfig() {
  prefs.begin("config", true);
  mqttHost    = pick("lanHost", PP_MQTT_HOST);
  mqttPort    = prefs.getInt("lanPort", 0) > 0 ? prefs.getInt("lanPort", 0) : PP_MQTT_PORT;
  mqttUser    = pick("lanUser", PP_MQTT_USER);
  mqttPass    = pick("lanPass", PP_MQTT_PASS);
  qos         = constrain(prefs.getInt("qos", 0), 0, 1);
  baseTopic   = pick("baseTopic", PP_BASE_TOPIC);
  locationTag = pick("location", PP_LOCATION);
#if PP_SECURE
  shKeyId     = pick("shKeyId", SH_KEY_ID);
  shKeyHex    = pick("shKey", SH_KEY);
  shRoot      = pick("shRoot", SH_ROOT);
  while (shRoot.endsWith("/")) shRoot.remove(shRoot.length() - 1);
  shKeyId.toLowerCase();
  shKeyOk = sh_parse_key(shKeyId.c_str(), shKeyHex.c_str(), shKey);
#endif
  prefs.end();
  if (baseTopic.length() == 0) baseTopic = defaultBaseTopic();
}
// ============================================


// ================== MQTT =====================
String statusTopic() { return baseTopic + "/status"; }

String beaconTopic() {
#if PP_SECURE
  return shRoot + "/" + baseTopic + "/powerbeacon";
#else
  return baseTopic + "/powerbeacon";
#endif
}

bool connectMQTT(const char* host, int port, const char* user, const char* pass) {
  if (wdtArmed) esp_task_wdt_reset();      // a connect can take a few seconds
  mqtt.setServer(host, port);
  clientId = baseTopic + "-" + WiFi.macAddress();

  String willPayload =
    "{\"node_id\":\"" + baseTopic +
    "\",\"location\":\"" + locationTag +
    "\",\"status\":\"OFF\",\"reason\":\"lwt\"}";

  Serial.printf("[MQTT] Connecting to %s:%d\n", host, port);

  bool ok = mqtt.connect(clientId.c_str(),
                         strlen(user) ? user : nullptr, strlen(pass) ? pass : nullptr,
                         statusTopic().c_str(), qos, true, willPayload.c_str());
  if (ok) {
    Serial.println("[MQTT] Connected");
    JsonDocument doc;
    doc["node_id"]  = baseTopic;
    doc["location"] = locationTag;
    doc["status"]   = "ON";
    char buf[192];
    serializeJson(doc, buf, sizeof buf);
    mqtt.publish(statusTopic().c_str(), buf, true);
  }
  return ok;
}

bool timeReady() { return time(nullptr) > (time_t)VALID_TIME; }

void publishBeacon() {
  JsonDocument doc;
  doc["node_id"]    = baseTopic;
  doc["location"]   = locationTag;
  doc["ip"]         = WiFi.localIP().toString();
  doc["mac"]        = WiFi.macAddress();
  doc["uptime_sec"] = (millis() - bootMillis) / 1000;
  doc["wifi_ssid"]  = WiFi.SSID();
  doc["rssi_dbm"]   = WiFi.RSSI();
  doc["broker"]     = activeBroker;
  doc["firmware"]   = FIRMWARE_VERSION;
  doc["status"]     = "ON";
  if (timeReady()) {
    char iso[24];
    time_t now = time(nullptr);
    strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    doc["timestamp"] = iso;
  } else {
    doc["timestamp"] = String("uptime+") + String((millis() - bootMillis) / 1000);
  }

#if PP_SECURE
  // SenseHive follows this plain topic for online and offline (it is set once, from a signed message)
  doc["sensehive_lwt"] = statusTopic();
  if (!shKeyOk || !timeReady()) {
    if (millis() - lastWarning > 30000) {
      lastWarning = millis();
      Serial.println(!shKeyOk ? "[SH] No valid SenseHive key (SH_KEY_ID 16 hex, SH_KEY 64 hex): beacon not sent"
                              : "[SH] Waiting for the clock (NTP) before sealing");
    }
    return;
  }
  char payload[512];
  size_t len = serializeJson(doc, payload, sizeof payload);
  static char sealed[SH_SEALED_SIZE(512)];
  String topic = beaconTopic();
  size_t n = sh_seal(shKey, shKeyId.c_str(), topic.c_str(), (unsigned long long)time(nullptr),
                     (const uint8_t*)payload, len, nullptr, sealed, sizeof sealed);
  if (n && mqtt.publish(topic.c_str(), sealed, false)) {
    Serial.printf("[SH] Sealed beacon sent to %s (%u bytes)\n", topic.c_str(), (unsigned)n);
  } else {
    Serial.println("[SH] Beacon not sent");
  }
#else
  char payload[512];
  serializeJson(doc, payload, sizeof payload);
  String topic = beaconTopic();
  if (mqtt.publish(topic.c_str(), payload, false)) {
    Serial.printf("[MQTT] Beacon sent to %s\n", topic.c_str());
  } else {
    Serial.println("[MQTT] Beacon not sent");
  }
#endif
}

void connectBroker() {
  if (connectMQTT(mqttHost.c_str(), mqttPort, mqttUser.c_str(), mqttPass.c_str())) {
    activeBroker = mqttHost == FALLBACK_MQTT_HOST ? "fallback" : "lan";
  } else if (mqttHost != FALLBACK_MQTT_HOST && connectMQTT(FALLBACK_MQTT_HOST, FALLBACK_MQTT_PORT, "", "")) {
    activeBroker = "fallback";
  }
}
// ============================================


// ================== SETUP ====================
bool portalSaved = false;

void runPortal(bool forced) {
  WiFiManager wm;
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_SEC);
  wm.setSaveConfigCallback([]() { portalSaved = true; });   // the portal was shown and saved

  char portStr[8];
  snprintf(portStr, sizeof portStr, "%d", PP_MQTT_PORT);
  WiFiManagerParameter p_host("lan_host", "MQTT host (the broker SenseHive uses)", PP_MQTT_HOST, 64);
  WiFiManagerParameter p_port("lan_port", "MQTT port", portStr, 6);
  WiFiManagerParameter p_user("lan_user", "MQTT username", "", 32);
  WiFiManagerParameter p_pass("lan_pass", "MQTT password", "", 32);
  WiFiManagerParameter p_qos("qos", "Last will QoS (0/1)", "0", 2);
  WiFiManagerParameter p_topic("base_topic", "Base topic (empty: from the MAC)", PP_BASE_TOPIC, 40);
  WiFiManagerParameter p_location("location", "Location / tag", "", 40);
  wm.addParameter(&p_host);
  wm.addParameter(&p_port);
  wm.addParameter(&p_user);
  wm.addParameter(&p_pass);
  wm.addParameter(&p_qos);
  wm.addParameter(&p_topic);
  wm.addParameter(&p_location);
#if PP_SECURE
  WiFiManagerParameter p_kid("sh_kid", "SenseHive key id (16 hex)", "", 17);
  WiFiManagerParameter p_key("sh_key", "SenseHive key (64 hex)", "", 65);
  WiFiManagerParameter p_root("sh_root", "SenseHive secure root", SH_ROOT, 64);
  wm.addParameter(&p_kid);
  wm.addParameter(&p_key);
  wm.addParameter(&p_root);
#endif

  bool saved = forced ? wm.startConfigPortal("PowerPulse-Setup") : wm.autoConnect("PowerPulse-Setup");
  if (!saved) {
    Serial.println("[WiFi] Setup portal timed out, restarting");
    ESP.restart();
  }
  if (forced || portalSaved) {                              // keep what was entered in the portal
    prefs.begin("config", false);
    prefs.putString("lanHost", p_host.getValue());
    prefs.putInt("lanPort", atoi(p_port.getValue()));
    prefs.putString("lanUser", p_user.getValue());
    prefs.putString("lanPass", p_pass.getValue());
    prefs.putInt("qos", atoi(p_qos.getValue()));
    prefs.putString("baseTopic", p_topic.getValue());
    prefs.putString("location", p_location.getValue());
#if PP_SECURE
    if (strlen(p_kid.getValue())) prefs.putString("shKeyId", p_kid.getValue());   // empty keeps the saved key
    if (strlen(p_key.getValue())) prefs.putString("shKey", p_key.getValue());
    prefs.putString("shRoot", p_root.getValue());
#endif
    prefs.end();
    if (forced) ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  bootMillis = millis();
  pinMode(CONFIG_PIN, INPUT_PULLUP);
  Serial.printf("\n[PowerPulse] %s\n", FIRMWARE_VERSION);

  WiFi.mode(WIFI_STA);
  if (digitalRead(CONFIG_PIN) == LOW) {
    runPortal(true);                       // saves and restarts
  } else if (strlen(PP_WIFI_SSID)) {
    WiFi.begin(PP_WIFI_SSID, PP_WIFI_PASS);
    Serial.printf("[WiFi] Connecting to %s", PP_WIFI_SSID);
    while (WiFi.status() != WL_CONNECTED && millis() - bootMillis < WIFI_CONNECT_TIMEOUT_MS) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) runPortal(false);
  } else {
    runPortal(false);                      // saved network, or the portal on first boot
  }
  Serial.printf("[WiFi] Connected, IP %s\n", WiFi.localIP().toString().c_str());

  loadConfig();
  Serial.printf("[PowerPulse] Beacon topic %s, status (last will) topic %s\n", beaconTopic().c_str(), statusTopic().c_str());
#if PP_SECURE
  if (!shKeyOk) Serial.println("[SH] No valid SenseHive key yet: set SH_KEY_ID and SH_KEY (config.h or the portal)");
#endif

  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");   // UTC, in the background
  mqtt.setBufferSize(1024);                // a sealed beacon is about 600 bytes
  mqtt.setKeepAlive(15);
  connectBroker();

  // Watchdog after the (blocking) Wi-Fi setup, so an open portal cannot trip it
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  // The Arduino core starts the watchdog at boot: change its settings, or start it if a build did not
  if (esp_task_wdt_reconfigure(&wdt_config) != ESP_OK) esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
  wdtArmed = true;
}
// ============================================


// ================== LOOP =====================
void loop() {
  esp_task_wdt_reset();

  // ---- WiFi management (timed reconnect) ----
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiFailStart == 0) wifiFailStart = millis();
    if (millis() - wifiFailStart > WIFI_MAX_FAILURE_TIME_MS) {
      Serial.println("[WiFi] Prolonged failure, rebooting");
      ESP.restart();
    }
    if (millis() - lastWifiRetry > WIFI_RETRY_INTERVAL_MS) {
      lastWifiRetry = millis();
      WiFi.reconnect();
    }
    delay(10);
    return;
  }
  wifiFailStart = 0;

  // ---- MQTT reconnect (no reboot) ----
  if (!mqtt.connected()) {
    if (millis() - lastMqttRetry > MQTT_RETRY_INTERVAL_MS) {
      lastMqttRetry = millis();
      connectBroker();
    }
    delay(10);
    return;
  }
  mqtt.loop();

  // ---- Publish beacon ----
  if (millis() - lastPublish > HEARTBEAT_INTERVAL) {
    lastPublish = millis();
    publishBeacon();
  }
  delay(10);
}
// ============================================

#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <esp_task_wdt.h>

// ================== CONFIG ==================
#define CONFIG_PIN 27
#define HEARTBEAT_INTERVAL 10000   // ms

#define FALLBACK_MQTT_HOST "broker.hivemq.com"
#define FALLBACK_MQTT_PORT 1883

#define FIRMWARE_VERSION "powerpulse-v1.0"

// Stability tuning
#define WIFI_RETRY_INTERVAL_MS     5000
#define WIFI_MAX_FAILURE_TIME_MS   90000   // 90 seconds
#define MQTT_RETRY_INTERVAL_MS     5000

#define WDT_TIMEOUT_SEC 8
// ============================================


// ================== GLOBALS ==================
WiFiClient espClient;
PubSubClient mqtt(espClient);
Preferences prefs;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

unsigned long lastPublish = 0;
unsigned long bootMillis  = 0;

// WiFi / MQTT state
unsigned long wifiFailStart = 0;
unsigned long lastWifiRetry = 0;
unsigned long lastMqttRetry = 0;

// NTP state
bool ntpReady = false;

// User-configured (NVS)
String lanHost, lanUser, lanPass;
int    lanPort, qos;
bool   lanEnabled = false;

String baseTopic   = "espnode-1";
String locationTag = "unknown";
String activeBroker = "none";
String clientId;

// Watchdog config (ESP-IDF v5.x)
esp_task_wdt_config_t wdt_config = {
  .timeout_ms = WDT_TIMEOUT_SEC * 1000,
  .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
  .trigger_panic = true
};
// ============================================


// ================== NVS ======================
void saveConfig(String host, int port, String user, String pass,
                int q, String topic, String location) {
  prefs.begin("config", false);
  prefs.putString("lanHost", host);
  prefs.putInt("lanPort", port);
  prefs.putString("lanUser", user);
  prefs.putString("lanPass", pass);
  prefs.putInt("qos", q);
  prefs.putString("baseTopic", topic);
  prefs.putString("location", location);
  prefs.end();
}

void loadConfig() {
  prefs.begin("config", true);
  lanHost     = prefs.getString("lanHost", "");
  lanPort     = prefs.getInt("lanPort", 1883);
  lanUser     = prefs.getString("lanUser", "");
  lanPass     = prefs.getString("lanPass", "");
  qos         = prefs.getInt("qos", 0);
  baseTopic   = prefs.getString("baseTopic", "espnode-1");
  locationTag = prefs.getString("location", "unknown");
  prefs.end();

  lanEnabled = lanHost.length() > 0;
}
// ============================================


// ================== MQTT =====================
bool connectMQTT(const char* host, int port,
                 const char* user, const char* pass) {

  mqtt.setServer(host, port);
  clientId = baseTopic + "-" + WiFi.macAddress();

  String willTopic = baseTopic + "/status";
  String willPayload =
    "{\"node_id\":\"" + baseTopic +
    "\",\"location\":\"" + locationTag +
    "\",\"status\":\"OFF\",\"reason\":\"lwt\"}";

  Serial.printf("[MQTT] Connecting to %s:%d\n", host, port);

  bool ok = mqtt.connect(
    clientId.c_str(),
    user,
    pass,
    willTopic.c_str(),
    qos,
    true,
    willPayload.c_str()
  );

  if (ok) {
    Serial.println("[MQTT] Connected");

    StaticJsonDocument<128> doc;
    doc["node_id"]  = baseTopic;
    doc["location"] = locationTag;
    doc["status"]   = "ON";

    char buf[128];
    serializeJson(doc, buf);
    mqtt.publish(willTopic.c_str(), buf, true);
  }

  return ok;
}

void publishBeacon() {
  StaticJsonDocument<512> doc;

  doc["node_id"]     = baseTopic;
  doc["location"]   = locationTag;
  doc["ip"]         = WiFi.localIP().toString();
  doc["mac"]        = WiFi.macAddress();
  doc["uptime_sec"] = (millis() - bootMillis) / 1000;
  doc["wifi_ssid"]  = WiFi.SSID();
  doc["rssi_dbm"]   = WiFi.RSSI();
  doc["broker"]     = activeBroker;
  doc["firmware"]   = FIRMWARE_VERSION;
  doc["status"]     = "ON";

  if (ntpReady) {
    doc["timestamp"] = timeClient.getFormattedTime();
  } else {
    doc["timestamp"] = String("uptime+") + doc["uptime_sec"].as<String>();
  }

  char payload[512];
  serializeJson(doc, payload);

  String topic = baseTopic + "/powerbeacon";
  mqtt.publish(topic.c_str(), payload, qos);
}
// ============================================


// ================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);

  bootMillis = millis();
  pinMode(CONFIG_PIN, INPUT_PULLUP);

  // Init watchdog (ESP-IDF v5.x)
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  WiFiManager wm;

  WiFiManagerParameter p_lan_host("lan_host", "LAN MQTT Host (optional)", "", 40);
  WiFiManagerParameter p_lan_port("lan_port", "LAN MQTT Port", "1883", 6);
  WiFiManagerParameter p_lan_user("lan_user", "LAN MQTT Username", "", 32);
  WiFiManagerParameter p_lan_pass("lan_pass", "LAN MQTT Password", "", 32);
  WiFiManagerParameter p_qos("qos", "MQTT QoS (0/1/2)", "0", 2);
  WiFiManagerParameter p_topic("base_topic", "MQTT Base Topic", "espnode-1", 40);
  WiFiManagerParameter p_location("location", "Location / Tag", "", 40);

  wm.addParameter(&p_lan_host);
  wm.addParameter(&p_lan_port);
  wm.addParameter(&p_lan_user);
  wm.addParameter(&p_lan_pass);
  wm.addParameter(&p_qos);
  wm.addParameter(&p_topic);
  wm.addParameter(&p_location);

  bool configMode = (digitalRead(CONFIG_PIN) == LOW);

  if (configMode) {
    wm.startConfigPortal("PowerPulse-Setup");

    saveConfig(
      p_lan_host.getValue(),
      atoi(p_lan_port.getValue()),
      p_lan_user.getValue(),
      p_lan_pass.getValue(),
      atoi(p_qos.getValue()),
      p_topic.getValue(),
      p_location.getValue()
    );

    ESP.restart();
  }

  wm.autoConnect("PowerPulse-Setup");
  loadConfig();

  // Start NTP async (non-blocking)
  timeClient.begin();

  // MQTT connect (no dependency on NTP)
  if (lanEnabled && connectMQTT(lanHost.c_str(), lanPort,
                                lanUser.c_str(), lanPass.c_str())) {
    activeBroker = "lan";
  } else if (connectMQTT(FALLBACK_MQTT_HOST, FALLBACK_MQTT_PORT, "", "")) {
    activeBroker = "fallback";
  }
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
    return;
  } else {
    wifiFailStart = 0;
  }

  // ---- MQTT reconnect (no reboot) ----
  if (!mqtt.connected()) {
    if (millis() - lastMqttRetry > MQTT_RETRY_INTERVAL_MS) {
      lastMqttRetry = millis();
      if (activeBroker == "lan") {
        connectMQTT(lanHost.c_str(), lanPort,
                    lanUser.c_str(), lanPass.c_str());
      } else {
        connectMQTT(FALLBACK_MQTT_HOST, FALLBACK_MQTT_PORT, "", "");
      }
    }
  }

  mqtt.loop();

  // ---- Async NTP update ----
  if (!ntpReady && timeClient.update()) {
    ntpReady = true;
  }

  // ---- Publish beacon ----
  if (millis() - lastPublish > HEARTBEAT_INTERVAL) {
    publishBeacon();
    lastPublish = millis();
  }
}
// ============================================

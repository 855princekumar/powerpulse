#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Preferences.h>

// ================== CONFIG ==================
#define CONFIG_PIN 27

#define HEARTBEAT_INTERVAL 10000   // ms
#define MQTT_KEEPALIVE      5      // seconds
#define OFF_DEBOUNCE_MS  8000      // ms

#define FALLBACK_MQTT_HOST "broker.hivemq.com"
#define FALLBACK_MQTT_PORT 1883

#define FIRMWARE_VERSION "power-beacon-v1.0"
// ============================================

// ================== GLOBALS ==================
WiFiClient espClient;
PubSubClient mqtt(espClient);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);
Preferences prefs;

unsigned long lastPublish   = 0;
unsigned long bootMillis    = 0;
unsigned long wifiLostAt    = 0;
bool offPublished           = false;

// User-configured
String lanHost, lanUser, lanPass;
int    lanPort, qos;
bool   lanEnabled = false;

String baseTopic   = "espnode-1";
String locationTag = "unknown";
String activeBroker = "none";
String clientId;
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


// ================== STATUS ===================
void publishStatus(const char* state, const char* reason) {
  StaticJsonDocument<192> doc;
  doc["node_id"]  = baseTopic;
  doc["location"] = locationTag;
  doc["status"]   = state;
  doc["reason"]   = reason;

  char buf[192];
  serializeJson(doc, buf);

  String topic = baseTopic + "/status";
  mqtt.publish(topic.c_str(), buf, true);

  Serial.printf("[STATUS] %s (%s)\n", state, reason);
}

void publishGracefulOff() {
  if (offPublished) return;
  publishStatus("OFF", "graceful");
  offPublished = true;
}
// ============================================


// ================== MQTT =====================
bool connectMQTT(const char* host, int port,
                 const char* user, const char* pass) {

  mqtt.setServer(host, port);
  mqtt.setKeepAlive(MQTT_KEEPALIVE);

  clientId = baseTopic + "-" + WiFi.macAddress();

  String willTopic = baseTopic + "/status";
  String willPayload =
    "{\"node_id\":\"" + baseTopic +
    "\",\"location\":\"" + locationTag +
    "\",\"status\":\"OFF\",\"reason\":\"lwt\"}";

  Serial.printf("[MQTT] Connecting %s:%d\n", host, port);

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
    publishStatus("ON", "alive");
    offPublished = false;
    return true;
  }

  Serial.printf("[MQTT] Failed (state=%d)\n", mqtt.state());
  return false;
}

void publishBeacon() {
  StaticJsonDocument<512> doc;

  doc["node_id"]        = baseTopic;
  doc["location"]      = locationTag;
  doc["timestamp"]     = timeClient.getFormattedTime();
  doc["ip"]            = WiFi.localIP().toString();
  doc["mac"]           = WiFi.macAddress();
  doc["uptime_sec"]    = (millis() - bootMillis) / 1000;
  doc["wifi_ssid"]     = WiFi.SSID();
  doc["rssi_dbm"]      = WiFi.RSSI();
  doc["broker_active"] = activeBroker;
  doc["firmware"]      = FIRMWARE_VERSION;

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

  WiFiManager wm;

  // Config portal fields
  WiFiManagerParameter p_lan_host("lan_host", "LAN MQTT Host", "", 40);
  WiFiManagerParameter p_lan_port("lan_port", "LAN MQTT Port", "1883", 6);
  WiFiManagerParameter p_lan_user("lan_user", "LAN MQTT Username", "", 32);
  WiFiManagerParameter p_lan_pass("lan_pass", "LAN MQTT Password", "", 32);
  WiFiManagerParameter p_qos("qos", "MQTT QoS (0/1/2)", "0", 2);
  WiFiManagerParameter p_topic("base_topic", "MQTT Base Topic", "espnode-1", 40);
  WiFiManagerParameter p_location("location", "Location / Tag", "", 40);
  WiFiManagerParameter p_reset("factory_reset", "Factory Reset (enter 1)", "0", 2);

  wm.addParameter(&p_lan_host);
  wm.addParameter(&p_lan_port);
  wm.addParameter(&p_lan_user);
  wm.addParameter(&p_lan_pass);
  wm.addParameter(&p_qos);
  wm.addParameter(&p_topic);
  wm.addParameter(&p_location);
  wm.addParameter(&p_reset);

  bool configMode = digitalRead(CONFIG_PIN) == LOW;

  if (configMode) {
    wm.startConfigPortal("PowerBeacon-Setup");

    if (String(p_reset.getValue()) == "1") {
      prefs.begin("config", false);
      prefs.clear();
      prefs.end();
    }

    saveConfig(
      p_lan_host.getValue(),
      atoi(p_lan_port.getValue()),
      p_lan_user.getValue(),
      p_lan_pass.getValue(),
      atoi(p_qos.getValue()),
      p_topic.getValue(),
      p_location.getValue()
    );

    delay(1000);
    ESP.restart();
  }

  if (!wm.autoConnect("PowerBeacon-Setup")) {
    ESP.restart();
  }

  loadConfig();

  timeClient.begin();
  timeClient.forceUpdate();

  if (lanEnabled &&
      connectMQTT(lanHost.c_str(), lanPort,
                  lanUser.c_str(), lanPass.c_str())) {
    activeBroker = "lan";
  } else {
    connectMQTT(FALLBACK_MQTT_HOST, FALLBACK_MQTT_PORT, "", "");
    activeBroker = "fallback";
  }
}
// ============================================


// ================== LOOP =====================
void loop() {

  // WiFi debounce logic
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiLostAt == 0) {
      wifiLostAt = millis();
    } else if (millis() - wifiLostAt > OFF_DEBOUNCE_MS) {
      publishGracefulOff();
    }
  } else {
    wifiLostAt = 0;
  }

  // MQTT reconnect (no clean disconnect)
  if (!mqtt.connected()) {
    if (activeBroker == "lan") {
      connectMQTT(lanHost.c_str(), lanPort,
                  lanUser.c_str(), lanPass.c_str());
    } else {
      connectMQTT(FALLBACK_MQTT_HOST, FALLBACK_MQTT_PORT, "", "");
    }
  }

  mqtt.loop();
  timeClient.update();

  if (millis() - lastPublish > HEARTBEAT_INTERVAL) {
    publishBeacon();
    lastPublish = millis();
  }
}
// ============================================

/*
 * ============================================================
 *  Omni-Node — ESP12-F IoT Gateway
 *  Firmware v1.0.0
 * ============================================================
 *  Features:
 *   - Dual-mode WiFi (STA + AP failsafe)
 *   - MQTT with LWT, configurable topic prefix
 *   - IO Polling Engine with pin mapping & data types
 *   - WebSocket live-stream of GPIO readings
 *   - On-device async web dashboard (Neo-Brutalist UI)
 *   - mDNS, NTP, OTA, LittleFS config persistence
 *   - Failsafe hardware button (short=reset, long=factory)
 *   - Web authentication
 * ============================================================
 */

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <Ticker.h>

// ─── Build-time defaults ────────────────────────────────────
#define FW_VERSION        "1.0.0"
#define AP_SSID           "OmniNode-Setup"
#define AP_PASS           "omninode"
#define MDNS_HOST         "omninode"
#define CONFIG_PATH       "/config.json"
#define FAILSAFE_PIN      0          // GPIO0 (FLASH button on most boards)
#define MAX_IO_PINS       8
#define LED_PIN           2          // On-board LED (active LOW)
#define NTP_SERVER        "pool.ntp.org"
#define NTP_OFFSET        25200      // UTC+7 (seconds)

// ─── Global objects ─────────────────────────────────────────
AsyncWebServer  server(80);
AsyncWebSocket  ws("/ws");
WiFiClient      espClient;
PubSubClient    mqtt(espClient);
WiFiUDP         ntpUDP;
NTPClient       ntp(ntpUDP, NTP_SERVER, NTP_OFFSET, 60000);
Ticker          ledTicker;

// ─── Configuration structure ────────────────────────────────
struct IOPin {
  uint8_t  pin;
  char     label[32];
  bool     enabled;
  char     type[20];       // input_analog, input_digital, output_digital
  char     varType[12];    // float32, int16, uint16, bool
  float    multiplier;
  float    lastValue;
};

struct Config {
  char  deviceId[16];
  // Network
  char  ssid[64];
  char  wifiPass[64];
  bool  staticIp;
  char  ip[16];
  char  subnet[16];
  char  gw[16];
  char  dns1[16];
  char  dns2[16];
  // MQTT
  char  broker[64];
  int   mqttPort;
  char  mqttUser[32];
  char  mqttPass[32];
  char  lwtTopic[64];
  char  prefix[64];
  // IO
  IOPin io[MAX_IO_PINS];
  int   ioCount;
  // System
  char  webPass[32];
  unsigned long pollInterval;
} cfg;

// ─── Runtime state ──────────────────────────────────────────
bool          apMode          = false;
unsigned long lastPoll        = 0;
unsigned long lastMqttRetry   = 0;
unsigned long btnPressStart   = 0;
bool          btnPressed      = false;
bool          factoryTriggered = false;

// ─── Forward declarations ───────────────────────────────────
void loadConfig();
void saveConfig();
void factoryReset();
void setupWiFi();
void setupMQTT();
void setupWebServer();
void setupOTA();
void handleButton();
void pollIO();
void broadcastWS();
void reconnectMQTT();
void publishIO();
String getTimestamp();
bool checkAuth(AsyncWebServerRequest *req);

// ═══════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n\n=== Omni-Node Gateway ==="));
  Serial.printf("Firmware v%s\n", FW_VERSION);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);          // LED off (active low)
  pinMode(FAILSAFE_PIN, INPUT_PULLUP);

  // Mount filesystem
  if (!LittleFS.begin()) {
    Serial.println(F("[FS] Mount failed – formatting…"));
    LittleFS.format();
    LittleFS.begin();
  }

  loadConfig();
  setupWiFi();

  // NTP
  if (!apMode) {
    ntp.begin();
    ntp.update();
    Serial.printf("[NTP] Time: %s\n", ntp.getFormattedTime().c_str());
  }

  setupWebServer();
  setupOTA();
  if (!apMode) setupMQTT();

  Serial.println(F("[SYS] Setup complete."));
}

// ═══════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════
void loop() {
  // Button handling
  handleButton();

  // OTA
  ArduinoOTA.handle();

  // mDNS
  MDNS.update();

  // NTP update (non-blocking, library handles interval)
  if (!apMode) ntp.update();

  // MQTT keep-alive / reconnect
  if (!apMode) {
    if (!mqtt.connected()) {
      if (millis() - lastMqttRetry > 5000) {
        lastMqttRetry = millis();
        reconnectMQTT();
      }
    } else {
      mqtt.loop();
    }
  }

  // IO Polling
  if (millis() - lastPoll >= cfg.pollInterval) {
    lastPoll = millis();
    pollIO();
    broadcastWS();
    if (!apMode && mqtt.connected()) publishIO();
  }

  // WebSocket cleanup
  ws.cleanupClients();
}

// ═══════════════════════════════════════════════════════════
//  CONFIG  (LittleFS JSON)
// ═══════════════════════════════════════════════════════════
void setDefaults() {
  strlcpy(cfg.deviceId, "OMNI-01",    sizeof(cfg.deviceId));
  cfg.ssid[0]    = '\0';
  cfg.wifiPass[0] = '\0';
  cfg.staticIp   = false;
  cfg.ip[0] = cfg.subnet[0] = cfg.gw[0] = '\0';
  strlcpy(cfg.dns1, "8.8.8.8",        sizeof(cfg.dns1));
  strlcpy(cfg.dns2, "1.1.1.1",        sizeof(cfg.dns2));
  cfg.broker[0]  = '\0';
  cfg.mqttPort   = 1883;
  cfg.mqttUser[0] = '\0';
  cfg.mqttPass[0] = '\0';
  strlcpy(cfg.lwtTopic, "status",     sizeof(cfg.lwtTopic));
  strlcpy(cfg.prefix,   "nodes/01",   sizeof(cfg.prefix));
  cfg.ioCount    = 0;
  strlcpy(cfg.webPass, "admin123",    sizeof(cfg.webPass));
  cfg.pollInterval = 5000;
}

void loadConfig() {
  setDefaults();
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) { Serial.println(F("[CFG] No config file, using defaults.")); return; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { Serial.printf("[CFG] Parse error: %s\n", err.c_str()); return; }

  strlcpy(cfg.deviceId,  doc["device_id"] | "OMNI-01", sizeof(cfg.deviceId));

  JsonObject net = doc["network"];
  strlcpy(cfg.ssid,      net["ssid"]   | "", sizeof(cfg.ssid));
  strlcpy(cfg.wifiPass,  net["pass"]   | "", sizeof(cfg.wifiPass));
  cfg.staticIp = net["static_ip"] | false;
  strlcpy(cfg.ip,        net["ip"]     | "", sizeof(cfg.ip));
  strlcpy(cfg.subnet,    net["subnet"] | "", sizeof(cfg.subnet));
  strlcpy(cfg.gw,        net["gw"]     | "", sizeof(cfg.gw));
  strlcpy(cfg.dns1,      net["dns1"]   | "8.8.8.8", sizeof(cfg.dns1));
  strlcpy(cfg.dns2,      net["dns2"]   | "1.1.1.1", sizeof(cfg.dns2));

  JsonObject mq = doc["mqtt"];
  strlcpy(cfg.broker,    mq["broker"]  | "", sizeof(cfg.broker));
  cfg.mqttPort = mq["port"] | 1883;
  strlcpy(cfg.mqttUser,  mq["user"]    | "", sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPass,  mq["pass"]    | "", sizeof(cfg.mqttPass));
  strlcpy(cfg.lwtTopic,  mq["lwt_topic"] | "status", sizeof(cfg.lwtTopic));
  strlcpy(cfg.prefix,    mq["prefix"]  | "nodes/01",  sizeof(cfg.prefix));

  JsonArray io = doc["io_setup"];
  cfg.ioCount = 0;
  for (JsonObject p : io) {
    if (cfg.ioCount >= MAX_IO_PINS) break;
    IOPin &pin = cfg.io[cfg.ioCount];
    pin.pin        = p["pin"] | 0;
    strlcpy(pin.label, p["label"]   | "gpio", sizeof(pin.label));
    pin.enabled    = p["enabled"]   | false;
    strlcpy(pin.type,  p["type"]    | "input_digital", sizeof(pin.type));
    strlcpy(pin.varType, p["var_type"] | "int16", sizeof(pin.varType));
    pin.multiplier = p["multiplier"] | 1.0f;
    pin.lastValue  = 0;
    cfg.ioCount++;
  }

  JsonObject sys = doc["system"];
  strlcpy(cfg.webPass, sys["web_pass"] | "admin123", sizeof(cfg.webPass));
  cfg.pollInterval = sys["poll_interval"] | 5000;

  Serial.printf("[CFG] Loaded. Device: %s  IO pins: %d\n", cfg.deviceId, cfg.ioCount);
}

void saveConfig() {
  JsonDocument doc;
  doc["device_id"] = cfg.deviceId;

  JsonObject net = doc["network"].to<JsonObject>();
  net["ssid"]      = cfg.ssid;
  net["pass"]      = cfg.wifiPass;
  net["static_ip"] = cfg.staticIp;
  net["ip"]        = cfg.ip;
  net["subnet"]    = cfg.subnet;
  net["gw"]        = cfg.gw;
  net["dns1"]      = cfg.dns1;
  net["dns2"]      = cfg.dns2;

  JsonObject mq = doc["mqtt"].to<JsonObject>();
  mq["broker"]    = cfg.broker;
  mq["port"]      = cfg.mqttPort;
  mq["user"]      = cfg.mqttUser;
  mq["pass"]      = cfg.mqttPass;
  mq["lwt_topic"] = cfg.lwtTopic;
  mq["prefix"]    = cfg.prefix;

  JsonArray io = doc["io_setup"].to<JsonArray>();
  for (int i = 0; i < cfg.ioCount; i++) {
    JsonObject p = io.add<JsonObject>();
    p["pin"]        = cfg.io[i].pin;
    p["label"]      = cfg.io[i].label;
    p["enabled"]    = cfg.io[i].enabled;
    p["type"]       = cfg.io[i].type;
    p["var_type"]   = cfg.io[i].varType;
    p["multiplier"] = cfg.io[i].multiplier;
  }

  JsonObject sys = doc["system"].to<JsonObject>();
  sys["web_pass"]      = cfg.webPass;
  sys["poll_interval"] = cfg.pollInterval;

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) { Serial.println(F("[CFG] Write failed!")); return; }
  serializeJsonPretty(doc, f);
  f.close();
  Serial.println(F("[CFG] Saved."));
}

void factoryReset() {
  Serial.println(F("[SYS] >>> FACTORY RESET <<<"));
  LittleFS.remove(CONFIG_PATH);
  delay(500);
  ESP.restart();
}

// ═══════════════════════════════════════════════════════════
//  WiFi
// ═══════════════════════════════════════════════════════════
void setupWiFi() {
  if (strlen(cfg.ssid) == 0) {
    // No SSID configured → start AP
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("[WiFi] AP Mode — SSID: %s  IP: %s\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str());
    ledTicker.attach(0.3, []() {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    });
  } else {
    // STA mode
    WiFi.mode(WIFI_STA);

    if (cfg.staticIp) {
      IPAddress ip, subnet, gw, dns1, dns2;
      ip.fromString(cfg.ip);
      subnet.fromString(cfg.subnet);
      gw.fromString(cfg.gw);
      dns1.fromString(cfg.dns1);
      dns2.fromString(cfg.dns2);
      WiFi.config(ip, gw, subnet, dns1, dns2);
    } else {
      // Set custom DNS even in DHCP mode
      IPAddress dns1, dns2;
      dns1.fromString(cfg.dns1);
      dns2.fromString(cfg.dns2);
      // After DHCP, we'll set DNS
    }

    WiFi.begin(cfg.ssid, cfg.wifiPass);
    Serial.printf("[WiFi] Connecting to %s", cfg.ssid);

    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
      delay(500);
      Serial.print('.');
      tries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      apMode = false;
      Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
      digitalWrite(LED_PIN, LOW); // LED on = connected

      // Set custom DNS after DHCP if not static
      if (!cfg.staticIp) {
        IPAddress dns1, dns2;
        dns1.fromString(cfg.dns1);
        dns2.fromString(cfg.dns2);
        // dns is already configured by WiFi.config or DHCP
      }

      // mDNS
      if (MDNS.begin(MDNS_HOST)) {
        Serial.printf("[mDNS] http://%s.local\n", MDNS_HOST);
        MDNS.addService("http", "tcp", 80);
      }
    } else {
      // Failed to connect → fall back to AP
      Serial.println(F("\n[WiFi] Connection failed → AP mode"));
      apMode = true;
      WiFi.mode(WIFI_AP);
      WiFi.softAP(AP_SSID, AP_PASS);
      ledTicker.attach(0.3, []() {
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      });
    }
  }
}

// ═══════════════════════════════════════════════════════════
//  MQTT
// ═══════════════════════════════════════════════════════════
void setupMQTT() {
  if (strlen(cfg.broker) == 0) return;
  mqtt.setServer(cfg.broker, cfg.mqttPort);
  mqtt.setCallback(mqttCallback);
  reconnectMQTT();
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  // Handle incoming commands (set outputs, etc.)
  String t = String(topic);
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];

  Serial.printf("[MQTT] << %s : %s\n", topic, msg.c_str());

  // Check if it's a set command for an output pin
  // Expected topic: <prefix>/<label>/set
  for (int i = 0; i < cfg.ioCount; i++) {
    if (!cfg.io[i].enabled) continue;
    if (strstr(cfg.io[i].type, "output") == nullptr) continue;

    String setTopic = String(cfg.prefix) + "/" + String(cfg.io[i].label) + "/set";
    if (t == setTopic) {
      int val = msg.toInt();
      digitalWrite(cfg.io[i].pin, val ? HIGH : LOW);
      Serial.printf("[IO] Set pin %d (%s) = %d\n", cfg.io[i].pin, cfg.io[i].label, val);
      break;
    }
  }
}

void reconnectMQTT() {
  if (strlen(cfg.broker) == 0) return;

  // Build LWT topic
  String lwtFull = String(cfg.prefix) + "/" + String(cfg.lwtTopic);

  Serial.printf("[MQTT] Connecting to %s:%d…\n", cfg.broker, cfg.mqttPort);

  bool ok;
  if (strlen(cfg.mqttUser) > 0) {
    ok = mqtt.connect(cfg.deviceId, cfg.mqttUser, cfg.mqttPass,
                      lwtFull.c_str(), 1, true, "offline");
  } else {
    ok = mqtt.connect(cfg.deviceId,
                      lwtFull.c_str(), 1, true, "offline");
  }

  if (ok) {
    Serial.println(F("[MQTT] Connected!"));
    // Publish online status
    mqtt.publish(lwtFull.c_str(), "online", true);

    // Subscribe to output set topics
    for (int i = 0; i < cfg.ioCount; i++) {
      if (!cfg.io[i].enabled) continue;
      if (strstr(cfg.io[i].type, "output") == nullptr) continue;
      String setTopic = String(cfg.prefix) + "/" + String(cfg.io[i].label) + "/set";
      mqtt.subscribe(setTopic.c_str());
      Serial.printf("[MQTT] Subscribed: %s\n", setTopic.c_str());
    }
  } else {
    Serial.printf("[MQTT] Failed, rc=%d\n", mqtt.state());
  }
}

// ═══════════════════════════════════════════════════════════
//  IO Polling
// ═══════════════════════════════════════════════════════════
void pollIO() {
  for (int i = 0; i < cfg.ioCount; i++) {
    IOPin &p = cfg.io[i];
    if (!p.enabled) continue;

    if (strcmp(p.type, "input_analog") == 0) {
      float raw = (float)analogRead(p.pin);
      p.lastValue = raw * p.multiplier;
    } else if (strcmp(p.type, "input_digital") == 0) {
      pinMode(p.pin, INPUT_PULLUP);
      p.lastValue = (float)digitalRead(p.pin) * p.multiplier;
    } else if (strcmp(p.type, "output_digital") == 0) {
      pinMode(p.pin, OUTPUT);
      p.lastValue = (float)digitalRead(p.pin);
    }
  }
}

void publishIO() {
  for (int i = 0; i < cfg.ioCount; i++) {
    IOPin &p = cfg.io[i];
    if (!p.enabled) continue;
    if (strstr(p.type, "output") != nullptr) continue; // don't publish outputs as sensor data

    String topic = String(cfg.prefix) + "/" + String(p.label);
    String val;

    if (strcmp(p.varType, "float32") == 0) {
      val = String(p.lastValue, 2);
    } else if (strcmp(p.varType, "bool") == 0) {
      val = (p.lastValue > 0.5) ? "1" : "0";
    } else {
      val = String((int)p.lastValue);
    }

    mqtt.publish(topic.c_str(), val.c_str());
  }
}

// ═══════════════════════════════════════════════════════════
//  WebSocket  (live-stream)
// ═══════════════════════════════════════════════════════════
void broadcastWS() {
  if (ws.count() == 0) return;

  JsonDocument doc;
  doc["ts"]      = getTimestamp();
  doc["uptime"]  = millis() / 1000;
  doc["heap"]    = ESP.getFreeHeap();
  doc["mqtt"]    = mqtt.connected();
  doc["wifi_rssi"] = WiFi.RSSI();

  JsonArray pins = doc["pins"].to<JsonArray>();
  for (int i = 0; i < cfg.ioCount; i++) {
    if (!cfg.io[i].enabled) continue;
    JsonObject p = pins.add<JsonObject>();
    p["pin"]   = cfg.io[i].pin;
    p["label"] = cfg.io[i].label;
    p["type"]  = cfg.io[i].type;
    p["value"] = cfg.io[i].lastValue;
  }

  String json;
  serializeJson(doc, json);
  ws.textAll(json);
}

void onWsEvent(AsyncWebSocket *s, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("[WS] Client #%u connected\n", client->id());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("[WS] Client #%u disconnected\n", client->id());
  }
}

// ═══════════════════════════════════════════════════════════
//  HTTP Authentication Helper
// ═══════════════════════════════════════════════════════════
bool checkAuth(AsyncWebServerRequest *req) {
  if (!req->authenticate("admin", cfg.webPass)) {
    req->requestAuthentication();
    return false;
  }
  return true;
}

// ═══════════════════════════════════════════════════════════
//  Timestamp helper
// ═══════════════════════════════════════════════════════════
String getTimestamp() {
  if (apMode) return "N/A";
  return ntp.getFormattedTime();
}

// ═══════════════════════════════════════════════════════════
//  OTA
// ═══════════════════════════════════════════════════════════
void setupOTA() {
  ArduinoOTA.setHostname(MDNS_HOST);
  ArduinoOTA.onStart([]() {
    Serial.println(F("[OTA] Start"));
    LittleFS.end();
  });
  ArduinoOTA.onEnd([]() {
    Serial.println(F("\n[OTA] Done!"));
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]\n", error);
  });
  ArduinoOTA.begin();
}

// ═══════════════════════════════════════════════════════════
//  Hardware Button
// ═══════════════════════════════════════════════════════════
void handleButton() {
  bool state = digitalRead(FAILSAFE_PIN) == LOW; // Active LOW

  if (state && !btnPressed) {
    // Button just pressed
    btnPressed = true;
    btnPressStart = millis();
    factoryTriggered = false;
  }

  if (state && btnPressed) {
    // Button held
    unsigned long held = millis() - btnPressStart;
    if (held >= 10000 && !factoryTriggered) {
      factoryTriggered = true;
      // Rapid blink to indicate factory reset
      for (int i = 0; i < 10; i++) {
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        delay(100);
      }
      factoryReset();
    }
  }

  if (!state && btnPressed) {
    // Button released
    unsigned long held = millis() - btnPressStart;
    btnPressed = false;
    if (held < 2000 && !factoryTriggered) {
      Serial.println(F("[SYS] Short press → Restart"));
      delay(200);
      ESP.restart();
    }
  }
}

// ═══════════════════════════════════════════════════════════
//  WEB SERVER
// ═══════════════════════════════════════════════════════════
void setupWebServer() {
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // ── Serve embedded pages ───────────────────────────────
  // Root → Dashboard
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;
    req->send(LittleFS, "/www/index.html", "text/html");
  });
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/www/style.css", "text/css");
  });
  server.on("/app.js", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/www/app.js", "application/javascript");
  });

  // ── API endpoints ─────────────────────────────────────
  // GET status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;

    JsonDocument doc;
    doc["device_id"]     = cfg.deviceId;
    doc["fw_version"]    = FW_VERSION;
    doc["uptime"]        = millis() / 1000;
    doc["heap"]          = ESP.getFreeHeap();
    doc["ap_mode"]       = apMode;
    doc["wifi_ssid"]     = apMode ? AP_SSID : cfg.ssid;
    doc["wifi_ip"]       = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    doc["wifi_rssi"]     = WiFi.RSSI();
    doc["mqtt_connected"]= mqtt.connected();
    doc["time"]          = getTimestamp();

    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });

  // GET config
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;

    File f = LittleFS.open(CONFIG_PATH, "r");
    if (!f) {
      req->send(404, "application/json", "{\"error\":\"no config\"}");
      return;
    }
    req->send(f, CONFIG_PATH, "application/json");
    f.close();
  });

  // POST config (full save)
  server.on("/api/config", HTTP_POST,
    [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!checkAuth(req)) return;

      // Collect body
      static String body;
      if (index == 0) body = "";
      for (size_t i = 0; i < len; i++) body += (char)data[i];
      if (index + len != total) return; // wait for full body

      // Parse and apply
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, body);
      if (err) {
        req->send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
      }

      // Device
      strlcpy(cfg.deviceId, doc["device_id"] | cfg.deviceId, sizeof(cfg.deviceId));

      // Network
      JsonObject net = doc["network"];
      if (net) {
        strlcpy(cfg.ssid,     net["ssid"]    | cfg.ssid,     sizeof(cfg.ssid));
        strlcpy(cfg.wifiPass, net["pass"]    | cfg.wifiPass, sizeof(cfg.wifiPass));
        cfg.staticIp = net["static_ip"] | cfg.staticIp;
        strlcpy(cfg.ip,       net["ip"]      | cfg.ip,       sizeof(cfg.ip));
        strlcpy(cfg.subnet,   net["subnet"]  | cfg.subnet,   sizeof(cfg.subnet));
        strlcpy(cfg.gw,       net["gw"]      | cfg.gw,       sizeof(cfg.gw));
        strlcpy(cfg.dns1,     net["dns1"]    | cfg.dns1,     sizeof(cfg.dns1));
        strlcpy(cfg.dns2,     net["dns2"]    | cfg.dns2,     sizeof(cfg.dns2));
      }

      // MQTT
      JsonObject mq = doc["mqtt"];
      if (mq) {
        strlcpy(cfg.broker,   mq["broker"]   | cfg.broker,   sizeof(cfg.broker));
        cfg.mqttPort = mq["port"] | cfg.mqttPort;
        strlcpy(cfg.mqttUser, mq["user"]     | cfg.mqttUser, sizeof(cfg.mqttUser));
        strlcpy(cfg.mqttPass, mq["pass"]     | cfg.mqttPass, sizeof(cfg.mqttPass));
        strlcpy(cfg.lwtTopic, mq["lwt_topic"]| cfg.lwtTopic, sizeof(cfg.lwtTopic));
        strlcpy(cfg.prefix,   mq["prefix"]   | cfg.prefix,   sizeof(cfg.prefix));
      }

      // IO
      if (doc.containsKey("io_setup")) {
        JsonArray io = doc["io_setup"];
        cfg.ioCount = 0;
        for (JsonObject p : io) {
          if (cfg.ioCount >= MAX_IO_PINS) break;
          IOPin &pin = cfg.io[cfg.ioCount];
          pin.pin = p["pin"] | 0;
          strlcpy(pin.label, p["label"] | "gpio", sizeof(pin.label));
          pin.enabled = p["enabled"] | false;
          strlcpy(pin.type, p["type"] | "input_digital", sizeof(pin.type));
          strlcpy(pin.varType, p["var_type"] | "int16", sizeof(pin.varType));
          pin.multiplier = p["multiplier"] | 1.0f;
          pin.lastValue = 0;
          cfg.ioCount++;
        }
      }

      // System
      JsonObject sys = doc["system"];
      if (sys) {
        strlcpy(cfg.webPass, sys["web_pass"] | cfg.webPass, sizeof(cfg.webPass));
        cfg.pollInterval = sys["poll_interval"] | cfg.pollInterval;
      }

      saveConfig();
      req->send(200, "application/json", "{\"ok\":true,\"msg\":\"Config saved. Restart to apply network changes.\"}");
    }
  );

  // POST restart
  server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;
    req->send(200, "application/json", "{\"ok\":true}");
    delay(500);
    ESP.restart();
  });

  // POST factory reset
  server.on("/api/factory-reset", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!checkAuth(req)) return;
    req->send(200, "application/json", "{\"ok\":true,\"msg\":\"Factory reset…\"}");
    delay(500);
    factoryReset();
  });

  // POST OTA update (firmware .bin upload)
  server.on("/api/ota", HTTP_POST,
    [](AsyncWebServerRequest *req) {
      bool hasError = Update.hasError();
      req->send(200, "application/json",
        hasError ? "{\"ok\":false,\"msg\":\"Update failed\"}"
                 : "{\"ok\":true,\"msg\":\"Update OK. Restarting…\"}");
      if (!hasError) {
        delay(500);
        ESP.restart();
      }
    },
    [](AsyncWebServerRequest *req, const String& filename, size_t index,
       uint8_t *data, size_t len, bool final) {
      if (!checkAuth(req)) return;
      if (index == 0) {
        Serial.printf("[OTA] Upload: %s\n", filename.c_str());
        Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
      }
      if (len) Update.write(data, len);
      if (final) {
        if (Update.end(true)) {
          Serial.printf("[OTA] Upload complete: %u bytes\n", index + len);
        } else {
          Update.printError(Serial);
        }
      }
    }
  );

  // Serve static files from LittleFS /www/
  server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");

  server.begin();
  Serial.println(F("[HTTP] Web server started on port 80"));
}

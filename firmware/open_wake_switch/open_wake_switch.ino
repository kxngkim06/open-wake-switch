// Open Wake Switch - firmware v1.0
// ESP32 (WROVER) + hobby servo that flips a wall light switch.
//
// Control path: phone web app <-> EMQX Cloud (MQTT/TLS) <-> ESP32 on GTother
// The alarm engine runs locally from NVS, so alarms still fire without internet
// (as long as the clock was synced since the last power-up).
//
// Libraries (Arduino Library Manager):
//   - ESP32Servo
//   - PubSubClient   (Nick O'Leary)
//   - ArduinoJson    (Benoit Blanchon, v7.x)
//
// Files in this sketch folder:
//   open_wake_switch.ino, schedule.h, config_json.h, secrets.h (yours), ca_cert.h
//
// MQTT topics
//   ows/cmd         <- "ON" / "OFF"                 (manual control, not retained)
//   ows/config/set  <- full config JSON              (retained by web app; device clears it after applying)
//   ows/config      -> config actually stored on the device (retained)
//   ows/state       -> status JSON (retained)
//   ows/online      -> "1" / "0" (retained, "0" via Last Will)
//   ows/error       -> last config rejection message (not retained)

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESP32Servo.h>
#include <time.h>

#include <esp_mac.h>

String chipMac() {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

#include "secrets.h"
#include "ca_cert.h"
#include "schedule.h"
#include "config_json.h"

#define FW_VERSION "1.0.0"

// ---------------- Servo (recalibrate after the bracket is mounted) ----------------
const int SERVO_PIN  = 25;
const int SERVO_MIN  = 900;     // FT1117M: 900-2100 us ~ 120 deg
const int SERVO_MAX  = 2100;
const int NEUTRAL_US = 1500;
const int ON_US      = 2100;
const int OFF_US     = 900;
const int HOLD_MS    = 400;

// ---------------- Time ----------------
const char* TZ_INFO = "EST5EDT,M3.2.0,M11.1.0";   // US Eastern with automatic DST
const time_t VALID_EPOCH = 1735689600;            // 2025-01-01: clock considered synced after this

// ---------------- Topics ----------------
const char* T_CMD     = "ows/cmd";
const char* T_CFG_SET = "ows/config/set";
const char* T_CFG     = "ows/config";
const char* T_STATE   = "ows/state";
const char* T_ONLINE  = "ows/online";
const char* T_ERROR   = "ows/error";

WiFiClientSecure tls;
PubSubClient mqtt(tls);
Preferences prefs;
Servo servo;

ows::Config cfg;
time_t lastFired = 0;          // epoch of the last alarm event handled (persisted)
time_t autoOffAt = 0;          // pending automatic OFF (RAM only)
String lastAction = "none";    // ON / OFF
String lastSource = "";        // alarm / manual / auto-off
time_t lastActionAt = 0;
String clientId;

unsigned long lastWifiTry = 0, lastMqttTry = 0, lastStatePub = 0, lastTick = 0;
bool statePending = true;

// =====================================================================
// helpers
// =====================================================================
bool timeValid() { return time(nullptr) > VALID_EPOCH; }

String fmtTime(time_t t) {
  if (t == 0) return "";
  char b[17]; ows::fmtLocalDateTime(t, b); return String(b);
}

void actuate(int pushUs) {
  servo.attach(SERVO_PIN, SERVO_MIN, SERVO_MAX);
  servo.writeMicroseconds(NEUTRAL_US); delay(250);
  servo.writeMicroseconds(pushUs);     delay(HOLD_MS);
  servo.writeMicroseconds(NEUTRAL_US); delay(400);
  servo.detach();                      // no holding current, no jitter, manual use stays free
}

void doAction(bool on, const char* source) {
  Serial.printf("[action] %s (%s)\n", on ? "ON" : "OFF", source);
  actuate(on ? ON_US : OFF_US);
  lastAction = on ? "ON" : "OFF";
  lastSource = source;
  lastActionAt = time(nullptr);
  if (!on) autoOffAt = 0;  // any OFF cancels a pending auto-off
  statePending = true;
}

// =====================================================================
// persistence
// =====================================================================
void saveConfig() {
  static char buf[1024];
  size_t n = ows::configToJson(cfg, buf, sizeof(buf));
  if (n) prefs.putString("cfg", buf);
}

void saveLastFired() { prefs.putLong64("lastFired", (int64_t)lastFired); }

void loadPersisted() {
  String s = prefs.getString("cfg", "");
  const char* err;
  if (s.length() && ows::configFromJson(s.c_str(), s.length(), cfg, err)) {
    Serial.println("[nvs] config loaded");
  } else {
    Serial.println("[nvs] no valid config, using defaults (all days off)");
  }
  lastFired = (time_t)prefs.getLong64("lastFired", 0);
}

// =====================================================================
// MQTT publishing
// =====================================================================
void publishConfig() {
  if (!mqtt.connected()) return;
  static char buf[1024];
  size_t n = ows::configToJson(cfg, buf, sizeof(buf));
  if (n) mqtt.publish(T_CFG, (const uint8_t*)buf, n, true);
}

void publishState() {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  time_t now = time(nullptr);
  doc["fw"] = FW_VERSION;
  doc["timeOk"] = timeValid();
  if (timeValid()) doc["now"] = fmtTime(now);
  doc["last"] = lastAction;
  doc["lastSrc"] = lastSource;
  if (lastActionAt) doc["lastAt"] = fmtTime(lastActionAt);
  doc["rssi"] = WiFi.RSSI();

  if (timeValid()) {
    ows::Event ev;
    if (ows::findNext(cfg, now, ev)) {
      JsonObject n = doc["next"].to<JsonObject>();
      n["at"] = fmtTime(ev.at);
      n["kind"] = ev.kind == ows::EV_WEEKLY ? "weekly" : ev.kind == ows::EV_OVERRIDE ? "override" : "oneshot";
    } else doc["next"] = nullptr;

    int y, m, d, mm;
    if (ows::findNextWeekly(cfg, now, y, m, d, mm)) {
      JsonObject w = doc["nextWeekly"].to<JsonObject>();
      char date[11], hm[6];
      ows::fmtDate(y, m, d, date); ows::fmtHM(mm, hm);
      w["date"] = date; w["t"] = hm;
    } else doc["nextWeekly"] = nullptr;
  }
  if (autoOffAt) doc["autoOffAt"] = fmtTime(autoOffAt);

  static char buf[600];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(T_STATE, (const uint8_t*)buf, n, true);
  lastStatePub = millis();
  statePending = false;
}

// =====================================================================
// MQTT receive
// =====================================================================
void onMessage(char* topic, byte* payload, unsigned int len) {
  if (strcmp(topic, T_CFG_SET) == 0) {
    if (len == 0) return;                       // our own "clear retained" echo
    ows::Config tmp;
    const char* err = nullptr;
    if (ows::configFromJson((const char*)payload, len, tmp, err)) {
      cfg = tmp;
      // Anything scheduled at or before "now" counts as handled, so editing a
      // time to a few minutes ago does not fire the light immediately.
      if (timeValid()) {
        time_t now = time(nullptr);
        if (now > lastFired) { lastFired = now; saveLastFired(); }
        ows::cleanup(cfg, now);
      }
      saveConfig();
      Serial.println("[cfg] applied");
    } else {
      Serial.printf("[cfg] rejected: %s\n", err);
      mqtt.publish(T_ERROR, err);
    }
    mqtt.publish(T_CFG_SET, (const uint8_t*)"", 0, true);   // clear retained request
    publishConfig();
    statePending = true;
    return;
  }

  if (strcmp(topic, T_CMD) == 0) {
    String msg;
    for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
    msg.trim(); msg.toUpperCase();
    if (msg == "ON")       doAction(true, "manual");
    else if (msg == "OFF") doAction(false, "manual");
    else Serial.printf("[cmd] unknown: %s\n", msg.c_str());
  }
}

// =====================================================================
// network (non-blocking style: alarms keep running while offline)
// =====================================================================
void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiTry < 30000 && lastWifiTry != 0) return;
  lastWifiTry = millis();
  Serial.println("[wifi] (re)connecting...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void maintainMQTT() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (millis() - lastMqttTry < 10000 && lastMqttTry != 0) return;
  lastMqttTry = millis();

  Serial.print("[mqtt] connecting... ");
  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS, T_ONLINE, 1, true, "0")) {
    Serial.println("OK");
    mqtt.publish(T_ONLINE, "1", true);
    mqtt.subscribe(T_CMD, 1);
    mqtt.subscribe(T_CFG_SET, 1);
    publishConfig();
    statePending = true;
  } else {
    Serial.printf("failed rc=%d\n", mqtt.state());
  }
}

// =====================================================================
// alarm engine tick (once per second)
// =====================================================================
void alarmTick() {
  if (!timeValid()) return;                 // never act on an unknown clock
  time_t now = time(nullptr);

  ows::Event due;
  if (ows::findDue(cfg, now, lastFired, due)) {
    lastFired = due.at;
    saveLastFired();
    Serial.printf("[alarm] firing (%s)\n", due.kind == ows::EV_ONESHOT ? "one-shot" :
                                           due.kind == ows::EV_OVERRIDE ? "override" : "weekly");
    doAction(true, "alarm");
    if (cfg.autoOffMin > 0) autoOffAt = now + (time_t)cfg.autoOffMin * 60;
  }

  if (autoOffAt && now >= autoOffAt) {
    doAction(false, "auto-off");
  }

  static int lastMinute = -1;
  struct tm lt; localtime_r(&now, &lt);
  if (lt.tm_min != lastMinute) {            // once a minute: housekeeping
    lastMinute = lt.tm_min;
    if (ows::cleanup(cfg, now)) { saveConfig(); publishConfig(); }
    statePending = true;                    // refresh "now"/"next" on the web app
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\nOpen Wake Switch v%s\n", FW_VERSION);

  setenv("TZ", TZ_INFO, 1); tzset();        // needed before parsing stored local times
  prefs.begin("ows", false);
  loadPersisted();

  ESP32PWM::allocateTimer(0);
  servo.setPeriodHertz(50);

  WiFi.mode(WIFI_STA);                       // client only: never AP mode on campus
  WiFi.setAutoReconnect(true);
  Serial.printf("[wifi] MAC %s\n", chipMac().c_str());
  maintainWiFi();

  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov", "time.google.com");

  if (strstr(CA_CERT, "BEGIN CERTIFICATE")) tls.setCACert(CA_CERT);
  else { Serial.println("[tls] WARNING: no CA cert in ca_cert.h -> certificate NOT verified"); tls.setInsecure(); }
  tls.setHandshakeTimeout(8);

  clientId = "ows-" + chipMac();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(2048);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);
}

void loop() {
  maintainWiFi();
  maintainMQTT();
  mqtt.loop();

  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    static bool announcedTime = false;
    if (!announcedTime && timeValid()) {
      announcedTime = true;
      Serial.printf("[time] synced: %s\n", fmtTime(time(nullptr)).c_str());
      statePending = true;
    }
    alarmTick();
  }

  if (statePending || millis() - lastStatePub > 60000) publishState();
}

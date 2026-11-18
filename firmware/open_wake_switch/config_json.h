// config_json.h - Config <-> JSON (ArduinoJson v7). Portable: also compiles on a PC for tests.
//
// Wire format (topic ows/config/set and ows/config):
// {
//   "enabled": true,
//   "days": [ {"on":false,"t":"10:00"}, {"on":true,"t":"07:30"}, ... ],   // 7 items, [0] = Sunday
//   "autoOffMin": 0,
//   "graceMin": 10,
//   "override": null | {"date":"2026-11-19","t":"06:45"} | {"date":"2026-11-19","skip":true},
//   "oneShots": ["2026-11-18T15:30", ...]                                   // max 5
// }
#pragma once
#include <ArduinoJson.h>
#include "schedule.h"

namespace ows {

// Parse + validate. On any error returns false and leaves `out` untouched.
inline bool configFromJson(const char* json, size_t len, Config& out, const char*& err) {
  JsonDocument doc;
  DeserializationError de = deserializeJson(doc, json, len);
  if (de) { err = "invalid JSON"; return false; }

  Config c;
  c.enabled = doc["enabled"] | true;

  JsonArrayConst days = doc["days"].as<JsonArrayConst>();
  if (days.isNull() || days.size() != 7) { err = "days must have 7 items"; return false; }
  for (int i = 0; i < 7; i++) {
    c.days[i].on = days[i]["on"] | false;
    int mm = parseHM(days[i]["t"] | "");
    if (mm < 0) { err = "bad day time"; return false; }
    c.days[i].min = mm;
  }

  int ao = doc["autoOffMin"] | 0;
  int gr = doc["graceMin"] | 10;
  if (ao < 0 || ao > 240) { err = "autoOffMin must be 0..240"; return false; }
  if (gr < 0 || gr > 60)  { err = "graceMin must be 0..60"; return false; }
  c.autoOffMin = ao; c.graceMin = gr;

  JsonVariantConst ov = doc["override"];
  if (!ov.isNull()) {
    int y, m, d;
    if (!parseDate(ov["date"] | "", y, m, d)) { err = "bad override date"; return false; }
    c.hasOverride = true; c.ovY = y; c.ovM = m; c.ovD = d;
    c.ovSkip = ov["skip"] | false;
    if (!c.ovSkip) {
      int mm = parseHM(ov["t"] | "");
      if (mm < 0) { err = "bad override time"; return false; }
      c.ovMin = mm;
    }
  }

  JsonArrayConst os = doc["oneShots"].as<JsonArrayConst>();
  if (!os.isNull()) {
    if (os.size() > (size_t)MAX_ONESHOTS) { err = "too many one-shot alarms (max 5)"; return false; }
    for (JsonVariantConst v : os) {
      time_t t = parseLocalDateTime(v | "");
      if (t == 0) { err = "bad one-shot time"; return false; }
      c.oneShot[c.nOneShot++] = t;
    }
  }

  out = c;
  err = nullptr;
  return true;
}

// Serialize to buf. Returns bytes written (0 on overflow).
inline size_t configToJson(const Config& c, char* buf, size_t bufSize) {
  JsonDocument doc;
  doc["enabled"] = c.enabled;
  JsonArray days = doc["days"].to<JsonArray>();
  char hm[6];
  for (int i = 0; i < 7; i++) {
    JsonObject o = days.add<JsonObject>();
    o["on"] = c.days[i].on;
    fmtHM(c.days[i].min, hm);
    o["t"] = hm;  // copied (char array)
  }
  doc["autoOffMin"] = c.autoOffMin;
  doc["graceMin"] = c.graceMin;

  if (c.hasOverride) {
    JsonObject ov = doc["override"].to<JsonObject>();
    char date[11]; fmtDate(c.ovY, c.ovM, c.ovD, date);
    ov["date"] = date;
    if (c.ovSkip) ov["skip"] = true;
    else { fmtHM(c.ovMin, hm); ov["t"] = hm; }
  } else {
    doc["override"] = nullptr;
  }

  JsonArray os = doc["oneShots"].to<JsonArray>();
  char dt[17];
  for (int i = 0; i < c.nOneShot; i++) { fmtLocalDateTime(c.oneShot[i], dt); os.add(dt); }

  size_t need = measureJson(doc);
  if (need + 1 > bufSize) return 0;
  return serializeJson(doc, buf, bufSize);
}

}  // namespace ows

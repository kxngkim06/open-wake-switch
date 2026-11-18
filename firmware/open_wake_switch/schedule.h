// schedule.h - Open Wake Switch alarm engine (pure C++, no Arduino deps -> unit-testable on a PC)
//
// Concepts
//   weekly   : one ON time per weekday (index 0 = Sunday ... 6 = Saturday)
//   override : "this time only" change for ONE date: a different time, or skip
//   oneShot  : temporary alarm at an exact local date/time (e.g. after a nap)
//
// All times are LOCAL time (TZ must be set with setenv/tzset or configTzTime first).
#pragma once
#include <time.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <initializer_list>

namespace ows {

const int MAX_ONESHOTS = 5;

struct DayCfg {
  bool on;
  int16_t min;      // minutes since midnight, 0..1439
};

struct Config {
  bool    enabled    = true;   // master switch for weekly alarms (+ override)
  DayCfg  days[7];             // 0 = Sunday
  int16_t autoOffMin = 0;      // 0 = disabled
  int16_t graceMin   = 10;     // how late a missed alarm may still fire

  bool    hasOverride = false;
  int     ovY = 0, ovM = 0, ovD = 0;
  bool    ovSkip = false;
  int16_t ovMin = 0;

  int     nOneShot = 0;
  time_t  oneShot[MAX_ONESHOTS];

  Config() {
    for (int i = 0; i < 7; i++) { days[i].on = false; days[i].min = 7 * 60 + 30; }
  }
};

enum EventKind : uint8_t { EV_WEEKLY = 0, EV_OVERRIDE = 1, EV_ONESHOT = 2 };

struct Event {
  time_t at;
  EventKind kind;
};

// ---------- small helpers ----------

// "07:30" -> 450, invalid -> -1
inline int parseHM(const char* s) {
  if (!s || strlen(s) != 5 || s[2] != ':') return -1;
  for (int i : {0, 1, 3, 4}) if (s[i] < '0' || s[i] > '9') return -1;
  int h = (s[0] - '0') * 10 + (s[1] - '0');
  int m = (s[3] - '0') * 10 + (s[4] - '0');
  if (h > 23 || m > 59) return -1;
  return h * 60 + m;
}

inline void fmtHM(int min, char* out /*>=6*/) {
  snprintf(out, 6, "%02d:%02d", (min / 60) % 24, min % 60);
}

// "2026-11-19" -> y,m,d ; returns false if malformed
inline bool parseDate(const char* s, int& y, int& m, int& d) {
  if (!s || strlen(s) < 10 || s[4] != '-' || s[7] != '-') return false;
  if (sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != 3) return false;
  return y >= 2024 && y <= 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

// local date + minutes-of-day -> epoch (DST handled by mktime)
inline time_t localEpoch(int y, int m, int d, int minOfDay) {
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d;
  t.tm_hour = minOfDay / 60; t.tm_min = minOfDay % 60; t.tm_sec = 0;
  t.tm_isdst = -1;
  return mktime(&t);
}

// "2026-11-18T15:30" -> epoch, invalid -> 0
inline time_t parseLocalDateTime(const char* s) {
  int y, m, d;
  if (!parseDate(s, y, m, d) || strlen(s) != 16 || s[10] != 'T') return 0;
  int mm = parseHM(s + 11);
  if (mm < 0) return 0;
  return localEpoch(y, m, d, mm);
}

inline void fmtLocalDateTime(time_t t, char* out /*>=17*/) {
  struct tm lt; localtime_r(&t, &lt);
  snprintf(out, 17, "%04d-%02d-%02dT%02d:%02d",
           lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
}

inline void fmtDate(int y, int m, int d, char* out /*>=11*/) {
  snprintf(out, 11, "%04d-%02d-%02d", y, m, d);
}

// Local calendar date of (today + dayOffset). Uses noon to stay clear of DST edges.
inline void dateWithOffset(time_t now, int dayOffset, int& y, int& m, int& d, int& wday) {
  struct tm lt; localtime_r(&now, &lt);
  lt.tm_mday += dayOffset;
  lt.tm_hour = 12; lt.tm_min = 0; lt.tm_sec = 0; lt.tm_isdst = -1;
  mktime(&lt);  // normalizes
  y = lt.tm_year + 1900; m = lt.tm_mon + 1; d = lt.tm_mday; wday = lt.tm_wday;
}

// ---------- event generation ----------

// Weekly/override event for one calendar date (at most one).
// ignoreOverride=true gives the plain weekly time (used to show "next weekly alarm").
inline bool dayEvent(const Config& c, int y, int m, int d, int wday, bool ignoreOverride, Event& ev) {
  if (!c.enabled) return false;
  bool isOvDay = c.hasOverride && c.ovY == y && c.ovM == m && c.ovD == d;
  if (isOvDay && !ignoreOverride) {
    if (c.ovSkip) return false;
    ev.at = localEpoch(y, m, d, c.ovMin); ev.kind = EV_OVERRIDE;
    return true;
  }
  if (!c.days[wday].on) return false;
  ev.at = localEpoch(y, m, d, c.days[wday].min); ev.kind = EV_WEEKLY;
  return true;
}

// Alarm that should fire NOW, if any.
// Rules: event time <= now, not older than graceMin, and newer than lastFired.
// If several qualify, the latest one is returned (one ON push covers all of them).
inline bool findDue(const Config& c, time_t now, time_t lastFired, Event& due) {
  const time_t oldest = now - (time_t)c.graceMin * 60;
  bool found = false;
  auto consider = [&](const Event& e) {
    if (e.at <= now && e.at >= oldest && e.at > lastFired) {
      if (!found || e.at > due.at) { due = e; found = true; }
    }
  };
  for (int off = -1; off <= 0; off++) {        // yesterday too (grace across midnight)
    int y, m, d, w; dateWithOffset(now, off, y, m, d, w);
    Event e;
    if (dayEvent(c, y, m, d, w, false, e)) consider(e);
  }
  for (int i = 0; i < c.nOneShot; i++) consider(Event{c.oneShot[i], EV_ONESHOT});
  return found;
}

// Next upcoming alarm (any kind), searching 8 days ahead.
inline bool findNext(const Config& c, time_t now, Event& next) {
  bool found = false;
  auto consider = [&](const Event& e) {
    if (e.at > now && (!found || e.at < next.at)) { next = e; found = true; }
  };
  for (int off = 0; off <= 7; off++) {
    int y, m, d, w; dateWithOffset(now, off, y, m, d, w);
    Event e;
    if (dayEvent(c, y, m, d, w, false, e)) consider(e);
  }
  for (int i = 0; i < c.nOneShot; i++) consider(Event{c.oneShot[i], EV_ONESHOT});
  return found;
}

// Next plain weekly alarm (override ignored) -> the date the "this time only" UI targets.
inline bool findNextWeekly(const Config& c, time_t now, int& y, int& m, int& d, int& minOfDay) {
  for (int off = 0; off <= 7; off++) {
    int yy, mm, dd, w; dateWithOffset(now, off, yy, mm, dd, w);
    Event e;
    if (dayEvent(c, yy, mm, dd, w, true, e) && e.at > now) {
      y = yy; m = mm; d = dd; minOfDay = c.days[w].min;
      return true;
    }
  }
  return false;
}

// Drop expired one-shots and past overrides. Returns true if config changed.
inline bool cleanup(Config& c, time_t now) {
  bool changed = false;
  const time_t expiry = now - (time_t)c.graceMin * 60 - 60;
  int k = 0;
  for (int i = 0; i < c.nOneShot; i++) {
    if (c.oneShot[i] >= expiry) c.oneShot[k++] = c.oneShot[i];
    else changed = true;
  }
  c.nOneShot = k;

  if (c.hasOverride) {
    // Override day is over once the end of that day + grace has passed
    time_t endOfOvDay = localEpoch(c.ovY, c.ovM, c.ovD, 23 * 60 + 59) + 60 + (time_t)c.graceMin * 60;
    if (now > endOfOvDay) { c.hasOverride = false; changed = true; }
  }
  return changed;
}

}  // namespace ows

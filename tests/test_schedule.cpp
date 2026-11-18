#include <cstdlib>
#include <cassert>
#include <iostream>
#include <string>
#include "../firmware/open_wake_switch/config_json.h"
using namespace ows;

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::cout << "FAIL: " << msg << "\n"; fails++; } else std::cout << "ok  : " << msg << "\n"; } while (0)

time_t T(const char* s) { return parseLocalDateTime(s); }
std::string F(time_t t) { char b[17]; fmtLocalDateTime(t, b); return b; }

Config base() {
  const char* j = R"({"enabled":true,"days":[
    {"on":false,"t":"10:00"},{"on":true,"t":"07:30"},{"on":true,"t":"08:15"},
    {"on":true,"t":"07:30"},{"on":true,"t":"08:15"},{"on":true,"t":"09:00"},{"on":false,"t":"10:00"}],
    "autoOffMin":0,"graceMin":10,"override":null,"oneShots":[]})";
  Config c; const char* err;
  bool ok = configFromJson(j, strlen(j), c, err);
  if (!ok) { std::cout << "base parse failed: " << err << "\n"; exit(1); }
  return c;
}

int main() {
  setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1); tzset();
  Event ev;

  // 2026-11-18 is a Wednesday (wday 3) -> 07:30
  Config c = base();
  CHECK(findNext(c, T("2026-11-18T06:00"), ev) && F(ev.at) == "2026-11-18T07:30" && ev.kind == EV_WEEKLY, "next weekly Wed 07:30");
  CHECK(!findDue(c, T("2026-11-18T07:29"), 0, ev), "not due 1 min early");
  CHECK(findDue(c, T("2026-11-18T07:30"), 0, ev) && F(ev.at) == "2026-11-18T07:30", "due exactly at 07:30");
  CHECK(!findDue(c, T("2026-11-18T07:31"), T("2026-11-18T07:30"), ev), "no double fire after lastFired");
  CHECK(findDue(c, T("2026-11-18T07:39"), 0, ev), "missed by 9 min -> still fires (grace 10)");
  CHECK(!findDue(c, T("2026-11-18T07:41"), 0, ev), "missed by 11 min -> skipped");

  // Saturday off, Sunday off -> next after Fri 09:00 is Mon 07:30
  CHECK(findNext(c, T("2026-11-20T09:01"), ev) && F(ev.at) == "2026-11-23T07:30", "weekend skipped -> Mon 07:30");

  // override: this Thursday (11-19) at 06:45 instead of 08:15
  { Config o = c; o.hasOverride = true; o.ovY = 2026; o.ovM = 11; o.ovD = 19; o.ovMin = 6*60+45;
    CHECK(findNext(o, T("2026-11-18T20:00"), ev) && F(ev.at) == "2026-11-19T06:45" && ev.kind == EV_OVERRIDE, "override 06:45 replaces Thu 08:15");
    CHECK(findDue(o, T("2026-11-19T06:45"), 0, ev), "override fires");
    CHECK(!findDue(o, T("2026-11-19T08:15"), T("2026-11-19T06:45"), ev), "weekly 08:15 does NOT also fire that day");
    int y,m,d,mm; CHECK(findNextWeekly(o, T("2026-11-18T20:00"), y,m,d,mm) && d == 19 && mm == 8*60+15, "nextWeekly ignores override (shows default)");
    CHECK(!cleanup(o, T("2026-11-19T12:00")), "override kept during its day");
    CHECK(cleanup(o, T("2026-11-20T00:20")) && !o.hasOverride, "override removed after its day + grace");
  }
  // override skip
  { Config o = c; o.hasOverride = true; o.ovY = 2026; o.ovM = 11; o.ovD = 19; o.ovSkip = true;
    CHECK(findNext(o, T("2026-11-18T20:00"), ev) && F(ev.at) == "2026-11-20T09:00", "skip Thu -> next is Fri 09:00");
    CHECK(!findDue(o, T("2026-11-19T08:15"), 0, ev), "skipped day does not fire");
  }
  // one-shot nap alarm
  { Config o = c; o.oneShot[o.nOneShot++] = T("2026-11-18T15:30");
    CHECK(findNext(o, T("2026-11-18T14:50"), ev) && F(ev.at) == "2026-11-18T15:30" && ev.kind == EV_ONESHOT, "one-shot is next");
    CHECK(findDue(o, T("2026-11-18T15:30"), T("2026-11-18T07:30"), ev) && ev.kind == EV_ONESHOT, "one-shot fires");
    CHECK(!cleanup(o, T("2026-11-18T15:35")), "one-shot kept within grace");
    CHECK(cleanup(o, T("2026-11-18T15:45")) && o.nOneShot == 0, "one-shot removed after expiry");
  }
  // master off disables weekly but not one-shots
  { Config o = c; o.enabled = false; o.oneShot[o.nOneShot++] = T("2026-11-18T15:30");
    CHECK(findNext(o, T("2026-11-18T06:00"), ev) && F(ev.at) == "2026-11-18T15:30", "master off: only one-shot remains");
  }
  // grace across midnight: alarm 23:58, device back at 00:05
  { Config o = c; o.days[3].min = 23*60+58;  // Wed 23:58
    CHECK(findDue(o, T("2026-11-19T00:05"), 0, ev) && F(ev.at) == "2026-11-18T23:58", "grace works across midnight");
  }
  // DST: 2027-03-14 (Sun) spring forward, 2026-11-01 fall back
  { Config o = c; o.days[0].on = true; o.days[0].min = 7*60;
    CHECK(findNext(o, T("2027-03-13T23:00"), ev) && F(ev.at) == "2027-03-14T07:00", "DST spring-forward day keeps 07:00 local");
    CHECK(findNext(o, T("2026-10-31T23:00"), ev) && F(ev.at) == "2026-11-01T07:00", "DST fall-back day keeps 07:00 local");
  }
  // JSON round trip + validation
  { Config o = c; o.hasOverride = true; o.ovY = 2026; o.ovM = 11; o.ovD = 19; o.ovMin = 405;
    o.oneShot[o.nOneShot++] = T("2026-11-18T15:30"); o.autoOffMin = 30;
    char buf[1024]; size_t n = configToJson(o, buf, sizeof buf);
    std::cout << "      json: " << buf << "\n";
    Config r; const char* err;
    CHECK(n > 0 && configFromJson(buf, n, r, err) && r.hasOverride && r.ovMin == 405 && r.nOneShot == 1 && r.autoOffMin == 30 && r.days[2].min == 495, "JSON round trip");
    const char* bad1 = R"({"days":[]})";
    CHECK(!configFromJson(bad1, strlen(bad1), r, err), std::string("reject: ") + (err?err:""));
    const char* bad2 = R"({"days":[{"on":true,"t":"25:00"},{"t":"07:00"},{"t":"07:00"},{"t":"07:00"},{"t":"07:00"},{"t":"07:00"},{"t":"07:00"}]})";
    CHECK(!configFromJson(bad2, strlen(bad2), r, err), std::string("reject: ") + (err?err:""));
    const char* bad3 = "not json";
    CHECK(!configFromJson(bad3, strlen(bad3), r, err), std::string("reject: ") + (err?err:""));
  }
  std::cout << (fails ? "\nSOME TESTS FAILED\n" : "\nALL TESTS PASSED\n");
  return fails;
}

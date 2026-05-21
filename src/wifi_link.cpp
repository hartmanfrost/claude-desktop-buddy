#include "wifi_link.h"
#include "wifi_creds.h"
#include <WiFi.h>
#include <ArduinoJson.h>

static const uint8_t  MAX_CAND          = 8;
static const uint8_t  MAX_FAILS_PER_NET = 3;
static const uint32_t CONNECT_TIMEOUT_MS = 15000;
static const uint32_t BACKOFF_MS         = 5UL * 60 * 1000;
static const uint32_t SCAN_TIMEOUT_MS    = 15000;

struct Candidate {
  char     ssid[33];
  char     psk[64];
  int32_t  rssi;
  uint8_t  fails;
};

static WLinkState _state = WLINK_OFF;
static uint32_t   _stateEnteredMs = 0;
static Candidate  _cands[MAX_CAND];
static uint8_t    _candN   = 0;
static uint8_t    _candIdx = 0;
static char       _curSsid[33] = "";
static int32_t    _curRssi = 0;
static uint32_t   _curIp   = 0;

static void _enter(WLinkState s) {
  _state = s;
  _stateEnteredMs = millis();
}

static void _buildCandidates() {
  _candN = 0;
  int n = WiFi.scanComplete();
  if (n < 0) n = 0;

  JsonDocument doc;
  wifiCredsLoad(doc);
  JsonArrayConst nets = doc["nets"].as<JsonArrayConst>();

  for (int i = 0; i < n && _candN < MAX_CAND; i++) {
    String visible = WiFi.SSID(i);
    int32_t rssi  = WiFi.RSSI(i);
    for (JsonVariantConst v : nets) {
      const char* s = v["ssid"] | "";
      if (!*s) continue;
      if (visible == s) {
        // Dedup: skip if we've already picked this SSID (multi-AP same network)
        bool dup = false;
        for (uint8_t k = 0; k < _candN; k++) {
          if (strcmp(_cands[k].ssid, s) == 0) {
            if (rssi > _cands[k].rssi) _cands[k].rssi = rssi;   // keep best
            dup = true;
            break;
          }
        }
        if (dup) break;
        Candidate& c = _cands[_candN];
        strncpy(c.ssid, s, sizeof(c.ssid)-1); c.ssid[sizeof(c.ssid)-1]=0;
        const char* p = v["psk"] | "";
        strncpy(c.psk, p, sizeof(c.psk)-1); c.psk[sizeof(c.psk)-1]=0;
        c.rssi = rssi;
        c.fails = 0;
        _candN++;
        break;
      }
    }
  }
  WiFi.scanDelete();

  // Sort by RSSI desc — n ≤ 8, insertion sort is fine.
  for (uint8_t i = 1; i < _candN; i++) {
    Candidate tmp = _cands[i];
    int j = (int)i - 1;
    while (j >= 0 && _cands[j].rssi < tmp.rssi) {
      _cands[j+1] = _cands[j];
      j--;
    }
    _cands[j+1] = tmp;
  }
  _candIdx = 0;
}

static void _tryNext() {
  if (_candIdx >= _candN) {
    Serial.println("wifi: candidates exhausted, backoff");
    _curSsid[0] = 0;
    _enter(WLINK_BACKOFF);
    return;
  }
  Candidate& c = _cands[_candIdx];
  strncpy(_curSsid, c.ssid, sizeof(_curSsid)-1); _curSsid[sizeof(_curSsid)-1]=0;
  Serial.printf("wifi: connecting to '%s' (rssi=%d, fails=%u/%u)\n",
                _curSsid, (int)c.rssi, c.fails, MAX_FAILS_PER_NET);
  WiFi.disconnect(true, true);   // clear any half-state
  WiFi.begin(_curSsid, c.psk);
  _enter(WLINK_CONNECTING);
}

void wifiLinkInit() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);   // we manage retries explicitly
  WiFi.persistent(false);         // don't burn flash on every begin()
  Serial.println("wifi: init");
  wifiLinkCredsChanged();
}

void wifiLinkCredsChanged() {
  if (wifiCredsCount() == 0) {
    WiFi.disconnect(true, true);
    _curSsid[0] = 0;
    _curRssi = 0; _curIp = 0;
    Serial.println("wifi: no creds, off");
    _enter(WLINK_OFF);
    return;
  }
  WiFi.scanDelete();
  WiFi.scanNetworks(true);        // async
  Serial.println("wifi: scan started");
  _enter(WLINK_SCANNING);
}

void wifiLinkTick() {
  uint32_t now = millis();

  switch (_state) {
    case WLINK_OFF:
      return;

    case WLINK_SCANNING: {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) {
        if (now - _stateEnteredMs > SCAN_TIMEOUT_MS) {
          Serial.println("wifi: scan timeout, restarting");
          WiFi.scanDelete();
          WiFi.scanNetworks(true);
          _stateEnteredMs = now;
        }
        return;
      }
      if (n == WIFI_SCAN_FAILED) {
        Serial.println("wifi: scan failed, backoff");
        _enter(WLINK_BACKOFF);
        return;
      }
      _buildCandidates();
      if (_candN == 0) {
        Serial.println("wifi: no saved networks visible");
        _enter(WLINK_BACKOFF);
        return;
      }
      Serial.printf("wifi: %u candidate(s) ranked\n", _candN);
      _tryNext();
      return;
    }

    case WLINK_CONNECTING: {
      wl_status_t s = WiFi.status();
      if (s == WL_CONNECTED) {
        _curRssi = WiFi.RSSI();
        _curIp   = (uint32_t)WiFi.localIP();
        Serial.printf("wifi: connected to '%s', rssi=%d, ip=%s\n",
                      _curSsid, (int)_curRssi,
                      WiFi.localIP().toString().c_str());
        _enter(WLINK_CONNECTED);
        return;
      }
      bool failed = (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL);
      bool timedOut = (now - _stateEnteredMs > CONNECT_TIMEOUT_MS);
      if (failed || timedOut) {
        Candidate& c = _cands[_candIdx];
        c.fails++;
        Serial.printf("wifi: '%s' failed (%s, %u/%u)\n",
                      _curSsid, failed ? "auth/ssid" : "timeout",
                      c.fails, MAX_FAILS_PER_NET);
        if (c.fails >= MAX_FAILS_PER_NET) {
          _candIdx++;   // move on
        }
        _tryNext();
      }
      return;
    }

    case WLINK_CONNECTED: {
      if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("wifi: lost '%s', rescanning\n", _curSsid);
        _curRssi = 0; _curIp = 0;
        wifiLinkCredsChanged();
        return;
      }
      // Refresh RSSI every 5s so the status ack stays current.
      if (now - _stateEnteredMs >= 5000) {
        _curRssi = WiFi.RSSI();
        _stateEnteredMs = now;
      }
      return;
    }

    case WLINK_BACKOFF:
      if (now - _stateEnteredMs >= BACKOFF_MS) {
        Serial.println("wifi: backoff over");
        wifiLinkCredsChanged();
      }
      return;
  }
}

WLinkState wifiLinkState() { return _state; }

const char* wifiLinkStateName() {
  switch (_state) {
    case WLINK_OFF:        return "off";
    case WLINK_SCANNING:   return "scanning";
    case WLINK_CONNECTING: return "connecting";
    case WLINK_CONNECTED:  return "connected";
    case WLINK_BACKOFF:    return "backoff";
  }
  return "?";
}

const char* wifiLinkSsid() { return _curSsid; }
int32_t     wifiLinkRssi() { return _curRssi; }
uint32_t    wifiLinkIp()   { return _curIp; }

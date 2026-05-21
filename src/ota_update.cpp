#include "ota_update.h"
#include "wifi_link.h"
#include <Arduino.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <M5StickCPlus.h>   // for spr extern via LovyanGFX

// Build-time configuration. Default fallbacks so the TU compiles even
// if platformio.ini didn't set them — production must set all four.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0"
#endif
#ifndef OTA_OWNER
#define OTA_OWNER "owner"
#endif
#ifndef OTA_REPO
#define OTA_REPO "repo"
#endif
#ifndef OTA_ASSET
#define OTA_ASSET "firmware.bin"
#endif

// TLS trust:
// pioarduino's prebuilt arduino-esp32 doesn't link the Mozilla cert bundle
// (`_binary_x509_crt_bundle_bin_*` symbols missing at link time). Until we
// either embed our own bundle via `board_build.embed_files` or rebuild the
// framework with CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y, fall back to
// setInsecure() — TLS still encrypts the channel, but a MITM on the route
// to GitHub could substitute firmware. Acceptable for a hobby/dev OTA on a
// trusted home network; FIXME before any deployment beyond that.
static void _configureTls(NetworkClientSecure& c) {
  c.setInsecure();
}

// LovyanGFX sprite — freed before OTA download to reclaim ~110KB heap.
extern TFT_eSprite spr;
// W/H are file-static const in main.cpp (internal linkage), so hardcode
// the same dimensions here for the post-failure recreate. Hardware-fixed
// for the Waveshare ESP32-C6-LCD-1.47 panel.
static const int OTA_SPR_W = 172;
static const int OTA_SPR_H = 320;

static const uint32_t BOOT_CHECK_DELAY_MS = 30000;        // settle after boot
static const uint32_t POLL_INTERVAL_MS    = 3600000UL;    // 1h between checks
static const uint32_t HTTP_TIMEOUT_MS     = 15000;
static const uint32_t DOWNLOAD_TIMEOUT_MS = 120000;

static OtaState  _state           = OTA_IDLE;
static uint32_t  _stateEnteredMs  = 0;
static uint32_t  _lastCheckMs     = 0;
static bool      _bootCheckDone   = false;
static bool      _checkPending    = false;   // forced by otaCheckNow()
static char      _remoteVersion[24] = "";
static char      _lastError[64]     = "";
static uint8_t   _progressPct       = 0;

// ─── version utils ──────────────────────────────────────────────────────────

static void _stripV(char* dst, size_t dstLen, const char* src) {
  size_t i = 0;
  if (src[0] == 'v' || src[0] == 'V') src++;
  while (src[i] && i < dstLen - 1) { dst[i] = src[i]; i++; }
  dst[i] = 0;
}

// Returns >0 if remote newer than current, <0 if older, 0 if equal.
// Accepts X / X.Y / X.Y.Z formats; missing components treated as 0.
static int _semverCmp(const char* a, const char* b) {
  for (int part = 0; part < 3; part++) {
    int va = 0, vb = 0;
    while (*a >= '0' && *a <= '9') { va = va*10 + (*a - '0'); a++; }
    while (*b >= '0' && *b <= '9') { vb = vb*10 + (*b - '0'); b++; }
    if (va != vb) return va - vb;
    if (*a == '.') a++;
    if (*b == '.') b++;
  }
  return 0;
}

// ─── lifecycle helpers ──────────────────────────────────────────────────────

static void _enter(OtaState s) {
  _state = s;
  _stateEnteredMs = millis();
}

static void _setError(const char* msg) {
  strncpy(_lastError, msg, sizeof(_lastError)-1);
  _lastError[sizeof(_lastError)-1] = 0;
  Serial.printf("ota: error: %s\n", _lastError);
  _enter(OTA_ERROR);
}

// Free heap and quiesce radios before a big TLS+flash download.
static void _quiesceForDownload() {
  Serial.println("ota: quiescing radios + UI");
  // Stop advertising so the 100ms broadcast bursts don't compete with the
  // WiFi RF slots — that's the main coex stall source on ESP32-C6.
  // Active connections keep working (kept-alive BLE link is cheap); they
  // simply won't receive any data until reboot since spr is gone.
  NimBLEDevice::stopAdvertising();
  // Free the 172*320*2 = ~110KB sprite. UI will be blank until reboot
  // (or restored on error).
  spr.deleteSprite();
  delay(150);
  Serial.printf("ota: free heap before download = %u\n", ESP.getFreeHeap());
}

static void _restoreAfterFailedDownload() {
  // Best-effort restore so the user can keep using the device after a
  // failed update attempt.
  spr.createSprite(OTA_SPR_W, OTA_SPR_H);
  NimBLEDevice::startAdvertising();
}

static void _onUpdateProgress(int cur, int total) {
  if (total <= 0) return;
  uint8_t pct = (uint8_t)((uint64_t)cur * 100 / total);
  if (pct != _progressPct) {
    _progressPct = pct;
    // Throttle log spam — every 10%.
    if (pct % 10 == 0) Serial.printf("ota: %u%% (%d/%d)\n", pct, cur, total);
  }
}

// ─── check phase: GET /releases/latest, parse tag_name ──────────────────────

static bool _doCheck() {
  if (wifiLinkState() != WLINK_CONNECTED) {
    _setError("wifi not connected");
    return false;
  }

  NetworkClientSecure client;
  _configureTls(client);
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setUserAgent("claudegochi-ota/" FIRMWARE_VERSION);

  char url[160];
  snprintf(url, sizeof(url),
           "https://api.github.com/repos/%s/%s/releases/latest",
           OTA_OWNER, OTA_REPO);
  Serial.printf("ota: GET %s\n", url);

  if (!http.begin(client, url)) {
    _setError("http begin failed");
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    char buf[64];
    snprintf(buf, sizeof(buf), "GET releases/latest → %d", code);
    _setError(buf);
    http.end();
    return false;
  }

  // Filter to keep heap small — release JSON can be 10-20KB but we only
  // need tag_name. ArduinoJson v7 stream filter pattern.
  JsonDocument filter;
  filter["tag_name"] = true;
  JsonDocument doc;
  DeserializationError derr = deserializeJson(
    doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (derr) {
    char buf[64];
    snprintf(buf, sizeof(buf), "json parse: %s", derr.c_str());
    _setError(buf);
    return false;
  }

  const char* tag = doc["tag_name"] | "";
  if (!*tag) {
    _setError("no tag_name in release");
    return false;
  }
  _stripV(_remoteVersion, sizeof(_remoteVersion), tag);
  Serial.printf("ota: remote=%s, current=%s\n", _remoteVersion, FIRMWARE_VERSION);
  return true;
}

// ─── download phase: HTTPUpdate to ota_1, ESP.restart on success ────────────

static void _doDownload() {
  _quiesceForDownload();

  NetworkClientSecure client;
  _configureTls(client);
  client.setTimeout(DOWNLOAD_TIMEOUT_MS / 1000);

  httpUpdate.setLedPin(-1, LOW);
  httpUpdate.rebootOnUpdate(false);   // we ESP.restart() ourselves
  httpUpdate.onProgress(_onUpdateProgress);

  char url[200];
  snprintf(url, sizeof(url),
           "https://github.com/%s/%s/releases/latest/download/%s",
           OTA_OWNER, OTA_REPO, OTA_ASSET);
  Serial.printf("ota: download %s\n", url);

  t_httpUpdate_return r = httpUpdate.update(client, url);
  switch (r) {
    case HTTP_UPDATE_OK:
      Serial.println("ota: update OK, restarting");
      _enter(OTA_DONE);
      delay(500);
      ESP.restart();
      // not reached
      return;
    case HTTP_UPDATE_NO_UPDATES:
      // Shouldn't happen — we already version-checked.
      Serial.println("ota: server says no updates");
      _restoreAfterFailedDownload();
      _enter(OTA_IDLE);
      return;
    case HTTP_UPDATE_FAILED:
    default: {
      char buf[64];
      snprintf(buf, sizeof(buf), "update fail %d: %s",
               (int)httpUpdate.getLastError(),
               httpUpdate.getLastErrorString().c_str());
      _restoreAfterFailedDownload();
      _setError(buf);
      return;
    }
  }
}

// ─── public API ─────────────────────────────────────────────────────────────

void otaInit() {
  Serial.printf("ota: init, current version=%s, target=%s/%s\n",
                FIRMWARE_VERSION, OTA_OWNER, OTA_REPO);
  _enter(OTA_IDLE);
}

void otaCheckNow() {
  Serial.println("ota: manual check requested");
  _checkPending = true;
}

void otaTick() {
  uint32_t now = millis();

  switch (_state) {
    case OTA_IDLE: {
      // Only act when WiFi is up.
      if (wifiLinkState() != WLINK_CONNECTED) return;

      bool dueByBoot   = !_bootCheckDone && (now >= BOOT_CHECK_DELAY_MS);
      bool dueByPoll   = _lastCheckMs && (now - _lastCheckMs >= POLL_INTERVAL_MS);
      bool dueByForce  = _checkPending;

      if (!(dueByBoot || dueByPoll || dueByForce)) return;

      _checkPending = false;
      _bootCheckDone = true;
      _lastCheckMs = now;
      _enter(OTA_CHECKING);
      // fallthrough to handle CHECKING in the same tick
      [[fallthrough]];
    }

    case OTA_CHECKING: {
      if (!_doCheck()) return;   // _setError already moved us to ERROR

      if (_semverCmp(_remoteVersion, FIRMWARE_VERSION) > 0) {
        Serial.println("ota: newer version available, downloading");
        _enter(OTA_DOWNLOADING);
        _doDownload();   // blocks until done/failed; updates state internally
      } else {
        Serial.println("ota: up to date");
        _enter(OTA_IDLE);
      }
      return;
    }

    case OTA_DOWNLOADING:
      // _doDownload() runs synchronously in CHECKING fallthrough; we never
      // sit in DOWNLOADING across ticks. Defensive: if we somehow land here,
      // wait for completion (no-op).
      return;

    case OTA_DONE:
      // Reboot already requested in _doDownload; nothing to do.
      return;

    case OTA_ERROR:
      // Back to IDLE after the normal poll interval. Don't hammer the API
      // on every tick when something's broken.
      if (now - _stateEnteredMs >= POLL_INTERVAL_MS) {
        Serial.println("ota: retry after backoff");
        _enter(OTA_IDLE);
      }
      return;
  }
}

OtaState    otaState()           { return _state; }
const char* otaCurrentVersion()  { return FIRMWARE_VERSION; }
const char* otaRemoteVersion()   { return _remoteVersion; }
uint8_t     otaProgressPct()     { return _state == OTA_DOWNLOADING ? _progressPct : 0; }
const char* otaLastError()       { return _lastError; }

const char* otaStateName() {
  switch (_state) {
    case OTA_IDLE:        return "idle";
    case OTA_CHECKING:    return "checking";
    case OTA_DOWNLOADING: return "downloading";
    case OTA_DONE:        return "done";
    case OTA_ERROR:       return "error";
  }
  return "?";
}

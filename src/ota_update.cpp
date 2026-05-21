#include "ota_update.h"
#include "wifi_link.h"
#include "character.h"      // characterPalette() for the OTA progress UI
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

// On-screen OTA status frame. Sprite is kept alive throughout the update
// so we can paint progress; the main loop's normal draw path doesn't run
// during the blocking HTTPUpdate.update() call, so our overlay sticks.
static int     _lastBytesDone  = -1;
static int     _lastBytesTotal = -1;
static const char* _lastErrMsg = nullptr;

static void _drawOtaStatus() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setFont(&fonts::Font0);
  spr.setTextDatum(TC_DATUM);

  // Title.
  spr.setTextSize(2);
  spr.setTextColor(p.text, p.bg);
  spr.drawString("OTA UPDATE", OTA_SPR_W/2, 24);

  // From → to.
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  char buf[64];
  snprintf(buf, sizeof(buf), "%s -> %s", FIRMWARE_VERSION,
           _remoteVersion[0] ? _remoteVersion : "?");
  spr.drawString(buf, OTA_SPR_W/2, 60);

  // Source repo, two lines so it fits 172px wide regardless of repo
  // name length. First line is the owner, second is "/repo" so it reads
  // as a path continuation.
  spr.setTextColor(p.textDim, p.bg);
  spr.drawString(OTA_OWNER, OTA_SPR_W/2, 76);
  snprintf(buf, sizeof(buf), "/%s", OTA_REPO);
  spr.drawString(buf, OTA_SPR_W/2, 90);

  // Progress bar.
  const int barX = 16, barY = 120, barW = OTA_SPR_W - 32, barH = 18;
  spr.drawRect(barX, barY, barW, barH, p.textDim);
  int fillW = (int)((uint64_t)(barW - 4) * _progressPct / 100);
  if (fillW > 0) spr.fillRect(barX + 2, barY + 2, fillW, barH - 4, p.body);

  // Percent (big, under the bar).
  spr.setTextSize(2);
  spr.setTextColor(p.text, p.bg);
  snprintf(buf, sizeof(buf), "%u%%", _progressPct);
  spr.drawString(buf, OTA_SPR_W/2, barY + barH + 6);

  // Byte counts.
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  if (_lastBytesDone >= 0 && _lastBytesTotal > 0) {
    if (_lastBytesTotal >= 1024 * 1024) {
      snprintf(buf, sizeof(buf), "%d / %d KB",
               _lastBytesDone / 1024, _lastBytesTotal / 1024);
    } else {
      snprintf(buf, sizeof(buf), "%d / %d B",
               _lastBytesDone, _lastBytesTotal);
    }
    spr.drawString(buf, OTA_SPR_W/2, barY + barH + 32);
  }

  // System info row — free heap + uptime, helps debug stalls.
  uint32_t heap = ESP.getFreeHeap();
  snprintf(buf, sizeof(buf), "heap %luK  up %lus",
           (unsigned long)(heap / 1024),
           (unsigned long)(millis() / 1000));
  spr.drawString(buf, OTA_SPR_W/2, barY + barH + 50);

  // State / error band at the bottom.
  if (_lastErrMsg) {
    spr.setTextSize(1);
    spr.setTextColor(0xF800, p.bg);
    spr.drawString("ERROR", OTA_SPR_W/2, 240);
    // Word-truncate to fit — error strings are short JSON or HTTP codes.
    spr.drawString(_lastErrMsg, OTA_SPR_W/2, 256);
  } else {
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString(otaStateName(), OTA_SPR_W/2, 250);
  }

  spr.setTextDatum(TL_DATUM);  // restore default datum for the rest of the UI
  spr.pushSprite(0, 0);
}

// Quiesce + paint initial OTA frame. Sprite is left alive — the LCD shows
// the update progress until reboot (or restoration on failure).
static void _quiesceForDownload() {
  Serial.println("ota: quiescing radios for download");
  NimBLEDevice::stopAdvertising();
  delay(50);
  Serial.printf("ota: free heap before download = %u\n", ESP.getFreeHeap());
  _lastBytesDone = 0;
  _lastBytesTotal = 0;
  _lastErrMsg = nullptr;
  _drawOtaStatus();
}

static void _restoreAfterFailedDownload() {
  // Sprite was never deleted; main loop resumes drawing next iteration.
  // We just need to re-enable BLE advertising. The OTA error frame stays
  // visible until the main loop's next draw paints over it.
  NimBLEDevice::startAdvertising();
}

static void _onUpdateProgress(int cur, int total) {
  if (total <= 0) return;
  uint8_t pct = (uint8_t)((uint64_t)cur * 100 / total);
  _lastBytesDone = cur;
  _lastBytesTotal = total;
  if (pct != _progressPct) {
    _progressPct = pct;
    // Throttle screen redraw + log to every 5% — pushSprite costs ~50ms,
    // doing it on every chunk would visibly slow the download.
    if (pct % 5 == 0 || pct == 100) {
      Serial.printf("ota: %u%% (%d/%d)\n", pct, cur, total);
      _drawOtaStatus();
    }
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

  // HTTPClient::getStream() chunked-decode path returned IncompleteInput
  // to ArduinoJson under arduino-esp32 v3.x — body wasn't drained before
  // the stream EOF'd. getString() blocks until the whole body lands, then
  // we parse from the in-memory copy. Release JSON is ~10-20KB so this
  // costs us heap briefly but is reliable.
  String body = http.getString();
  http.end();

  JsonDocument filter;
  filter["tag_name"] = true;
  JsonDocument doc;
  DeserializationError derr = deserializeJson(
    doc, body, DeserializationOption::Filter(filter));

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
  // GitHub Releases sends a 302 from github.com → release-assets.github
  // usercontent.com, and HTTPUpdate refuses redirects by default. FORCE
  // accepts cross-host hops (STRICT would reject the domain change).
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
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
      _progressPct = 100;
      _drawOtaStatus();              // final 100% frame before reboot
      _enter(OTA_DONE);
      delay(800);
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
      snprintf(_lastError, sizeof(_lastError), "code %d: %s",
               (int)httpUpdate.getLastError(),
               httpUpdate.getLastErrorString().c_str());
      Serial.printf("ota: error: %s\n", _lastError);
      // Paint the error on screen and hold long enough to read before the
      // main loop's next draw overwrites it.
      _lastErrMsg = _lastError;
      _drawOtaStatus();
      delay(3000);
      _lastErrMsg = nullptr;
      _restoreAfterFailedDownload();
      _enter(OTA_ERROR);
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

#include <M5StickCPlus.h>
#include <LittleFS.h>
#include <FS.h>
#include <esp_mac.h>
#include <stdarg.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <esp_sntp.h>
// arduino-esp32 v3 dropped the implicit `using fs::File`; pull it back so
// the upstream `File f = LittleFS.open(...)` style keeps compiling.
using fs::File;
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
#include "wifi_link.h"
#include "ota_update.h"
// ST7789 panel on the Waveshare board reports 172x320 in portrait. The
// menu / panel layout below auto-centers; the pet and HUD areas use these
// constants directly so widening the canvas just adds breathing room.
const int W = 172, H = 320;
const int CX = W / 2;
const int CY_BASE = H / 2;
// On M5StickC this drove an active-low red LED on GPIO10. The Waveshare
// board has no plain LED — only a WS2812 fired through M5.Beep — so the
// pulse path is gated on LED_PIN >= 0 to stay a no-op here.
const int LED_PIN = -1;

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
// brightLevel mirrors settings().bright so the rest of the codebase can read
// it as a plain uint8_t without pulling stats.h into every TU. Initialised
// to max; settingsLoad() in setup() overwrites it with the persisted value.
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_SLOT, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = false;
bool     gifAvailable = false;            // any GIF pack present (preserved name for xfer.h)
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use a GIF pack

// Installed GIF packs (scanned at boot from /characters/). With all four
// bundled (bufo, cogito, clawd, hoodie) plus optional user uploads we
// cap at 8 — plenty of headroom, and the names array is fixed-size to
// avoid heap fragmentation on a long-running device.
const uint8_t   GIFS_MAX = 8;
char            gifNames[GIFS_MAX][24];   // external linkage — xfer.h scans this after folder push
uint8_t         gifCount = 0;
uint8_t         gifIdx   = 0;             // external linkage — xfer.h patches it after folder push

// Persisted GIF selection — Preferences NVS, namespace "buddy", key "gif".
// Survives reboots so the user keeps the pack they picked.
static uint8_t gifIdxLoad() {
  Preferences p; p.begin("buddy", true);
  uint8_t v = p.getUChar("gif", 0);
  p.end();
  return v;
}
static void gifIdxSave(uint8_t v) {
  Preferences p; p.begin("buddy", false);
  p.putUChar("gif", v);
  p.end();
}

// Refresh `gifNames` / `gifCount` from LittleFS. Called at boot and again
// after a folder-push transfer that touches /characters/ (xfer.h's
// char_end path).
void refreshGifList() {
  gifCount = characterListInstalled(gifNames, GIFS_MAX);
  gifAvailable = (gifCount > 0);
  if (gifIdx >= gifCount) gifIdx = 0;
}

// Cycle ASCII species 0..N-1 → GIF[0..M-1] → species 0. Species lead
// the list (matches upstream's original ordering). Combined position
// for the info-page counter: species → 1..N, GIFs → N+1..N+M.
// NVS "species" key stays 0xFF while on any GIF; "gif" holds the index.
static void nextPet() {
  uint8_t nSp = buddySpeciesCount();
  if (buddyMode) {
    // Currently on ASCII species — advance within the species pool.
    if (buddySpeciesIdx() + 1 < nSp) {
      buddyNextSpecies();
    } else if (gifAvailable) {
      // Past the last species → jump to GIF 0.
      buddyMode = false;
      gifIdx = 0;
      gifIdxSave(0);
      speciesIdxSave(SPECIES_GIF);
      characterInit(gifNames[gifIdx]);
    }
  } else {
    // Currently on a GIF — advance within the GIF pool.
    if (gifIdx + 1 < gifCount) {
      gifIdx++;
      gifIdxSave(gifIdx);
      characterInit(gifNames[gifIdx]);
    } else {
      // Past the last GIF → wrap back to species 0.
      buddyMode = true;
      buddySetSpeciesIdx(0);
      speciesIdxSave(0);
    }
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

uint32_t promptArrivedMs = 0;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { M5.Axp.ScreenBreath(20 + brightLevel * 20); }

// _clkLastRead lives below in main.cpp's TU; forward-declare it so ntpTick
// (defined earlier in the file) can reset the RTC-read cache.
extern uint32_t _clkLastRead;

// Local UTC offset in seconds. Runtime mirror of settings().tzOffsetSec —
// kept as a non-namespaced global because data.h's _applyJson needs to
// patch it from a different translation unit via `extern int32_t`. Three
// independent updaters: settingsLoad() on boot (NVS), desktop bridge,
// GeoIP autodetect. _applyTzOffsetChange() reapplies the wall clock
// whenever this value moves.
int32_t _tzOffsetSec = 0;

// State for NTP drift measurement + tz-change re-application. Truth UTC
// at last sync is the source of all subsequent time computation; we
// derive local-on-display by adding _tzOffsetSec at read time, so a tz
// flip after sync repaints the clock without losing accuracy.
static uint32_t _ntpLastSyncMs  = 0;   // millis() at the moment of last sync
static uint32_t _ntpLastSyncUtc = 0;   // truth UTC seconds at last sync

// Apply UTC truth to the system clock + M5.Rtc using the current
// _tzOffsetSec. M5Shim's SetTime/SetDate decompose local components into
// time(NULL) under TZ=UTC0, so time(NULL) ends up holding local-epoch.
static void _applyTimeUtc(uint32_t utcEpoch) {
  _ntpLastSyncUtc = utcEpoch;
  _ntpLastSyncMs  = millis();
  time_t local = (time_t)utcEpoch + _tzOffsetSec;
  struct tm lt; gmtime_r(&local, &lt);
  RTC_TimeTypeDef tm = { (uint8_t)lt.tm_hour, (uint8_t)lt.tm_min, (uint8_t)lt.tm_sec };
  RTC_DateTypeDef dt = { (uint8_t)lt.tm_wday, (uint8_t)(lt.tm_mon + 1),
                         (uint8_t)lt.tm_mday, (uint16_t)(lt.tm_year + 1900) };
  M5.Rtc.SetTime(&tm);
  M5.Rtc.SetDate(&dt);
  _clkLastRead = 0;
  _rtcValid = true;
}

// Called by anyone updating _tzOffsetSec (data.h on desktop time msg,
// geoipTick on autodetect): replay the current UTC truth through
// _applyTimeUtc so the wall clock jumps to the new locale immediately
// instead of waiting for the next hourly NTP resync.
void _applyTzOffsetChange() {
  if (_ntpLastSyncMs == 0) return;     // never synced — nothing to repaint
  uint32_t elapsedMs = millis() - _ntpLastSyncMs;
  int32_t  drift = stats().clockDriftPpm;
  int64_t  correctionMs = -((int64_t)elapsedMs * drift) / 1000000LL;
  uint32_t curUtc = _ntpLastSyncUtc + (elapsedMs + (uint32_t)correctionMs) / 1000;
  _applyTimeUtc(curUtc);
}

// Minimal SNTP/UDP client. Hits pool.ntp.org:123 directly so we get truth
// on demand instead of relying on configTime's hidden background refresh
// schedule — that matters for drift measurement, where we need a fresh
// truth value at a known moment. Returns 0 on failure.
static uint32_t fetchNtpUtc() {
  WiFiUDP udp;
  if (!udp.begin(0)) return 0;
  IPAddress addr;
  if (!WiFi.hostByName("pool.ntp.org", addr)) { udp.stop(); return 0; }
  uint8_t pkt[48] = {0};
  pkt[0] = 0xE3;   // LI=3 (no warning), VN=4, mode=3 (client)
  udp.beginPacket(addr, 123);
  udp.write(pkt, 48);
  udp.endPacket();
  uint32_t deadline = millis() + 3000;
  while ((int32_t)(millis() - deadline) < 0) {
    int n = udp.parsePacket();
    if (n >= 48) {
      udp.read(pkt, 48);
      udp.stop();
      uint32_t ntpSec = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16)
                     | ((uint32_t)pkt[42] << 8)  | (uint32_t)pkt[43];
      if (ntpSec < 2208988800UL) return 0;     // malformed / pre-1970
      return ntpSec - 2208988800UL;
    }
    delay(10);
  }
  udp.stop();
  return 0;
}

// SNTP poller + drift estimator. configTime() in setup() kicks off the
// arduino-esp32 background SNTP so the first sync lands without us
// blocking. Once synced, we run our own hourly UDP NTP query — compare
// the truth against the drifted local time(NULL), compute crystal drift
// in PPM, smooth into stats().clockDriftPpm and persist.
static void ntpTick() {
  static uint32_t lastPollMs = 0;
  static bool     synced     = false;
  uint32_t now = millis();
  if (wifiLinkState() != WLINK_CONNECTED) return;
  uint32_t interval = synced ? 3600000UL : 2000UL;  // post-sync: 1h; pre: 2s
  if (lastPollMs && now - lastPollMs < interval) return;
  lastPollMs = now;

  if (!synced) {
    // Initial sync: rely on configTime() background acquisition. As soon
    // as time(NULL) crosses a sane epoch threshold we lock in.
    time_t t = time(NULL);
    if (t < 1700000000) return;
    _applyTimeUtc((uint32_t)t);
    synced = true;
    // Stop the background SNTP daemon — it would otherwise periodically
    // overwrite time(NULL) with real UTC, which our M5Shim-driven local-
    // epoch convention reads back as a UTC-looking wall clock. From here
    // on we own the resync schedule via fetchNtpUtc() below.
    esp_sntp_stop();
    Serial.printf("ntp: initial sync at utc=%lu (tz %+ld), background sntp stopped\n",
                  (unsigned long)t, (long)_tzOffsetSec);
    return;
  }

  // Periodic resync: fetch truth via direct UDP NTP, measure drift
  // against the drifted local time(NULL) BEFORE overwriting it. Math
  // runs entirely in UTC so a tz change between syncs doesn't pollute
  // the measurement.
  uint32_t observedUtc = (uint32_t)time(NULL) - _tzOffsetSec;
  uint32_t truthUtc    = fetchNtpUtc();
  if (truthUtc == 0) {
    Serial.println("ntp: udp query failed, will retry next cycle");
    return;
  }
  int32_t  driftSec   = (int32_t)observedUtc - (int32_t)truthUtc;
  int32_t  elapsedSec = (int32_t)truthUtc - (int32_t)_ntpLastSyncUtc;

  if (elapsedSec >= 60) {
    int32_t driftPpm = (int32_t)((int64_t)driftSec * 1000000LL / elapsedSec);
    // Sanity-clamp: real crystals drift ±100 ppm; >1000 ppm is a
    // measurement artefact (sync race, network jitter) — drop it.
    if (driftPpm > -1000 && driftPpm < 1000) {
      statsOnClockDrift(driftPpm);
      Serial.printf("ntp: resync — drift=%dppm (observedUtc=%lu, truthUtc=%lu, "
                    "elapsed=%lds, smoothed=%dppm)\n",
                    driftPpm, (unsigned long)observedUtc,
                    (unsigned long)truthUtc,
                    (long)elapsedSec, (int)stats().clockDriftPpm);
    } else {
      Serial.printf("ntp: drift %dppm out of range, ignored\n", driftPpm);
    }
  }
  _applyTimeUtc(truthUtc);
}

// GeoIP timezone autodetect. Runs once per boot after WiFi up, only if
// no other source has populated _tzOffsetSec yet (i.e. NVS was empty and
// the desktop bridge hasn't pushed a time message). Hits worldtimeapi.org
// over plain HTTP — the response just contains tz offsets, no
// authentication needed, IP is the implicit query parameter.
static void geoipTick() {
  static bool     done           = false;
  static uint32_t lastAttemptMs  = 0;
  static uint8_t  attempts       = 0;
  if (done) return;
  if (wifiLinkState() != WLINK_CONNECTED) return;
  // If anyone else (NVS load / desktop / previous geoip) already set tz,
  // skip — desktop is more accurate, NVS preserves last truth.
  if (_tzOffsetSec != 0) { done = true; return; }
  uint32_t now = millis();
  if (lastAttemptMs && now - lastAttemptMs < 30000) return;   // 30s retry
  if (attempts >= 5) { done = true; return; }                  // give up after 5
  lastAttemptMs = now;
  attempts++;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(5000);
  http.setUserAgent("claudegochi-geoip/" FIRMWARE_VERSION);
  if (!http.begin(client, "http://worldtimeapi.org/api/ip")) return;
  int code = http.GET();
  if (code != 200) { http.end(); return; }
  String body = http.getString();
  http.end();

  JsonDocument filter;
  filter["raw_offset"] = true;
  filter["dst_offset"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return;
  int32_t raw = doc["raw_offset"] | 0;
  int32_t dst = doc["dst_offset"] | 0;
  int32_t newTz = raw + dst;
  if (newTz == 0) {
    Serial.println("geoip: offset 0 (UTC) — keeping uncalibrated");
    done = true;
    return;
  }
  Serial.printf("geoip: tz_offset=%ld sec (raw=%d, dst=%d)\n",
                (long)newTz, raw, dst);
  _tzOffsetSec = newTz;
  settings().tzOffsetSec = newTz;
  settingsSave();
  _applyTzOffsetChange();
  done = true;
}

// time(NULL) drifts with the crystal between NTP syncs. Apply the
// learned drift correction so the displayed wall clock matches reality
// even mid-cycle. Called from clockRefreshRtc().
static time_t correctedTime() {
  time_t raw = time(NULL);
  int32_t drift = stats().clockDriftPpm;
  if (drift == 0 || _ntpLastSyncMs == 0) return raw;
  uint32_t elapsedMs = millis() - _ntpLastSyncMs;
  // Internal clock runs at (1 + drift*1e-6) of true rate, so time(NULL)
  // has accumulated elapsedMs/1000 * drift/1e6 seconds of excess. Subtract.
  int64_t correctionSec = -((int64_t)elapsedMs * (int64_t)drift) / 1000000000LL;
  return raw + correctionSec;
}

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    M5.Axp.SetLDO2(true);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5.Beep.tone(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

const char* menuItems[] = { "settings", "check ota", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 7;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "brightness", "sound", "bluetooth", "wifi", "led", "transcript", "clock rot", "ascii pet", "reset", "back" };
const uint8_t SETTINGS_N = 10;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      s.bright = brightLevel;        // persist; falls through to settingsSave()
      applyBrightness();
      break;
    case 1: s.sound = !s.sound; break;
    case 2:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 3: s.wifi = !s.wifi; break;   // stored only — no WiFi stack linked
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6: s.clockRot = (s.clockRot + 1) % 3; break;
    case 7: nextPet(); return;
    case 8: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 9: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// UI-scale constants — Waveshare 172x320 is ~1.27× wider and 1.33× taller
// than the M5StickC Plus 135x240 the upstream layouts were tuned for.
// Width gets a direct ~1.27× bump on the panels; height comes from
// swapping the menus to the Cyrillic 8×13 font (= 33 % wider, 60 % taller
// than the upstream 6×8 GLCD), so item line-spacing widens accordingly.
const int MENU_W       = 160;
const int MENU_LINE_H  = 18;
const int MENU_PAD_TOP = 12;
const int MENU_HINT_H  = 20;

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at
// bottom. Default labels reflect the single-button mapping: tap to
// navigate (↓), hold ~½s to confirm/page (→).
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "tap", const char* rightLbl = "hold") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setFont(&m5CyrillicFont());
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, PANEL);
  // 8 px/glyph monospace at the 8x13 Cyrillic font.
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 8 + 4;
  spr.fillTriangle(x, hy + 2, x + 8, hy + 2, x + 4, hy + 9, p.textDim);
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 8 + 4;
  spr.fillTriangle(x, hy, x, hy + 9, x + 7, hy + 4, p.textDim);
  spr.setFont(&fonts::Font0);
}

static void drawSettings() {
  const Palette& p = characterPalette();
  // When the "ascii pet" row (index 7) is the active selection, expand
  // the panel: 1 line for the pink name + 3 lines for a mini preview
  // thumbnail rendered via characterRenderTo (peek mode, half-scale).
  int extra = (settingsSel == 7) ? MENU_LINE_H * 4 : 0;
  int mw = MENU_W, mh = MENU_PAD_TOP * 2 + SETTINGS_N * MENU_LINE_H + MENU_HINT_H + extra;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setFont(&m5CyrillicFont());
  spr.setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    // Rows after the pet row get pushed down by the extra line so the
    // name slot is reserved between them.
    int yOff = (i > 7 && settingsSel == 7) ? MENU_LINE_H * 4 : 0;
    int rowY = my + MENU_PAD_TOP + i * MENU_LINE_H + yOff;
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, rowY);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 42, rowY);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 6) {
      static const char* const RN[] = { "auto", "port", "land" };
      spr.print(RN[s.clockRot]);
    } else if (i == 7) {
      // Combined position: species 1..nSp first, then GIFs nSp+1..nSp+M.
      uint8_t nSp   = buddySpeciesCount();
      uint8_t total = nSp + gifCount;
      uint8_t pos   = buddyMode ? (buddySpeciesIdx() + 1)
                                : (nSp + gifIdx + 1);
      spr.printf("%u/%u", pos, total);
      // Pet name on the next line, right-aligned, in pink. Only shown
      // when the row is the active selection so the menu stays compact
      // when the user isn't hovering the pet picker.
      if (sel) {
        const char* nm = buddyMode ? buddySpeciesName()
                                   : (gifCount > 0 ? gifNames[gifIdx] : "");
        if (nm && *nm) {
          static const uint16_t PINK = 0xFB14;   // RGB565 ≈ #F86CA0, soft hot-pink
          spr.setTextColor(PINK, PANEL);
          spr.setTextDatum(TR_DATUM);
          spr.drawString(nm, mx + mw - 6, rowY + MENU_LINE_H);
          spr.setTextDatum(TL_DATUM);
          // Mini preview, right-aligned under the pet name (same 6 px
          // right padding as the name drawString above). GIF packs
          // render via characterRenderTo (peek path — half-scale,
          // ~48×50 for a 96×100 source); the box is filled with the
          // pack's pal.bg first so transparent pixels (rendered as
          // pal.bg by gifDrawCb) blend with the box, not the menu
          // PANEL. ASCII species fall back to a body-colour disc.
          int boxW = 56, boxH = 56;
          int boxX = mx + mw - 6 - boxW;
          int boxY = rowY + MENU_LINE_H * 2;
          if (!buddyMode && characterLoaded()) {
            uint16_t bg = characterPalette().bg;
            spr.fillRoundRect(boxX, boxY, boxW, boxH, 3, bg);
            spr.drawRoundRect(boxX, boxY, boxW, boxH, 3, p.textDim);
            characterRenderTo(&spr, boxX + boxW/2, boxY + boxH/2);
          } else if (buddyMode) {
            // ASCII species render. buddyRenderTo paints at the
            // species' hardcoded geometry at 1× scale: body lines run
            // from BUDDY_Y_BASE=30 down ~6 rows of 8 px each, so the
            // body's visual centre sits ~24 px below BUDDY_Y_BASE.
            // buddyShift translates so that visual centre lands at the
            // box centre, putting the species inside the frame instead
            // of dangling off the bottom.
            spr.fillRoundRect(boxX, boxY, boxW, boxH, 3, 0x0000);
            spr.drawRoundRect(boxX, boxY, boxW, boxH, 3, p.textDim);
            int dx = (boxX + boxW/2) - 86;          // BUDDY_X_CENTER
            int dy = (boxY + boxH/2) - (30 + 24);   // BUDDY_Y_BASE + body half-height
            buddyShift(dx, dy);
            buddyRenderTo(&spr, 1 /* P_IDLE */);
            buddyShift(0, 0);
          }
        }
      }
    }
  }
  spr.setFont(&fonts::Font0);
  drawMenuHints(p, mx, mw, my + mh - 14, "Next", "Change");
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = MENU_W, mh = MENU_PAD_TOP * 2 + RESET_N * MENU_LINE_H + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setFont(&m5CyrillicFont());
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + MENU_PAD_TOP + i * MENU_LINE_H);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  spr.setFont(&fonts::Font0);
  drawMenuHints(p, mx, mw, my + mh - 14);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1:
      // "check ota" — kick the OTA module's manual check. It picks up
      // on the next otaTick() (within a frame). Close the menu so the
      // user sees the resulting OTA progress overlay if an update fires.
      otaCheckNow();
      menuOpen = false;
      characterInvalidate();
      break;
    case 2: M5.Axp.PowerOff(); break;
    case 3:
    case 4:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 3) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 5: dataSetDemo(!dataDemo()); break;
    case 6: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = MENU_W, mh = MENU_PAD_TOP * 2 + MENU_N * MENU_LINE_H + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setFont(&m5CyrillicFont());
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + MENU_PAD_TOP + i * MENU_LINE_H);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 5) spr.print(dataDemo() ? "  on" : "  off");
  }
  spr.setFont(&fonts::Font0);
  drawMenuHints(p, mx, mw, my + mh - 14);
}

// Clock orientation: gravity along the in-plane X axis means the stick is
// on its side. Signed counter for hysteresis on both transitions — same
// pattern as face-down nap.
//   0 = portrait (sprite path, pet sleeps underneath)
//   1 = landscape, BtnA-side down (M5.Lcd rotation 1)
//   3 = landscape, USB-side down (M5.Lcd rotation 3)
static uint8_t clockOrient   = 0;
static int8_t  orientFrames  = 0;
static uint8_t paintedOrient = 0;
// RTC and IMU share an I2C bus. Reading the RTC at 60fps starves the IMU
// reads in clockUpdateOrient — orientation detection gets noisy. Cache the
// time once per second; mood logic and drawClock both read from here.
static RTC_TimeTypeDef _clkTm;
static RTC_DateTypeDef _clkDt;
uint32_t               _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool            _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = M5.Axp.GetVBusVoltage() > 4.0f;
  // Drift-corrected wall clock. correctedTime() returns the system time
  // minus accumulated crystal drift since the last NTP sync, so the
  // clock face stays accurate between hourly resyncs.
  time_t now = correctedTime();
  struct tm lt; localtime_r(&now, &lt);
  _clkTm.Hours   = lt.tm_hour;
  _clkTm.Minutes = lt.tm_min;
  _clkTm.Seconds = lt.tm_sec;
  _clkDt.WeekDay = lt.tm_wday;
  _clkDt.Month   = lt.tm_mon + 1;
  _clkDt.Date    = lt.tm_mday;
  _clkDt.Year    = lt.tm_year + 1900;
}

static void clockUpdateOrient() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  uint8_t lock = settings().clockRot;
  if (lock == 1) { clockOrient = 0; return; }
  if (lock == 2) {
    // Locked landscape: never drop to 0, but still pick 1 vs 3 from
    // gravity so the cradle works either way up. Need a strong tilt
    // for the 1↔3 swap so handling jitter doesn't flip it; otherwise
    // hold whatever we last had (or 1 from boot).
    if (clockOrient == 0) clockOrient = (ax >= 0) ? 1 : 3;
    if      (ax >  0.5f && clockOrient != 1) clockOrient = 1;
    else if (ax < -0.5f && clockOrient != 3) clockOrient = 3;
    return;
  }
  // Dual threshold: strict to enter (must be clearly sideways), loose to
  // stay (tolerate ~65° of tilt). With one shared threshold a slight lean
  // while sitting on the long edge puts ax right at the boundary and the
  // counter ratchets down in ~half a second.
  bool side = (clockOrient == 0)
    ? fabsf(ax) > 0.7f && fabsf(ay) < 0.5f && fabsf(az) < 0.5f
    : fabsf(ax) > 0.4f;
  if (side) { if (orientFrames < 20) orientFrames++; }
  else      { if (orientFrames > -10) orientFrames--; }
  if (clockOrient == 0 && orientFrames >= 15) {
    clockOrient = (ax > 0) ? 1 : 3;
  } else if (clockOrient != 0 && orientFrames <= -8) {
    clockOrient = 0;
  } else if (clockOrient != 0 && side) {
    // Direct 1↔3: a fast flip keeps |ax|>0.7 (just changes sign), so
    // `side` never drops and the exit-via-0 path can't fire. Watch for
    // ax sign disagreeing with the stored orientation.
    static int8_t swapFrames = 0;
    uint8_t want = (ax > 0) ? 1 : 3;
    if (want != clockOrient) { if (++swapFrames >= 8) { clockOrient = want; swapFrames = 0; } }
    else swapFrames = 0;
  }
}

// Clock face: shown when charging on USB with nothing else going on.
// Portrait paints the upper ~110px to the sprite; pet renders below.
// Landscape draws direct to LCD with rotation — sprite stays untouched.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clkDt.WeekDay % 7; }
static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clkTm.Seconds);
  uint8_t mi = (_clkDt.Month >= 1 && _clkDt.Month <= 12) ? _clkDt.Month - 1 : 0;
  char dl[8]; snprintf(dl, sizeof(dl), "%s %02u", MON[mi], _clkDt.Date);

  if (clockOrient == 0) {
    paintedOrient = 0;
    const bool tallPet = !buddyMode && characterIsTall();
    if (!tallPet) {
      // Short-pack mode (original layout): pet peeks at the top, clock
      // owns the bottom. Wipe the area below the peek strip cleanly.
      spr.fillRect(0, 90, W, H - 90, p.bg);
    } else {
      // Tall-pack mode (e.g. hoodie): pet keeps the whole panel; clock
      // overlays the lower band with a stippled dim so the pet stays
      // readable through the time/date glyphs. Stipple = every other
      // pixel painted with bg, ~50% perceived dim without an alpha buffer.
      const int dimY = 170, dimH = 100;
      for (int y = dimY; y < dimY + dimH; y++) {
        for (int x = (y & 1); x < W; x += 2) {
          spr.drawPixel(x, y, p.bg);
        }
      }
    }
    spr.setTextDatum(MC_DATUM);
    // Sizes bumped one step (4→5, 2→3) and y positions stretched into the
    // taller 320 px panel. HH:MM at size 5 = 5 chars × 30 px = 150 px,
    // fits 172-wide screen with 11 px each side.
    spr.setTextSize(5); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, 180);
    spr.setTextSize(3); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, 224);
    spr.setTextSize(1);                                     spr.drawString(dl, CX, 250);
    spr.setTextDatum(TL_DATUM);
    return;
  }

  // Landscape: 240×135 direct-to-LCD. Full fill only on entry; after that
  // text glyph bg cells repaint themselves and the pet box (small, ~90×50)
  // gets a fillRect each pet tick — small enough not to tear.
  M5.Lcd.setRotation(clockOrient);
  static uint8_t lastSec = 0xFF;
  bool repaint = paintedOrient != clockOrient;
  if (repaint) { M5.Lcd.fillScreen(p.bg); paintedOrient = clockOrient; lastSec = 0xFF; }

  // Seconds tick at 1Hz; redrawing 3 strings at 60fps is 180 SPI ops/sec
  // for nothing. Gate on the second changing (or full repaint).
  if (repaint || _clkTm.Seconds != lastSec) {
    lastSec = _clkTm.Seconds;
    char wdl[12]; snprintf(wdl, sizeof(wdl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkDt.Date);
    char ssl[3]; snprintf(ssl, sizeof(ssl), "%02u", _clkTm.Seconds);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextSize(3); M5.Lcd.setTextColor(p.text, p.bg);    M5.Lcd.drawString(hm, 170, 42);
    M5.Lcd.setTextSize(2); M5.Lcd.setTextColor(p.textDim, p.bg); M5.Lcd.drawString(ssl, 170, 72);
                                                                  M5.Lcd.drawString(wdl, 170, 102);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextSize(1);
  }

  // Pet on left at 5 fps. Clear includes the overlay-particle zone above
  // the body (y<30) — species draw Zzz/hearts there via BUDDY_Y_OVERLAY=6
  // which doesn't go through _yb, so the box has to cover it.
  static uint32_t lastPetTick = 0;
  if (millis() - lastPetTick >= 200) {
    lastPetTick = millis();
    if (buddyMode) {
      // ASCII glyphs don't self-clear; wipe the box each tick. Species
      // hardcode BUDDY_X_CENTER=67 / BUDDY_Y_OVERLAY=6 for particles so
      // keep portrait coords and just swap the surface — pet lands
      // upper-left of landscape, which is where we want it anyway.
      M5.Lcd.fillRect(0, 0, 115, 90, p.bg);
      buddyRenderTo(&M5.Lcd, activeState);
    } else {
      // Full-frame GIFs paint every pixel (transparent → pal.bg), so a
      // per-tick clear just adds a visible black flash between wipe and
      // last scanline. The entry fillScreen on paintedOrient change
      // already covers the surround.
      characterSetState(activeState);
      characterRenderTo(&M5.Lcd, 57, 45);
    }
  }
  M5.Lcd.setRotation(0);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  // recentlyCompleted → P_HEART one-shot, fired by edge-detect in loop().
  // CELEBRATE stays reserved for level-up, so it reads as a rarer milestone
  // than every Claude turn ending.
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 36, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 16;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(4, y); spr.print(section);
  y += 16;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 56);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(8, 184); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 110);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setFont(&m5CyrillicFont());
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[40]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(4, y); spr.print(b); y += 14;
  };

  if (infoPage == 0) {
    // ABOUT is a wall of body text — swap to the small (6x12) Cyrillic
    // font so every paragraph fits on screen without scrolling, and
    // shrink line spacing to match.
    spr.setFont(&m5CyrillicFontSmall());
    auto lnSmall = [&](const char* s) {
      spr.setCursor(4, y); spr.print(s); y += 12;
    };
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    lnSmall("I watch your Claude");
    lnSmall("desktop sessions.");
    y += 4;
    lnSmall("I sleep when nothing's");
    lnSmall("happening, wake when");
    lnSmall("you start working,");
    lnSmall("get impatient when");
    lnSmall("approvals pile up.");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    lnSmall("Tap the Left button");
    lnSmall("on a prompt to approve.");
    y += 4;
    spr.setTextColor(p.textDim, p.bg);
    lnSmall("18 species. Settings");
    lnSmall("> ascii pet to cycle.");
    spr.setFont(&m5CyrillicFont());   // restore big font for header style

  } else if (infoPage == 1) {
    _infoHeader(p, y, "Left Button", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("tap");
    spr.setTextColor(p.textDim, p.bg); ln("    next screen");
    ln("    approve prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("hold ~0.5s");
    spr.setTextColor(p.textDim, p.bg); ln("    page / scroll");
    ln("    deny prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("double-tap");
    spr.setTextColor(p.textDim, p.bg); ln("    open menu"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("Right Button");
    spr.setTextColor(p.textDim, p.bg); ln("    reboots the chip");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    int vBat_mV = (int)(M5.Axp.GetBatVoltage() * 1000);
    int iBat_mA = (int)M5.Axp.GetBatCurrent();
    int vBus_mV = (int)(M5.Axp.GetVBusVoltage() * 1000);
    int pct = (vBat_mV - 3200) / 10;   // (v-3.2)/(4.2-3.2)*100 = (v-3.2)*100 = (mv-3200)/10
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    bool usb = vBus_mV > 4000;
    bool charging = usb && iBat_mA > 1;
    bool full = usb && vBat_mV > 4100 && iBat_mA < 10;

    // 8x13 Cyrillic font already reads as a "big" header at size 1 —
    // bumping it to 2 (16x26) was sized for the smaller 6x8 default and
    // now looks oversized.
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(4, y);
    spr.printf("%d%%", pct);
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(48, y);
    spr.print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
    y += 16;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    ln("  current  %+dmA", iBat_mA);
    if (usb) ln("  usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
    y += 8;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
    ln("  temp     %dC", (int)M5.Axp.GetTempInAXP192());

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    // 8x13 reads as a header at size 1 — size 2 was for the upstream
    // 6x8 default and now over-fills the row.
    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setCursor(4, y);
    spr.print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    y += 16;

    spr.setTextColor(p.textDim, p.bg);
    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 8;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" Open Claude desktop");
      ln(" > Developer");
      ln(" > Hardware Buddy");
      y += 4;
      ln(" auto-connects via BLE");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    y += 8;
    spr.setTextColor(p.textDim, p.bg);
    ln("built by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Vladyslav Kovalenko");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("github.com/hartmanfrost");
    ln("/claude-desktop-buddy");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("firmware");
    y += 4;
    ln(otaCurrentVersion());
    y += 4;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 4;
    ln("Waveshare");
    ln("ESP32-C6-LCD-1.47");
  }
  spr.setFont(&fonts::Font0);
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
//
// `width` is measured in codepoints (visible glyphs), not bytes — every
// Cyrillic letter is 2 UTF-8 bytes, so the upstream byte-counted version
// wrapped Russian text at half the visible width and put one word per
// row. The buffer (`out[row]`) is still indexed by bytes, so the
// algorithm tracks both counts in parallel.
static uint8_t wrapInto(const char* in, char out[][64], uint8_t maxRows, uint8_t width) {
  auto cp_count = [](const char* s, const char* end) -> uint16_t {
    uint16_t n = 0;
    for (const char* q = s; q < end; q++) {
      if ((*q & 0xC0) != 0x80) n++;          // skip UTF-8 continuation bytes
    }
    return n;
  };

  uint8_t row    = 0;
  uint16_t colCp = 0;                          // glyphs already on this row
  uint16_t colB  = 0;                          // bytes already on this row
  const char* p  = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint16_t wlenB  = (uint16_t)(p - w);
    if (wlenB == 0) break;
    uint16_t wlenCp = cp_count(w, p);
    uint16_t needCp = (colCp > 0 ? 1 : 0) + wlenCp;
    if (colCp + needCp > width) {
      out[row][colB] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' ';
      colB = 1; colCp = 1;                     // continuation indent
    }
    if (colB > 1 || (colB == 1 && out[row][0] != ' ')) {
      out[row][colB++] = ' ';
      colCp++;
    } else if (colB == 1 && row > 0) {
      // indent space already accounted for
    }
    // Hard-break overlong words across rows — same shape as upstream but
    // step a codepoint at a time so we don't slice a multi-byte glyph.
    while (wlenCp > width - colCp) {
      uint16_t takeCp = width - colCp;
      const char* q = w;
      while (takeCp > 0 && (size_t)(q - w) < wlenB) {
        do { q++; } while (q < w + wlenB && (*q & 0xC0) == 0x80);
        takeCp--;
      }
      uint16_t takeB = (uint16_t)(q - w);
      memcpy(&out[row][colB], w, takeB);
      colB  += takeB;
      colCp += (width - colCp);
      w     += takeB;
      wlenB -= takeB;
      wlenCp = cp_count(w, w + wlenB);
      out[row][colB] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' ';
      colB = 1; colCp = 1;
    }
    memcpy(&out[row][colB], w, wlenB);
    colB  += wlenB;
    colCp += wlenCp;
  }
  if (colB > 0 && row < maxRows) {
    out[row][colB] = 0;
    row++;
  }
  return row;
}

static void drawApproval() {
  const Palette& p = characterPalette();
  // Approval area grew from 78 to 110 px to match the 8x13 Cyrillic font
  // we now use everywhere — five rows at 14 px line height + a 4 px top
  // margin + 16 px for the bottom hint row.
  const int AREA = 110;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);

  spr.setFont(&m5CyrillicFont());
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, H - AREA + 6);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  // Tool name in size 2 only if it'd still fit; native 8x13 is already
  // big enough that ≥10 chars usually overflow at 2× (16 px → 160 px).
  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 8 ? 2 : 1);
  spr.setCursor(6, H - AREA + 22);
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  // Hint wraps at ~20 chars (172 / 8 = 21 max) to two rows under the tool.
  spr.setTextColor(p.textDim, p.bg);
  int hlen = strlen(tama.promptHint);
  spr.setCursor(6, H - AREA + 56);
  spr.printf("%.20s", tama.promptHint);
  if (hlen > 20) {
    spr.setCursor(6, H - AREA + 72);
    spr.printf("%.20s", tama.promptHint + 20);
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(8, H - 16);
    spr.print("sent...");
  } else {
    // Indent away from the rounded corners. Left = approve via short tap,
    // right = deny via long hold (matches our single-button decoder).
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(8, H - 16);
    spr.print("tap: ok");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 88, H - 16);
    spr.print("hold: deny");
  }
  spr.setFont(&fonts::Font0);
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  // Heart scaled up to ~12×8 (was 8×5) to match the bigger pet-stats
  // layout below. Same shape, just denser pixels.
  if (filled) {
    spr.fillCircle(x - 3, y, 3, col);
    spr.fillCircle(x + 3, y, 3, col);
    spr.fillTriangle(x - 6, y + 2, x + 6, y + 2, x, y + 8, col);
  } else {
    spr.drawCircle(x - 3, y, 3, col);
    spr.drawCircle(x + 3, y, 3, col);
    spr.drawLine(x - 6, y + 2, x, y + 8, col);
    spr.drawLine(x + 6, y + 2, x, y + 8, col);
  }
}

static void drawPetStats(const Palette& p) {
  // Layout rebuilt for the full 172×320 panel — upstream coordinates
  // crammed everything into the upper-left 135×180 of the M5StickC. All
  // x positions get a ~1.3× horizontal stretch, line spacing grows with
  // the 8×13 Cyrillic font, and graphics primitives are bumped one
  // pixel up so they read at arm's length.
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setFont(&m5CyrillicFont());
  spr.setTextSize(1);
  int y = TOP + 24;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, y - 4); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(76 + i * 22, y + 2, i < mood, moodCol);

  y += 24;
  spr.setCursor(8, y - 4); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 56 + i * 12;
    if (i < fed) spr.fillCircle(px, y + 1, 3, p.body);
    else         spr.drawCircle(px, y + 1, 3, p.textDim);
  }

  y += 24;
  spr.setCursor(8, y - 4); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 76 + i * 18;
    if (i < en) spr.fillRect(px, y - 3, 14, 8, enCol);
    else        spr.drawRect(px, y - 3, 14, 8, p.textDim);
  }

  y += 30;
  spr.fillRoundRect(8, y - 4, 60, 20, 4, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(14, y - 1); spr.printf("Lv %u", stats().level);

  y += 28;
  spr.setTextColor(p.textDim, p.bg);

  // EXP toward next level: progress through current 50K-token bucket.
  // Bypass-permissions users have nothing on approved/denied so those rows
  // get suppressed below — surface the actual progression signal here.
  uint32_t exp = stats().tokens % TOKENS_PER_LEVEL;
  spr.setCursor(8, y);
  if (exp >= 1000) spr.printf("exp      %lu.%luK/%uK", exp/1000, (exp/100)%10, (unsigned)(TOKENS_PER_LEVEL/1000));
  else             spr.printf("exp      %lu/%uK",     exp,                    (unsigned)(TOKENS_PER_LEVEL/1000));
  y += 14;

  if (stats().approvals > 0) {
    spr.setCursor(8, y);
    spr.printf("approved %u", stats().approvals);
    y += 14;
  }
  if (stats().denials > 0) {
    spr.setCursor(8, y);
    spr.printf("denied   %u", stats().denials);
    y += 14;
  }

  uint32_t nap = stats().napSeconds;
  spr.setCursor(8, y);
  spr.printf("napped   %luh%02lum", nap/3600, (nap/60)%60);
  y += 14;

  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(8, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000,    (v/100)%10);
    else                spr.printf("%s%lu",      label, v);
  };
  tokFmt("tokens   ", stats().tokens,   y); y += 14;
  tokFmt("today    ", tama.tokensToday, y);
  spr.setFont(&fonts::Font0);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setFont(&m5CyrillicFont());
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(8, y); spr.print(s); y += 14;
  };
  auto gap = [&]() { y += 6; };

  y += 16;  // room for the PET header drawn by drawPet()

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens =");
  ln(p.textDim, " level up + confetti"); gap();

  // No IMU on this board, so face-down nap never triggers — the upstream
  // ENERGY mechanic is effectively static. Hide the row to avoid lying.
  gap();

  ln(p.textDim, "tap = next screen");
  ln(p.textDim, "hold = page / deny");
  ln(p.textDim, "double-tap = menu");
  spr.setFont(&fonts::Font0);
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 70;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — title left, counter right
  // (use the same big Cyrillic font as the body for consistency).
  spr.setFont(&m5CyrillicFont());
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(8, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 36, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
  spr.setFont(&fonts::Font0);
}

// ─── ASCII slot machine (DISP_SLOT) ────────────────────────────────────────
// Easter-egg one-armed bandit. Enter via the tap-cycle through display
// modes (it sits after DISP_INFO). Inside DISP_SLOT, long-press = spin,
// short tap = exit to the next display mode like everywhere else.
//
// Mechanics: three independent reels, each settles at a different
// timestamp so the human gets the satisfying clack-clack-clack feel.
// Three-of-a-kind triggers a P_CELEBRATE one-shot so the pet joins in.
static const char SLOT_SYMBOLS[] = "7$C*?@";   // 6 symbols, including 'C' for Claude
static const uint8_t SLOT_N_SYMBOLS = sizeof(SLOT_SYMBOLS) - 1;
static uint8_t  slotReel[3]     = {0, 0, 0};
static uint32_t slotStopMs[3]   = {0, 0, 0};
static bool     slotSpinning    = false;
static bool     slotWinPending  = false;   // true between settle and next spin
static uint32_t slotLastWinMs   = 0;

static void slotStartSpin() {
  if (slotSpinning) return;
  uint32_t now = millis();
  slotSpinning   = true;
  slotWinPending = false;
  // Stagger reel-stops 600 ms apart. Final reel takes 2s total — fast
  // enough not to bore, long enough that the suspense lands.
  slotStopMs[0] = now + 800;
  slotStopMs[1] = now + 1400;
  slotStopMs[2] = now + 2000;
}

static void slotTick() {
  if (!slotSpinning) return;
  uint32_t now = millis();
  bool allStopped = true;
  for (uint8_t i = 0; i < 3; i++) {
    if ((int32_t)(now - slotStopMs[i]) < 0) {
      // Cycle every 80 ms while spinning — blurry but readable
      slotReel[i] = (uint8_t)((now / 80 + i * 2) % SLOT_N_SYMBOLS);
      allStopped = false;
    } else if ((int32_t)(now - slotStopMs[i]) < 100) {
      // Lock in the final symbol exactly once at stop time. random8() so
      // results don't correlate to the spin start time.
      slotReel[i] = (uint8_t)(esp_random() % SLOT_N_SYMBOLS);
    }
  }
  if (allStopped) {
    slotSpinning   = false;
    slotWinPending = (slotReel[0] == slotReel[1] && slotReel[1] == slotReel[2]);
    if (slotWinPending) {
      slotLastWinMs = now;
      triggerOneShot(P_CELEBRATE, 3000);
      beep(2600, 120);
    } else {
      beep(900, 60);
    }
  }
}

static void drawSlot() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);

  // Title, top-centre under the bezel margin.
  spr.setFont(&fonts::Font0);
  spr.setTextSize(2);
  spr.setTextColor(p.text, p.bg);
  spr.setTextDatum(TC_DATUM);
  spr.drawString("SLOT", W/2, 20);

  // 3 reels in a centered row. Box ~46x68, gap 8 → total width 154, fits 172.
  const int boxW = 46, boxH = 68, gap = 8;
  const int totalW = boxW * 3 + gap * 2;
  const int startX = (W - totalW) / 2;
  const int boxY = 110;

  spr.setFont(&m5CyrillicFont());
  spr.setTextSize(4);
  // Use MC_DATUM (middle-centre) instead of the TC_DATUM inherited from
  // the title above — TC put the glyph top at y, which slid the symbol
  // about half a glyph height below the box. MC anchors the visual
  // centre of the digit on the box centre.
  spr.setTextDatum(MC_DATUM);
  for (int i = 0; i < 3; i++) {
    int x = startX + i * (boxW + gap);
    spr.fillRoundRect(x, boxY, boxW, boxH, 4, p.bg);
    uint16_t border = slotSpinning ? p.body
                    : (slotWinPending ? GREEN : p.textDim);
    spr.drawRoundRect(x, boxY, boxW, boxH, 4, border);
    spr.setTextColor(p.text, p.bg);
    char sym[2] = { SLOT_SYMBOLS[slotReel[i]], 0 };
    spr.drawString(sym, x + boxW/2, boxY + boxH/2);
  }
  spr.setTextDatum(TC_DATUM);   // restore for the labels below

  // Status line under the reels.
  spr.setFont(&fonts::Font0);
  spr.setTextSize(2);
  if (slotSpinning) {
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString("spinning...", W/2, 210);
  } else if (slotWinPending) {
    // Blink JACKPOT for the first 3 seconds after a win
    bool on = ((millis() / 250) & 1) || (millis() - slotLastWinMs > 3000);
    spr.setTextColor(on ? GREEN : p.bg, p.bg);
    spr.drawString("JACKPOT!", W/2, 210);
  } else {
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString("pull lever", W/2, 210);
  }

  // Hint at bottom.
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.drawString("hold = spin", W/2, 270);
  spr.drawString("tap = next page", W/2, 284);
  spr.setTextDatum(TL_DATUM);
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();
  // Small clock at the top-centre of the home screen — sits above the
  // pet character. Centre placement avoids the panel's rounded corners
  // clipping the digits. Only renders when the wall clock is actually
  // valid (NTP / desktop has synced). Cleared each frame so character
  // particles (Zzz, hearts) don't bleed over.
  if (dataRtcValid()) {
    // Font0 size 1 glyphs are 8 px tall. Top y=2 placed the digits
    // under the rounded-corner bezel band; nudge down by ~half a glyph
    // (4 px) so the readout clears the curve.
    const int CW = 34, CH = 12;
    spr.fillRect((W - CW) / 2, 4, CW, CH, p.bg);
    spr.setFont(&fonts::Font0);
    spr.setTextSize(1);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextDatum(TC_DATUM);
    char hm[8]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
    spr.drawString(hm, W / 2, 6);
    spr.setTextDatum(TL_DATUM);
  }
  // u8g2 8x13 monospace Cyrillic font. 172 px / 8 = 21 chars per row.
  // LH bumped to 14 to keep a 1 px gap between rows at the new font
  // height (13 px glyphs).
  const int SHOW = 3, LH = 14, WIDTH = 20;
  const int AREA = SHOW * LH + 4;
  // Tall packs (hoodie reaches into this strip at fit-to-panel scale)
  // get a stippled half-alpha veil — every other pixel set to pal.bg
  // — so the pet stays visible through the gaps between glyphs.
  // Per-glyph opaque bg from setTextColor still gives the text proper
  // legibility on top. Short packs never paint into this band, so the
  // cheaper fillRect is fine.
  //
  // Special case: when the transcript content actually changes
  // (lineGen ticks), do a full opaque fillRect first to wipe the
  // previous text and invalidate the pet so it repaints into the
  // HUD area on the next character tick. Without this, a shorter
  // new transcript line lets the tail of the previous (longer) line
  // stay visible under the stipple — confusing on tall packs and
  // generally noisy on short packs.
  bool petUnderHud = !buddyMode && characterIsTall();
  static uint16_t lastLineGenSeen = 0;
  bool textChanged = (tama.lineGen != lastLineGenSeen);
  lastLineGenSeen = tama.lineGen;
  if (textChanged || !petUnderHud) {
    spr.fillRect(0, H - AREA, W, AREA, p.bg);
    if (textChanged && petUnderHud) characterInvalidate();
  } else {
    for (int y = H - AREA; y < H; y++) {
      for (int x = (y & 1); x < W; x += 2) {
        spr.drawPixel(x, y, p.bg);
      }
    }
  }
  spr.setFont(&m5CyrillicFont());
  spr.setTextSize(1);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  // Wrap all transcript lines into a flat display buffer. When the desktop
  // sent no entries but populated msg (e.g. a user-typed status the buddy
  // briefly mirrors), fall back to wrapping msg through the same path so
  // longer Cyrillic strings get rows instead of clipping off-screen.
  static char disp[32][64];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  uint8_t nLines = tama.nLines;
  if (nLines == 0) {
    nDisp = wrapInto(tama.msg, disp, 32, WIDTH);
    for (uint8_t j = 0; j < nDisp; j++) srcOf[j] = 0;
    nLines = nDisp > 0 ? 1 : 0;
  } else {
    for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
      uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
      for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
      nDisp += got;
    }
  }
  if (nDisp == 0) { spr.setFont(&fonts::Font0); return; }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  // `nLines` is the local source-line count (== 1 when we synthesized rows
  // from msg). Using tama.nLines directly here would underflow to 0xFF
  // when there were no real entries and dim the msg rows.
  uint8_t newest = nLines - 1;
  // Center every transcript row on X — the rounded bottom-left/right
  // corners clip the first/last glyph of any left-aligned line, and the
  // entries the desktop sends ("(called Read)", "(called Edit)", short
  // status lines) all fit centered without wrapping.
  spr.setTextDatum(MC_DATUM);
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.drawString(disp[row], W / 2, H - AREA + 2 + i * LH + LH / 2);
  }
  spr.setTextDatum(TL_DATUM);
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 24, H - LH - 2);
    spr.printf("-%u", msgScroll);
  }
  spr.setFont(&fonts::Font0);   // restore default for the next drawer
}

void setup() {
  M5.begin();
  M5.Lcd.setRotation(0);
  M5.Imu.Init();
  M5.Beep.begin();
  startBt();
  if (LED_PIN >= 0) { pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, HIGH); }
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  // Hydrate global brightLevel from persisted setting before the first
  // applyBrightness() call — otherwise the screen briefly flashes at
  // hardcoded default 4 before settling on whatever the user picked.
  brightLevel = settings().bright;
  applyBrightness();
  // Same idea for the timezone: NVS holds the last known offset (from
  // either GeoIP or desktop). Populating early lets the first clock
  // refresh + the first NTP sync apply local time without a UTC flash.
  _tzOffsetSec = settings().tzOffsetSec;
  petNameLoad();
  buddyInit();

  // BLE stays always-on; s.bt is stored as a preference only.
  spr.createSprite(W, H);
  // Scan /characters/ once, populate gifNames[]. Pick the previously-
  // selected pack from NVS (clamped if it's gone — e.g. after a folder
  // push wiped it), then load it.
  refreshGifList();
  gifIdx = gifIdxLoad();
  if (gifIdx >= gifCount) gifIdx = 0;
  if (gifCount > 0) characterInit(gifNames[gifIdx]);

  // WiFi: read /config/wifi.json, scan, connect to strongest saved AP.
  // Non-blocking — wifiLinkTick() drives the state machine from loop().
  wifiLinkInit();

  // SNTP: kick off background time fetch. configTime(0,0,...) writes UTC
  // into the system clock; ntpTick() applies the locale offset (set on the
  // runtime _tzOffsetSec global by the desktop bridge) and pushes local
  // into M5.Rtc + flips _rtcValid. After the initial sync, ntpTick also
  // runs its own UDP NTP queries hourly to measure crystal drift.
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  Serial.printf("ntp: SNTP started, tz_offset=%ld sec, saved drift=%dppm\n",
                (long)_tzOffsetSec, (int)stats().clockDriftPpm);

  // OTA: poll GitHub Releases for newer firmware. First check is delayed
  // 30s after boot so WiFi has a chance to associate; subsequent checks
  // run hourly. Manual trigger via {"cmd":"ota"}.
  otaInit();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(2);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 12);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 12);
    } else {
      // First boot, no owner pushed yet — say hi.
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 12);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 12);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    spr.pushSprite(0, 0);
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  M5.update();
  M5.Beep.update();
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  wifiLinkTick();
  ntpTick();
  geoipTick();
  otaTick();
  slotTick();

  // Feed the mood-activity ring on every transcript bump — proxies "Claude
  // is talking to me" when there are no approvals to time.
  static uint16_t prevLineGenStats = 0;
  if (tama.lineGen != prevLineGenStats) {
    statsOnLineGen();
    prevLineGenStats = tama.lineGen;
  }

  // Edge-detect Claude finishing a turn: fire P_HEART once on the rising
  // edge of recentlyCompleted. The bridge holds the flag for a short
  // window, so without edge detection HEART would re-trigger every loop.
  static bool prevCompleted = false;
  if (tama.recentlyCompleted && !prevCompleted) {
    triggerOneShot(P_HEART, 2500);
  }
  prevCompleted = tama.recentlyCompleted;

  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // Persona-state LED. The Waveshare board has one WS2812 (vs. the
  // M5StickC's single red LED), so each persona state gets a distinct
  // color/pattern instead of just on-or-off — useful peripheral cue when
  // the screen is out of sight.
  //
  //   sleep      off (don't disturb)
  //   idle       off (resting; the screen says it all)
  //   busy       amber slow breathe (Claude is working)
  //   attention  red fast blink (approval waiting — same urgency the
  //              upstream LED conveyed)
  //   celebrate  rainbow cycle (level up)
  //   dizzy      purple flicker
  //   heart      soft pink steady (responsive-approval thank-you)
  uint8_t lr = 0, lg = 0, lb = 0;
  if (settings().led) {
    auto triBreathe = [](uint32_t period_ms, uint32_t t) -> uint8_t {
      // 0 → 255 → 0 triangle over `period_ms`. Cheaper than sinf and
      // perceptually close enough on a single dim NeoPixel.
      uint32_t phase = t % period_ms;
      uint32_t half = period_ms / 2;
      return phase < half ? (phase * 255 / half) : (255 - (phase - half) * 255 / half);
    };
    switch (activeState) {
      case P_BUSY: {
        uint8_t a = triBreathe(1600, now);
        lr = (uint16_t)a * 255 / 255;          // amber: full red,
        lg = (uint16_t)a *  80 / 255;          //        third green,
        lb = 0;                                //        no blue
        break;
      }
      case P_ATTENTION: {
        bool on = (now / 400) % 2;
        lr = on ? 0xFF : 0;
        break;
      }
      case P_CELEBRATE: {
        // 6-segment hue cycle — three primaries + three secondaries, 1 s
        // per segment. Wraps the rainbow without an HSV→RGB conversion.
        static const uint8_t WHEEL[6][3] = {
          {255,   0,   0}, {255, 165,   0}, {255, 255,   0},
          {  0, 255,   0}, {  0,   0, 255}, {180,   0, 255},
        };
        uint8_t i = (now / 200) % 6;
        lr = WHEEL[i][0]; lg = WHEEL[i][1]; lb = WHEEL[i][2];
        break;
      }
      case P_DIZZY: {
        bool on = (now / 90) % 2;             // ~5 Hz flicker
        lr = on ? 180 : 60;
        lb = on ? 255 : 80;
        break;
      }
      case P_HEART: {
        lr = 255; lg = 60; lb = 100;          // dim pink, no animation
        break;
      }
      case P_SLEEP:
      case P_IDLE:
      default:
        break;                                 // off
    }
  }

  // Status-update pulse: when `tama.lineGen` ticks (the desktop sent a
  // new transcript line / msg), overlay a 1 s blue triangle fade on top
  // of whatever the persona-state palette decided. Doesn't replace the
  // attention blink — that still pokes through on the next cycle.
  static uint16_t prevLineGenLed = 0;
  static uint32_t statusPulseStartMs = 0;
  if (settings().led && tama.lineGen != prevLineGenLed) {
    statusPulseStartMs = now ? now : 1;       // 0 means "no pulse"
    prevLineGenLed = tama.lineGen;
  }
  if (statusPulseStartMs) {
    uint32_t elapsed = now - statusPulseStartMs;
    const uint32_t pulseDur = 1000;
    if (elapsed >= pulseDur) {
      statusPulseStartMs = 0;
    } else {
      uint32_t half = pulseDur / 2;
      uint8_t blueB = elapsed < half
        ? (uint8_t)(elapsed * 255 / half)
        : (uint8_t)(255 - (elapsed - half) * 255 / half);
      // Override the persona color while pulsing — clearer "something
      // happened" cue than blending with red attention etc.
      lr = 0;
      lg = 0;
      lb = blueB;
    }
  }

  M5.Beep.setLed(lr, lg, lb);

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // BtnA: step through fake scenarios
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
    if (screenOff) {
      if (M5.BtnA.isPressed()) swallowBtnA = true;
      if (M5.BtnB.isPressed()) swallowBtnB = true;
    }
    wake();
  }

  // AXP power button (left side): short-press toggles screen off.
  // Long-press (6s) still powers off the device via AXP hardware.
  if (M5.Axp.GetBtnPress() == 0x02) {
    if (screenOff) {
      wake();
    } else {
      M5.Axp.SetLDO2(false);
      screenOff = true;
    }
  }

  if (M5.BtnA.pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    beep(800, 60);
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (M5.BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        beep(2400, 60);
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
      } else {
        beep(1800, 30);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // BtnB: pet → heart
  if (M5.BtnB.wasPressed()) {
    if (swallowBtnB) { swallowBtnB = false; }
    else
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    } else if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsSel);
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      beep(2400, 30);
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else if (displayMode == DISP_SLOT) {
      // Slot machine: hold = pull the lever. Beep deferred — slotTick
      // fires the win/loss tone when reels settle, lever-press itself
      // gets a softer click.
      beep(1600, 40);
      slotStartSpin();
    } else {
      beep(2400, 30);
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
    }
  }

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  // Show the clock face only after CLOCK_IDLE_MS of nothing happening
  // (no sessions, no prompt, no recent transcript change, no buttons).
  // Button activity counts so pressing the Left button dismisses an
  // already-shown clock back to the buddy/HUD view and starts the timer
  // fresh.
  static const uint32_t CLOCK_IDLE_MS = 60000;
  static uint32_t lastActiveMs = 0;
  static uint16_t prevLineGen  = 0;
  bool buttonActivity = M5.BtnA.isPressed() || M5.BtnB.isPressed();
  if (tama.sessionsRunning > 0 || tama.sessionsWaiting > 0 || tama.promptId[0]
      || tama.lineGen != prevLineGen || buttonActivity) {
    lastActiveMs = now;
  }
  prevLineGen = tama.lineGen;
  bool idleEnough = (uint32_t)(now - lastActiveMs) >= CLOCK_IDLE_MS;
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && idleEnough
               && dataRtcValid() && _onUsb;
  if (clocking) clockUpdateOrient();
  else { clockOrient = 0; orientFrames = 0; paintedOrient = 0; }
  bool landscapeClock = clocking && clockOrient != 0;

  static bool wasClocking = false;
  static bool wasLandscape = false;
  if (clocking != wasClocking || landscapeClock != wasLandscape) {
    // Tall packs (e.g. hoodie at 2×=434 px) lose their face in peek
    // mode — half-scale shrinks them into the 70 px header strip and
    // there's no room. For those we keep the home-size GIF and let
    // drawClock paint a dimmed overlay on top instead.
    if (clocking && !landscapeClock && !characterIsTall()) characterSetPeek(true);
    else applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
    wasLandscape = landscapeClock;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clkTm.Hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  // ---- Architectural full-clear on layer transitions ----
  // Each drawer paints into its own sub-region and trusts whatever was
  // there before to either stay (pet on top) or be overwritten. That
  // breaks when a *larger* surface (passkey, approval, clock) yields to
  // a *smaller* one (HUD): the leftover pixels of the bigger surface
  // sit there with nothing to overwrite them. Track a fingerprint of
  // every visible layer this frame; whenever it changes, wipe the
  // sprite and force the pet/character to repaint from scratch. Pet/
  // GIF code is event-gated, so we invalidate them too — otherwise
  // they'd render only on their next animation tick.
  uint32_t frameSig = 0;
  frameSig |= ((uint32_t)displayMode & 0x3) << 0;
  frameSig |= (uint32_t)(menuOpen     ? 1 : 0) << 2;
  frameSig |= (uint32_t)(settingsOpen ? 1 : 0) << 3;
  frameSig |= (uint32_t)(resetOpen    ? 1 : 0) << 4;
  frameSig |= (uint32_t)(inPrompt     ? 1 : 0) << 5;
  frameSig |= (uint32_t)(clocking     ? 1 : 0) << 6;
  frameSig |= (uint32_t)(landscapeClock ? 1 : 0) << 7;
  frameSig |= (uint32_t)(pk ? 1 : 0) << 8;
  frameSig |= (uint32_t)(responseSent ? 1 : 0) << 9;
  frameSig |= (uint32_t)(statsIsNapping() ? 1 : 0) << 10;
  frameSig |= (uint32_t)(screenOff ? 1 : 0) << 11;
  static uint32_t lastFrameSig = 0xFFFFFFFF;
  if (frameSig != lastFrameSig) {
    spr.fillSprite(characterPalette().bg);
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    lastFrameSig = frameSig;
  }

  if (statsIsNapping() || screenOff || landscapeClock) {
    // skip sprite render — napping (idle), powered off, or landscape clock
    // (which draws direct-to-LCD below)
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(8, 90);
      spr.print("installing");
      spr.setCursor(8, 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(8, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(9, 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(8, 100);
      spr.print("no character loaded");
    }
  }
  if (landscapeClock) {
    drawClock();
  } else if (!screenOff) {
    // Special case: when the device is both clocking (1 min idle) and
    // napping (5 min idle), the upper-render skip above was leaving the
    // sprite frozen and drawClock was gated behind !statsIsNapping(), so
    // the screensaver locked at the moment nap kicked in. Always paint +
    // push the clock face while clocking, even mid-nap.
    if (statsIsNapping() && clocking) {
      drawClock();
      spr.pushSprite(0, 0);
    } else if (!statsIsNapping()) {
      if (blePasskey()) drawPasskey();
      else if (clocking) drawClock();
      else if (displayMode == DISP_INFO) drawInfo();
      else if (displayMode == DISP_PET) drawPet();
      else if (displayMode == DISP_SLOT) drawSlot();
      else if (settings().hud) drawHUD();
      if (resetOpen) drawReset();
      else if (settingsOpen) drawSettings();
      else if (menuOpen) drawMenu();
      spr.pushSprite(0, 0);
    }
  }

  // Idle-based nap: no IMU on this board, so the old face-down detector is
  // gone. Instead nap when nothing's happening for IDLE_NAP_MS — reuses
  // lastActiveMs from the clock-mode block above (already tracks sessions,
  // prompts, transcript ticks, button activity). Skipped during approval.
  // Skipped also until first observed activity, so a fresh boot waits for
  // a real signal before the pet starts napping.
  static const uint32_t IDLE_NAP_MS = 5UL * 60 * 1000;
  bool restingNow = !inPrompt
                 && lastActiveMs > 0
                 && (uint32_t)(now - lastActiveMs) >= IDLE_NAP_MS;
  if (!statsIsNapping() && restingNow) {
    statsBeginNap();
    M5.Axp.ScreenBreath(8);
    dimmed = true;
  } else if (statsIsNapping() && !restingNow) {
    statsEndNap();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // No auto-off on USB power — clock face wants to stay visible while charging.
  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    M5.Axp.SetLDO2(false);
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}

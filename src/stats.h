#pragma once
#include <Arduino.h>
#include <Preferences.h>

// Header-only with file-static state: include from exactly one translation
// unit (main.cpp). Including from a second .cpp produces duplicate symbols.

// Persistent stats backed by NVS. Load once at boot; save sparingly
// (NVS sectors have ~100K write cycles). We save on significant events
// only — approval, denial, nap end — never on a timer.

static const uint32_t TOKENS_PER_LEVEL = 50000;

struct Stats {
  uint32_t napSeconds;       // cumulative face-down time
  uint16_t approvals;
  uint16_t denials;
  uint16_t velocity[8];      // ring buffer: seconds-to-respond per approval
  uint8_t  velIdx;
  uint8_t  velCount;
  uint8_t  level;
  uint32_t tokens;          // cumulative output tokens, drives level
  // ESP32-C6 crystal drift in parts per million. Positive = internal
  // clock runs fast relative to NTP truth. Updated by main.cpp's ntpTick
  // after each successful sync that has a previous reference point;
  // smoothed with a 3:1 IIR so single noisy measurements don't whipsaw.
  // Persisted so the device can apply correction immediately at boot
  // before the first re-sync lands. 0 means uncalibrated.
  int32_t  clockDriftPpm;
};

static Stats _stats;
static Preferences _prefs;
static bool _dirty = false;

inline void statsLoad() {
  _prefs.begin("buddy", true);
  _stats.napSeconds = _prefs.getUInt("nap", 0);
  _stats.approvals  = _prefs.getUShort("appr", 0);
  _stats.denials    = _prefs.getUShort("deny", 0);
  _stats.velIdx     = _prefs.getUChar("vidx", 0);
  _stats.velCount   = _prefs.getUChar("vcnt", 0);
  _stats.level      = _prefs.getUChar("lvl", 0);
  _stats.tokens     = _prefs.getUInt("tok", 0);
  _stats.clockDriftPpm = _prefs.getInt("drift", 0);
  size_t got = _prefs.getBytes("vel", _stats.velocity, sizeof(_stats.velocity));
  if (got != sizeof(_stats.velocity)) memset(_stats.velocity, 0, sizeof(_stats.velocity));
  _prefs.end();
  // Level is derived from tokens; if NVS has level set but tokens at 0,
  // backfill so the derivation holds.
  if (_stats.tokens == 0 && _stats.level > 0) {
    _stats.tokens = (uint32_t)_stats.level * TOKENS_PER_LEVEL;
  }
}

inline void statsSave() {
  if (!_dirty) return;
  _prefs.begin("buddy", false);
  _prefs.putUInt("nap", _stats.napSeconds);
  _prefs.putUShort("appr", _stats.approvals);
  _prefs.putUShort("deny", _stats.denials);
  _prefs.putUChar("vidx", _stats.velIdx);
  _prefs.putUChar("vcnt", _stats.velCount);
  _prefs.putUChar("lvl", _stats.level);
  _prefs.putUInt("tok", _stats.tokens);
  _prefs.putInt("drift", _stats.clockDriftPpm);
  _prefs.putBytes("vel", _stats.velocity, sizeof(_stats.velocity));
  _prefs.end();
  _dirty = false;
}

// Transcript activity ring — used as mood proxy when velocity ring is empty
// (bypass-permissions users never produce approvals, so velocity stays 0 and
// mood would otherwise lock at neutral 2). Holds timestamps of the last 30
// lineGen ticks; statsActivityLastHour() filters to the last 60 minutes.
static const uint8_t ACTIVITY_RING_N = 30;
static uint32_t _activityRing[ACTIVITY_RING_N] = {0};
static uint8_t  _activityIdx = 0;

inline void statsOnLineGen() {
  _activityRing[_activityIdx] = millis();
  _activityIdx = (_activityIdx + 1) % ACTIVITY_RING_N;
}

inline uint8_t statsActivityLastHour() {
  uint32_t now = millis();
  uint32_t cutoff = (now > 3600000UL) ? (now - 3600000UL) : 0;
  uint8_t count = 0;
  for (uint8_t i = 0; i < ACTIVITY_RING_N; i++) {
    uint32_t ts = _activityRing[i];
    if (ts != 0 && ts >= cutoff && ts <= now) count++;
  }
  return count;
}

// Level is token-driven now; approvals only feed mood/velocity.
inline void statsOnApproval(uint32_t secondsToRespond) {
  _stats.approvals++;
  // Cast both args to the same uint32_t so g++ on riscv32 doesn't fail to
  // deduce std::min's template — uint32_t and `unsigned` are different
  // canonical types under this toolchain.
  _stats.velocity[_stats.velIdx] =
      (uint16_t)min((uint32_t)secondsToRespond, (uint32_t)65535u);
  _stats.velIdx = (_stats.velIdx + 1) % 8;
  if (_stats.velCount < 8) _stats.velCount++;
  _dirty = true; statsSave();
}

// Tokens feed the pet. 50K per level, 5K per pip on the fed bar.
// Bridge sends cumulative since its start; we add the delta. A drop means
// the bridge restarted — resync without adding, don't lose NVS progress.
static uint32_t _lastBridgeTokens = 0;
static bool _tokensSynced = false;       // first-sight latch — see below
static bool _levelUpPending = false;

inline void statsOnBridgeTokens(uint32_t bridgeTotal) {
  // The bridge sends its cumulative total since IT started. We track deltas.
  // Bridge restart → number drops → resync. But on DEVICE reboot,
  // _lastBridgeTokens is back to 0 while the bridge's total isn't — first
  // packet would re-credit the entire session. Latch on first sight instead.
  if (!_tokensSynced) {
    _lastBridgeTokens = bridgeTotal;
    _tokensSynced = true;
    return;
  }
  if (bridgeTotal < _lastBridgeTokens) {
    _lastBridgeTokens = bridgeTotal;     // bridge restarted
    return;
  }
  uint32_t delta = bridgeTotal - _lastBridgeTokens;
  _lastBridgeTokens = bridgeTotal;
  if (delta == 0) return;

  uint8_t lvlBefore = (uint8_t)(_stats.tokens / TOKENS_PER_LEVEL);
  _stats.tokens += delta;
  uint8_t lvlAfter = (uint8_t)(_stats.tokens / TOKENS_PER_LEVEL);

  // Heartbeats are timer-driven telemetry — don't wear NVS on every delta.
  // Tokens accumulate in RAM, persist only on the milestone. Worst case on
  // hard power-off: lose up to 50K tokens of progress.
  if (lvlAfter > lvlBefore) {
    _stats.level = lvlAfter;
    _levelUpPending = true;
    _dirty = true; statsSave();
  }
}

inline bool statsPollLevelUp() {
  bool r = _levelUpPending;
  _levelUpPending = false;
  return r;
}

inline void statsOnDenial() { _stats.denials++; _dirty = true; statsSave(); }

// Record a fresh drift measurement. First non-zero value is taken raw;
// subsequent measurements feed a 3:1 IIR (75% old, 25% new) so a single
// outlier (network jitter, sync race) doesn't tank the saved value.
inline void statsOnClockDrift(int32_t newPpm) {
  if (_stats.clockDriftPpm == 0) _stats.clockDriftPpm = newPpm;
  else _stats.clockDriftPpm = (_stats.clockDriftPpm * 3 + newPpm) / 4;
  _dirty = true; statsSave();
}

inline void statsMarkDirty() { _dirty = true; }

// Median of the velocity ring buffer. 0 if empty.
inline uint16_t statsMedianVelocity() {
  if (_stats.velCount == 0) return 0;
  uint16_t tmp[8];
  memcpy(tmp, _stats.velocity, sizeof(tmp));
  uint8_t n = _stats.velCount;
  // insertion sort, n ≤ 8
  for (uint8_t i = 1; i < n; i++) {
    uint16_t k = tmp[i]; int8_t j = i - 1;
    while (j >= 0 && tmp[j] > k) { tmp[j+1] = tmp[j]; j--; }
    tmp[j+1] = k;
  }
  return tmp[n/2];
}

// 0..4 tier. Velocity sets the base; heavy denial ratio drags it down.
// Fallback path: when there's no approval data (velCount==0), proxy mood
// off transcript activity in the last hour — keeps the bar alive for
// bypass-permissions users whose velocity ring never fills.
inline uint8_t statsMoodTier() {
  uint16_t vel = statsMedianVelocity();
  int8_t tier;
  if (vel == 0) {
    uint8_t act = statsActivityLastHour();
    if      (act == 0)  tier = 1;
    else if (act < 5)   tier = 2;
    else if (act < 15)  tier = 3;
    else                tier = 4;
  }
  else if (vel < 15) tier = 4;
  else if (vel < 30) tier = 3;
  else if (vel < 60) tier = 2;
  else if (vel < 120) tier = 1;
  else tier = 0;
  uint16_t a = _stats.approvals, d = _stats.denials;
  if (a + d >= 3) {                    // need a few decisions before judging
    if (d > a) tier -= 2;
    else if (d * 2 > a) tier -= 1;     // deny rate > 33%
  }
  if (tier < 0) tier = 0;
  return (uint8_t)tier;
}

// Energy: starts at 3/5 on boot. Awake: drains 1 tier per 2h. Napping:
// refills 1 tier per 1h, capped at 5. Old face-down detector is dead on
// this board (no IMU); main.cpp drives begin/end from idle-detection.
static uint32_t _lastNapEndMs = 0;
static uint8_t  _energyAtNap  = 3;     // baseline at last wake
static bool     _napping      = false;
static uint32_t _napStartMs   = 0;
static uint8_t  _napStartTier = 3;     // tier captured at nap start

inline bool statsIsNapping() { return _napping; }

inline uint8_t statsEnergyTier() {
  if (_napping) {
    uint32_t hoursNapped = (millis() - _napStartMs) / 3600000UL;
    int e = (int)_napStartTier + (int)hoursNapped;
    if (e > 5) e = 5;
    return (uint8_t)e;
  }
  uint32_t hoursSince = (millis() - _lastNapEndMs) / 3600000UL;
  int e = (int)_energyAtNap - (int)(hoursSince / 2);
  if (e < 0) e = 0; if (e > 5) e = 5;
  return (uint8_t)e;
}

inline void statsBeginNap() {
  if (_napping) return;
  _napStartTier = statsEnergyTier();   // capture awake tier as nap start
  _napStartMs   = millis();
  _napping      = true;
}

// Wake from nap: commit accumulated refill to baseline, persist cumulative
// nap seconds. Idempotent — calling without an active nap just resets the
// awake-baseline timer (mirrors the old statsOnWake() semantics).
inline void statsEndNap() {
  uint32_t now = millis();
  if (_napping) {
    uint32_t napSecs = (now - _napStartMs) / 1000;
    uint32_t hoursNapped = napSecs / 3600;
    int e = (int)_napStartTier + (int)hoursNapped;
    if (e > 5) e = 5;
    _energyAtNap = (uint8_t)e;
    _stats.napSeconds += napSecs;
    _dirty = true; statsSave();
    _napping = false;
  }
  _lastNapEndMs = now;
}

inline uint8_t statsFedProgress() {
  return (uint8_t)((_stats.tokens % TOKENS_PER_LEVEL) / (TOKENS_PER_LEVEL / 10));
}

// --- Settings --------------------------------------------------------------

struct Settings {
  bool sound;
  bool bt;
  bool wifi;     // placeholder — no WiFi stack linked yet, just stores the pref
  bool led;
  bool hud;
  uint8_t clockRot;  // 0=auto 1=portrait 2=landscape
  uint8_t bright;    // 0..4 → ScreenBreath 20..100 in applyBrightness()
  // Local UTC offset in seconds. Three independent sources write this:
  // (a) GeoIP autodetect at first boot, (b) desktop bridge time msg (most
  // accurate, overrides everything), (c) NVS persistence across reboots
  // so the device stays localised even without WiFi/desktop. Default 0
  // = UTC if none of the above has fired yet.
  int32_t tzOffsetSec;
};

static Settings _settings = { true, true, false, true, true, 0, 4, 0 };

inline void settingsLoad() {
  _prefs.begin("buddy", true);
  _settings.sound = _prefs.getBool("s_snd", true);
  _settings.bt    = _prefs.getBool("s_bt",  true);
  _settings.wifi  = _prefs.getBool("s_wifi",false);
  _settings.led   = _prefs.getBool("s_led", true);
  _settings.hud      = _prefs.getBool("s_hud", true);
  _settings.clockRot = _prefs.getUChar("s_crot", 0);
  if (_settings.clockRot > 2) _settings.clockRot = 0;
  // Default 4 (max) matches the hardcoded init used before persistence.
  // Devices upgrading from older firmware land on this default; first
  // brightness change writes the key and it sticks from then on.
  _settings.bright = _prefs.getUChar("s_bright", 4);
  if (_settings.bright > 4) _settings.bright = 4;
  _settings.tzOffsetSec = _prefs.getInt("s_tz", 0);
  _prefs.end();
}

inline void settingsSave() {
  _prefs.begin("buddy", false);
  _prefs.putBool("s_snd", _settings.sound);
  _prefs.putBool("s_bt",  _settings.bt);
  _prefs.putBool("s_wifi",_settings.wifi);
  _prefs.putBool("s_led", _settings.led);
  _prefs.putBool("s_hud", _settings.hud);
  _prefs.putUChar("s_crot", _settings.clockRot);
  _prefs.putUChar("s_bright", _settings.bright);
  _prefs.putInt("s_tz", _settings.tzOffsetSec);
  _prefs.end();
}

static char _petName[24] = "Buddy";
static char _ownerName[32] = "";

inline void petNameLoad() {
  _prefs.begin("buddy", true);
  _prefs.getString("petname", _petName, sizeof(_petName));
  _prefs.getString("owner", _ownerName, sizeof(_ownerName));
  _prefs.end();
}

// Strip JSON-breaking chars — these names go into a printf'd JSON string
// unescaped (xfer.h status response). A quote persists to NVS and breaks
// the status endpoint until the name is re-set.
static void _safeCopy(char* dst, size_t dstLen, const char* src) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j < dstLen - 1; i++) {
    char c = src[i];
    if (c != '"' && c != '\\' && c >= 0x20) dst[j++] = c;
  }
  dst[j] = 0;
}

inline void petNameSet(const char* name) {
  _safeCopy(_petName, sizeof(_petName), name);
  _prefs.begin("buddy", false);
  _prefs.putString("petname", _petName);
  _prefs.end();
}

inline const char* petName() { return _petName; }

inline void ownerSet(const char* name) {
  _safeCopy(_ownerName, sizeof(_ownerName), name);
  _prefs.begin("buddy", false);
  _prefs.putString("owner", _ownerName);
  _prefs.end();
}

inline const char* ownerName() { return _ownerName; }

inline uint8_t speciesIdxLoad() {
  _prefs.begin("buddy", true);
  uint8_t v = _prefs.getUChar("species", 0xFF);
  _prefs.end();
  return v;
}

inline void speciesIdxSave(uint8_t idx) {
  _prefs.begin("buddy", false);
  _prefs.putUChar("species", idx);
  _prefs.end();
}

inline Settings& settings() { return _settings; }

inline const Stats& stats() { return _stats; }

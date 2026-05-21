#pragma once
#include <stdint.h>

// GitHub Releases OTA. Polls the configured public repo's "latest" release
// every hour while WiFi is up, compares tag_name vs FIRMWARE_VERSION
// (semver), and downloads firmware.bin via HTTPS + Update class if newer.
// State is kept inside ota_update.cpp so HTTPUpdate/WiFi.h don't leak.
//
// Configured at build time via platformio.ini build_flags:
//   FIRMWARE_VERSION  current version, e.g. "0.1.0"
//   OTA_OWNER         GitHub owner (user or org)
//   OTA_REPO          repo name
//   OTA_ASSET         release asset filename to fetch (e.g. firmware-X.bin)

enum OtaState : uint8_t {
  OTA_IDLE,         // waiting for next scheduled check
  OTA_CHECKING,     // GET /releases/latest in flight
  OTA_DOWNLOADING,  // HTTPUpdate streaming firmware
  OTA_DONE,         // updated, awaiting reboot
  OTA_ERROR,        // last attempt failed, will retry on next cycle
};

void        otaInit();
void        otaTick();
void        otaCheckNow();           // manual trigger via protocol cmd

OtaState    otaState();
const char* otaStateName();
const char* otaCurrentVersion();     // FIRMWARE_VERSION as compiled in
const char* otaRemoteVersion();      // last-seen tag_name; "" before first check
uint8_t     otaProgressPct();        // 0..100 during DOWNLOADING; else 0
const char* otaLastError();          // human-readable last error; "" if none

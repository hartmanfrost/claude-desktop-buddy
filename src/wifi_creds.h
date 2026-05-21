#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <FS.h>

// Persistent WiFi credentials store. Multiple SSID/PSK pairs kept as
// /config/wifi.json on LittleFS. File format:
//   {"nets":[{"ssid":"Home","psk":"abc"},{"ssid":"Work","psk":"def"}]}
//
// Header-only — include only from xfer.h (and indirectly from main.cpp).
// LittleFS lives on a separate partition from the app, so creds survive
// firmware reflash. Wiped via "wifi clear" cmd or by deleting the file.

using fs::File;

static const uint8_t WIFI_MAX_NETS = 8;
static const char* const WIFI_CRED_PATH = "/config/wifi.json";

// Loads into `doc`. Guarantees doc["nets"] is a JsonArray (possibly empty)
// so callers can iterate without null-checking. Bad/missing file → empty.
inline void wifiCredsLoad(JsonDocument& doc) {
  doc.clear();
  // exists() check first — otherwise LittleFS vfs spams ERROR logs on every
  // open of a missing file, and the desktop polls status every ~2s.
  if (LittleFS.exists(WIFI_CRED_PATH)) {
    File f = LittleFS.open(WIFI_CRED_PATH, "r");
    if (f) {
      deserializeJson(doc, f);
      f.close();
    }
  }
  if (!doc["nets"].is<JsonArray>()) {
    doc["nets"].to<JsonArray>();
  }
}

inline bool wifiCredsSave(const JsonDocument& doc) {
  LittleFS.mkdir("/config");
  File f = LittleFS.open(WIFI_CRED_PATH, "w");
  if (!f) return false;
  size_t n = serializeJson(doc, f);
  f.close();
  return n > 0;
}

// Add or update by SSID. Existing entry with same SSID has its PSK
// replaced; otherwise appends. Returns false if SSID missing or cap hit.
inline bool wifiCredsAdd(const char* ssid, const char* psk) {
  if (!ssid || !*ssid) return false;
  JsonDocument doc;
  wifiCredsLoad(doc);
  JsonArray nets = doc["nets"].as<JsonArray>();
  for (JsonVariant v : nets) {
    if (strcmp(v["ssid"] | "", ssid) == 0) {
      v["psk"] = psk ? psk : "";
      return wifiCredsSave(doc);
    }
  }
  if (nets.size() >= WIFI_MAX_NETS) return false;
  JsonObject o = nets.add<JsonObject>();
  o["ssid"] = ssid;
  o["psk"]  = psk ? psk : "";
  return wifiCredsSave(doc);
}

inline bool wifiCredsRemove(const char* ssid) {
  if (!ssid) return false;
  JsonDocument doc;
  wifiCredsLoad(doc);
  JsonArray nets = doc["nets"].as<JsonArray>();
  for (size_t i = 0; i < nets.size(); i++) {
    if (strcmp(nets[i]["ssid"] | "", ssid) == 0) {
      nets.remove(i);
      return wifiCredsSave(doc);
    }
  }
  return false;
}

// Replace entire list from caller-provided array of {ssid,psk} objects.
// Returns number actually stored (capped at WIFI_MAX_NETS, blanks dropped).
inline uint8_t wifiCredsReplaceAll(JsonArrayConst src) {
  JsonDocument out;
  JsonArray nets = out["nets"].to<JsonArray>();
  uint8_t count = 0;
  for (JsonVariantConst v : src) {
    if (count >= WIFI_MAX_NETS) break;
    const char* s = v["ssid"] | "";
    if (!*s) continue;
    JsonObject o = nets.add<JsonObject>();
    o["ssid"] = s;
    o["psk"]  = v["psk"] | "";
    count++;
  }
  wifiCredsSave(out);
  return count;
}

inline bool wifiCredsClear() {
  return LittleFS.remove(WIFI_CRED_PATH);
}

inline uint8_t wifiCredsCount() {
  JsonDocument doc;
  wifiCredsLoad(doc);
  return (uint8_t)doc["nets"].as<JsonArray>().size();
}

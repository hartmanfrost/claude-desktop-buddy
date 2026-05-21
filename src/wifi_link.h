#pragma once
#include <stdint.h>

// Non-blocking WiFi station that uses /config/wifi.json as the credential
// source. On init (and whenever creds change) it kicks off an async scan,
// cross-references visible APs with saved ones, ranks by RSSI, and
// connects to the strongest. Three consecutive auth/timeout failures for
// one SSID push it to the next candidate. When all are exhausted, the
// link backs off for 5 minutes before rescanning.
//
// State is kept inside wifi_link.cpp so WiFi.h doesn't leak into other
// translation units.

enum WLinkState : uint8_t {
  WLINK_OFF,
  WLINK_SCANNING,
  WLINK_CONNECTING,
  WLINK_CONNECTED,
  WLINK_BACKOFF,
};

void        wifiLinkInit();
void        wifiLinkTick();
void        wifiLinkCredsChanged();   // call from cmd:"wifi" handler

WLinkState  wifiLinkState();
const char* wifiLinkStateName();      // for logs / JSON status
const char* wifiLinkSsid();           // current target; "" if none
int32_t     wifiLinkRssi();           // dBm; 0 if not connected
uint32_t    wifiLinkIp();             // host-order IPv4; 0 if not connected

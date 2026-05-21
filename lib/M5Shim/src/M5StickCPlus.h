// M5StickCPlus drop-in shim for the Waveshare ESP32-C6-LCD-1.47.
//
// The display sits on top of LovyanGFX because TFT_eSPI's ESP32 processor
// code does direct GPIO/SPI register writes that the ESP-IDF v5 retyping
// changed under it on RISC-V chips. LovyanGFX has native ESP32-C6 support.
//
// Upstream firmware uses TFT_eSPI / TFT_eSprite types directly — we keep
// those names alive as thin LovyanGFX subclasses, so claude-desktop-buddy's
// `TFT_eSprite spr = TFT_eSprite(&M5.Lcd);` line is still valid.
//
// - M5.Lcd       → LovyanGFX driving the on-board ST7789 172x320 panel
// - M5.Imu       → static "face-up, no shake" — board has no accelerometer
// - M5.Axp       → backlight PWM + USB-only power, no battery / AXP192
// - M5.Rtc       → ESP32-C6 internal time-of-day via <time.h>
// - M5.Beep      → no-op (no buzzer); flashes the RGB LED briefly instead
// - M5.BtnA      → first edge of the BOOT button (next/approve)
// - M5.BtnB      → synthesized double-tap on BOOT (page/scroll/deny)
// - hold A 600ms → opens the menu, same as the original

#pragma once

// Tell LovyanGFX we'll supply our own panel config via LGFX_USE_V1.
#define LGFX_USE_V1
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <math.h>
#include <time.h>
#include <sys/time.h>

// ---------------------------------------------------------------------------
// Color name aliases. Upstream uses GREEN, RED, etc. bare — TFT_eSPI defines
// these via Adafruit_GFX includes. LovyanGFX has the TFT_* prefixed copies
// but not the bare names, so re-add the ones the buddy code touches.
// ---------------------------------------------------------------------------
#ifndef BLACK
#define BLACK   0x0000
#endif
#ifndef WHITE
#define WHITE   0xFFFF
#endif
#ifndef RED
#define RED     0xF800
#endif
#ifndef GREEN
#define GREEN   0x07E0
#endif
#ifndef BLUE
#define BLUE    0x001F
#endif
#ifndef YELLOW
#define YELLOW  0xFFE0
#endif
#ifndef CYAN
#define CYAN    0x07FF
#endif
#ifndef MAGENTA
#define MAGENTA 0xF81F
#endif
#ifndef ORANGE
#define ORANGE  0xFD20
#endif

// ---------------------------------------------------------------------------
// RTC types — match the M5StickCPlus layout so the same field names work.
// ---------------------------------------------------------------------------
struct RTC_TimeTypeDef {
  uint8_t Hours;
  uint8_t Minutes;
  uint8_t Seconds;
};

struct RTC_DateTypeDef {
  uint8_t  WeekDay;   // 0 = Sunday … 6 = Saturday
  uint8_t  Month;     // 1..12
  uint8_t  Date;      // 1..31
  uint16_t Year;
};

// ---------------------------------------------------------------------------
// TFT_eSPI / TFT_eSprite alias layer.
//
// In the original TFT_eSPI library, `class TFT_eSprite : public TFT_eSPI` —
// so a `TFT_eSPI* tgt = &spr` assignment works. LovyanGFX splits the
// hierarchy: LGFX_Device and LGFX_Sprite both descend from LovyanGFX
// independently. Make TFT_eSPI an alias for the LovyanGFX drawing base
// and both ends of the upstream pattern collapse onto the same pointer
// type, no upstream-header surgery needed.
//
// (Trade-off: `class TFT_eSPI;` forward declarations can't refer to an
// alias, so the two headers that used to do that — buddy.h and character.h
// — now include M5StickCPlus.h directly. That's the only upstream-header
// change this port makes.)
// ---------------------------------------------------------------------------
using TFT_eSPI = lgfx::LovyanGFX;

class TFT_eSprite : public lgfx::LGFX_Sprite {
 public:
  explicit TFT_eSprite(TFT_eSPI* parent) : lgfx::LGFX_Sprite(parent) {}
};

// ---------------------------------------------------------------------------
// One virtual button. M5.BtnA is the live BOOT button, M5.BtnB is the
// synthesized double-tap. The single-button decoder lives in update().
// ---------------------------------------------------------------------------
class M5Button {
 public:
  bool isPressed() const { return _held; }
  bool wasPressed() { bool v = _pressEdge; _pressEdge = false; return v; }
  bool wasReleased() { bool v = _releaseEdge; _releaseEdge = false; return v; }
  bool pressedFor(uint32_t ms) const {
    return _held && (millis() - _pressedAt) >= ms;
  }

  // Internal — used by the M5 decoder.
  void _fire_press(uint32_t now) {
    _held = true;
    _pressEdge = true;
    _pressedAt = now;
  }
  void _fire_release() {
    _held = false;
    _releaseEdge = true;
  }
  void _force_release_no_event() {
    _held = false;
  }
  // Synthesize a "currently held for `heldForMs` already" state. Used by
  // the single-button decoder to satisfy main.cpp's pressedFor(600) check
  // on a double-tap gesture, without the user actually holding the button.
  void _synthesize_hold(uint32_t now, uint32_t heldForMs) {
    _held = true;
    _pressedAt = now - heldForMs;
  }

 private:
  bool     _held = false;
  bool     _pressEdge = false;
  bool     _releaseEdge = false;
  uint32_t _pressedAt = 0;
};

// ---------------------------------------------------------------------------
// AXP192 stub. Brightness drives the LCD backlight via LovyanGFX's built-in
// PWM light driver (configured in the panel below). Voltages and battery
// percentage report a steady USB-only state because there's no PMIC.
// ---------------------------------------------------------------------------
class M5Lcd;
class M5Axp {
 public:
  void begin(M5Lcd* lcd);
  // 0..100 in the original M5; map straight to brightness.
  void ScreenBreath(uint8_t v);
  // Backlight on/off proxy. SetLDO2(false) → backlight 0; SetLDO2(true)
  // restores the last brightness.
  void SetLDO2(bool on);

  // Always-on USB, no battery → fixed-ish readings the UI can render.
  float   GetVBusVoltage() const { return 5.0f; }
  float   GetBatVoltage()  const { return 4.20f; }   // shows "full" in UI
  float   GetBatCurrent()  const { return 0.0f; }
  float   GetTempInAXP192() const { return 35.0f; }
  uint8_t GetBtnPress() const { return 0x00; }
  void    PowerOff() { ESP.restart(); }

 private:
  uint8_t _brightness = 60;
  bool    _on = true;
  M5Lcd*  _lcd = nullptr;
};

// ---------------------------------------------------------------------------
// IMU stub. checkShake() / isFaceDown() in main.cpp treat constant input
// as "no motion, face-up" — so shake/nap effectively disable themselves.
// ---------------------------------------------------------------------------
class M5Imu {
 public:
  void Init() {}
  void getAccelData(float* ax, float* ay, float* az) {
    if (ax) *ax = 0.0f;
    if (ay) *ay = 0.0f;
    if (az) *az = 1.0f;
  }
};

// ---------------------------------------------------------------------------
// Beeper stub. The original used a passive piezo on GPIO2; we have a single
// WS2812 RGB instead. The same pixel doubles as the persona-state indicator
// (replacing the M5StickC's red LED on GPIO10) — main.cpp drives it via
// `setLed` every frame, and per-tone flashes overlay briefly on top.
// ---------------------------------------------------------------------------
class M5Beep {
 public:
  void begin();
  void tone(uint16_t freq, uint16_t durMs);
  void update();
  // Drive the on-board WS2812 to a steady color (0 = off). Per-tone
  // flashes take priority for their short duration; this is the
  // "background" the LED returns to once a flash ends.
  void setLed(uint8_t r, uint8_t g, uint8_t b);
};

// ---------------------------------------------------------------------------
// RTC backed by the ESP32-C6 internal clock (settimeofday/gettimeofday).
// ---------------------------------------------------------------------------
class M5Rtc {
 public:
  void GetTime(RTC_TimeTypeDef* t);
  void GetDate(RTC_DateTypeDef* d);
  void SetTime(const RTC_TimeTypeDef* t);
  void SetDate(const RTC_DateTypeDef* d);

 private:
  struct tm _wallclock {};
  bool       _haveDate = false;
  bool       _haveTime = false;
  void _commit();
};

// ---------------------------------------------------------------------------
// The actual display. LovyanGFX panel config goes inline so M5.Lcd is a
// fully-formed device on construction — no board-init boilerplate in
// main.cpp. Inherits from LGFX_Device (not TFT_eSPI, which is the drawing
// base alias) so we get init / setPanel without ambiguity.
// ---------------------------------------------------------------------------
class M5Lcd : public lgfx::LGFX_Device {
 public:
  M5Lcd();
  // Direct access to the embedded LovyanGFX light, so M5Axp can drive PWM
  // without duplicating LEDC plumbing.
  void setBacklight(uint8_t brightness0to255) {
    _light.setBrightness(brightness0to255);
  }

 private:
  lgfx::Panel_ST7789 _panel_st7789;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;
};

// ---------------------------------------------------------------------------
// Top-level M5 singleton. Composition mirrors the upstream namespace.
// ---------------------------------------------------------------------------
class M5StickCPlus {
 public:
  M5Lcd    Lcd;
  M5Axp    Axp;
  M5Imu    Imu;
  M5Beep   Beep;
  M5Rtc    Rtc;
  M5Button BtnA;
  M5Button BtnB;

  void begin();
  void update();
};

extern M5StickCPlus M5;

// ---------------------------------------------------------------------------
// Cyrillic-capable fonts (u8g2 with ASCII + Cyrillic ranges). LovyanGFX
// renders u8g2 fonts directly via the U8g2font adapter. Use with:
//     spr.setFont(&m5CyrillicFont());
// and restore the default with `spr.setFont(&lgfx::fonts::Font0)` afterwards.
//
// Two sizes shipped: 8×13 for prominent text (HUD, menus, info bodies)
// and 6×12 for places that need to fit more lines on screen (ABOUT page).
// ---------------------------------------------------------------------------
const lgfx::U8g2font& m5CyrillicFont();
const lgfx::U8g2font& m5CyrillicFontSmall();

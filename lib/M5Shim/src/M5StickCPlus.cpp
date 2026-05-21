#include "M5StickCPlus.h"
#include "cyrillic_font.h"
#include "cyrillic_font_small.h"
#include <Adafruit_NeoPixel.h>

// Two font sizes shipped — 8x13 reads well at arm's length and is the
// default for HUD/menus/most info pages; 6x12 squeezes more lines per
// page when needed (e.g. the long ABOUT body text).
static const lgfx::U8g2font _cyrillic_font(u8g2_font_8x13_t_cyrillic);
static const lgfx::U8g2font _cyrillic_font_small(u8g2_font_6x12_t_cyrillic);
const lgfx::U8g2font& m5CyrillicFont()      { return _cyrillic_font; }
const lgfx::U8g2font& m5CyrillicFontSmall() { return _cyrillic_font_small; }

#ifndef BOOT_BUTTON_GPIO
#define BOOT_BUTTON_GPIO 9
#endif
#ifndef RGB_LED_GPIO
#define RGB_LED_GPIO 8
#endif
#ifndef LCD_BL_GPIO
#define LCD_BL_GPIO 22
#endif

// Single global, mirrors the upstream library.
M5StickCPlus M5;

// Waveshare's WS2812 on this board is the RGB-order variant — `NEO_GRB`
// makes pure-red commands light up green because the library swaps the
// first two channels in the buffer.
static Adafruit_NeoPixel _rgb(1, RGB_LED_GPIO, NEO_RGB + NEO_KHZ800);

// ---------------------------------------------------------------------------
// Single-button decoder for the BOOT switch (active-low, no external pull).
//
// Tap (<400 ms hold, no follow-up): A.wasReleased — cycles screens, approves.
//   The wasReleased is deferred ~350 ms to leave room for a 2nd tap.
//
// Double-tap: synthesizes A.pressedFor(600) for one main.cpp loop iteration,
//   so the upstream menu code that fires on `pressedFor(600)` opens the menu
//   without the user having to hold the button. (A real hold-to-menu would
//   conflict with the long-press → B mapping below.)
//
// Long press (≥400 ms then release): B.wasPressed — pages/scrolls, denies.
// ---------------------------------------------------------------------------
namespace {

// 5 ms is enough to ride out switch contact bounce on the BOOT button —
// 15 ms was adding noticeable latency to taps and shrinking the effective
// window for back-to-back double-taps.
constexpr uint32_t DEBOUNCE_MS    = 5;
constexpr uint32_t SHORT_TAP_MAX  = 400;   // hold ≥ this → long press → B
constexpr uint32_t DOUBLE_TAP_GAP = 350;   // window for second tap

bool     g_lastRaw       = true;
bool     g_debounced     = true;
uint32_t g_lastEdgeMs    = 0;
uint32_t g_pressStartMs  = 0;

bool     g_pendingTapRelease = false;
uint32_t g_pendingTapAt      = 0;
uint32_t g_lastTapReleaseMs  = 0;

bool     g_secondPressActive = false;

// Crossed SHORT_TAP_MAX while still held → this gesture is committed to
// being a long press. We drop BtnA's _held flag so main.cpp's
// `pressedFor(600)` check can't fire mid-hold and steal the gesture from
// the long-press → B mapping (which would otherwise both fire and the
// menu would open AND a menu item would get confirmed on release).
bool     g_longPressArmed = false;

// One-shot "fake long press" simulation. When a double-tap completes, we
// arm BtnA for a single loop iteration so main.cpp's `pressedFor(600)`
// check fires and opens the menu. Cleared on the very next update().
bool     g_fakeLongPress = false;

} // namespace

// ---------------------------------------------------------------------------
// M5Lcd — LovyanGFX panel config for the Waveshare ESP32-C6-LCD-1.47.
// ---------------------------------------------------------------------------
M5Lcd::M5Lcd() {
  {
    auto cfg = _bus.config();
    cfg.spi_host    = SPI2_HOST;
    cfg.spi_mode    = 0;
    cfg.freq_write  = 40 * 1000 * 1000;
    cfg.freq_read   = 16 * 1000 * 1000;
    cfg.spi_3wire   = false;
    cfg.use_lock    = true;
    cfg.dma_channel = SPI_DMA_CH_AUTO;
    cfg.pin_sclk    = 7;
    cfg.pin_mosi    = 6;
    cfg.pin_miso    = -1;
    cfg.pin_dc      = 15;
    _bus.config(cfg);
    _panel_st7789.setBus(&_bus);
  }
  {
    auto cfg = _panel_st7789.config();
    cfg.pin_cs           = 14;
    cfg.pin_rst          = 21;
    cfg.pin_busy         = -1;
    // ST7789 172x320 variant: the panel sits inside a 240x320 controller
    // window, offset 34px on X. LovyanGFX wants the *visible* dimensions
    // here and folds the offset into memory_width/height + offset_x/y.
    cfg.memory_width     = 240;
    cfg.memory_height    = 320;
    cfg.panel_width      = 172;
    cfg.panel_height     = 320;
    cfg.offset_x         = 34;
    cfg.offset_y         = 0;
    cfg.offset_rotation  = 0;
    cfg.dummy_read_pixel = 8;
    cfg.dummy_read_bits  = 1;
    cfg.readable         = false;
    cfg.invert           = true;
    cfg.rgb_order        = false;
    cfg.dlen_16bit       = false;
    cfg.bus_shared       = false;
    _panel_st7789.config(cfg);
  }
  {
    auto cfg = _light.config();
    cfg.pin_bl      = LCD_BL_GPIO;
    cfg.invert      = false;
    cfg.freq        = 20000;
    cfg.pwm_channel = 0;
    _light.config(cfg);
    _panel_st7789.setLight(&_light);
  }
  setPanel(&_panel_st7789);
}

// ---------------------------------------------------------------------------
// M5Axp — brightness goes through the LovyanGFX light, no LEDC plumbing here.
// ---------------------------------------------------------------------------
void M5Axp::begin(M5Lcd* lcd) {
  _lcd = lcd;
  ScreenBreath(_brightness);
}

void M5Axp::ScreenBreath(uint8_t v) {
  if (v > 100) v = 100;
  _brightness = v;
  if (_on && _lcd) _lcd->setBacklight((uint16_t)v * 255 / 100);
}

void M5Axp::SetLDO2(bool on) {
  _on = on;
  if (!_lcd) return;
  if (on) _lcd->setBacklight((uint16_t)_brightness * 255 / 100);
  else    _lcd->setBacklight(0);
}

// ---------------------------------------------------------------------------
// M5Rtc — composes Date+Time into a single settimeofday() call.
// ---------------------------------------------------------------------------
void M5Rtc::GetTime(RTC_TimeTypeDef* t) {
  if (!t) return;
  time_t now;
  time(&now);
  struct tm lt;
  localtime_r(&now, &lt);
  t->Hours   = lt.tm_hour;
  t->Minutes = lt.tm_min;
  t->Seconds = lt.tm_sec;
}

void M5Rtc::GetDate(RTC_DateTypeDef* d) {
  if (!d) return;
  time_t now;
  time(&now);
  struct tm lt;
  localtime_r(&now, &lt);
  d->WeekDay = lt.tm_wday;
  d->Month   = lt.tm_mon + 1;
  d->Date    = lt.tm_mday;
  d->Year    = lt.tm_year + 1900;
}

void M5Rtc::SetTime(const RTC_TimeTypeDef* t) {
  if (!t) return;
  time_t now;
  time(&now);
  localtime_r(&now, &_wallclock);
  _wallclock.tm_hour = t->Hours;
  _wallclock.tm_min  = t->Minutes;
  _wallclock.tm_sec  = t->Seconds;
  _haveTime = true;
  _commit();
}

void M5Rtc::SetDate(const RTC_DateTypeDef* d) {
  if (!d) return;
  time_t now;
  time(&now);
  localtime_r(&now, &_wallclock);
  _wallclock.tm_year = d->Year - 1900;
  _wallclock.tm_mon  = d->Month - 1;
  _wallclock.tm_mday = d->Date;
  _wallclock.tm_wday = d->WeekDay;
  _haveDate = true;
  _commit();
}

void M5Rtc::_commit() {
  if (!(_haveDate && _haveTime)) return;
  // mktime treats _wallclock as local time. The bridge already adjusted
  // the epoch into local; force UTC for the mktime call so the wallclock
  // round-trips cleanly.
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t t = mktime(&_wallclock);
  struct timeval tv = { t, 0 };
  settimeofday(&tv, nullptr);
}

// ---------------------------------------------------------------------------
// M5Beep — drives the on-board NeoPixel. No real audio; the same pixel is
// shared between the short tone-flash bursts and the steady "attention"
// background color main.cpp uses to replace the M5StickC's red LED.
// ---------------------------------------------------------------------------
namespace {
uint32_t g_beepUntil    = 0;
uint32_t g_attentionRgb = 0;   // background color when no tone is active

void _rgbApply(uint32_t color) {
  _rgb.setPixelColor(0, color);
  _rgb.show();
}
}

void M5Beep::begin() {
  // NeoPixel is already initialized + brightness-set in M5StickCPlus::begin();
  // don't re-set brightness here or we'd undo the boot-time tuning.
  _rgbApply(0);
}

void M5Beep::tone(uint16_t freq, uint16_t durMs) {
  uint32_t c = (freq >= 1800) ? 0x00FF40 : (freq >= 1000 ? 0x40FFA0 : 0xFF2010);
  _rgbApply(c);
  g_beepUntil = millis() + (durMs ? durMs : 60);
}

void M5Beep::update() {
  if (g_beepUntil && (int32_t)(millis() - g_beepUntil) >= 0) {
    g_beepUntil = 0;
    // Restore the steady attention color, not just off — so the pulse
    // main.cpp drives doesn't get briefly erased by every beep.
    _rgbApply(g_attentionRgb);
  }
}

void M5Beep::setLed(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  if (color == g_attentionRgb) return;     // dedup writes — WS2812 refresh
                                           // costs ~30µs and main.cpp calls
                                           // this every frame.
  g_attentionRgb = color;
  if (!g_beepUntil) _rgbApply(color);      // tone in progress wins for now
}

// ---------------------------------------------------------------------------
// M5StickCPlus aggregate
// ---------------------------------------------------------------------------
void M5StickCPlus::begin() {
  Serial.begin(115200);

  // Display init via LovyanGFX. Panel and light are configured in M5Lcd's
  // constructor, so this is just the bus + power-on + first paint.
  Lcd.init();
  Lcd.setRotation(0);
  Lcd.fillScreen(TFT_BLACK);

  Axp.begin(&Lcd);

  // 120/255 is bright enough to read at arm's length in daylight without
  // drawing more than ~5 mA on the WS2812. The persona-state palette in
  // main.cpp uses raw RGB triplets and trusts the global brightness to
  // pull them down.
  _rgb.begin();
  _rgb.setBrightness(120);
  // Smooth HSV self-test so we know the WS2812 wiring is alive even
  // before the buddy reaches an active persona state. ~1.2 s total
  // rainbow sweep + a soft fade-out at the end. uint32_t counter on
  // purpose — uint16_t wraps on `h += 384` past 65280 and the loop
  // becomes infinite, looking like a stuck boot.
  for (uint32_t h = 0; h < 65536; h += 384) {
    // Adafruit NeoPixel's ColorHSV takes a uint16_t hue, gamma32 keeps the
    // perceived hue change linear-looking on a single dim pixel.
    _rgb.setPixelColor(0,
        Adafruit_NeoPixel::gamma32(Adafruit_NeoPixel::ColorHSV((uint16_t)h, 255, 255)));
    _rgb.show();
    delay(6);
  }
  // Fade brightness 255 → 0 to settle the sweep instead of cutting abruptly.
  for (int v = 255; v >= 0; v -= 8) {
    _rgb.setPixelColor(0,
        Adafruit_NeoPixel::gamma32(Adafruit_NeoPixel::ColorHSV(0, 0, (uint8_t)v)));
    _rgb.show();
    delay(8);
  }
  _rgb.setPixelColor(0, 0);
  _rgb.show();

  pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);

  Beep.begin();
}

void M5StickCPlus::update() {
  uint32_t now = millis();

  // Clear any fake long press from the previous iteration — main.cpp had a
  // chance to act on pressedFor(600). Release synthetic BtnA so wasReleased
  // can fire next.
  if (g_fakeLongPress) {
    g_fakeLongPress = false;
    BtnA._fire_release();
  }

  bool raw = digitalRead(BOOT_BUTTON_GPIO);   // HIGH = released
  if (raw != g_lastRaw) {
    g_lastRaw = raw;
    g_lastEdgeMs = now;
  }
  bool stable = (now - g_lastEdgeMs) >= DEBOUNCE_MS;
  if (stable && raw != g_debounced) {
    g_debounced = raw;
    bool nowPressed = (raw == LOW);

    if (nowPressed) {
      g_pressStartMs = now;
      g_longPressArmed = false;
      if (g_pendingTapRelease &&
          (now - g_lastTapReleaseMs) <= DOUBLE_TAP_GAP) {
        // Inside the double-tap window — this is the second press.
        g_secondPressActive = true;
        g_pendingTapRelease = false;
      }
      BtnA._fire_press(now);
    } else {
      uint32_t holdDur = now - g_pressStartMs;

      if (g_secondPressActive) {
        g_secondPressActive = false;
        if (holdDur >= SHORT_TAP_MAX) {
          // The second "tap" was actually held long → treat as long press
          // (no fake-menu, no B — the user just held the second press).
          BtnA._force_release_no_event();
          BtnB._fire_press(now);
          BtnB._fire_release();
        } else {
          // Real double-tap → fake a long press for one loop iteration so
          // main.cpp's pressedFor(600) check opens the menu.
          BtnA._synthesize_hold(now, 700);
          g_fakeLongPress = true;
        }
      } else if (g_longPressArmed) {
        // Long press already fired B at the moment the hold crossed the
        // threshold (see below) — release just resets state, no further
        // event. The user got their LED-flash feedback the instant the
        // action committed, instead of having to release and guess.
        g_longPressArmed = false;
        g_pendingTapRelease = false;
      } else if (holdDur < SHORT_TAP_MAX) {
        // Short tap — defer the release to see if a second tap follows.
        g_pendingTapRelease = true;
        g_pendingTapAt = now + DOUBLE_TAP_GAP;
        g_lastTapReleaseMs = now;
        BtnA._force_release_no_event();
      } else {
        // Hold crossed the threshold mid-debounce (rare) — same as armed.
        BtnA._force_release_no_event();
        BtnB._fire_press(now);
        BtnB._fire_release();
        g_pendingTapRelease = false;
      }
    }
  }

  // Mid-hold long-press detection. Once the user has held BOOT for ≥
  // SHORT_TAP_MAX, commit immediately to the long-press → B mapping:
  //   - drop BtnA's _held flag so main.cpp's pressedFor(600) check can't
  //     fire mid-hold and steal the gesture for the menu;
  //   - fire BtnB right now (not on release) so the user gets feedback
  //     the instant the action commits, instead of having to release and
  //     guess whether they held long enough. main.cpp's beep() then
  //     flashes the RGB LED via the M5Beep stub.
  if (g_debounced == LOW && BtnA.isPressed() && !g_longPressArmed
      && (now - g_pressStartMs) >= SHORT_TAP_MAX) {
    g_longPressArmed = true;
    BtnA._force_release_no_event();
    BtnB._fire_press(now);
    BtnB._fire_release();
  }

  if (g_pendingTapRelease && (int32_t)(now - g_pendingTapAt) >= 0) {
    g_pendingTapRelease = false;
    BtnA._fire_release();
  }

  Beep.update();
}

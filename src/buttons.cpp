#include "buttons.h"
#include "mqtt_config.h"
#include "rhythm_mp3.h"
#include "rhythm_sd_pins.h"
#include <SPI.h>
#include <string.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <driver/gpio.h>
#endif

// SD uses Arduino SPI. CS must be high before SPI.end() or the card stays selected and drives MISO (GPIO 9) low.
static inline void releaseSdSpiBus() {
#if RHYTHM_ENABLE_SD && (RHYTHM_SD_CS_PIN >= 0) && defined(ARDUINO_ARCH_ESP32)
  rhythmSdUnmountIfMounted();
  pinMode(RHYTHM_SD_CS_PIN, OUTPUT);
  digitalWrite(RHYTHM_SD_CS_PIN, HIGH);
  delayMicroseconds(30);
  SPI.end();
#endif
}

// Debounce state indexed by (pin & 0x3F) — must match GPIO numbers used on BTN_PINS.
static uint8_t g_last[64];
static uint32_t g_tchg[64];
static bool g_debounce_inited = false;

static void debounceInitOnce() {
  if (g_debounce_inited)
    return;
  memset(g_last, 1, sizeof g_last); // assume pull-up idle = HIGH
  memset(g_tchg, 0, sizeof g_tchg);
  g_debounce_inited = true;
}

// Remux pin for button read after SPI.end — do **not** overwrite debounce state every frame
// (resetPinToInputPullup syncs g_last from digitalRead, which erases 1→0 edges on SD CS / shared keys).
static void muxPinAsInputPullupOnly(int p) {
#if defined(ARDUINO_ARCH_ESP32)
  gpio_reset_pin((gpio_num_t)p);
#endif
  pinMode(p, INPUT_PULLUP);
#if defined(ARDUINO_ARCH_ESP32)
  gpio_pullup_en((gpio_num_t)p);
  gpio_pulldown_dis((gpio_num_t)p);
#endif
}

static void resetPinToInputPullup(int p, uint32_t t) {
  muxPinAsInputPullupOnly(p);
  int idx = p & 0x3F;
  g_last[idx] = digitalRead(p) ? 1u : 0u;
  g_tchg[idx] = t;
}

void buttonsRestoreInputPullups() {
  debounceInitOnce();
  releaseSdSpiBus();
  uint32_t t = millis();
  for (int i = 0; i < 10; i++) {
    int p = BTN_PINS[i];
#if LILYGO_ETH_BOARD == 2
    // T-ETH-Lite: W5500 uses SPI on GPIO 10/11/12 — reassigning them as GPIO kills Ethernet (and per-loop restore caused boot/WDT loops).
    if (p == 10 || p == 11 || p == 12)
      continue;
#endif
    resetPinToInputPullup(p, t);
  }
}

void buttonsRefreshSdSharedPins() {
#if !RHYTHM_ENABLE_SD || (RHYTHM_SD_CS_PIN < 0)
  return;
#endif
  debounceInitOnce();
  releaseSdSpiBus();
  const int cand[] = {RHYTHM_SD_MISO_PIN, RHYTHM_SD_MOSI_PIN, RHYTHM_SD_CS_PIN};
  for (unsigned k = 0; k < sizeof(cand) / sizeof(cand[0]); k++) {
    int p = cand[k];
#if LILYGO_ETH_BOARD == 2
    if (p == 10 || p == 11 || p == 12)
      continue;
#endif
    bool used = false;
    for (int i = 0; i < 10; i++) {
      if (BTN_PINS[i] == p) {
        used = true;
        break;
      }
    }
    if (!used)
      continue;
    muxPinAsInputPullupOnly(p);
  }
}

#if RHYTHM_ENABLE_SD
// A pin the SD/SPI peripheral is actively driving cannot be read as a button.
// Covers every SD line, not just MISO. BTN_PINS[1] used to be GPIO 12, which is
// also RHYTHM_SD_CS_PIN; CS toggles LOW on every SD transaction, so sampling it
// while SD was mounted read chip-select traffic as phantom presses on the left
// blue key and fired the relay and blue DMX at random during songs. That key now
// lives on GPIO 42, so no key overlaps SD any more and this guard is defensive —
// keep it, so the next pin reassignment cannot reintroduce the same fault.
static inline bool pinOwnedBySd(int pin) {
  if (!rhythmMp3SdMounted())
    return false;
#if RHYTHM_SD_MISO_PIN >= 0
  if (pin == RHYTHM_SD_MISO_PIN) return true;
#endif
#if RHYTHM_SD_MOSI_PIN >= 0
  if (pin == RHYTHM_SD_MOSI_PIN) return true;
#endif
#if RHYTHM_SD_SCK_PIN >= 0
  if (pin == RHYTHM_SD_SCK_PIN) return true;
#endif
#if RHYTHM_SD_CS_PIN >= 0
  if (pin == RHYTHM_SD_CS_PIN) return true;
#endif
  return false;
}
#endif

bool readLevelDebounced(int pin, bool &edgeDown, bool &edgeUp) {
  debounceInitOnce();
  int idx = pin & 0x3F;
#if RHYTHM_ENABLE_SD
  if (pinOwnedBySd(pin)) {
    g_last[idx] = 1u; // parked released — never report an edge from SPI traffic
    g_tchg[idx] = millis();
    edgeDown = edgeUp = false;
    return false;
  }
#endif
  uint8_t v = digitalRead(pin) ? 1u : 0u;
  edgeDown = edgeUp = false;
  if (v != g_last[idx]) {
    if (millis() - g_tchg[idx] > 35) {
      g_tchg[idx] = millis();
      uint8_t prev = g_last[idx];
      g_last[idx] = v;
      if (prev == 1 && v == 0)
        edgeDown = true;
      if (prev == 0 && v == 1)
        edgeUp = true;
    }
  } else
    g_tchg[idx] = millis();
  return (v == 0);
}

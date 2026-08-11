#include "relay_output.h"
#include "relay_config.h"

#include <driver/gpio.h>

#if RELAY_ACTIVE_LOW
#define RELAY_LEVEL_ACTIVE 0
#define RELAY_LEVEL_IDLE 1
#else
#define RELAY_LEVEL_ACTIVE 1
#define RELAY_LEVEL_IDLE 0
#endif

// Open drain only makes sense when idle is the un-driven state (active low).
// INPUT_OUTPUT (not plain OUTPUT) so digitalRead() returns the real pad level —
// that is what distinguishes "driven high but loaded down" from "never driven".
#if RELAY_OPEN_DRAIN && RELAY_ACTIVE_LOW
#define RELAY_GPIO_MODE GPIO_MODE_INPUT_OUTPUT_OD
#define RELAY_MODE_NAME "open-drain"
#else
#define RELAY_GPIO_MODE GPIO_MODE_INPUT_OUTPUT
#define RELAY_MODE_NAME "push-pull"
#endif

static uint8_t s_heldMask = 0;
static bool s_active = false;
static bool s_pinReady = false;

static void relayDrive(bool active) {
  gpio_set_level((gpio_num_t)RELAY_PIN, active ? RELAY_LEVEL_ACTIVE : RELAY_LEVEL_IDLE);
  s_active = active;
}

static void relayApply() {
  const bool want = s_heldMask != 0;
  if (want == s_active)
    return;
  relayDrive(want);
  Serial.printf("[RLY] %s  held=0x%X\n", want ? "CLOSED" : "OPEN", (unsigned)s_heldMask);
}

void relayOutputEarlyInit() {
  gpio_reset_pin((gpio_num_t)RELAY_PIN);
  // Latch the idle level BEFORE the pin becomes an output — otherwise an
  // active-low relay gets a low pulse wide enough to click on every reset.
  gpio_set_level((gpio_num_t)RELAY_PIN, RELAY_LEVEL_IDLE);
  gpio_set_direction((gpio_num_t)RELAY_PIN, RELAY_GPIO_MODE);
  gpio_pullup_dis((gpio_num_t)RELAY_PIN);
  gpio_pulldown_dis((gpio_num_t)RELAY_PIN);
  gpio_set_level((gpio_num_t)RELAY_PIN, RELAY_LEVEL_IDLE);

  s_heldMask = 0;
  s_active = false;
  s_pinReady = true;
}

bool relayOutputBegin() {
  relayOutputEarlyInit();
  Serial.printf("[RLY] GPIO%d %s  active=%s  boot=OPEN  fires on any color hold\n", (int)RELAY_PIN,
                RELAY_MODE_NAME, RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
  return true;
}

void relayOutputSetColor(ActuatorColor color, bool on) {
  if (color >= ACTUATOR_COLOR_COUNT)
    return;
  if (!s_pinReady)
    relayOutputEarlyInit();

  const uint8_t bit = (uint8_t)(1u << (uint8_t)color);
  if (on)
    s_heldMask |= bit;
  else
    s_heldMask &= (uint8_t)~bit;

  relayApply();
}

void relayOutputAllOff() {
  if (!s_pinReady) {
    relayOutputEarlyInit();
    return;
  }
  s_heldMask = 0;
  relayApply();
}

bool relayOutputActive() { return s_active; }

uint8_t relayOutputHeldMask() { return s_heldMask; }

void relayOutputSelfTest() {
#if RELAY_SELFTEST_CYCLES > 0
  if (!s_pinReady)
    relayOutputEarlyInit();

  Serial.printf("[RLY] SELFTEST GPIO%d %s active=%s — %d cycles, %dms each.\n", (int)RELAY_PIN, RELAY_MODE_NAME,
                RELAY_ACTIVE_LOW ? "LOW" : "HIGH", (int)RELAY_SELFTEST_CYCLES, (int)RELAY_SELFTEST_MS);
  Serial.println("[RLY] SELFTEST nothing else is running — no ESP-NOW, no solenoids, no DMX.");

  for (int i = 1; i <= (int)RELAY_SELFTEST_CYCLES; i++) {
    relayDrive(true);
    delay((uint32_t)RELAY_SELFTEST_MS);
    const int padOn = digitalRead(RELAY_PIN);

    relayDrive(false);
    delay((uint32_t)RELAY_SELFTEST_MS);
    const int padOff = digitalRead(RELAY_PIN);

    Serial.printf("[RLY] SELFTEST %d/%d  close: cmd=%d pad=%d  |  open: cmd=%d pad=%d  %s\n", i,
                  (int)RELAY_SELFTEST_CYCLES, RELAY_LEVEL_ACTIVE, padOn, RELAY_LEVEL_IDLE, padOff,
                  (padOn == RELAY_LEVEL_ACTIVE && padOff == RELAY_LEVEL_IDLE) ? "pad OK" : "PAD MISMATCH");
  }

  s_heldMask = 0;
  relayDrive(false);
  Serial.println("[RLY] SELFTEST done — relay released. Did you hear/feel 3 clicks?");
#endif
}

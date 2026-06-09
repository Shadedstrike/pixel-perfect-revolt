#include "dmx_output.h"
#include "dmx_config.h"

#include <esp_dmx.h>

static const dmx_port_t kDmxPort = DMX_NUM_1;

static bool s_ready = false;
static bool s_dirty = false;
static uint8_t s_frame[DMX_SLOT_COUNT];
static uint32_t s_lastSendMs = 0;

static uint16_t parSlot(uint8_t parOffset) {
  return (uint16_t)(DMX_PAR_START_ADDR + parOffset);
}

bool dmxOutputBegin() {
  dmx_config_t config = DMX_CONFIG_DEFAULT;
  const int personalityCount = 1;
  dmx_personality_t personalities[] = {{1, "Pyrrisma DMX"}};

  if (!dmx_driver_install(kDmxPort, &config, personalities, personalityCount)) {
    Serial.println("[DMX] driver_install failed");
    return false;
  }
  if (!dmx_set_pin(kDmxPort, DMX_TX_PIN, DMX_RX_PIN, DMX_RTS_PIN)) {
    Serial.println("[DMX] set_pin failed");
    return false;
  }

  memset(s_frame, 0, sizeof(s_frame));
  s_frame[0] = 0; // null start code
  s_ready = true;
  s_dirty = true;

  Serial.printf("[DMX] TX=%d RX=%d RTS=%d  par@%u (6ch)  bubble@%u  slots=%u @ %uHz\n", (int)DMX_TX_PIN,
                (int)DMX_RX_PIN, (int)DMX_RTS_PIN, (unsigned)DMX_PAR_START_ADDR, (unsigned)DMX_BUBBLE_ADDR,
                (unsigned)DMX_SLOT_COUNT, (unsigned)DMX_REFRESH_HZ);
  return true;
}

bool dmxOutputReady() { return s_ready; }

void dmxOutputSetSlot(uint16_t slot, uint8_t level) {
  if (!s_ready || slot >= DMX_SLOT_COUNT)
    return;
  if (s_frame[slot] == level)
    return;
  s_frame[slot] = level;
  s_dirty = true;
}

uint8_t dmxOutputGetSlot(uint16_t slot) {
  if (slot >= DMX_SLOT_COUNT)
    return 0;
  return s_frame[slot];
}

void dmxOutputClearPar() {
  if (!s_ready)
    return;
  for (uint8_t i = 0; i < 6; i++)
    dmxOutputSetSlot(parSlot(i), 0);
}

void dmxOutputSetBubble(bool on) {
  dmxOutputSetSlot((uint16_t)DMX_BUBBLE_ADDR, on ? (uint8_t)DMX_BUBBLE_ON_LEVEL : 0);
}

static void dmxOutputSend() {
  dmx_write(kDmxPort, s_frame, DMX_SLOT_COUNT);
  dmx_send(kDmxPort);
  dmx_wait_sent(kDmxPort, DMX_TIMEOUT_TICK);
  s_dirty = false;
  s_lastSendMs = millis();
}

void dmxOutputService(uint32_t nowMs) {
  if (!s_ready)
    return;

  const uint32_t intervalMs = 1000u / (uint32_t)DMX_REFRESH_HZ;
  if (!s_dirty && (nowMs - s_lastSendMs) < intervalMs)
    return;

  dmxOutputSend();
}

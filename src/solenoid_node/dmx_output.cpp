#include "dmx_output.h"
#include "dmx_config.h"
#include "actuator_config.h"
#include "actuator_protocol.h"

#include <Arduino.h>
#include <cstring>
#include <esp_dmx.h>
#include <math.h>

static const dmx_port_t kDmxPort = DMX_NUM_1;

static bool s_ready = false;
static bool s_dirty = false;
static uint8_t s_frame[DMX_SLOT_COUNT];
static uint32_t s_lastSendMs = 0;
static uint32_t s_bubbleFanEndMs = 0;
static uint32_t s_bubbleFanStartMs = 0;
static bool s_holdActive[ACTUATOR_COLOR_COUNT] = {};
static bool s_bubbleLatched[ACTUATOR_COLOR_COUNT] = {};
static uint8_t s_liveR = 0;
static uint8_t s_liveG = 0;
static uint8_t s_liveB = 0;
static uint8_t s_liveAmber = 0;
static bool s_idleActive = false;
static uint8_t s_idleR = 0;
static uint8_t s_idleG = 0;
static uint8_t s_idleB = 0;
static uint8_t s_idleW = 0;
static uint8_t s_idleAmber = 0;

static uint16_t parSlotAt(uint16_t addr, uint8_t parOffset) {
  return (uint16_t)(addr + parOffset);
}

static uint16_t bubbleSlot(uint8_t bubbleOffset) {
  return (uint16_t)(DMX_BUBBLE_ADDR + bubbleOffset);
}

static void dmxOutputPushFrame() {
  if (!s_ready)
    return;
  dmx_write(kDmxPort, s_frame, DMX_SLOT_COUNT);
  dmx_send(kDmxPort);
  dmx_wait_sent(kDmxPort, DMX_TIMEOUT_TICK);
  s_dirty = false;
  s_lastSendMs = millis();
}

static void dmxOutputSendNow() { dmxOutputPushFrame(); }

static void dmxOutputSetParAtAddr(uint16_t addr, uint8_t channelCount, uint8_t r, uint8_t g, uint8_t b, uint8_t w,
                                  uint8_t amber) {
  if (!s_ready)
    return;
  dmxOutputSetSlot(parSlotAt(addr, DMX_PAR_CH_RED), r);
  dmxOutputSetSlot(parSlotAt(addr, DMX_PAR_CH_GREEN), g);
  dmxOutputSetSlot(parSlotAt(addr, DMX_PAR_CH_BLUE), b);
  dmxOutputSetSlot(parSlotAt(addr, DMX_PAR_CH_WHITE), w);
  dmxOutputSetSlot(parSlotAt(addr, DMX_PAR_CH_AMBER), amber);
  dmxOutputSetSlot(parSlotAt(addr, DMX_PAR_CH_UV), 0);
  for (uint8_t ch = DMX_PAR_CHANNEL_COUNT; ch < channelCount; ch++)
    dmxOutputSetSlot(parSlotAt(addr, ch), 0);
}

static void dmxOutputSetPar(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber) {
  dmxOutputSetParAtAddr(DMX_PAR_START_ADDR, DMX_PAR_CHANNEL_COUNT, r, g, b, w, amber);
}

static void dmxOutputLevelsFromHolds(const bool active[ACTUATOR_COLOR_COUNT], uint8_t *r, uint8_t *g, uint8_t *b,
                                     uint8_t *amber) {
  *r = active[ACTUATOR_COLOR_RED] ? (uint8_t)DMX_LEVEL_FULL : 0;
  *g = active[ACTUATOR_COLOR_GREEN] ? (uint8_t)DMX_LEVEL_FULL : 0;
  *b = active[ACTUATOR_COLOR_BLUE] ? (uint8_t)DMX_LEVEL_FULL : 0;
  *amber = active[ACTUATOR_COLOR_YELLOW] ? (uint8_t)DMX_LEVEL_FULL : 0;
}

static uint8_t dmxOutputBubbleFanLevel(uint32_t nowMs) {
  if (s_bubbleFanEndMs == 0 || (int32_t)(nowMs - s_bubbleFanEndMs) >= 0)
    return 0;

  const uint8_t fanMin = (uint8_t)DMX_BUBBLE_FAN_MIN;
  const uint8_t fanMax = (uint8_t)DMX_BUBBLE_FAN_MAX;
  const uint32_t elapsed = nowMs - s_bubbleFanStartMs;
  const uint32_t remaining = s_bubbleFanEndMs - nowMs;

  uint8_t level = fanMax;
  if (elapsed < (uint32_t)BUBBLE_FAN_RAMP_UP_MS) {
    const float t = (float)elapsed / (float)BUBBLE_FAN_RAMP_UP_MS;
    level = (uint8_t)lroundf((float)fanMin + (float)(fanMax - fanMin) * t);
  }

  if (remaining < (uint32_t)BUBBLE_FAN_RAMP_DOWN_MS) {
    const float t = (float)remaining / (float)BUBBLE_FAN_RAMP_DOWN_MS;
    const uint8_t rampDown = (uint8_t)lroundf((float)fanMin + (float)(fanMax - fanMin) * t);
    if (rampDown < level)
      level = rampDown;
  }

  if (level < fanMin)
    level = fanMin;
  return level;
}

static void dmxOutputApplyBubbleLeds(uint32_t nowMs, uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber) {
  uint8_t bubbleR = r;
  uint8_t bubbleG = g;
  uint8_t bubbleB = b;
  if (amber) {
    bubbleR = (uint8_t)min(255, (int)bubbleR + (int)amber);
    bubbleG = (uint8_t)min(255, (int)bubbleG + (int)amber);
  }
  const uint8_t fan = dmxOutputBubbleFanLevel(nowMs);
  dmxOutputSetBubbleLevels(fan, 0, w, bubbleB, bubbleG, bubbleR);
}

static bool dmxOutputAnyLiveLevels() {
  return s_liveR || s_liveG || s_liveB || s_liveAmber;
}

static void dmxOutputIdlePattern(uint32_t nowMs, uint32_t phaseMs, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *amber) {
  const float t = (nowMs + phaseMs) / 1000.0f;
  const float beat = 0.60f + 0.40f * sinf(2.0f * (float)M_PI * 0.65f * t);
  const float pr = sinf(2.0f * (float)M_PI * (0.38f * t));
  const float pg = sinf(2.0f * (float)M_PI * (0.38f * t) + 2.094395f);
  const float pb = sinf(2.0f * (float)M_PI * (0.38f * t) + 4.188790f);
  const float pa = sinf(2.0f * (float)M_PI * 0.27f * t);
  *r = (uint8_t)lroundf((0.45f + 0.55f * (0.5f + 0.5f * pr)) * beat * 255.0f);
  *g = (uint8_t)lroundf((0.45f + 0.55f * (0.5f + 0.5f * pg)) * beat * 255.0f);
  *b = (uint8_t)lroundf((0.45f + 0.55f * (0.5f + 0.5f * pb)) * beat * 255.0f);
  *amber = (uint8_t)lroundf((0.35f + 0.65f * (0.5f + 0.5f * pa)) * 240.0f);
}

static void dmxOutputRefreshOutputs(uint32_t nowMs) {
  if (!s_ready)
    return;

  if (s_idleActive) {
    uint8_t parR = 0;
    uint8_t parG = 0;
    uint8_t parB = 0;
    uint8_t parAmber = 0;
    uint8_t bubbleR = 0;
    uint8_t bubbleG = 0;
    uint8_t bubbleB = 0;
    uint8_t bubbleAmber = 0;
    dmxOutputIdlePattern(nowMs, 0, &parR, &parG, &parB, &parAmber);
    dmxOutputIdlePattern(nowMs, 550, &bubbleR, &bubbleG, &bubbleB, &bubbleAmber);
    dmxOutputSetPar(parR, parG, parB, 0, parAmber);
    dmxOutputApplyBubbleLeds(nowMs, bubbleR, bubbleG, bubbleB, 0, bubbleAmber);
    return;
  }

  if (dmxOutputAnyLiveLevels()) {
    dmxOutputSetPar(s_liveR, s_liveG, s_liveB, 0, s_liveAmber);
    dmxOutputApplyBubbleLeds(nowMs, s_liveR, s_liveG, s_liveB, 0, s_liveAmber);
    return;
  }

  dmxOutputSetPar(0, 0, 0, 0, 0);

  uint8_t bubbleR = 0;
  uint8_t bubbleG = 0;
  uint8_t bubbleB = 0;
  uint8_t bubbleAmber = 0;
  dmxOutputLevelsFromHolds(s_bubbleLatched, &bubbleR, &bubbleG, &bubbleB, &bubbleAmber);
  dmxOutputApplyBubbleLeds(nowMs, bubbleR, bubbleG, bubbleB, 0, bubbleAmber);
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
  s_frame[0] = 0;
  s_ready = true;
  s_dirty = true;

  Serial.printf("[DMX] TX=%d RX=%d RTS=%d  par@%u  bubble@%u (6ch)  slots=%u @ %uHz\n", (int)DMX_TX_PIN,
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

void dmxOutputSetParLevels(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber) {
  dmxOutputSetPar(r, g, b, w, amber);
}

void dmxOutputClearPar() { dmxOutputSetPar(0, 0, 0, 0, 0); }

void dmxOutputSetBubbleLevels(uint8_t fan, uint8_t macro, uint8_t w, uint8_t b, uint8_t g, uint8_t r) {
  if (!s_ready)
    return;
  dmxOutputSetSlot(bubbleSlot(DMX_BUBBLE_CH_FAN), fan);
  dmxOutputSetSlot(bubbleSlot(DMX_BUBBLE_CH_MACRO), macro);
  dmxOutputSetSlot(bubbleSlot(DMX_BUBBLE_CH_WHITE), w);
  dmxOutputSetSlot(bubbleSlot(DMX_BUBBLE_CH_GREEN), g);
  dmxOutputSetSlot(bubbleSlot(DMX_BUBBLE_CH_BLUE), r);
  dmxOutputSetSlot(bubbleSlot(DMX_BUBBLE_CH_RED), b);
}

void dmxOutputClearBubble() { dmxOutputSetBubbleLevels(0, 0, 0, 0, 0, 0); }

void dmxOutputRefreshColorHolds() { dmxOutputRefreshOutputs(millis()); }

void dmxOutputSetColorHold(ActuatorColor color, bool on) {
  if (!s_ready || color >= ACTUATOR_COLOR_COUNT || s_idleActive)
    return;

  s_holdActive[color] = on;
  if (on) {
    s_bubbleLatched[color] = true;
    const uint8_t full = (uint8_t)DMX_LEVEL_FULL;
    switch (color) {
      case ACTUATOR_COLOR_RED:
        s_liveR = full;
        break;
      case ACTUATOR_COLOR_GREEN:
        s_liveG = full;
        break;
      case ACTUATOR_COLOR_BLUE:
        s_liveB = full;
        break;
      case ACTUATOR_COLOR_YELLOW:
        s_liveAmber = full;
        break;
      default:
        break;
    }
  } else {
    switch (color) {
      case ACTUATOR_COLOR_RED:
        s_liveR = 0;
        break;
      case ACTUATOR_COLOR_GREEN:
        s_liveG = 0;
        break;
      case ACTUATOR_COLOR_BLUE:
        s_liveB = 0;
        break;
      case ACTUATOR_COLOR_YELLOW:
        s_liveAmber = 0;
        break;
      default:
        break;
    }
  }

  dmxOutputRefreshOutputs(millis());
  dmxOutputSendNow();
}

void dmxOutputSetCombinedRgbHold(uint8_t r, uint8_t g, uint8_t b, uint8_t amber) {
  if (!s_ready || s_idleActive)
    return;

  s_liveR = r;
  s_liveG = g;
  s_liveB = b;
  s_liveAmber = amber;

  const bool nextActive[ACTUATOR_COLOR_COUNT] = {
      r > 0,
      g > 0,
      b > 0,
      amber > 0,
  };

  for (int i = 0; i < ACTUATOR_COLOR_COUNT; i++) {
    if (nextActive[i])
      s_bubbleLatched[i] = true;
    s_holdActive[i] = nextActive[i];
  }

  dmxOutputRefreshOutputs(millis());
  dmxOutputSendNow();
}

bool dmxOutputBubbleFanActive(uint32_t nowMs) {
  return s_bubbleFanEndMs != 0 && (int32_t)(nowMs - s_bubbleFanEndMs) < 0;
}

bool dmxOutputBubblePartyActive(uint32_t nowMs) { return dmxOutputBubbleFanActive(nowMs); }

void dmxOutputExtendBubbleParty(uint32_t nowMs, uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber) {
  (void)r;
  (void)g;
  (void)b;
  (void)w;
  (void)amber;

  s_bubbleFanStartMs = nowMs;
  s_bubbleFanEndMs = nowMs + (uint32_t)BUBBLE_PARTY_MS;

  dmxOutputRefreshOutputs(nowMs);
  dmxOutputSendNow();

  Serial.printf("[DMX] bubble fan START %ums  ramp %u->%u over %ums\n", (unsigned)BUBBLE_PARTY_MS,
                (unsigned)DMX_BUBBLE_FAN_MIN, (unsigned)DMX_BUBBLE_FAN_MAX, (unsigned)BUBBLE_FAN_RAMP_UP_MS);
}

void dmxOutputKillBubbleParty() {
  if (!s_bubbleFanEndMs)
    return;
  s_bubbleFanEndMs = 0;
  s_bubbleFanStartMs = 0;
  dmxOutputRefreshOutputs(millis());
  dmxOutputSendNow();
  Serial.println("[DMX] bubble fan KILL (idle)");
}

void dmxOutputSetIdleLevels(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber) {
  (void)r;
  (void)g;
  (void)b;
  (void)w;
  (void)amber;

  s_idleActive = true;
  s_liveR = s_liveG = s_liveB = s_liveAmber = 0;
  dmxOutputRefreshOutputs(millis());
  dmxOutputSendNow();
}

void dmxOutputClearIdle() {
  if (!s_idleActive)
    return;
  s_idleActive = false;
  s_idleR = s_idleG = s_idleB = s_idleW = s_idleAmber = 0;
  s_liveR = s_liveG = s_liveB = s_liveAmber = 0;
  dmxOutputRefreshOutputs(millis());
  dmxOutputSendNow();
  Serial.println("[DMX] idle override OFF");
}

void dmxOutputServiceBubbleParty(uint32_t nowMs) {
  if (!s_bubbleFanEndMs)
    return;

  if ((int32_t)(nowMs - s_bubbleFanEndMs) >= 0) {
    s_bubbleFanEndMs = 0;
    s_bubbleFanStartMs = 0;
    Serial.println("[DMX] bubble fan OFF");
  }
}

void dmxOutputService(uint32_t nowMs) {
  if (!s_ready)
    return;

  dmxOutputServiceBubbleParty(nowMs);

  const uint32_t intervalMs = 1000u / (uint32_t)DMX_REFRESH_HZ;
  if (nowMs - s_lastSendMs < intervalMs)
    return;

  dmxOutputRefreshOutputs(nowMs);
  dmxOutputPushFrame();
}

void dmxOutputForceSafeOutputs() {
  if (!s_ready)
    return;

  s_bubbleFanEndMs = 0;
  s_bubbleFanStartMs = 0;
  s_idleActive = false;
  s_liveR = s_liveG = s_liveB = s_liveAmber = 0;
  memset(s_holdActive, 0, sizeof(s_holdActive));

  dmxOutputRefreshOutputs(millis());
  dmxOutputPushFrame();
}

void dmxOutputBootSafeState() {
  if (!s_ready)
    return;

  s_bubbleFanEndMs = 0;
  s_bubbleFanStartMs = 0;
  s_idleActive = false;
  s_liveR = s_liveG = s_liveB = s_liveAmber = 0;
  memset(s_holdActive, 0, sizeof(s_holdActive));
  memset(s_bubbleLatched, 0, sizeof(s_bubbleLatched));
  memset(s_frame, 0, sizeof(s_frame));
  s_frame[0] = 0;
  s_dirty = true;

  Serial.println("[DMX] boot safe — fan off, burst zero frames");
  for (int i = 0; i < 8; i++) {
    dmxOutputRefreshOutputs(millis());
    dmxOutputPushFrame();
    delay(5);
  }
}

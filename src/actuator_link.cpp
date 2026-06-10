#include "actuator_link.h"
#include "actuator_config.h"
#include "actuator_protocol.h"
#include "espnow_actuator.h"
#include "serial_status.h"

#include <WiFi.h>
#include <cstring>

static uint32_t s_txCount = 0;

static const char *colorSlotName(int slot) {
  switch (slot) {
    case 0:
      return "yellow";
    case 1:
      return "blue";
    case 2:
      return "green";
    case 3:
      return "red";
    default:
      return nullptr;
  }
}

static ActuatorColor colorSlotToEnum(int slot) {
  switch (slot) {
    case 0:
      return ACTUATOR_COLOR_YELLOW;
    case 1:
      return ACTUATOR_COLOR_BLUE;
    case 2:
      return ACTUATOR_COLOR_GREEN;
    case 3:
      return ACTUATOR_COLOR_RED;
    default:
      return ACTUATOR_COLOR_COUNT;
  }
}

static bool sideColorPhysicallyDown(const bool down[10], int slot) {
  switch (slot) {
    case 0:
      return down[0] || down[6];
    case 1:
      return down[1] || down[7];
    case 2:
      return down[2] || down[8];
    case 3:
      return down[3] || down[9];
    default:
      return false;
  }
}

static bool s_colorRemoteOn[4] = {};

static bool actuatorPublishColor(ActuatorColor color, bool on) {
  if (color >= ACTUATOR_COLOR_COUNT)
    return false;
  if (!espnowActuatorReady())
    return false;

  const ActuatorCmdPacket pkt = espnowActuatorMakePacket(color, on);
  const bool ok = espnowActuatorSend(&pkt);
  if (!ok) {
    Serial.printf("[ESPNOW] tx FAIL color=%u on=%d seq=%u\n", (unsigned)color, on ? 1 : 0, (unsigned)pkt.seq);
  } else {
    ++s_txCount;
    Serial.printf("[ESPNOW] tx color=%s on=%d seq=%u total_tx=%u\n", serialStatusColorName(color), on ? 1 : 0,
                  (unsigned)pkt.seq, (unsigned)s_txCount);
  }
  return ok;
}

void actuatorLinkSyncSideColumnHolds(const bool down[10]) {
  for (int slot = 0; slot < 4; slot++) {
    const bool phys = sideColorPhysicallyDown(down, slot);
    const bool remote = s_colorRemoteOn[slot];
    if (phys == remote)
      continue;

    const ActuatorColor color = colorSlotToEnum(slot);
    if (color >= ACTUATOR_COLOR_COUNT)
      continue;

    s_colorRemoteOn[slot] = phys;
    Serial.printf("[ACT] sync %s -> %s\n", colorSlotName(slot), phys ? "ON" : "OFF");
    actuatorPublishColor(color, phys);
  }
}

static void actuatorHeartbeat() {
  static uint32_t lastHb = 0;
  const uint32_t now = millis();
  if (now - lastHb < (uint32_t)ACTUATOR_HB_MS)
    return;
  lastHb = now;

  int activeHolds = 0;
  for (int i = 0; i < 4; i++) {
    if (s_colorRemoteOn[i])
      ++activeHolds;
  }

  Serial.printf("[HB] role=CONTROLLER  ESPNOW=%s  ch=%u  mac=%s  tx=%u  active_holds=%d  uptime=%lus\n",
                espnowActuatorReady() ? "OK" : "DOWN", (unsigned)ESPNOW_WIFI_CHANNEL, WiFi.macAddress().c_str(),
                (unsigned)s_txCount, activeHolds, (unsigned long)(now / 1000));
}

bool actuatorLinkSetup() {
  Serial.println("[BOOT] Actuator link: side-column hold -> ESP-NOW -> remote solenoids + motors + DMX.");
  Serial.printf("[BOOT] ESP-NOW channel=%u  bubble: any btn %ums hold -> +%ums fan (stackable)\n",
                (unsigned)ESPNOW_WIFI_CHANNEL, (unsigned)BUBBLE_HOLD_TRIGGER_MS, (unsigned)BUBBLE_PARTY_MS);
  Serial.printf("[BOOT] synth idle: kill bubble fan + mirror LED idx %u to DMX @ %uHz\n",
                (unsigned)ACTUATOR_IDLE_MIRROR_LED_IDX, (unsigned)(1000u / ACTUATOR_IDLE_DMX_MS));
  const bool ok = espnowActuatorBeginTx();
  Serial.printf("[BOOT] ESP-NOW TX %s  mac=%s\n", ok ? "ready" : "FAILED", WiFi.macAddress().c_str());
  return ok;
}

void actuatorLinkLoop() { actuatorHeartbeat(); }

static uint8_t matchLevel(bool active) { return active ? (uint8_t)255 : 0; }

static void deriveMatchedLightLevels(const bool down[10], uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *w,
                                     uint8_t *amber) {
  *r = matchLevel(down[3] || down[9]);
  *g = matchLevel(down[2] || down[8]);
  *b = matchLevel(down[1] || down[7]);
  *amber = matchLevel(down[0] || down[6]);
  *w = 0;
  if (!*r && !*g && !*b && !*amber && (down[4] || down[5])) {
    *r = *g = *b = (uint8_t)255;
  }
}

static bool actuatorPublishBubblePartyExtend(const bool down[10], uint32_t intervalIndex) {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t w = 0;
  uint8_t amber = 0;
  deriveMatchedLightLevels(down, &r, &g, &b, &w, &amber);

  if (!espnowActuatorReady()) {
    Serial.println("[ACT] bubble fan extend skipped — ESP-NOW not ready");
    return false;
  }

  const ActuatorCmdPacket pkt = espnowActuatorMakeBubblePartyPacket(r, g, b, w, amber);
  const bool ok = espnowActuatorSend(&pkt);
  Serial.printf("[ACT] bubble fan +%ums tx %s  interval=%u  seq=%u\n", (unsigned)BUBBLE_PARTY_MS, ok ? "OK" : "FAIL",
                (unsigned)intervalIndex, (unsigned)pkt.seq);
  return ok;
}

void actuatorLinkBubbleHoldCheck(const bool down[10], uint32_t nowMs) {
  bool anyDown = false;
  for (int i = 0; i < 10; i++) {
    if (down[i]) {
      anyDown = true;
      break;
    }
  }

  static uint32_t holdStartMs = 0;
  static uint32_t firedIntervals = 0;

  if (!anyDown) {
    holdStartMs = 0;
    firedIntervals = 0;
    return;
  }

  if (holdStartMs == 0)
    holdStartMs = nowMs;

  const uint32_t holdMs = nowMs - holdStartMs;
  if (holdMs < (uint32_t)BUBBLE_HOLD_TRIGGER_MS)
    return;

  const uint32_t completedIntervals = holdMs / (uint32_t)BUBBLE_HOLD_TRIGGER_MS;
  while (firedIntervals < completedIntervals) {
    ++firedIntervals;
    actuatorPublishBubblePartyExtend(down, firedIntervals);
  }
}

static bool actuatorPublishPacket(const ActuatorCmdPacket *pkt, const char *label) {
  if (!espnowActuatorReady() || !pkt)
    return false;
  const bool ok = espnowActuatorSend(pkt);
  Serial.printf("[ACT] %s tx %s seq=%u\n", label, ok ? "OK" : "FAIL", (unsigned)pkt->seq);
  return ok;
}

static void actuatorForceAllColorsOff() {
  for (int slot = 0; slot < 4; slot++) {
    if (!s_colorRemoteOn[slot])
      continue;
    const ActuatorColor color = colorSlotToEnum(slot);
    if (color >= ACTUATOR_COLOR_COUNT)
      continue;
    s_colorRemoteOn[slot] = false;
    actuatorPublishColor(color, false);
  }
}

void actuatorLinkUpdateIdle(bool synthIdle, uint32_t nowMs, uint8_t mirrorR, uint8_t mirrorG, uint8_t mirrorB) {
  static bool lastSynthIdle = false;
  static uint32_t lastIdleTxMs = 0;

  if (synthIdle && !lastSynthIdle) {
    Serial.println("[ACT] synth idle enter — kill bubble fan, release holds, mirror DMX");
    const ActuatorCmdPacket killPkt = espnowActuatorMakeBubbleKillPacket();
    actuatorPublishPacket(&killPkt, "bubble_kill");
    actuatorForceAllColorsOff();
  }

  if (!synthIdle && lastSynthIdle) {
    Serial.println("[ACT] synth idle exit — restore button-driven DMX");
    const ActuatorCmdPacket endPkt = espnowActuatorMakeIdleEndPacket();
    actuatorPublishPacket(&endPkt, "idle_end");
  }

  lastSynthIdle = synthIdle;

  if (!synthIdle)
    return;

  if (nowMs - lastIdleTxMs < (uint32_t)ACTUATOR_IDLE_DMX_MS)
    return;
  lastIdleTxMs = nowMs;

  uint8_t amber = 0;
  if (mirrorR > 0 && mirrorG > 0 && mirrorB < 64)
    amber = (uint8_t)min(255, ((int)mirrorR + (int)mirrorG) / 2);

  const ActuatorCmdPacket pkt = espnowActuatorMakeIdleDmxPacket(mirrorR, mirrorG, mirrorB, 0, amber);
  actuatorPublishPacket(&pkt, "idle_dmx");
}

bool actuatorPublishForSideColumn(int btnIdx, bool on) {
  (void)btnIdx;
  (void)on;
  return false;
}

bool actuatorPublishForGpio(int gpio, bool on) {
  (void)gpio;
  (void)on;
  return false;
}

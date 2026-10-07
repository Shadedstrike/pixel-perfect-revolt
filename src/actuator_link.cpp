#include "actuator_link.h"
#include "actuator_config.h"
#include "actuator_protocol.h"
#include "espnow_actuator.h"
#include "serial_status.h"

#include <WiFi.h>
#include <atomic>
#include <cmath>
#include <cstring>

static std::atomic<uint32_t> s_txCount{0};
static std::atomic<bool> s_purgeActive{false};
static std::atomic<bool> s_primeActive{false};
static bool s_primeExitArmed = false;
static bool s_purgeNeedsRelease = false;
static uint32_t s_purgeStartedMs = 0;
static uint32_t s_lastPurgeTxMs = 0;
static uint32_t s_lastPrimeTxMs = 0;
static bool actuatorPublishPacket(ActuatorCmdPacket *pkt, const char *label);
static void actuatorForceAllColorsOff();

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

static std::atomic<bool> s_colorRemoteOn[4];

static bool actuatorPublishColor(ActuatorColor color, bool on, bool verbose = true) {
  if (color >= ACTUATOR_COLOR_COUNT)
    return false;
  if (!espnowActuatorReady())
    return false;

  ActuatorCmdPacket pkt = espnowActuatorMakePacket(color, on);
  const bool ok = espnowActuatorSend(&pkt);
  if (!ok) {
    Serial.printf("[ESPNOW] tx FAIL color=%u on=%d seq=%u\n", (unsigned)color, on ? 1 : 0, (unsigned)pkt.seq);
  } else if (verbose) {
    ++s_txCount;
    Serial.printf("[ESPNOW] tx color=%s on=%d seq=%u total_tx=%u\n", serialStatusColorName(color), on ? 1 : 0,
                  (unsigned)pkt.seq, (unsigned)s_txCount);
  }
  return ok;
}

void actuatorLinkSyncSideColumnHolds(const bool down[10]) {
  if (s_purgeActive || s_primeActive)
    return;
  static uint32_t lastRefreshMs = 0;
  static uint8_t refreshSlot = 0;
  const uint32_t now = millis();
  const bool refresh = now - lastRefreshMs >= 250u;
  if (refresh) {
    lastRefreshMs = now;
    refreshSlot = (uint8_t)((refreshSlot + 1u) & 3u);
  }
  for (int slot = 0; slot < 4; slot++) {
    const bool phys = sideColorPhysicallyDown(down, slot);
    const bool remote = s_colorRemoteOn[slot].load();
    const bool keepalive = refresh && slot == refreshSlot;
    if (phys == remote && !keepalive)
      continue;

    const ActuatorColor color = colorSlotToEnum(slot);
    if (color >= ACTUATOR_COLOR_COUNT)
      continue;

    if (phys != remote)
      Serial.printf("[ACT] sync %s -> %s\n", colorSlotName(slot), phys ? "ON" : "OFF");
    if (actuatorPublishColor(color, phys, phys != remote))
      s_colorRemoteOn[slot].store(phys);
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
    if (s_colorRemoteOn[i].load())
      ++activeHolds;
  }

  Serial.printf("[HB] role=CONTROLLER  ESPNOW=%s  ch=%u  mac=%s  tx=%u  active_holds=%d  uptime=%lus\n",
                espnowActuatorReady() ? "OK" : "DOWN", (unsigned)ESPNOW_WIFI_CHANNEL, WiFi.macAddress().c_str(),
                (unsigned)s_txCount.load(), activeHolds, (unsigned long)(now / 1000));
}

bool actuatorLinkSetup() {
  Serial.println("[BOOT] Actuator link: side-column hold -> ESP-NOW -> remote solenoids + motors + DMX.");
  Serial.printf("[BOOT] ESP-NOW channel=%u  bubble: %ums hold -> %ums fan once (re-arm on release)\n",
                (unsigned)ESPNOW_WIFI_CHANNEL, (unsigned)BUBBLE_HOLD_TRIGGER_MS, (unsigned)BUBBLE_PARTY_MS);
  Serial.printf("[BOOT] synth idle: kill bubble fan + local PAR/bubble DMX animation @ %uHz keepalive\n",
                (unsigned)(1000u / ACTUATOR_IDLE_DMX_MS));
  const bool ok = espnowActuatorBeginTx();
  Serial.printf("[BOOT] ESP-NOW TX %s  mac=%s\n", ok ? "ready" : "FAILED", WiFi.macAddress().c_str());
  return ok;
}

void actuatorLinkLoop() {
  actuatorHeartbeat();
  const uint32_t now = millis();
  // End locally at five minutes as well as on the actuator. Check expiry before
  // retransmitting so no late keepalive can start a second purge cycle.
  if (s_purgeActive && now - s_purgeStartedMs >= (uint32_t)ACTUATOR_PURGE_DURATION_MS) {
    s_purgeActive = false;
    actuatorForceAllColorsOff();
    Serial.println("[PURGE] COMPLETE — five minutes elapsed; normal operation restored after button release");
  }
  // Repeat while active so a single dropped broadcast cannot leave the UI in
  // PURGE while the pumps stay idle.
  if (s_purgeActive && now - s_lastPurgeTxMs >= 500u) {
    s_lastPurgeTxMs = now;
    ActuatorCmdPacket pkt = espnowActuatorMakePurgePacket();
    espnowActuatorSend(&pkt);
  }
  if (s_primeActive && now - s_lastPrimeTxMs >= 500u) {
    s_lastPrimeTxMs = now;
    ActuatorCmdPacket pkt = espnowActuatorMakePrimePacket(true);
    espnowActuatorSend(&pkt);
  }
}

void actuatorLinkPurgeCheck(const bool down[10], uint32_t nowMs) {
  static uint32_t frontHoldStart = 0;
  if (s_purgeActive)
    return;
  const bool bothFront = down[4] && down[5];
  if (s_purgeNeedsRelease) {
    if (!bothFront) {
      s_purgeNeedsRelease = false;
      frontHoldStart = 0;
    }
    return;
  }
  if (!bothFront) {
    frontHoldStart = 0;
    return;
  }
  if (!frontHoldStart)
    frontHoldStart = nowMs;
  if (nowMs - frontHoldStart < 40000u)
    return;
  ActuatorCmdPacket pkt = espnowActuatorMakePurgePacket();
  if (actuatorPublishPacket(&pkt, "purge")) {
    s_purgeActive = true;
    s_purgeNeedsRelease = true;
    s_purgeStartedMs = nowMs;
    s_lastPurgeTxMs = nowMs;
    actuatorForceAllColorsOff();
    Serial.println("[PURGE] ACTIVE — all pumps reverse for five minutes");
  }
}

bool actuatorLinkPurgeActive() { return s_purgeActive; }

void actuatorLinkPrimeCheck(const bool down[10], const bool edgeDown[10], uint32_t nowMs) {
  static uint32_t frontRightHoldStart = 0;
  if (s_purgeActive)
    return;

  if (s_primeActive) {
    bool anyDown = false;
    bool anyEdgeDown = false;
    for (int i = 0; i < 10; ++i) {
      anyDown |= down[i];
      anyEdgeDown |= edgeDown[i];
    }
    if (!anyDown)
      s_primeExitArmed = true; // do not let the trigger hold immediately cancel PRIME
    if (!s_primeExitArmed || !anyEdgeDown)
      return;

    ActuatorCmdPacket pkt = espnowActuatorMakePrimePacket(false);
    actuatorPublishPacket(&pkt, "prime_off");
    actuatorForceAllColorsOff();
    s_primeActive.store(false);
    s_primeExitArmed = false;
    Serial.println("[PRIME] stopped — normal button operation restored");
    return;
  }

  // PRIME is deliberately an exclusive gesture: only front-right may be held.
  bool onlyFrontRight = down[5];
  for (int i = 0; i < 10; ++i) {
    if (i != 5 && down[i])
      onlyFrontRight = false;
  }
  if (!onlyFrontRight) {
    frontRightHoldStart = 0;
    return;
  }
  if (!frontRightHoldStart)
    frontRightHoldStart = nowMs;
  if (nowMs - frontRightHoldStart < 25000u)
    return;

  actuatorForceAllColorsOff();
  ActuatorCmdPacket pkt = espnowActuatorMakePrimePacket(true);
  if (actuatorPublishPacket(&pkt, "prime_on")) {
    s_primeActive = true;
    s_primeExitArmed = false;
    s_lastPrimeTxMs = nowMs;
    Serial.println("[PRIME] ACTIVE — all pumps continuous; release, then press any button to stop");
  }
}

bool actuatorLinkPrimeActive() { return s_primeActive; }

static uint8_t matchLevel(bool active) { return active ? (uint8_t)255 : 0; }

static void deriveMatchedLightLevels(const bool down[10], uint32_t nowMs, uint8_t *r, uint8_t *g, uint8_t *b,
                                     uint8_t *w, uint8_t *amber) {
  *r = matchLevel(down[3] || down[9]);
  *g = matchLevel(down[2] || down[8]);
  *b = matchLevel(down[1] || down[7]);
  *amber = matchLevel(down[0] || down[6]);
  *w = 0;
  if (!*r && !*g && !*b && !*amber && (down[4] || down[5])) {
    *r = *g = *b = (uint8_t)255;
  }

  if (!*r && !*g && !*b && !*amber)
    return;

  const float t = nowMs / 1000.0f;
  const float pulse = 0.72f + 0.28f * sinf(2.0f * (float)M_PI * 1.8f * t);
  *r = (uint8_t)lroundf((float)(*r)*pulse);
  *g = (uint8_t)lroundf((float)(*g)*pulse);
  *b = (uint8_t)lroundf((float)(*b)*pulse);
  *amber = (uint8_t)lroundf((float)(*amber)*pulse);
}

static bool actuatorPublishBubblePartyExtend(const bool down[10], uint32_t nowMs, uint32_t intervalIndex) {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t w = 0;
  uint8_t amber = 0;
  deriveMatchedLightLevels(down, nowMs, &r, &g, &b, &w, &amber);

  if (!espnowActuatorReady()) {
    Serial.println("[ACT] bubble fan extend skipped — ESP-NOW not ready");
    return false;
  }

  ActuatorCmdPacket pkt = espnowActuatorMakeBubblePartyPacket(r, g, b, w, amber);
  const bool ok = espnowActuatorSend(&pkt);
  Serial.printf("[ACT] bubble fan +%ums tx %s  interval=%u  seq=%u\n", (unsigned)BUBBLE_PARTY_MS, ok ? "OK" : "FAIL",
                (unsigned)intervalIndex, (unsigned)pkt.seq);
  return ok;
}

static bool actuatorPublishPacket(ActuatorCmdPacket *pkt, const char *label) {
  if (!espnowActuatorReady() || !pkt)
    return false;
  const bool ok = espnowActuatorSend(pkt);
  Serial.printf("[ACT] %s tx %s seq=%u\n", label, ok ? "OK" : "FAIL", (unsigned)pkt->seq);
  return ok;
}

void actuatorLinkBubbleHoldCheck(const bool down[10], uint32_t nowMs) {
  // Front-right alone is reserved for the 25 s PRIME gesture. Do not start the
  // bubble-party effect partway through that deliberate hold.
  bool onlyFrontRight = down[5];
  for (int i = 0; i < 10; ++i) {
    if (i != 5 && down[i])
      onlyFrontRight = false;
  }
  if (onlyFrontRight)
    return;

  bool anyDown = false;
  for (int i = 0; i < 10; i++) {
    if (down[i]) {
      anyDown = true;
      break;
    }
  }

  static uint32_t holdStartMs = 0;
  static bool bubbleArmed = true;

  if (!anyDown) {
    holdStartMs = 0;
    bubbleArmed = true;
    return;
  }

  if (!bubbleArmed)
    return;

  if (holdStartMs == 0)
    holdStartMs = nowMs;

  const uint32_t holdMs = nowMs - holdStartMs;
  if (holdMs < (uint32_t)BUBBLE_HOLD_TRIGGER_MS)
    return;

  actuatorPublishBubblePartyExtend(down, nowMs, 1);
  bubbleArmed = false;
}

void actuatorLinkSyncDmxRgb(const bool down[10], uint32_t nowMs, bool synthIdle) {
  if (synthIdle || !espnowActuatorReady())
    return;

  static uint32_t lastTxMs = 0;
  static uint8_t lastR = 0;
  static uint8_t lastG = 0;
  static uint8_t lastB = 0;
  static uint8_t lastA = 0;

  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t w = 0;
  uint8_t amber = 0;
  deriveMatchedLightLevels(down, nowMs, &r, &g, &b, &w, &amber);

  const bool sideActive = r || g || b || amber;
  if (!sideActive) {
    if (lastR || lastG || lastB || lastA) {
      ActuatorCmdPacket pkt = espnowActuatorMakeRgbHoldPacket(0, 0, 0, 0, 0);
      actuatorPublishPacket(&pkt, "rgb_hold_clear");
      lastR = lastG = lastB = lastA = 0;
    }
    return;
  }

  if (nowMs - lastTxMs < (uint32_t)ACTUATOR_RGB_SYNC_MS && r == lastR && g == lastG && b == lastB && amber == lastA)
    return;

  lastTxMs = nowMs;
  lastR = r;
  lastG = g;
  lastB = b;
  lastA = amber;

  ActuatorCmdPacket pkt = espnowActuatorMakeRgbHoldPacket(r, g, b, w, amber);
  actuatorPublishPacket(&pkt, "rgb_hold");
}

static void actuatorForceAllColorsOff() {
  for (int slot = 0; slot < 4; slot++) {
    const ActuatorColor color = colorSlotToEnum(slot);
    if (color >= ACTUATOR_COLOR_COUNT)
      continue;
    s_colorRemoteOn[slot].store(false);
    actuatorPublishColor(color, false);
  }
}

void actuatorLinkUpdateIdle(bool synthIdle, uint32_t nowMs, uint8_t mirrorR, uint8_t mirrorG, uint8_t mirrorB) {
  (void)mirrorR;
  (void)mirrorG;
  (void)mirrorB;

  static bool lastSynthIdle = false;
  static uint32_t lastIdleTxMs = 0;

  if (synthIdle && !lastSynthIdle) {
    Serial.println("[ACT] synth idle enter — kill bubble fan, release holds, start DMX idle animation");
    ActuatorCmdPacket killPkt = espnowActuatorMakeBubbleKillPacket();
    actuatorPublishPacket(&killPkt, "bubble_kill");
    actuatorForceAllColorsOff();
    lastIdleTxMs = 0;
  }

  if (!synthIdle && lastSynthIdle) {
    Serial.println("[ACT] synth idle exit — restore button-driven DMX");
    ActuatorCmdPacket endPkt = espnowActuatorMakeIdleEndPacket();
    actuatorPublishPacket(&endPkt, "idle_end");
  }

  lastSynthIdle = synthIdle;

  if (!synthIdle)
    return;

  if (lastIdleTxMs != 0 && nowMs - lastIdleTxMs < (uint32_t)ACTUATOR_IDLE_DMX_MS)
    return;
  lastIdleTxMs = nowMs;

  ActuatorCmdPacket pkt = espnowActuatorMakeIdleDmxPacket(0, 0, 0, 0, 0);
  actuatorPublishPacket(&pkt, "idle_anim");
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

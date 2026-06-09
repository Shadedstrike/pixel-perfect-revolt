#include "actuator_link.h"
#include "actuator_config.h"
#include "actuator_protocol.h"
#include "espnow_actuator.h"

#include <WiFi.h>
#include <cstring>

static uint32_t s_txCount = 0;

static const char *sideColumnBtnIdxToColor(int btnIdx) {
  switch (btnIdx) {
    case 0:
    case 6:
      return "yellow";
    case 1:
    case 7:
      return "blue";
    case 2:
    case 8:
      return "green";
    case 3:
    case 9:
      return "red";
    default:
      return nullptr;
  }
}

static const char *gpioToColorName(int gpio) {
  switch (gpio) {
    case 38:
    case 11:
    case 39:
      return "yellow";
    case 12:
    case 2:
      return "blue";
    case 5:
    case 9:
    case 15:
      return "green";
    case 7:
    case 8:
      return "red";
    default:
      return nullptr;
  }
}

static ActuatorColor colorNameToEnum(const char *c) {
  if (!c)
    return ACTUATOR_COLOR_COUNT;
  if (!strcmp(c, "red"))
    return ACTUATOR_COLOR_RED;
  if (!strcmp(c, "green"))
    return ACTUATOR_COLOR_GREEN;
  if (!strcmp(c, "blue"))
    return ACTUATOR_COLOR_BLUE;
  if (!strcmp(c, "yellow"))
    return ACTUATOR_COLOR_YELLOW;
  return ACTUATOR_COLOR_COUNT;
}

static int colorEnumToHoldSlot(ActuatorColor color) {
  switch (color) {
    case ACTUATOR_COLOR_YELLOW:
      return 0;
    case ACTUATOR_COLOR_BLUE:
      return 1;
    case ACTUATOR_COLOR_GREEN:
      return 2;
    case ACTUATOR_COLOR_RED:
      return 3;
    default:
      return -1;
  }
}

static int sideColumnBtnIdxToHoldSlot(int btnIdx) {
  const char *color = sideColumnBtnIdxToColor(btnIdx);
  if (!color)
    return -1;
  return colorEnumToHoldSlot(colorNameToEnum(color));
}

static uint8_t s_colorHoldCount[4];

static bool actuatorPublishColor(const char *color, bool on) {
  const ActuatorColor ac = colorNameToEnum(color);
  if (ac >= ACTUATOR_COLOR_COUNT)
    return false;
  if (!espnowActuatorReady())
    return false;

  const ActuatorCmdPacket pkt = espnowActuatorMakePacket(ac, on);
  const bool ok = espnowActuatorSend(&pkt);
  if (!ok) {
    Serial.printf("[ESPNOW] tx FAIL color=%s on=%d seq=%u\n", color, on ? 1 : 0, (unsigned)pkt.seq);
  } else {
    ++s_txCount;
    Serial.printf("[ESPNOW] tx color=%s on=%d seq=%u total_tx=%u\n", color, on ? 1 : 0, (unsigned)pkt.seq,
                  (unsigned)s_txCount);
  }
  return ok;
}

void actuatorSolenoidSideColumnHold(int btnIdx, bool on) {
  const char *color = sideColumnBtnIdxToColor(btnIdx);
  const int slot = sideColumnBtnIdxToHoldSlot(btnIdx);
  if (!color || slot < 0) {
    Serial.printf("[ACT] btn=%d ignored (no color map)\n", btnIdx);
    return;
  }

  if (on) {
    if (s_colorHoldCount[slot] == 0) {
      Serial.printf("[ACT] side_btn=%d -> %s ON (held)\n", btnIdx, color);
      actuatorPublishColor(color, true);
    } else {
      Serial.printf("[ACT] side_btn=%d -> %s hold+1 (count=%u)\n", btnIdx, color,
                    (unsigned)(s_colorHoldCount[slot] + 1));
    }
    if (s_colorHoldCount[slot] < 255)
      ++s_colorHoldCount[slot];
    return;
  }

  if (s_colorHoldCount[slot] == 0) {
    Serial.printf("[ACT] side_btn=%d -> %s release ignored (count=0)\n", btnIdx, color);
    return;
  }
  --s_colorHoldCount[slot];
  if (s_colorHoldCount[slot] == 0) {
    Serial.printf("[ACT] side_btn=%d -> %s OFF (released)\n", btnIdx, color);
    actuatorPublishColor(color, false);
  } else {
    Serial.printf("[ACT] side_btn=%d -> %s hold-1 (count=%u)\n", btnIdx, color, (unsigned)s_colorHoldCount[slot]);
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
    if (s_colorHoldCount[i] != 0)
      ++activeHolds;
  }

  Serial.printf("[HB] role=CONTROLLER  ESPNOW=%s  ch=%u  mac=%s  tx=%u  active_holds=%d  uptime=%lus\n",
                espnowActuatorReady() ? "OK" : "DOWN", (unsigned)ESPNOW_WIFI_CHANNEL, WiFi.macAddress().c_str(),
                (unsigned)s_txCount, activeHolds, (unsigned long)(now / 1000));
}

bool actuatorLinkSetup() {
  Serial.println("[BOOT] Actuator link: side-column hold -> ESP-NOW -> remote solenoids + motors + DMX.");
  Serial.printf("[BOOT] ESP-NOW channel=%u  mode=hold-while-pressed\n", (unsigned)ESPNOW_WIFI_CHANNEL);
  const bool ok = espnowActuatorBeginTx();
  Serial.printf("[BOOT] ESP-NOW TX %s  mac=%s\n", ok ? "ready" : "FAILED", WiFi.macAddress().c_str());
  return ok;
}

void actuatorLinkLoop() { actuatorHeartbeat(); }

bool actuatorPublishForSideColumn(int btnIdx, bool on) {
  return actuatorPublishColor(sideColumnBtnIdxToColor(btnIdx), on);
}

bool actuatorPublishForGpio(int gpio, bool on) {
  return actuatorPublishColor(gpioToColorName(gpio), on);
}

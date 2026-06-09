// Actuator node: ESP-NOW -> Telesyn motor drivers + MCP23017 solenoids + DMX512.
//
// Flash: pio run -e solenoid-node -t upload
// Monitor: pio device monitor -e solenoid-node

#include <Arduino.h>
#include <WiFi.h>

#include "actuator_config.h"
#include "actuator_protocol.h"
#include "motor_config.h"
#include "dmx_config.h"
#include "dmx_output.h"
#include "espnow_actuator.h"
#include "motor_output.h"
#include "serial_status.h"
#include "solenoid_output.h"

static uint32_t s_rxCount = 0;
static uint32_t s_lastRxMs = 0;
static ActuatorColor s_lastColor = ACTUATOR_COLOR_COUNT;
static bool s_lastOn = false;

static void waitForUsbSerial() {
  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 4000)
    delay(10);
}

static void applyActuatorColor(ActuatorColor color, bool on) {
  Serial.printf("[ACT] %s %s", serialStatusColorName(color), on ? "ON" : "OFF");

  if (motorOutputHasMotor(color)) {
    motorOutputSetColor(color, on);
    Serial.printf(" | mot=%s", on ? "RUN" : "STOP");
  } else {
    Serial.print(" | mot=—");
  }

  solenoidOutputSetChannel((uint8_t)color, on);

  int8_t parOffset = -1;
  bool bubble = false;
  switch (color) {
    case ACTUATOR_COLOR_RED:
      parOffset = DMX_PAR_CH_RED;
      break;
    case ACTUATOR_COLOR_GREEN:
      parOffset = DMX_PAR_CH_GREEN;
      break;
    case ACTUATOR_COLOR_BLUE:
      parOffset = DMX_PAR_CH_BLUE;
      break;
    case ACTUATOR_COLOR_YELLOW:
      parOffset = DMX_PAR_CH_AMBER;
      bubble = true;
      break;
    default:
      Serial.println(" | ignored");
      return;
  }

  if (dmxOutputReady() && parOffset >= 0) {
    const uint16_t slot = (uint16_t)(DMX_PAR_START_ADDR + (uint8_t)parOffset);
    dmxOutputSetSlot(slot, on ? (uint8_t)DMX_LEVEL_FULL : 0);
    Serial.printf(" | dmx=%u", (unsigned)slot);
  } else {
    Serial.print(" | dmx=SKIP");
  }

  if (bubble) {
    dmxOutputSetBubble(on);
    Serial.print(" | bubble");
  }

  Serial.println();
}

static void onEspnowCmd(const ActuatorCmdPacket *pkt, const uint8_t mac[6]) {
  (void)mac;
  ++s_rxCount;
  s_lastRxMs = millis();
  s_lastColor = (ActuatorColor)pkt->color;
  s_lastOn = pkt->on != 0;
  applyActuatorColor((ActuatorColor)pkt->color, pkt->on != 0);
}

static void debugHeartbeat(uint32_t now) {
  static uint32_t lastHb = 0;
  if (now - lastHb < (uint32_t)ACTUATOR_HB_MS)
    return;
  lastHb = now;

  const uint32_t sinceRx = s_lastRxMs ? (now - s_lastRxMs) : 0;
  const char *lastColor =
      s_lastColor < ACTUATOR_COLOR_COUNT ? serialStatusColorName(s_lastColor) : "none";

  Serial.printf("[HB] ACTUATOR  ESPNOW=%s  STBY=%s  SOL=%s  DMX=%s  rx=%u  last=%s %s  ago=%lums  ch=%u  mac=%s  up=%lus\n",
                espnowActuatorReady() ? "OK" : "DOWN",
                motorOutputStbyLevel() < 0 ? "?" : (motorOutputStbyEnabled() ? "H" : "L"),
                solenoidOutputReady() ? "OK" : "FAIL", dmxOutputReady() ? "OK" : "FAIL", (unsigned)s_rxCount,
                lastColor, s_lastOn ? "ON" : "OFF", s_lastRxMs ? (unsigned long)sinceRx : 0UL,
                (unsigned)ESPNOW_WIFI_CHANNEL, WiFi.macAddress().c_str(), (unsigned long)(now / 1000));
}

static void printBootConfig() {
  Serial.println("[BOOT] Subsystems:");
  Serial.printf("  ESP-NOW: RX broadcast  channel=%u\n", (unsigned)ESPNOW_WIFI_CHANNEL);
  Serial.printf("  Motors: STBY=GPIO%d  red=%d/%d  green=%d/%d  yellow=%d/%d\n", (int)MOTOR_STBY_PIN,
                (int)MOTOR_RED_IN1, (int)MOTOR_RED_IN2, (int)MOTOR_GREEN_IN1, (int)MOTOR_GREEN_IN2,
                (int)MOTOR_YELLOW_IN1, (int)MOTOR_YELLOW_IN2);
  Serial.printf("  Solenoids: MCP23017 SDA=%d SCL=%d  ch0=red ch1=green ch2=blue ch3=yellow\n", (int)I2C_SDA,
                (int)I2C_SCL);
  Serial.printf("  DMX: TX=%d RX=%d RTS=%d  par@%u  bubble@%u\n", (int)DMX_TX_PIN, (int)DMX_RX_PIN,
                (int)DMX_RTS_PIN, (unsigned)DMX_PAR_START_ADDR, (unsigned)DMX_BUBBLE_ADDR);
  Serial.println("[BOOT] Waiting for ESP-NOW from controller...");
}

void setup() {
  waitForUsbSerial();
  motorOutputEarlyInit();

  serialStatusBanner("ACTUATOR (ESP-NOW — motors + solenoids + DMX)");
  Serial.println("[BOOT] Serial OK — logging enabled");

  motorOutputBegin();
  const bool solOk = solenoidOutputBegin();
  const bool dmxOk = dmxOutputBegin();
  const bool espOk = espnowActuatorBeginRx(onEspnowCmd);

  printBootConfig();
  Serial.printf("[BOOT] solenoids=%s  dmx=%s  espnow=%s\n", solOk ? "OK" : "FAIL", dmxOk ? "OK" : "FAIL",
                espOk ? "OK" : "FAIL");
}

void loop() {
  const uint32_t now = millis();
  debugHeartbeat(now);
  dmxOutputService(now);
  delay(1);
}

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
#include "serial_log.h"

static uint32_t s_rxCount = 0;
static uint32_t s_lastRxMs = 0;
static ActuatorColor s_lastColor = ACTUATOR_COLOR_COUNT;
static bool s_lastOn = false;
static bool s_solOk = false;
static bool s_dmxOk = false;
static bool s_espOk = false;
static bool s_usbMonitorLogged = false;
static bool s_setupDone = false;

static void printBootConfig() {
  Serial.println("[BOOT] Subsystems:");
  Serial.printf("  ESP-NOW: RX broadcast  channel=%u\n", (unsigned)ESPNOW_WIFI_CHANNEL);
  Serial.printf("  Motors: STBY=GPIO%d  red=%d/%d  green=%d/%d  yellow=%d/%d\n", (int)MOTOR_STBY_PIN,
                (int)MOTOR_RED_IN1, (int)MOTOR_RED_IN2, (int)MOTOR_GREEN_IN1, (int)MOTOR_GREEN_IN2,
                (int)MOTOR_YELLOW_IN1, (int)MOTOR_YELLOW_IN2);
  Serial.printf("  Solenoids: MCP23017 SDA=%d SCL=%d  ch0=red ch1=green ch2=blue ch3=yellow\n", (int)I2C_SDA,
                (int)I2C_SCL);
  Serial.printf("  DMX: TX=%d RX=%d RTS=%d  par@%u  bubble@%u (6ch)\n", (int)DMX_TX_PIN, (int)DMX_RX_PIN,
                (int)DMX_RTS_PIN, (unsigned)DMX_PAR_START_ADDR, (unsigned)DMX_BUBBLE_ADDR);
  Serial.println("[BOOT] Waiting for ESP-NOW from controller...");
}

static void printStatusSnapshot(const char *reason) {
  Serial.println();
  Serial.printf("[SERIAL] %s  up=%lus\n", reason, (unsigned long)(millis() / 1000));
  serialStatusBanner("ACTUATOR (ESP-NOW — motors + solenoids + DMX)");
  printBootConfig();
  Serial.printf("[BOOT] solenoids=%s  dmx=%s  espnow=%s\n", s_solOk ? "OK" : "FAIL", s_dmxOk ? "OK" : "FAIL",
                s_espOk ? "OK" : "FAIL");
  solenoidOutputPrintI2cScan();
}

static void onUsbMonitorConnected() {
  printStatusSnapshot("USB monitor connected — status snapshot");
  s_usbMonitorLogged = true;
}

static void applyActuatorColor(ActuatorColor color, bool on) {
  Serial.printf("[ACT] %s %s", serialStatusColorName(color), on ? "ON" : "OFF");

  if (motorOutputHasMotor(color)) {
    motorOutputSetColor(color, on);
    Serial.printf(" | mot=%s", on ? "RUN" : "STOP");
  } else {
    Serial.print(" | mot=—");
  }

  if (color != ACTUATOR_COLOR_BLUE)
    solenoidOutputSetChannel((uint8_t)color, on);
  else
    Serial.print(" | sol=—(blue disabled)");

  if (color >= ACTUATOR_COLOR_COUNT) {
    Serial.println(" | ignored");
    return;
  }

  if (dmxOutputReady()) {
    dmxOutputSetColorHold(color, on);
    Serial.print(" | dmx=leds");
    if (dmxOutputBubbleFanActive(millis()))
      Serial.print("+fan");
  } else {
    Serial.print(" | dmx=SKIP");
  }

  Serial.println();
}

static void onBubblePartyCmd(const ActuatorCmdPacket *pkt) {
  const uint32_t now = millis();
  dmxOutputExtendBubbleParty(now, pkt->level_r, pkt->level_g, pkt->level_b, pkt->level_w, pkt->color);
}

static bool s_failsafeTripped = false;

static void actuatorForceAllOutputsOff(const char *reason) {
  Serial.printf("[SAFE] %s — solenoids/motors/fan OFF\n", reason);
  solenoidOutputAllOff();
  motorOutputAllOff();
  if (dmxOutputReady())
    dmxOutputForceSafeOutputs();
}

static void actuatorRxWatchdog(uint32_t now) {
  if (!s_lastRxMs)
    return;
  if (now - s_lastRxMs < (uint32_t)ACTUATOR_RX_FAILSAFE_MS)
    return;
  if (!solenoidOutputAnyOn())
    return;
  if (s_failsafeTripped)
    return;

  s_failsafeTripped = true;
  actuatorForceAllOutputsOff("ESP-NOW timeout");
}

static void onEspnowCmd(const ActuatorCmdPacket *pkt, const uint8_t mac[6]) {
  (void)mac;
  ++s_rxCount;
  s_lastRxMs = millis();
  s_failsafeTripped = false;

  if (pkt->on == ACTUATOR_ON_BUBBLE_PARTY) {
    onBubblePartyCmd(pkt);
    return;
  }

  if (pkt->on == ACTUATOR_ON_BUBBLE_KILL) {
    if (dmxOutputReady())
      dmxOutputKillBubbleParty();
    return;
  }

  if (pkt->on == ACTUATOR_ON_IDLE_DMX) {
    if (dmxOutputReady())
      dmxOutputSetIdleLevels(pkt->level_r, pkt->level_g, pkt->level_b, pkt->level_w, pkt->color);
    return;
  }

  if (pkt->on == ACTUATOR_ON_IDLE_END) {
    if (dmxOutputReady())
      dmxOutputClearIdle();
    return;
  }

  if (pkt->on == ACTUATOR_ON_RGB_HOLD) {
    if (dmxOutputReady())
      dmxOutputSetCombinedRgbHold(pkt->level_r, pkt->level_g, pkt->level_b, pkt->color);
    return;
  }

  s_lastColor = (ActuatorColor)pkt->color;
  s_lastOn = pkt->on != 0;
  applyActuatorColor((ActuatorColor)pkt->color, pkt->on != 0);
}

static void debugHeartbeat(uint32_t now) {
  static uint32_t lastHb = 0;
  static uint8_t hbCount = 0;
  if (now - lastHb < (uint32_t)ACTUATOR_HB_MS)
    return;
  lastHb = now;
  ++hbCount;

  const uint32_t sinceRx = s_lastRxMs ? (now - s_lastRxMs) : 0;
  const char *lastColor =
      s_lastColor < ACTUATOR_COLOR_COUNT ? serialStatusColorName(s_lastColor) : "none";

  const bool i2cAck = solenoidOutputProbeMcp();
  const char *i2cStr = i2cAck ? "OK" : "NO_ACK";
  const char *initStr = solenoidOutputReady() ? "OK" : "FAIL";

  Serial.printf("[HB] ACTUATOR  ESPNOW=%s  I2C_0x%02X=%s(init=%s)  SDA=%d SCL=%d  STBY=%s  DMX=%s  "
                "rx=%u  last=%s %s  ago=%lums  ch=%u  mac=%s  up=%lus\n",
                espnowActuatorReady() ? "OK" : "DOWN", (unsigned)MCP23017_ADDR, i2cStr, initStr, (int)I2C_SDA,
                (int)I2C_SCL, motorOutputStbyLevel() < 0 ? "?" : (motorOutputStbyEnabled() ? "H" : "L"),
                dmxOutputReady() ? "OK" : "FAIL", (unsigned)s_rxCount, lastColor, s_lastOn ? "ON" : "OFF",
                s_lastRxMs ? (unsigned long)sinceRx : 0UL, (unsigned)ESPNOW_WIFI_CHANNEL,
                WiFi.macAddress().c_str(), (unsigned long)(now / 1000));

  if ((hbCount % 6) == 0)
    solenoidOutputPrintI2cScan();
}

static void debugAlive(uint32_t now) {
  static uint32_t lastAlive = 0;
  if (now - lastAlive < 2000)
    return;
  lastAlive = now;
  Serial.printf("[ALIVE] up=%lus  usb=%s  setup=%s\n", (unsigned long)(now / 1000), (bool)Serial ? "yes" : "no",
                s_setupDone ? "done" : "running");
}

void setup() {
  serialLogBegin();
  Serial.println();
  Serial.println("[BOOT] solenoid-node reset — non-blocking USB serial @ 115200");

  serialStatusBanner("ACTUATOR (ESP-NOW — motors + solenoids + DMX)");
  Serial.println("[BOOT] motor GPIO early init...");
  motorOutputEarlyInit();

  Serial.println("[BOOT] motor drivers...");
  motorOutputBegin();

  Serial.println("[BOOT] MCP23017 / I2C...");
  s_solOk = solenoidOutputBegin();

  Serial.println("[BOOT] DMX512...");
  s_dmxOk = dmxOutputBegin();

  Serial.println("[BOOT] ESP-NOW RX...");
  s_espOk = espnowActuatorBeginRx(onEspnowCmd);

  solenoidOutputAllOff();
  if (s_dmxOk)
    dmxOutputBootSafeState();

  printBootConfig();
  Serial.printf("[BOOT] solenoids=%s  dmx=%s  espnow=%s\n", s_solOk ? "OK" : "FAIL", s_dmxOk ? "OK" : "FAIL",
                s_espOk ? "OK" : "FAIL");
  Serial.println("[BOOT] setup complete — [ALIVE] every 2s, [HB] every 5s");
  Serial.println("[BOOT] If monitor was empty: press EN/RESET on the board now");

  s_setupDone = true;
  if (Serial)
    s_usbMonitorLogged = true;
}

void loop() {
  if (Serial && !s_usbMonitorLogged)
    onUsbMonitorConnected();

  const uint32_t now = millis();
  debugAlive(now);
  debugHeartbeat(now);
  actuatorRxWatchdog(now);
  dmxOutputService(now);
  delay(1);
}

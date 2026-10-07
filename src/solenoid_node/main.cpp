// Actuator node: ESP-NOW -> Telesyn motor drivers + MCP23017 solenoids + DMX512.
//
// Flash: pio run -e solenoid-node -t upload
// Monitor: pio device monitor -e solenoid-node

#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <cstring>
#include <freertos/queue.h>

#include "actuator_config.h"
#include "actuator_protocol.h"
#include "motor_config.h"
#include "dmx_config.h"
#include "dmx_output.h"
#include "espnow_actuator.h"
#include "motor_output.h"
#include "relay_config.h"
#include "relay_output.h"
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
static bool s_purgeActive = false;
static bool s_primeActive = false;
static uint32_t s_purgeStartedMs = 0;
static QueueHandle_t s_cmdQueue = nullptr;
static std::atomic<uint32_t> s_cmdQueueDrops{0};
static const uint32_t MOTOR_START_DELAY_MS = 50;
static const uint32_t SOLENOID_RELEASE_HOLD_MS = 1000;
// A short 67% duty cycle retains a visually continuous spray while reducing
// fluid use and pump heat. PRIME deliberately bypasses this scheduler.
struct TimedColorOutput {
  bool requested;
  bool motorStarted;
  bool motorPulseOn;
  uint32_t motorStartAt;
  uint32_t motorToggleAt;
  uint32_t solenoidOffAt;
};
static TimedColorOutput s_timed[ACTUATOR_COLOR_COUNT] = {};

static void printBootConfig() {
  Serial.println("[BOOT] Subsystems:");
  Serial.printf("  ESP-NOW: RX broadcast  channel=%u\n", (unsigned)ESPNOW_WIFI_CHANNEL);
  Serial.printf("  Motors: STBY=GPIO%d  red=%d/%d  green=%d/%d  yellow=%d/%d\n", (int)MOTOR_STBY_PIN,
                (int)MOTOR_RED_IN1, (int)MOTOR_RED_IN2, (int)MOTOR_GREEN_IN1, (int)MOTOR_GREEN_IN2,
                (int)MOTOR_YELLOW_IN1, (int)MOTOR_YELLOW_IN2);
  Serial.printf("  Solenoids: MCP23017 SDA=%d SCL=%d  ch0=yellow ch1=blue ch2=green ch3=red\n", (int)I2C_SDA,
                (int)I2C_SCL);
  Serial.printf("  Relay: GPIO%d active-%s  closed while any color held\n", (int)RELAY_PIN,
                RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
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
  if (color >= ACTUATOR_COLOR_COUNT || s_purgeActive || s_primeActive)
    return;

  TimedColorOutput &timed = s_timed[(uint8_t)color];
  // Controller state is refreshed periodically to repair dropped ESP-NOW
  // transitions. A refresh must not restart the motor pulse or perpetually move
  // the delayed solenoid-OFF deadline forward.
  if (timed.requested == on)
    return;

  Serial.printf("[ACT] %s %s", serialStatusColorName(color), on ? "ON" : "OFF");
  const uint32_t now = millis();
  timed.requested = on;
  if (on) {
    timed.solenoidOffAt = 0;
    timed.motorStartAt = now + MOTOR_START_DELAY_MS;
    timed.motorStarted = false;
    timed.motorPulseOn = false;
    timed.motorToggleAt = 0;
  } else {
    timed.motorStartAt = 0;
    timed.motorStarted = false;
    timed.motorPulseOn = false;
    timed.motorToggleAt = 0;
    timed.solenoidOffAt = now + SOLENOID_RELEASE_HOLD_MS;
  }

  if (motorOutputHasMotor(color)) {
    if (!on)
      motorOutputSetColor(color, false);
    Serial.printf(" | mot=%s", on ? "DELAY_50MS" : "STOP");
  } else {
    Serial.print(" | mot=—");
  }

  if (color != ACTUATOR_COLOR_BLUE || SOLENOID_BLUE_ENABLED) {
    if (on)
      solenoidOutputSetChannel((uint8_t)color, true);
    Serial.printf(" | sol=%s", on ? "ON" : "HOLD_1S");
  }
  else
    Serial.print(" | sol=—(blue disabled)");

  if (color >= ACTUATOR_COLOR_COUNT) {
    Serial.println(" | ignored");
    return;
  }

  // Every color hold closes the relay — including blue, whose solenoid is off.
  relayOutputSetColor(color, on);
  Serial.printf(" | relay=%s", relayOutputActive() ? "CLOSED" : "OPEN");

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

static void serviceTimedColorOutputs(uint32_t now) {
  if (s_purgeActive || s_primeActive)
    return;
  for (uint8_t i = 0; i < ACTUATOR_COLOR_COUNT; ++i) {
    TimedColorOutput &timed = s_timed[i];
    const ActuatorColor c = (ActuatorColor)i;
    if (timed.requested && !timed.motorStarted && timed.motorStartAt &&
        (int32_t)(now - timed.motorStartAt) >= 0) {
      timed.motorStartAt = 0;
      timed.motorStarted = true;
      timed.motorPulseOn = true;
      timed.motorToggleAt = now + ACTUATOR_MOTOR_PULSE_ON_MS;
      if (motorOutputHasMotor(c))
        motorOutputSetColor(c, true);
    }
    if (timed.requested && timed.motorStarted && timed.motorToggleAt &&
        (int32_t)(now - timed.motorToggleAt) >= 0) {
      timed.motorPulseOn = !timed.motorPulseOn;
      timed.motorToggleAt =
          now + (timed.motorPulseOn ? ACTUATOR_MOTOR_PULSE_ON_MS : ACTUATOR_MOTOR_PULSE_OFF_MS);
      if (motorOutputHasMotor(c))
        motorOutputSetColor(c, timed.motorPulseOn);
    }
    if (!timed.requested && timed.solenoidOffAt && (int32_t)(now - timed.solenoidOffAt) >= 0) {
      timed.solenoidOffAt = 0;
      if (c != ACTUATOR_COLOR_BLUE || SOLENOID_BLUE_ENABLED)
        solenoidOutputSetChannel(i, false);
    }
  }
}

static void onBubblePartyCmd(const ActuatorCmdPacket *pkt) {
  const uint32_t now = millis();
  dmxOutputExtendBubbleParty(now, pkt->level_r, pkt->level_g, pkt->level_b, pkt->level_w, pkt->color);
}

static bool s_failsafeTripped = false;

static void actuatorForceAllOutputsOff(const char *reason) {
  Serial.printf("[SAFE] %s — solenoids/relay/motors/fan OFF\n", reason);
  solenoidOutputAllOff();
  relayOutputAllOff();
  motorOutputAllOff();
  memset(s_timed, 0, sizeof(s_timed));
  if (dmxOutputReady())
    dmxOutputForceSafeOutputs();
}

static void actuatorRxWatchdog(uint32_t now) {
  if (!s_lastRxMs)
    return;
  if (now - s_lastRxMs < (uint32_t)ACTUATOR_RX_FAILSAFE_MS)
    return;
  // Include motors: an MCP/I2C failure can leave the solenoid state dark while a
  // pump is still running. Every energized output participates in the failsafe.
  if (!solenoidOutputAnyOn() && !relayOutputActive() && !motorOutputAnyOn())
    return;
  if (s_failsafeTripped)
    return;

  s_failsafeTripped = true;
  actuatorForceAllOutputsOff("ESP-NOW timeout");
}

static void processEspnowCmd(const ActuatorCmdPacket *pkt) {
  ++s_rxCount;
  s_lastRxMs = millis();
  s_failsafeTripped = false;

  if (pkt->on == ACTUATOR_ON_PURGE) {
    if (!s_purgeActive) {
      actuatorForceAllOutputsOff("enter purge");
      s_purgeActive = true;
      s_purgeStartedMs = millis();
      s_primeActive = false;
      motorOutputAllReverse();
      Serial.println("[PURGE] ACTIVE — all pumps reverse (IN1=LOW, IN2=HIGH) for five minutes");
    }
    return;
  }

  if (s_purgeActive)
    return;

  if (pkt->on == ACTUATOR_ON_PRIME) {
    const bool on = pkt->level_g != 0;
    actuatorForceAllOutputsOff(on ? "enter prime" : "exit prime");
    s_primeActive = on;
    if (on)
      motorOutputAllForward();
    Serial.printf("[PRIME] %s — all pumps %s\n", on ? "ACTIVE" : "OFF", on ? "continuous" : "stopped");
    return;
  }

  // PRIME keepalives are exclusively PRIME packets. The first ordinary command
  // is therefore also an idempotent escape hatch if PRIME_OFF was dropped.
  if (s_primeActive) {
    actuatorForceAllOutputsOff("ordinary command exits prime");
    s_primeActive = false;
    Serial.println("[PRIME] OFF — normal command received");
  }

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

  if (pkt->on == ACTUATOR_ON_DIRECT) {
    const ActuatorColor c = (ActuatorColor)pkt->color;
    const uint8_t mask = pkt->level_r;
    const bool on = pkt->level_g != 0;
    if (c < ACTUATOR_COLOR_COUNT) {
      if (mask & ACTUATOR_TARGET_SOLENOID)
        s_timed[(uint8_t)c].solenoidOffAt = 0;
      if (mask & ACTUATOR_TARGET_MOTOR) {
        s_timed[(uint8_t)c].requested = false;
        s_timed[(uint8_t)c].motorStartAt = 0;
        s_timed[(uint8_t)c].motorStarted = on;
        s_timed[(uint8_t)c].motorPulseOn = on;
        s_timed[(uint8_t)c].motorToggleAt = 0;
      }
    }
    // Independent targets — a plain colour ON cannot separate these.
    if ((mask & ACTUATOR_TARGET_SOLENOID) && (c != ACTUATOR_COLOR_BLUE || SOLENOID_BLUE_ENABLED))
      solenoidOutputSetChannel((uint8_t)c, on);
    if ((mask & ACTUATOR_TARGET_MOTOR) && motorOutputHasMotor(c))
      motorOutputSetColor(c, on);
    if (mask & ACTUATOR_TARGET_RELAY)
      relayOutputSetColor(c, on);
    Serial.printf("[ACT] direct %s mask=0x%X %s\n", serialStatusColorName(c), (unsigned)mask, on ? "ON" : "OFF");
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

// ESP-NOW invokes its callback on the Wi-Fi task/core. Never touch GPIO, I2C,
// DMX, or loop-owned timing state there. Queue a value-copy and let loop() remain
// the sole owner of all physical outputs.
static void queueEspnowCmd(const ActuatorCmdPacket *pkt, const uint8_t mac[6]) {
  (void)mac;
  if (!s_cmdQueue || !pkt)
    return;
  if (xQueueSend(s_cmdQueue, pkt, 0) == pdTRUE)
    return;

  // Prefer fresh state under an exceptional backlog. Discarding the oldest
  // packet is safer than losing a recent OFF/PRIME/PURGE command.
  ActuatorCmdPacket discarded;
  xQueueReceive(s_cmdQueue, &discarded, 0);
  xQueueSend(s_cmdQueue, pkt, 0);
  ++s_cmdQueueDrops;
}

static void serviceEspnowCommands() {
  if (!s_cmdQueue)
    return;
  ActuatorCmdPacket pkt;
  uint8_t serviced = 0;
  while (serviced < 32 && xQueueReceive(s_cmdQueue, &pkt, 0) == pdTRUE) {
    processEspnowCmd(&pkt);
    ++serviced;
  }
  static uint32_t lastReportedDrops = 0;
  const uint32_t drops = s_cmdQueueDrops.load();
  if (drops != lastReportedDrops) {
    lastReportedDrops = drops;
    Serial.printf("[ESPNOW] command queue overflow drops=%u\n", (unsigned)drops);
  }
}

static void debugHeartbeat(uint32_t now) {
  static uint32_t lastHb = 0;
  if (now - lastHb < (uint32_t)ACTUATOR_HB_MS)
    return;
  lastHb = now;

  const uint32_t sinceRx = s_lastRxMs ? (now - s_lastRxMs) : 0;
  const char *lastColor =
      s_lastColor < ACTUATOR_COLOR_COUNT ? serialStatusColorName(s_lastColor) : "none";

  const bool i2cAck = solenoidOutputProbeMcp();
  const char *i2cStr = i2cAck ? "OK" : "NO_ACK";
  const char *initStr = solenoidOutputReady() ? "OK" : "FAIL";

  Serial.printf("[HB] ACTUATOR  ESPNOW=%s  I2C_0x%02X=%s(init=%s)  SDA=%d SCL=%d  STBY=%s  DMX=%s  "
                "RLY=%s(held=0x%X)  rx=%u  last=%s %s  ago=%lums  ch=%u  mac=%s  up=%lus\n",
                espnowActuatorReady() ? "OK" : "DOWN", (unsigned)MCP23017_ADDR, i2cStr, initStr, (int)I2C_SDA,
                (int)I2C_SCL, motorOutputStbyLevel() < 0 ? "?" : (motorOutputStbyEnabled() ? "H" : "L"),
                dmxOutputReady() ? "OK" : "FAIL", relayOutputActive() ? "CLOSED" : "OPEN",
                (unsigned)relayOutputHeldMask(), (unsigned)s_rxCount, lastColor, s_lastOn ? "ON" : "OFF",
                s_lastRxMs ? (unsigned long)sinceRx : 0UL, (unsigned)ESPNOW_WIFI_CHANNEL,
                WiFi.macAddress().c_str(), (unsigned long)(now / 1000));

  // Do not run a 126-address I2C scan in normal operation. On a sick bus each
  // address can consume the Wire timeout, stalling command service for seconds.
  // The heartbeat's targeted MCP probe above is sufficient and bounded.
}

static void debugAlive(uint32_t now) {
  static uint32_t lastAlive = 0;
  if (now - lastAlive < 2000)
    return;
  lastAlive = now;
  // Relay state here too (every 2s) — [HB] is only every 5s and easy to miss.
  Serial.printf("[ALIVE] up=%lus  usb=%s  setup=%s  RLY=%s(held=0x%X) GPIO%d=%d  SOL=%s\n",
                (unsigned long)(now / 1000), (bool)Serial ? "yes" : "no", s_setupDone ? "done" : "running",
                relayOutputActive() ? "CLOSED" : "OPEN", (unsigned)relayOutputHeldMask(), (int)RELAY_PIN,
                digitalRead(RELAY_PIN), solenoidOutputAnyOn() ? "ON" : "off");
}

void setup() {
  serialLogBegin();
  Serial.println();
  Serial.println("[BOOT] solenoid-node reset — non-blocking USB serial @ 115200");

  serialStatusBanner("ACTUATOR (ESP-NOW — motors + solenoids + DMX)");
  // Relay first: park it open before anything else can glitch the pin.
  Serial.println("[BOOT] relay GPIO early init...");
  relayOutputEarlyInit();

  Serial.println("[BOOT] motor GPIO early init...");
  motorOutputEarlyInit();

  Serial.println("[BOOT] motor drivers...");
  motorOutputBegin();

  Serial.println("[BOOT] relay...");
  relayOutputBegin();
  relayOutputSelfTest();

  Serial.println("[BOOT] MCP23017 / I2C...");
  s_solOk = solenoidOutputBegin();

  Serial.println("[BOOT] DMX512...");
  s_dmxOk = dmxOutputBegin();

  Serial.println("[BOOT] ESP-NOW RX...");
  s_cmdQueue = xQueueCreate(32, sizeof(ActuatorCmdPacket));
  if (!s_cmdQueue)
    Serial.println("[BOOT] ERROR: ESP-NOW command queue allocation failed");
  s_espOk = s_cmdQueue && espnowActuatorBeginRx(queueEspnowCmd);

  solenoidOutputAllOff();
  relayOutputAllOff();
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

  serviceEspnowCommands();
  const uint32_t now = millis();
  if (s_purgeActive && now - s_purgeStartedMs >= (uint32_t)ACTUATOR_PURGE_DURATION_MS) {
    motorOutputAllOff();
    s_purgeActive = false;
    Serial.println("[PURGE] COMPLETE — five minutes elapsed; all pumps stopped");
  }
  serviceTimedColorOutputs(now);
  debugAlive(now);
  debugHeartbeat(now);
  actuatorRxWatchdog(now);
  dmxOutputService(now);
  delay(1);
}

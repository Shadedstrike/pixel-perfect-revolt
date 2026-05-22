#include "mqtt_link.h"
#include "mqtt_config.h"
#include "mqtt_protocol.h"
#include "lilygo_eth_w5500.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <cstring>

#ifndef MQTT_CTRL_HB_MS
#define MQTT_CTRL_HB_MS 5000
#endif

static WiFiClient s_wifi;
static PubSubClient s_mqtt(s_wifi);
static bool s_haveWifi = false;

// Periodic link status (matches solenoid-node [HB] idea); runs even when WiFi/MQTT down.
static void mqttControllerHeartbeat() {
  static uint32_t s_lastHb = 0;
  const uint32_t now = millis();
  if (now - s_lastHb < (uint32_t)MQTT_CTRL_HB_MS)
    return;
  s_lastHb = now;

  const char *mqttSt = s_mqtt.connected() ? "OK" : "DOWN";

#if LILYGO_ETH_BOARD != 0
  const bool link = lilygoEthW5500Connected();
  Serial.printf("[HB] ETH=%s ip=%s MQTT=%s  broker=%s:%u  (ctrl)\n", link ? "OK" : "DOWN",
                link ? lilygoEthLocalIP().toString().c_str() : "—", mqttSt, MQTT_HOST,
                (unsigned)MQTT_PORT);
#else
  const wl_status_t st = WiFi.status();
  const bool wifiOk = (st == WL_CONNECTED);
  Serial.printf("[HB] WiFi=%s(%d) ip=%s MQTT=%s  broker=%s:%u  (ctrl)\n", wifiOk ? "OK" : "DOWN",
                (int)st, wifiOk ? WiFi.localIP().toString().c_str() : "—", mqttSt, MQTT_HOST,
                (unsigned)MQTT_PORT);
#endif
}

static bool netLinkUp() {
#if LILYGO_ETH_BOARD != 0
  return lilygoEthW5500Connected();
#else
  return WiFi.status() == WL_CONNECTED;
#endif
}

// Side-column GPIOs must match BTN_PINS in config.cpp (indices 0–3, 6–9).
// Right column uses 39 (yellow) and 15 (green); 11/9 kept for older wiring.
// Note: default RHYTHM_SD_CS_PIN is GPIO 12 — that key is unreliable while SD SPI is active (e.g. rhythm MP3).
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

// Bottom→top each column: yellow, blue, green, red (matches BTN_PINS order in config.cpp).
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

static bool mqttPublishSolenoidColor(const char *color, bool on) {
  if (!color)
    return false;
  if (!s_mqtt.connected())
    return false;
  char payload[24];
  snprintf(payload, sizeof(payload), "%s,%d", color, on ? 1 : 0);
  bool ok = s_mqtt.publish(MQTT_TOPIC_SOLENOID_CMD, payload, false);
  if (!ok)
    Serial.println("[MQTT] publish failed (buffer/full?)");
  else
    Serial.printf("[MQTT] tx %s \"%s\"\n", MQTT_TOPIC_SOLENOID_CMD, payload);
  return ok;
}

// One monostable timer per MQTT color (left/right keys that share a color share one coil).
static uint32_t s_solenoidPulseEndMs[4];
static const char *const kPulseColorNames[4] = {"yellow", "blue", "green", "red"};

static int colorNameToPulseSlot(const char *c) {
  if (!c)
    return -1;
  for (int i = 0; i < 4; i++) {
    if (!strcmp(c, kPulseColorNames[i]))
      return i;
  }
  return -1;
}

void mqttSolenoidPulseOnSideColumnPress(int btnIdx) {
  const char *color = sideColumnBtnIdxToColor(btnIdx);
  if (!color)
    return;
  int slot = colorNameToPulseSlot(color);
  if (slot < 0)
    return;
  uint32_t now = millis();
  s_solenoidPulseEndMs[slot] = now + (uint32_t)MQTT_SOLENOID_PULSE_MS;
  mqttPublishSolenoidColor(color, true);
}

void mqttSolenoidPulseService(uint32_t nowMs) {
  for (int i = 0; i < 4; i++) {
    uint32_t end = s_solenoidPulseEndMs[i];
    if (end == 0)
      continue;
    // Fired when nowMs has reached end (wrap-safe for ~49-day uptime).
    if ((int32_t)(nowMs - end) < 0)
      continue;
    s_solenoidPulseEndMs[i] = 0;
    mqttPublishSolenoidColor(kPulseColorNames[i], false);
  }
}

static void wifiEnsure() {
  if (s_haveWifi && WiFi.status() == WL_CONNECTED)
    return;
  s_haveWifi = false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

static void mqttReconnect() {
  if (!netLinkUp())
    return;
  if (s_mqtt.connected())
    return;

  const char *user = MQTT_USER[0] ? MQTT_USER : nullptr;
  const char *pass = MQTT_PASSWORD[0] ? MQTT_PASSWORD : nullptr;
  if (s_mqtt.connect("pyrrisma-ctrl", user, pass)) {
    Serial.println("[MQTT] connected");
  } else {
    Serial.printf("[MQTT] connect failed rc=%d\n", s_mqtt.state());
  }
}

bool mqttLinkSetup() {
  s_mqtt.setServer(MQTT_HOST, MQTT_PORT);
  s_mqtt.setBufferSize(128);

#if LILYGO_ETH_BOARD != 0
  // Do not block for DHCP here — the Arduino loop task WDT (~5s) resets if setup() waits 45s.
  Serial.println("[ETH] starting W5500 (DHCP)...");
  if (!lilygoEthW5500Begin()) {
    Serial.println("[ETH] ETH.begin failed");
    return false;
  }
  if (lilygoEthW5500Connected()) {
    Serial.printf("[ETH] OK ip=%s\n", lilygoEthLocalIP().toString().c_str());
    mqttReconnect();
  } else {
    Serial.println("[ETH] DHCP in background; MQTT when link up");
  }
  return true;
#else
  // Same: WiFi often needs >5s; association finishes in mqttLinkLoop.
  wifiEnsure();
  Serial.printf("[WiFi] started (non-blocking) SSID=%s\n", WIFI_SSID);
  if (WiFi.status() == WL_CONNECTED) {
    s_haveWifi = true;
    Serial.printf("[WiFi] OK ip=%s\n", WiFi.localIP().toString().c_str());
    mqttReconnect();
    return true;
  }
  return false;
#endif
}

void mqttLinkLoop() {
  mqttControllerHeartbeat();
  mqttSolenoidPulseService(millis());

#if LILYGO_ETH_BOARD != 0
  if (!lilygoEthW5500Connected()) {
    return;
  }
#else
  if (WiFi.status() != WL_CONNECTED) {
    s_haveWifi = false;
    static uint32_t lastAttempt;
    if (millis() - lastAttempt > 5000) {
      lastAttempt = millis();
      wifiEnsure();
    }
    return;
  }
  s_haveWifi = true;
#endif
  if (!s_mqtt.connected()) {
    static uint32_t lastReconnect;
    if (millis() - lastReconnect > 3000) {
      lastReconnect = millis();
      mqttReconnect();
    }
    return;
  }
  s_mqtt.loop();
}

bool mqttPublishSolenoidForSideColumn(int btnIdx, bool on) {
  return mqttPublishSolenoidColor(sideColumnBtnIdxToColor(btnIdx), on);
}

bool mqttPublishSolenoidForGpio(int gpio, bool on) {
  return mqttPublishSolenoidColor(gpioToColorName(gpio), on);
}

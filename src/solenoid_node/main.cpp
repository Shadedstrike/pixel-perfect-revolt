// Solenoid actuator node: MQTT -> Adafruit "I2C to 8 Channel Solenoid Driver" (MCP23017 + MOSFETs).
// STEMMA QT / Qwiic: connect SDA/SCL + GND; power Vcc from 3.3 V (ESP32) or 5 V per Adafruit.
// Solenoid rail: center V+ terminal = 3–24 V per coil rating; common GND with ESP32.
//
// Wiring / topology:
//   - Flash: pio run -e solenoid-node -t upload
//   - I2C default address 0x20 (change MCP23017_ADDR if A0–A2 jumpers set).
//   - MQTT: mqtt_config.h + topic pyrrisma/solenoid/cmd (see mqtt_protocol.h).
//
// Outputs 0–7 = MCP23017 port A (board labels 0–7 / A0–A7). HIGH = solenoid ON (Adafruit example).
// Color map (must match mqtt_link.cpp gpioToColorName on controller):
//   red=0, green=1, blue=2, yellow=3

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <cctype>
#include <cstring>
#include <strings.h>
#include <PubSubClient.h>
#include <Adafruit_MCP23X17.h>

#include "mqtt_config.h"
#include "mqtt_protocol.h"
#include "lilygo_eth_w5500.h"

#ifndef I2C_SDA
#define I2C_SDA 17
#endif
#ifndef I2C_SCL
#define I2C_SCL 18
#endif

#ifndef MCP23017_ADDR
#define MCP23017_ADDR 0x20
#endif

#ifndef SOLENOID_DEBUG_HB_MS
#define SOLENOID_DEBUG_HB_MS 5000
#endif

static bool s_mcpOk = false;

static Adafruit_MCP23X17 mcp;
static WiFiClient s_wifi;
static PubSubClient s_mqtt(s_wifi);

static bool i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// Serial status every SOLENOID_DEBUG_HB_MS (WiFi/ETH, I2C ACK at MCP23017_ADDR, MQTT).
static void debugHeartbeat(uint32_t now) {
  static uint32_t lastHb = 0;
  if (now - lastHb < (uint32_t)SOLENOID_DEBUG_HB_MS)
    return;
  lastHb = now;

  const bool i2cAck = i2cProbe(MCP23017_ADDR);
  const char *i2cStr = i2cAck ? "OK" : "NO_ACK";
  const char *mqttStr = s_mqtt.connected() ? "OK" : "DOWN";

#if LILYGO_ETH_BOARD != 0
  const bool link = lilygoEthW5500Connected();
  Serial.printf("[HB] ETH=%s ip=%s  I2C_0x%02X=%s(init=%s)  MQTT=%s  SDA=%d SCL=%d\n", link ? "OK" : "DOWN",
                link ? lilygoEthLocalIP().toString().c_str() : "—", MCP23017_ADDR, i2cStr, s_mcpOk ? "OK" : "FAIL",
                mqttStr, (int)I2C_SDA, (int)I2C_SCL);
#else
  const bool wifiOk = (WiFi.status() == WL_CONNECTED);
  Serial.printf("[HB] WiFi=%s(%d) ip=%s  I2C_0x%02X=%s(init=%s)  MQTT=%s  SDA=%d SCL=%d\n",
                wifiOk ? "OK" : "DOWN", (int)WiFi.status(), wifiOk ? WiFi.localIP().toString().c_str() : "—",
                MCP23017_ADDR, i2cStr, s_mcpOk ? "OK" : "FAIL", mqttStr, (int)I2C_SDA, (int)I2C_SCL);
#endif
}

static int channelForColor(const char *c) {
  if (!c || !*c)
    return -1;
  if (!strcasecmp(c, "red"))
    return 0;
  if (!strcasecmp(c, "green"))
    return 1;
  if (!strcasecmp(c, "blue"))
    return 2;
  if (!strcasecmp(c, "yellow"))
    return 3;
  return -1;
}

static void setSolenoid(uint8_t ch, bool on) {
  if (!s_mcpOk || ch >= 8)
    return;
  mcp.digitalWrite(ch, on ? HIGH : LOW);
}

static void onMqttMessage(char *topic, byte *payload, unsigned int len) {
  (void)topic;
  if (len >= 32) {
    Serial.printf("[MQTT] rx ignored len=%u (max 31)\n", len);
    return;
  }
  char buf[32];
  memcpy(buf, payload, len);
  buf[len] = 0;

  char *comma = strchr(buf, ',');
  if (!comma) {
    Serial.printf("[MQTT] rx ignored no_comma \"%s\"\n", buf);
    return;
  }
  *comma = 0;
  // Trim color token — brokers/HA often append \r\n; strcmp would fail on "yellow\r".
  char *cstart = buf;
  while (*cstart && std::isspace((unsigned char)*cstart))
    ++cstart;
  char *cend = comma;
  while (cend > cstart && std::isspace((unsigned char)cend[-1]))
    --cend;
  *cend = '\0';

  char *vstart = comma + 1;
  while (*vstart && std::isspace((unsigned char)*vstart))
    ++vstart;
  int on = atoi(vstart);

  int ch = channelForColor(cstart);
  if (ch < 0) {
    Serial.printf("[MQTT] rx ignored unknown_color len=%u \"%s\"\n", len, cstart);
    return;
  }
  if (!s_mcpOk) {
    static bool s_loggedMcpSkip;
    if (!s_loggedMcpSkip) {
      s_loggedMcpSkip = true;
      Serial.println("[SOL] MCP23017 not initialized — commands parsed but not driven");
    }
    return;
  }
  setSolenoid((uint8_t)ch, on != 0);
  Serial.printf("[SOL] ch=%d %s\n", ch, on ? "ON" : "OFF");
}

static void mqttCallback(char *topic, byte *payload, unsigned int len) {
  char preview[40];
  unsigned n = len < sizeof(preview) - 1 ? len : sizeof(preview) - 2;
  memcpy(preview, payload, n);
  preview[n] = 0;
  Serial.printf("[MQTT] rx topic=%s len=%u \"%s\"\n", topic ? topic : "?", len, preview);
  onMqttMessage(topic, payload, len);
}

static bool netLinkUp() {
#if LILYGO_ETH_BOARD != 0
  return lilygoEthW5500Connected();
#else
  return WiFi.status() == WL_CONNECTED;
#endif
}

static void mqttReconnect() {
  if (!netLinkUp())
    return;
  const char *user = MQTT_USER[0] ? MQTT_USER : nullptr;
  const char *pass = MQTT_PASSWORD[0] ? MQTT_PASSWORD : nullptr;
  if (s_mqtt.connect("pyrrisma-solenoid", user, pass)) {
    s_mqtt.subscribe(MQTT_TOPIC_SOLENOID_CMD);
    Serial.println("[MQTT] subscribed");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Pyrrisma solenoid node (MCP23017) ===");
  Serial.printf("[I2C] SDA=GPIO%d SCL=GPIO%d  MCP target=0x%02X  hb every %dms\n", (int)I2C_SDA, (int)I2C_SCL,
                MCP23017_ADDR, SOLENOID_DEBUG_HB_MS);

  Wire.begin(I2C_SDA, I2C_SCL, 100000);
  s_mcpOk = mcp.begin_I2C(MCP23017_ADDR, &Wire);
  if (!s_mcpOk) {
    Serial.printf("[MCP23017] begin_I2C(0x%02X) failed — check STEMMA QT / wiring\n", MCP23017_ADDR);
  } else {
    Serial.printf("[MCP23017] OK addr=0x%02X\n", MCP23017_ADDR);
    for (int i = 0; i < 8; i++) {
      mcp.pinMode(i, OUTPUT);
      mcp.digitalWrite(i, LOW);
    }
  }

  s_mqtt.setServer(MQTT_HOST, MQTT_PORT);
  s_mqtt.setCallback(mqttCallback);
  s_mqtt.setBufferSize(128);

#if LILYGO_ETH_BOARD != 0
  Serial.println("[ETH] starting W5500 (DHCP)...");
  if (!lilygoEthW5500Begin())
    Serial.println("[ETH] ETH.begin failed");
  else {
    uint32_t t0 = millis();
    while (!lilygoEthW5500Connected() && millis() - t0 < 45000)
      delay(200);
    if (lilygoEthW5500Connected())
      Serial.printf("[ETH] OK %s\n", lilygoEthLocalIP().toString().c_str());
    else
      Serial.println("[ETH] no IP yet (cable/DHCP); will retry in loop");
  }
#else
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WiFi] connecting to %s ...\n", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000)
    delay(200);
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("[WiFi] OK %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("[WiFi] failed, retrying in loop");
#endif
}

void loop() {
  const uint32_t now = millis();
  debugHeartbeat(now);

  if (!netLinkUp()) {
#if LILYGO_ETH_BOARD == 0
    static uint32_t t;
    if (millis() - t > 5000) {
      t = millis();
      WiFi.reconnect();
    }
#endif
    delay(100);
    return;
  }
  if (!s_mqtt.connected()) {
    static uint32_t tr;
    if (millis() - tr > 3000) {
      tr = millis();
      mqttReconnect();
    }
    delay(50);
    return;
  }
  s_mqtt.loop();
}

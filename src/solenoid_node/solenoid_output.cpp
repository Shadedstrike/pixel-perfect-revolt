#include "solenoid_output.h"
#include "serial_log.h"

#include <Wire.h>
#include <Adafruit_MCP23X17.h>

#ifndef MCP23017_ADDR
#define MCP23017_ADDR 0x20
#endif

static Adafruit_MCP23X17 s_mcp;
static bool s_ready = false;
static bool s_channelOn[8] = {};

static bool i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void solenoidOutputPrintI2cScan() {
  Serial.printf("[I2C] scan SDA=%d SCL=%d  devices:", (int)I2C_SDA, (int)I2C_SCL);
  bool any = false;
  for (uint8_t addr = 1; addr < 127; addr++) {
    if ((addr & 0x0F) == 0)
      Serial.printf("\n[I2C] scan ... 0x%02X", addr);
    if (!i2cProbe(addr))
      continue;
    Serial.printf("%s0x%02X", any ? "," : " ", addr);
    any = true;
  }
  Serial.println(any ? "" : " none");
}

bool solenoidOutputProbeMcp() { return i2cProbe(MCP23017_ADDR); }

bool solenoidOutputBegin() {
  serialLogBegin();
  Wire.begin(I2C_SDA, I2C_SCL, 100000);
  Wire.setTimeOut(50);

  Serial.printf("[I2C] init SDA=GPIO%d SCL=GPIO%d @ 100kHz  MCP target=0x%02X\n", (int)I2C_SDA, (int)I2C_SCL,
                MCP23017_ADDR);

  const bool ack = i2cProbe(MCP23017_ADDR);
  Serial.printf("[I2C] MCP23017@0x%02X probe=%s\n", MCP23017_ADDR, ack ? "ACK" : "NO_ACK");

  s_ready = ack && s_mcp.begin_I2C(MCP23017_ADDR, &Wire);
  if (!s_ready) {
    Serial.printf("[SOL] MCP23017 begin_I2C(0x%02X) FAILED — solenoids disabled until I2C OK\n", MCP23017_ADDR);
    return false;
  }
  for (int i = 0; i < 8; i++) {
    s_mcp.pinMode(i, OUTPUT);
    s_mcp.digitalWrite(i, LOW);
  }
  Serial.printf("[SOL] MCP23017 OK addr=0x%02X  ch0=red ch1=green ch2=blue ch3=yellow\n", MCP23017_ADDR);
  return true;
}

bool solenoidOutputReady() { return s_ready; }

void solenoidOutputSetChannel(uint8_t ch, bool on) {
  if (!s_ready) {
    Serial.printf("[SOL] skip ch=%u (MCP not ready)\n", (unsigned)ch);
    return;
  }
  if (ch >= 8)
    return;
  s_channelOn[ch] = on;
  s_mcp.digitalWrite(ch, on ? HIGH : LOW);
  Serial.printf("[SOL] ch=%u %s\n", (unsigned)ch, on ? "ON" : "OFF");
}

void solenoidOutputAllOff() {
  for (uint8_t ch = 0; ch < 4; ch++)
    solenoidOutputSetChannel(ch, false);
}

bool solenoidOutputAnyOn() {
  for (uint8_t ch = 0; ch < 4; ch++) {
    if (s_channelOn[ch])
      return true;
  }
  return false;
}

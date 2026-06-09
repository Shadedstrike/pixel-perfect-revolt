#include "solenoid_output.h"

#include <Wire.h>
#include <Adafruit_MCP23X17.h>

#ifndef MCP23017_ADDR
#define MCP23017_ADDR 0x20
#endif

static Adafruit_MCP23X17 s_mcp;
static bool s_ready = false;

static bool i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool solenoidOutputBegin() {
  Wire.begin(I2C_SDA, I2C_SCL, 100000);
  Wire.setTimeOut(50);

  const bool ack = i2cProbe(MCP23017_ADDR);
  Serial.printf("[SOL] I2C scan SDA=%d SCL=%d  MCP23017@0x%02X=%s\n", (int)I2C_SDA, (int)I2C_SCL, MCP23017_ADDR,
                ack ? "ACK" : "NO_ACK");

  s_ready = ack && s_mcp.begin_I2C(MCP23017_ADDR, &Wire);
  if (!s_ready) {
    Serial.printf("[SOL] MCP23017 init FAILED — solenoids disabled until I2C OK\n");
    return false;
  }
  for (int i = 0; i < 8; i++) {
    s_mcp.pinMode(i, OUTPUT);
    s_mcp.digitalWrite(i, LOW);
  }
  Serial.printf("[SOL] MCP23017 OK addr=0x%02X\n", MCP23017_ADDR);
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
  s_mcp.digitalWrite(ch, on ? HIGH : LOW);
  Serial.printf("[SOL] ch=%u %s\n", (unsigned)ch, on ? "ON" : "OFF");
}

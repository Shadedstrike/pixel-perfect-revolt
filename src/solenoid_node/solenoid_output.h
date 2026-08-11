#ifndef SOLENOID_NODE_SOLENOID_OUTPUT_H
#define SOLENOID_NODE_SOLENOID_OUTPUT_H

#include <Arduino.h>

#ifndef I2C_SDA
#define I2C_SDA 18
#endif
#ifndef I2C_SCL
#define I2C_SCL 17
#endif

#ifndef MCP23017_ADDR
#define MCP23017_ADDR 0x20
#endif

// Blue (MCP ch 2) is normally never fired — see main.cpp. Set to 1 to include it,
// e.g. -DSOLENOID_BLUE_ENABLED=1 in platformio.ini.
#ifndef SOLENOID_BLUE_ENABLED
#define SOLENOID_BLUE_ENABLED 0
#endif

bool solenoidOutputBegin();
bool solenoidOutputReady();
void solenoidOutputSetChannel(uint8_t ch, bool on);
void solenoidOutputAllOff();
bool solenoidOutputAnyOn();

// Print all I2C devices on the bus (requires Wire already begun).
void solenoidOutputPrintI2cScan();
// Live ACK check for MCP23017 (does not require MCP driver init).
bool solenoidOutputProbeMcp();

#endif

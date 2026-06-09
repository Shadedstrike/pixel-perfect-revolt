#ifndef SOLENOID_NODE_SOLENOID_OUTPUT_H
#define SOLENOID_NODE_SOLENOID_OUTPUT_H

#include <Arduino.h>

#ifndef I2C_SDA
#define I2C_SDA 18
#endif
#ifndef I2C_SCL
#define I2C_SCL 17
#endif

bool solenoidOutputBegin();
bool solenoidOutputReady();
void solenoidOutputSetChannel(uint8_t ch, bool on);

#endif

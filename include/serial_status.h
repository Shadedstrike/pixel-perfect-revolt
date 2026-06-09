#ifndef SERIAL_STATUS_H
#define SERIAL_STATUS_H

#include <Arduino.h>
#include "actuator_protocol.h"

// Call once at boot after Serial.begin — prints role banner + monitor hints.
void serialStatusBanner(const char *roleTitle);

const char *serialStatusColorName(ActuatorColor color);
const char *serialStatusColorNameU8(uint8_t color);

#endif

#ifndef SOLENOID_NODE_SERIAL_LOG_H
#define SOLENOID_NODE_SERIAL_LOG_H

#include <Arduino.h>

// USB CDC writes must never block setup — a full TX buffer with no host hangs boot forever.
inline void serialLogBegin() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
}

#endif

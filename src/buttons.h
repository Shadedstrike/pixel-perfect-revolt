#ifndef BUTTONS_H
#define BUTTONS_H

#include <Arduino.h>
#include "config.h"

bool readLevelDebounced(int pin, bool &edgeDown, bool &edgeUp);

// After SD/SPI use, reclaim button GPIOs. Do not call every loop() — conflicts with W5500 on T-ETH-Lite (SPI 10/11/12).
// Elite: SD shares 9/11/12 with keys; Lite: skip 10/11/12 here so Ethernet keeps SPI.
void buttonsRestoreInputPullups();

// Each loop: re-mux SD-overlap keys only (MISO/MOSI/CS) for pins that are both SD and BTN. Elite: MOSI 11 no longer a key; CS 12 may still overlap.
void buttonsRefreshSdSharedPins();

#endif // BUTTONS_H


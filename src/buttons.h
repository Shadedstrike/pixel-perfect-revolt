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

// Fast input poll (defined in main.cpp). Samples all 10 buttons, latches edges
// for the main loop to consume, and pushes side-column holds straight to the
// actuator link — so relay/solenoid latency does not depend on how long the main
// loop takes. Called once per loop AND from inside the rhythm audio pump, which
// is what keeps taps responsive during song mode. Self-throttling; main thread
// only. Worst-case gap between polls is tracked in inputFastPollWorstGapMs().
void inputFastPoll(uint32_t now);
uint32_t inputFastPollWorstGapMs(bool reset);
// Read and clear the latched state/edges for one button.
void inputFastPollTake(int i, bool &level, bool &edgeDown, bool &edgeUp);

#endif // BUTTONS_H


#ifndef BUTTONS_H
#define BUTTONS_H

#include <Arduino.h>
#include "config.h"

// Contact-bounce lockout, ms. Leading-edge: a change is accepted immediately and
// further changes on that pin are ignored for this long. Costs no latency. Arcade
// microswitches bounce for ~1-5 ms, so 10 covers it with margin. Raising this
// directly caps how fast the relay can be retriggered (1000/BTN_DEBOUNCE_MS Hz).
#ifndef BTN_DEBOUNCE_MS
#define BTN_DEBOUNCE_MS 10
#endif

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
// Sampler task period, ms. Bounds button->relay latency.
#ifndef INPUT_TASK_PERIOD_MS
#define INPUT_TASK_PERIOD_MS 2
#endif

void inputFastPoll(uint32_t now);
// Start the dedicated sampler task (call once from setup). Runs above loop()
// priority so LCD/MP3 stalls cannot delay input.
void inputFastPollStartTask();
bool inputFastPollTaskRunning();
uint32_t inputFastPollWorstGapMs(bool reset);
// Read and clear the latched state/edges for one button.
void inputFastPollTake(int i, bool &level, bool &edgeDown, bool &edgeUp);

#endif // BUTTONS_H


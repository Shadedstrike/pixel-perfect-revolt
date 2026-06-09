#ifndef ACTUATOR_LINK_H
#define ACTUATOR_LINK_H

#include <Arduino.h>

bool actuatorLinkSetup();
void actuatorLinkLoop();

// Side-column press: latch ON over ESP-NOW, auto OFF after ACTUATOR_PULSE_MS (retrigger extends).
void actuatorSolenoidPulseOnSideColumnPress(int btnIdx);
void actuatorSolenoidPulseService(uint32_t nowMs);

bool actuatorPublishForSideColumn(int btnIdx, bool on);
bool actuatorPublishForGpio(int gpio, bool on);

#endif

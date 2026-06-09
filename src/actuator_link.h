#ifndef ACTUATOR_LINK_H
#define ACTUATOR_LINK_H

#include <Arduino.h>

bool actuatorLinkSetup();
void actuatorLinkLoop();

// Side-column hold: ON while pressed, OFF on release (left/right pairs share a color).
void actuatorSolenoidSideColumnHold(int btnIdx, bool on);

bool actuatorPublishForSideColumn(int btnIdx, bool on);
bool actuatorPublishForGpio(int gpio, bool on);

#endif

#ifndef MQTT_LINK_H
#define MQTT_LINK_H

#include <Arduino.h>

bool mqttLinkSetup();
void mqttLinkLoop();

// Side-column press: latch solenoid ON over MQTT, auto OFF after MQTT_SOLENOID_PULSE_MS (retrigger extends).
void mqttSolenoidPulseOnSideColumnPress(int btnIdx);
void mqttSolenoidPulseService(uint32_t nowMs);

// Direct on/off (bypasses pulse shaping). Prefer mqttSolenoidPulseOnSideColumnPress from the main UI.
bool mqttPublishSolenoidForSideColumn(int btnIdx, bool on);

// Legacy: map by GPIO (must stay in sync with gpioToColorName in mqtt_link.cpp).
bool mqttPublishSolenoidForGpio(int gpio, bool on);

#endif

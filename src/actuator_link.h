#ifndef ACTUATOR_LINK_H
#define ACTUATOR_LINK_H

#include <Arduino.h>

bool actuatorLinkSetup();
void actuatorLinkLoop();

void actuatorLinkSyncSideColumnHolds(const bool down[10]);
void actuatorLinkSyncDmxRgb(const bool down[10], uint32_t nowMs, bool synthIdle);
void actuatorLinkBubbleHoldCheck(const bool down[10], uint32_t nowMs);
void actuatorLinkPurgeCheck(const bool down[10], uint32_t nowMs);
bool actuatorLinkPurgeActive();
void actuatorLinkPrimeCheck(const bool down[10], const bool edgeDown[10], uint32_t nowMs);
bool actuatorLinkPrimeActive();

// Call after LED render: idle enter kills bubble fan + mirrors one idle LED to DMX.
void actuatorLinkUpdateIdle(bool synthIdle, uint32_t nowMs, uint8_t mirrorR, uint8_t mirrorG, uint8_t mirrorB);

bool actuatorPublishForSideColumn(int btnIdx, bool on);
bool actuatorPublishForGpio(int gpio, bool on);

#endif

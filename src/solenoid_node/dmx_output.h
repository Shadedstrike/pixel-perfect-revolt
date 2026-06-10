#ifndef SOLENOID_NODE_DMX_OUTPUT_H
#define SOLENOID_NODE_DMX_OUTPUT_H

#include <Arduino.h>
#include <cstdint>

#include "actuator_protocol.h"

bool dmxOutputBegin();
bool dmxOutputReady();

void dmxOutputSetSlot(uint16_t slot, uint8_t level);
uint8_t dmxOutputGetSlot(uint16_t slot);

void dmxOutputSetParLevels(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber);
void dmxOutputClearPar();

void dmxOutputSetBubbleLevels(uint8_t fan, uint8_t macro, uint8_t w, uint8_t b, uint8_t g, uint8_t r);
void dmxOutputClearBubble();

void dmxOutputSetColorHold(ActuatorColor color, bool on);
void dmxOutputSetCombinedRgbHold(uint8_t r, uint8_t g, uint8_t b, uint8_t amber);
void dmxOutputRefreshColorHolds();

bool dmxOutputBubbleFanActive(uint32_t nowMs);
bool dmxOutputBubblePartyActive(uint32_t nowMs);
void dmxOutputExtendBubbleParty(uint32_t nowMs, uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber);
void dmxOutputKillBubbleParty();
void dmxOutputServiceBubbleParty(uint32_t nowMs);

void dmxOutputSetIdleLevels(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber);
void dmxOutputClearIdle();

void dmxOutputService(uint32_t nowMs);

// Zero PAR + bubble fan on boot; burst DMX so fixtures latch off before bubbles.
void dmxOutputBootSafeState();

// Kill fan/idle/live levels (keep bubble color latch).
void dmxOutputForceSafeOutputs();

#endif

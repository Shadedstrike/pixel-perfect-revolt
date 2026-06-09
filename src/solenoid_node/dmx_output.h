#ifndef SOLENOID_NODE_DMX_OUTPUT_H
#define SOLENOID_NODE_DMX_OUTPUT_H

#include <Arduino.h>
#include <cstdint>

bool dmxOutputBegin();
bool dmxOutputReady();

// DMX slot index: 1 = first channel after start code (fixture address 1).
void dmxOutputSetSlot(uint16_t slot, uint8_t level);
uint8_t dmxOutputGetSlot(uint16_t slot);

void dmxOutputClearPar();
void dmxOutputSetBubble(bool on);

// Call often from loop(); sends frame when due or when dirty.
void dmxOutputService(uint32_t nowMs);

#endif

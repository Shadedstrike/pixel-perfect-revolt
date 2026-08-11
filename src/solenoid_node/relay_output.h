#ifndef SOLENOID_NODE_RELAY_OUTPUT_H
#define SOLENOID_NODE_RELAY_OUTPUT_H

#include <Arduino.h>

#include "actuator_protocol.h"

// Park the pin inactive. Safe to call before begin (and before anything else in
// setup) — this is what keeps the relay from clicking during boot.
void relayOutputEarlyInit();

bool relayOutputBegin();

// Follows the buttons: the relay is closed while ANY color is held, including
// blue (whose solenoid is disabled). Held colors are reference-counted by a
// bitmask, so releasing red while green is still down keeps the relay closed.
void relayOutputSetColor(ActuatorColor color, bool on);

void relayOutputAllOff();

// True = relay energized (contacts closed).
bool relayOutputActive();

// Bit N set = ActuatorColor N currently held.
uint8_t relayOutputHeldMask();

// Blocking boot self-test: drives the pin active/idle directly, bypassing the
// held-mask, with nothing else in the system running. Logs commanded level vs
// actual pad readback each step. Leaves the relay released.
void relayOutputSelfTest();

#endif

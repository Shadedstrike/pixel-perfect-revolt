#ifndef PI_LINK_H
#define PI_LINK_H

#include <Arduino.h>

// USB-serial control link used by the Raspberry Pi visualizer.
// PI_GAME suppresses the local synth but leaves buttons, LEDs and ESP-NOW live.
void piLinkSetup();
void piLinkLoop(uint32_t nowMs);
bool piLinkSynthMuted();

#endif

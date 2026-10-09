#ifndef PI_LINK_H
#define PI_LINK_H

#include <Arduino.h>

// USB-serial control link used by the Raspberry Pi visualizer.
// PI_GAME suppresses the local synth but leaves buttons, LEDs and ESP-NOW live.
void piLinkSetup();
void piLinkLoop(uint32_t nowMs);
bool piLinkSynthMuted();
// Queue a high-priority button event for the Pi. Events are buffered briefly so
// routine diagnostic output cannot make gameplay input disappear.
void piLinkButtonPressed(uint8_t index, uint32_t nowMs);
// External-game UI ownership. These keep the local rhythm game's gestures and
// screens untouched while the Raspberry Pi owns playback.
void piLinkDrawLcd(uint32_t nowMs);
void piLinkRenderLeds(uint32_t nowMs);

#endif

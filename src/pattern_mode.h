#ifndef PATTERN_MODE_H
#define PATTERN_MODE_H

#include <Arduino.h>

void patternModeLoop(uint32_t now, const bool *down, const bool *edgeDown);
bool patternModeIsActive();
bool patternModeSuppressNormalUi();
bool patternModeShouldSilenceSynth();
bool patternModeDrawLcd(uint32_t now);
void patternModeApplyLeds(uint32_t now, const bool *down);

#endif

#ifndef DEBUG_H
#define DEBUG_H

#include <Arduino.h>
#include "config.h"

// Debug Watch Variables for PlatformIO debugger
struct DebugWatchVars {
  // Button state
  bool buttonsDown[10];
  bool anyButtonDown;
  uint32_t lastPressTime;
  
  // Audio state
  float targetFreqL;
  float targetFreqR;
  float currentFreqL;
  float currentFreqR;
  float ampLeft;
  float ampRight;
  
  // Scale/Note state
  uint8_t currentScaleIndex;
  int8_t leftOffset;
  int8_t rightOffset;
  
  // Idle state
  bool isIdle;
  uint8_t idleModeValue;
  uint32_t timeSinceLastPress;
  
  // I2S state
  bool i2sOk;
  int i2sErrorCount;
  
  // LED state (sample first LED)
  uint8_t led0_r;
  uint8_t led0_g;
  uint8_t led0_b;
  
  // System state
  uint32_t uptimeMs;
  uint32_t loopCount;
};

extern DebugWatchVars dbgWatch;

void testButtonGPIOs();
void identifyBreakoutPins();

#endif // DEBUG_H


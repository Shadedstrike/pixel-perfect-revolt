#ifndef LEDS_H
#define LEDS_H

#include <Arduino.h>
#include <Adafruit_PWMServoDriver.h>
#include "config.h"

// LED state variables
extern float pressHue[10];
extern const uint32_t PRESS_FADE_MS;
extern const uint32_t IDLE_AFTER_MS;
extern const float IDLE_V_MIN;
extern const float IDLE_V_MAX;
extern const float IDLE_LFO_HZ;
extern const float IDLE_SAT;
extern const float PRESS_V;
extern const float RAINBOW_HZ;

// Flame params
extern const uint32_t FLAME_UPDATE_MS;
extern bool flipLeft;
extern bool flipRight;
extern float flameL[4];
extern float flameR[4];
extern uint32_t lastFlameStepMs;

// Button/LED state
extern bool down[10];
extern bool edgeDownArr[10];
extern bool edgeUpArr[10];
extern uint32_t releaseTs[10];
extern uint32_t lastPressMs;

extern uint8_t lastR[10];
extern uint8_t lastG[10];
extern uint8_t lastB[10];
extern float idleRateDegPerSec[10];
extern float idleBaseHue[10];

extern uint8_t idleMode;
extern uint32_t bothHoldStart;
extern bool holdLatch;

extern float rgbBaseHue[10];
extern float rgbHueRateDegPerSec[10];
extern float rainbowStartHue[10];

// PCA9685 drivers
extern Adafruit_PWMServoDriver pcaA;
extern Adafruit_PWMServoDriver pcaB;
extern uint16_t lastPWM_A[16];
extern uint16_t lastPWM_B[16];

// Functions
void getPressColorForGPIO(int gpio, uint8_t &r, uint8_t &g, uint8_t &b);
void pcaSet(uint8_t drv, uint8_t ch, uint8_t v);
void setLED_RGB(uint8_t i, uint8_t r, uint8_t g, uint8_t b);
// Front 16/46 rhythm beat flash — full drive (setLED_RGB applies FRONT_LED_BRIGHTNESS_SCALE).
void setLED_RGBRhythmFront(uint8_t i, uint8_t r, uint8_t g, uint8_t b);
void hsv2rgb(float h, float s, float v, uint8_t &r, uint8_t &g, uint8_t &b);
void playBootAnimation(); // Boot-up LED animation: green/turquoise pulsing
void applyVFloor(uint8_t &r, uint8_t &g, uint8_t &b, float floorV);
void warmManualRGB(float v, uint8_t &r, uint8_t &g, uint8_t &b);
void flameStep(float f[4]);
void flameToRGB(float v, bool cool, uint8_t &r, uint8_t &g, uint8_t &b);
void mixGroupColors(uint32_t now, const int *idxs, int n, float &mr, float &mg, float &mb, float &sumW);

#endif // LEDS_H


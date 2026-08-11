#ifndef AUDIO_H
#define AUDIO_H

#include <Arduino.h>
#include <driver/i2s.h>
#include "config.h"

// Synth state
extern float freqL;
extern float freqR;
extern float curFreqL;
extern float curFreqR;
extern float vibPhaseL;
extern float vibPhaseR;
extern float phaseL;
extern float phaseR;
extern float ampL;
extern float ampR;
extern uint32_t lastGoodI2S;
extern int i2s_consec_errors;
extern bool i2s_initialized;

void audioInit();
void playTestTone(float freqHz, uint32_t durationMs);
void playGeigerCounter(); // Randomized geiger counter noise (left then right)
void playPowerUpSound(); // Chill power-up sound with rising tones
void playWakeupSequence(uint32_t animStartTime = 0); // Combined wakeup: power-up then static with overlap. animStartTime: when animation started (0 = no LED updates)
void playFallingShepardTone(uint32_t durationMs = 2000); // Falling shepard's tone illusion
void audioDiagnosticTest(); // Continuous test pattern
void audioRender(float wantL, float wantR);
void audioPrintDiagnostics(); // Print I2S diagnostic information

// Main synth waveform (both L/R voices). Cycle with hold: left+right GREEN ~1.5s.
typedef enum : uint8_t {
  AUDIO_WAVE_SINE = 0,
  AUDIO_WAVE_TRIANGLE,
  AUDIO_WAVE_SOFT_SQUARE,
  AUDIO_WAVE_RICH,
  AUDIO_WAVE_COUNT
} AudioWaveShape;

void audioCycleWaveShape();
// Step +1 / -1 with wraparound (select menu).
void audioStepWaveShape(int delta);
AudioWaveShape audioGetWaveShape();
const char *audioWaveShapeName(AudioWaveShape w);
// Sample main-console waveform at phase [0, 2pi) for a given shape (LCD preview, etc.).
float audioWaveShapeSample(AudioWaveShape shape, float phaseRad);

#endif // AUDIO_H

